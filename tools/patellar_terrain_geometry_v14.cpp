#include "IsomApi.h"
#include "../MappingCoreLib/MappingCore.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

static ScMap copyToScMap(const MapFile & src)
{
    ScMap dest {};
    dest.tileWidth = uint16_t(src.getTileWidth());
    dest.tileHeight = uint16_t(src.getTileHeight());
    dest.tileset = src.getTileset();
    dest.isomRects.assign(src.isomRects.size(), {});
    if ( !src.isomRects.empty() )
        std::memcpy(&dest.isomRects[0], &src.isomRects[0], src.isomRects.size()*sizeof(Chk::IsomRect));
    dest.editorTiles = src.editorTiles;
    dest.tiles = src.tiles;
    return dest;
}

static void copyFromScMap(MapFile & dest, const ScMap & src)
{
    dest.dimensions.tileWidth = src.tileWidth;
    dest.dimensions.tileHeight = src.tileHeight;
    dest.tileset = src.tileset;
    dest.isomRects.assign(src.isomRects.size(), {});
    if ( !src.isomRects.empty() )
        std::memcpy(&dest.isomRects[0], &src.isomRects[0], src.isomRects.size()*sizeof(Chk::IsomRect));
    dest.editorTiles = src.editorTiles;
    dest.tiles = src.tiles;
}

static bool loadCv5(const std::string & path, Sc::Terrain_::Tiles & out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if ( !f ) return false;
    const auto size = size_t(f.tellg());
    f.seekg(0, std::ios::beg);
    if ( size == 0 || size % sizeof(Sc::Isom::TileGroup) != 0 ) return false;
    std::vector<uint8_t> bytes(size);
    if ( !f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)) ) return false;
    const size_t groups = size / sizeof(Sc::Isom::TileGroup);
    const auto * raw = reinterpret_cast<const Sc::Isom::TileGroup*>(bytes.data());
    out.tileGroups.assign(raw, raw + groups);
    out.loadIsom(size_t(Sc::Terrain::Tileset::Jungle));
    std::cout << "Loaded Jungle CV5 groups: " << groups << std::endl;
    return true;
}

static Chk::Unit makeStart(int tileX, int tileY, u8 owner)
{
    static u32 nextClassId = 1;
    Chk::Unit unit {};
    unit.classId = nextClassId++;
    unit.xc = u16(tileX*32 + 16);
    unit.yc = u16(tileY*32 + 16);
    unit.type = Sc::Unit::Type::StartLocation;
    unit.validFieldFlags = 0x0001;
    unit.owner = owner;
    unit.hitpointPercent = 100;
    unit.shieldPercent = 100;
    unit.energyPercent = 100;
    return unit;
}

static void configure1v1(MapFile & mapFile)
{
    for ( size_t i=0; i<Sc::Player::Total; ++i )
    {
        mapFile.slotTypes[i] = Sc::Player::SlotType::Inactive;
        mapFile.iownSlotTypes[i] = Sc::Player::SlotType::Inactive;
        mapFile.playerRaces[i] = Chk::Race::Inactive;
    }
    for ( size_t i=0; i<2; ++i )
    {
        mapFile.slotTypes[i] = Sc::Player::SlotType::GameOpen;
        mapFile.iownSlotTypes[i] = Sc::Player::SlotType::GameOpen;
        mapFile.playerRaces[i] = Chk::Race::UserSelectable;
    }
    mapFile.playerRaces[11] = Chk::Race::Neutral;
}

struct TerrainCounts {
    size_t water = 0;
    size_t jungle = 0;
    size_t raised = 0;
    size_t other = 0;
};

static TerrainCounts countTerrain(const std::vector<u16> & tiles, const Sc::Terrain_::Tiles & jungleData)
{
    TerrainCounts c {};
    for ( u16 tile : tiles )
    {
        const size_t group = size_t(Sc::Terrain::getTileGroup(tile));
        if ( group >= jungleData.tileGroups.size() ) { ++c.other; continue; }
        const auto tt = size_t(jungleData.tileGroups[group].terrainType);
        if ( tt == Sc::Isom::Brush::Jungle::Water ) ++c.water;
        else if ( tt == Sc::Isom::Brush::Jungle::Jungle_ ) ++c.jungle;
        else if ( tt == Sc::Isom::Brush::Jungle::RaisedJungle ) ++c.raised;
        else ++c.other;
    }
    return c;
}

static void printCounts(const char * label, const TerrainCounts & c)
{
    std::cout << label << ": water=" << c.water << " jungle=" << c.jungle
              << " raised=" << c.raised << " transition/other=" << c.other << std::endl;
}

static bool ellipse(double x, double y, double cx, double cy, double rx, double ry)
{
    const double dx = (x-cx)/rx;
    const double dy = (y-cy)/ry;
    return dx*dx + dy*dy <= 1.0;
}

static double halfWidthAtY(double y)
{
    struct Knot { double y; double hw; };
    static const Knot k[] = {
        {0,24},{8,29},{16,30},{24,37},{32,41},{40,43},{48,39},
        {56,40},{64,42},{72,40},{80,43},{88,41},{96,37},{104,30},
        {112,29},{120,25},{127,24}
    };
    for ( size_t i=1; i<sizeof(k)/sizeof(k[0]); ++i )
    {
        if ( y <= k[i].y )
        {
            const double t = (y-k[i-1].y)/(k[i].y-k[i-1].y);
            return k[i-1].hw + t*(k[i].hw-k[i-1].hw);
        }
    }
    return k[sizeof(k)/sizeof(k[0])-1].hw;
}

// One continuous mainland. The round lateral sesamoids are raised protrusions
// connected to the body by land necks; they are NOT water islands.
static bool isLand(double x, double y)
{
    const bool trunk = std::abs(x-64.0) <= halfWidthAtY(y);
    const bool leftSesamoid = ellipse(x,y,13.5,63.5,12.5,14.5);
    const bool rightSesamoid = ellipse(x,y,114.5,63.5,12.5,14.5);
    const bool leftNeck = (x >= 18 && x <= 34 && y >= 50 && y <= 77);
    const bool rightNeck = (x >= 94 && x <= 110 && y >= 50 && y <= 77);
    return trunk || leftSesamoid || rightSesamoid || leftNeck || rightNeck;
}

static bool topMainHigh(double x, double y)
{
    if ( y < 1 || y > 22 ) return false;
    double hw = 25;
    if ( y < 7 ) hw = 23 + y*0.45;
    else if ( y > 16 ) hw = 28 - (y-16)*0.55;
    return std::abs(x-64.0) <= hw;
}

static bool bottomMainHigh(double x, double y)
{
    return topMainHigh(x,127.0-y);
}

static bool patellaHigh(double x, double y)
{
    return ellipse(x,y,64,63.5,15.0,11.5);
}

static bool leftSesamoidHigh(double x, double y)
{
    return ellipse(x,y,13.5,63.5,10.5,12.5);
}

static bool rightSesamoidHigh(double x, double y)
{
    return ellipse(x,y,114.5,63.5,10.5,12.5);
}

// Trochlear ridges: elongated 2F ridges embedded inside 1F lateral lanes.
static bool leftTrochleaHigh(double x, double y)
{
    if ( !ellipse(x,y,39.5,63.5,8.0,24.0) ) return false;
    // Slight waist to make a curved/condylar look rather than a solid rectangle.
    if ( y > 56 && y < 71 && x > 43 ) return false;
    return true;
}

static bool rightTrochleaHigh(double x, double y)
{
    return leftTrochleaHigh(127.0-x,y);
}

static bool anyHigh(double x, double y)
{
    return topMainHigh(x,y) || bottomMainHigh(x,y) || patellaHigh(x,y) ||
           leftSesamoidHigh(x,y) || rightSesamoidHigh(x,y) ||
           leftTrochleaHigh(x,y) || rightTrochleaHigh(x,y);
}

static bool mainRampLow(double x, double y)
{
    const bool top = (x >= 61 && x <= 67 && y >= 16 && y <= 25); // ~6-tile effective target
    const bool bottom = (x >= 61 && x <= 67 && y >= 102 && y <= 111);
    return top || bottom;
}

static bool patellaRampLow(double x, double y)
{
    const bool north = (x >= 62 && x <= 66 && y >= 49 && y <= 56); // ~4 Zealots target
    const bool south = (x >= 62 && x <= 66 && y >= 71 && y <= 79);
    return north || south;
}

static bool sesamoidRampLow(double x, double y)
{
    // Two narrow entries on each raised sesamoid: upper + lower, ~2-3 Zealots.
    const bool lu = (x >= 19 && x <= 27 && y >= 53 && y <= 56);
    const bool ll = (x >= 19 && x <= 27 && y >= 69 && y <= 72);
    const bool ru = (x >= 100 && x <= 108 && y >= 53 && y <= 56);
    const bool rl = (x >= 100 && x <= 108 && y >= 69 && y <= 72);
    return lu || ll || ru || rl;
}

static bool trochleaRampLow(double x, double y)
{
    // Modest side access to the 2F portions of the trochlear ridges.
    const bool lu = (x >= 31 && x <= 37 && y >= 42 && y <= 46);
    const bool ll = (x >= 31 && x <= 37 && y >= 79 && y <= 83);
    const bool ru = (x >= 90 && x <= 96 && y >= 42 && y <= 46);
    const bool rl = (x >= 90 && x <= 96 && y >= 79 && y <= 83);
    return lu || ll || ru || rl;
}

static bool forceLow(double x, double y)
{
    return mainRampLow(x,y) || patellaRampLow(x,y) || sesamoidRampLow(x,y) || trochleaRampLow(x,y);
}

static bool stamp(ScMap & scMap, Chk::IsomCache & cache, size_t ix, size_t iy, size_t terrainType)
{
    if ( !scMap.placeIsomTerrain({ix,iy}, terrainType, 1, cache) ) return false;
    scMap.updateTilesFromIsom(cache);
    cache.finalizeUndoableOperation();
    return true;
}

int main(int argc, char ** argv)
{
    if ( argc < 3 ) {
        std::cerr << "usage: IsomTerrain.exe <jungle.cv5> <output.scx>" << std::endl;
        return 2;
    }

    constexpr auto TS = Sc::Terrain::Tileset::Jungle;
    constexpr size_t WATER = Sc::Isom::Brush::Jungle::Water;
    constexpr size_t JUNGLE = Sc::Isom::Brush::Jungle::Jungle_;
    constexpr size_t HIGH = Sc::Isom::Brush::Jungle::RaisedJungle;

    Sc::Terrain_::Tiles jungleData;
    if ( !loadCv5(argv[1], jungleData) ) return 3;

    auto mapFile = std::make_unique<MapFile>(TS,128,128);
    mapFile->setSaveType(SaveType::ExpansionScx);
    ScMap scMap = copyToScMap(*mapFile);
    Chk::IsomCache cache(TS,128,128,jungleData);

    // Proven v1.2 base: start with all legal Jungle low ground.
    const uint16_t jungleValue = ((cache.getTerrainTypeIsomValue(JUNGLE) << 4) | Chk::IsomRect::EditorFlag::Modified);
    scMap.isomRects.assign(scMap.getIsomWidth()*scMap.getIsomHeight(), Chk::IsomRect{jungleValue,jungleValue,jungleValue,jungleValue});
    cache.setAllChanged();
    scMap.updateTilesFromIsom(cache);
    cache.finalizeUndoableOperation();

    // Stage A: carve ONLY the outside of the approved knee silhouette to Water.
    size_t waterStamps = 0;
    for ( int ix=0; ix<=64; ++ix )
    {
        int y=0;
        if ( (ix+y)&1 ) ++y;
        for ( ; y<=127; y+=2 )
        {
            const double x = double(ix*2);
            if ( !isLand(x,double(y)) )
            {
                if ( !stamp(scMap,cache,size_t(ix),size_t(y),WATER) ) return 10;
                ++waterStamps;
            }
        }
    }
    std::cout << "Water silhouette stamps=" << waterStamps << std::endl;
    printCounts("AFTER SILHOUETTE", countTerrain(scMap.tiles,jungleData));

    // Stage B: actual 2F terrain. Naturals and central connector remain 1F.
    size_t highStamps = 0;
    for ( int ix=0; ix<=64; ++ix )
    {
        int y=0;
        if ( (ix+y)&1 ) ++y;
        for ( ; y<=127; y+=2 )
        {
            const double x = double(ix*2);
            if ( isLand(x,double(y)) && anyHigh(x,double(y)) && !forceLow(x,double(y)) )
            {
                if ( !stamp(scMap,cache,size_t(ix),size_t(y),HIGH) ) return 11;
                ++highStamps;
            }
        }
    }
    std::cout << "Raised high-ground stamps=" << highStamps << std::endl;

    // Stage C: repaint intended slope approaches as low Jungle. This is done last so
    // every high-ground area has deliberate, visible access instead of sealed cliffs.
    size_t lowCutStamps = 0;
    for ( int ix=0; ix<=64; ++ix )
    {
        int y=0;
        if ( (ix+y)&1 ) ++y;
        for ( ; y<=127; y+=2 )
        {
            const double x = double(ix*2);
            if ( isLand(x,double(y)) && forceLow(x,double(y)) )
            {
                if ( !stamp(scMap,cache,size_t(ix),size_t(y),JUNGLE) ) return 12;
                ++lowCutStamps;
            }
        }
    }
    std::cout << "Ramp/low-cut stamps=" << lowCutStamps << std::endl;

    const TerrainCounts before = countTerrain(scMap.tiles,jungleData);
    printCounts("BEFORE SAVE", before);
    if ( before.water < 2500 || before.jungle < 2500 || before.raised < 300 ) {
        std::cerr << "Terrain mix sanity check failed before save" << std::endl;
        return 13;
    }

    copyFromScMap(*mapFile,scMap);
    configure1v1(*mapFile);
    mapFile->addUnit(makeStart(64,12,0));
    mapFile->addUnit(makeStart(64,115,1));
    mapFile->setScenarioName(RawString("Patellar Luxation v1.4 TERRAIN GEOMETRY TEST"));
    mapFile->setScenarioDescription(RawString("Terrain-only 1v1 test: connected knee silhouette, 2F mains/patella/sesamoids/trochlear ridges, 1F naturals/connector, deliberate wide/narrow access. No resources."));

    if ( !mapFile->save(argv[2],true,false,false,true) ) return 20;

    MapFile verify(argv[2]);
    if ( verify.empty() || verify.getTileWidth()!=128 || verify.getTileHeight()!=128 || verify.getTileset()!=TS ) return 21;
    const TerrainCounts after = countTerrain(verify.tiles,jungleData);
    printCounts("AFTER REOPEN", after);
    size_t starts=0;
    for ( size_t i=0; i<verify.numUnits(); ++i )
        if ( verify.getUnit(i).type==Sc::Unit::Type::StartLocation ) ++starts;
    if ( starts!=2 || verify.numUnits()!=2 || after.water<2500 || after.jungle<2500 || after.raised<300 ) return 22;
    std::cout << "starts=" << starts << " units=" << verify.numUnits() << std::endl;
    return 0;
}
