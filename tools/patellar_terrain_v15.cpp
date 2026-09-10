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
    size_t highJungle = 0;
    size_t raised = 0;
    size_t other = 0;
};

static TerrainCounts countTerrain(const std::vector<u16> & tiles, const Sc::Terrain_::Tiles & data)
{
    TerrainCounts c {};
    for ( u16 tile : tiles )
    {
        const size_t group = size_t(Sc::Terrain::getTileGroup(tile));
        if ( group >= data.tileGroups.size() ) { ++c.other; continue; }
        const auto tt = size_t(data.tileGroups[group].terrainType);
        if ( tt == Sc::Isom::Brush::Jungle::Water ) ++c.water;
        else if ( tt == Sc::Isom::Brush::Jungle::Jungle_ ) ++c.jungle;
        else if ( tt == Sc::Isom::Brush::Jungle::HighJungle ) ++c.highJungle;
        else if ( tt == Sc::Isom::Brush::Jungle::RaisedJungle ) ++c.raised;
        else ++c.other;
    }
    return c;
}

static void printCounts(const char * label, const TerrainCounts & c)
{
    std::cout << label << ": water=" << c.water << " jungle=" << c.jungle
              << " highJungle=" << c.highJungle << " raised=" << c.raised
              << " transition/other=" << c.other << std::endl;
}

static bool ellipse(double x, double y, double cx, double cy, double rx, double ry)
{
    const double dx = (x-cx)/rx;
    const double dy = (y-cy)/ry;
    return dx*dx + dy*dy <= 1.0;
}

// Wider polar caps than v1.4 so the two main bases sit close to the map edges
// and have substantially more usable building area.
static double halfWidthAtY(double y)
{
    struct Knot { double y; double hw; };
    static const Knot k[] = {
        {0,35},{8,36},{16,35},{24,39},{32,41},{40,43},{48,39},
        {56,40},{64,42},{72,40},{80,43},{88,41},{96,39},{104,35},
        {112,36},{120,35},{127,35}
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

static bool isLand(double x, double y)
{
    const bool trunk = std::abs(x-64.0) <= halfWidthAtY(y);
    const bool leftSesamoid = ellipse(x,y,13.5,63.5,12.5,14.5);
    const bool rightSesamoid = ellipse(x,y,114.5,63.5,12.5,14.5);
    const bool leftNeck = (x >= 18 && x <= 35 && y >= 48 && y <= 79);
    const bool rightNeck = (x >= 93 && x <= 110 && y >= 48 && y <= 79);
    return trunk || leftSesamoid || rightSesamoid || leftNeck || rightNeck;
}

static bool topMainHigh(double x, double y)
{
    if ( y < 0 || y > 26 ) return false;
    double hw = 31.5;
    if ( y > 18 ) hw = 31.5 - (y-18)*0.55;
    return std::abs(x-64.0) <= hw;
}
static bool bottomMainHigh(double x, double y) { return topMainHigh(x,127.0-y); }
static bool patellaHigh(double x, double y) { return ellipse(x,y,64,63.5,15.0,11.5); }
static bool leftSesamoidHigh(double x, double y) { return ellipse(x,y,13.5,63.5,10.5,12.5); }
static bool rightSesamoidHigh(double x, double y) { return ellipse(x,y,114.5,63.5,10.5,12.5); }

// Very thin elongated trochlear ridges, as requested: visually a narrow vertical
// 2F line rather than the broad bulky ridges from v1.4.
static bool leftTrochleaHigh(double x, double y)
{
    if ( y < 39 || y > 88 ) return false;
    const double phase = (y-39.0)/(88.0-39.0);
    const double cx = 40.0 + 1.2*std::sin(phase*3.14159265358979323846);
    return std::abs(x-cx) <= 3.0;
}
static bool rightTrochleaHigh(double x, double y) { return leftTrochleaHigh(127.0-x,y); }

static bool anyHigh(double x, double y)
{
    return topMainHigh(x,y) || bottomMainHigh(x,y) || patellaHigh(x,y) ||
           leftSesamoidHigh(x,y) || rightSesamoidHigh(x,y) ||
           leftTrochleaHigh(x,y) || rightTrochleaHigh(x,y);
}

static bool stamp(ScMap & scMap, Chk::IsomCache & cache, size_t ix, size_t iy, size_t terrainType)
{
    if ( !scMap.placeIsomTerrain({ix,iy}, terrainType, 1, cache) ) return false;
    scMap.updateTilesFromIsom(cache);
    cache.finalizeUndoableOperation();
    return true;
}

struct Patch {
    int sx, sy, w, h;
    int dx, dy;
    const char * name;
};

// Copy exact MTXM ramp tiles from Falkoner's Jungle Custom Ramps map.
// These are real hand-authored ramp/stair tile combinations, not low-ground
// rectangles painted over a sealed cliff.
static bool copyPatch(MapFile & dest, const MapFile & src, const Patch & p)
{
    const int sw = int(src.getTileWidth()), sh = int(src.getTileHeight());
    const int dw = int(dest.getTileWidth()), dh = int(dest.getTileHeight());
    if ( p.sx < 0 || p.sy < 0 || p.dx < 0 || p.dy < 0 ||
         p.sx+p.w > sw || p.sy+p.h > sh || p.dx+p.w > dw || p.dy+p.h > dh )
        return false;

    std::cout << "PATCH " << p.name << " src=(" << p.sx << "," << p.sy << ") "
              << p.w << "x" << p.h << " dst=(" << p.dx << "," << p.dy << ")" << std::endl;

    for ( int y=0; y<p.h; ++y )
    for ( int x=0; x<p.w; ++x )
    {
        const u16 t = src.tiles[size_t(p.sy+y)*sw + size_t(p.sx+x)];
        dest.tiles[size_t(p.dy+y)*dw + size_t(p.dx+x)] = t;
        dest.editorTiles[size_t(p.dy+y)*dw + size_t(p.dx+x)] = t;
    }
    return true;
}

static bool patchMatches(const MapFile & dest, const MapFile & src, const Patch & p)
{
    const int sw = int(src.getTileWidth()), dw = int(dest.getTileWidth());
    for ( int y=0; y<p.h; ++y )
    for ( int x=0; x<p.w; ++x )
        if ( dest.tiles[size_t(p.dy+y)*dw + size_t(p.dx+x)] !=
             src.tiles[size_t(p.sy+y)*sw + size_t(p.sx+x)] )
            return false;
    return true;
}

int main(int argc, char ** argv)
{
    if ( argc < 4 ) {
        std::cerr << "usage: IsomTerrain.exe <jungle.cv5> <Jungle Custom Ramps.scx> <output.scx>" << std::endl;
        return 2;
    }

    constexpr auto TS = Sc::Terrain::Tileset::Jungle;
    constexpr size_t WATER = Sc::Isom::Brush::Jungle::Water;
    constexpr size_t JUNGLE = Sc::Isom::Brush::Jungle::Jungle_;
    constexpr size_t HIGH = Sc::Isom::Brush::Jungle::HighJungle;

    Sc::Terrain_::Tiles jungleData;
    if ( !loadCv5(argv[1], jungleData) ) return 3;

    MapFile rampSource(argv[2]);
    if ( rampSource.empty() || rampSource.getTileset() != TS ) {
        std::cerr << "Could not open Jungle Custom Ramps source" << std::endl;
        return 4;
    }

    auto mapFile = std::make_unique<MapFile>(TS,128,128);
    mapFile->setSaveType(SaveType::ExpansionScx);
    ScMap scMap = copyToScMap(*mapFile);
    Chk::IsomCache cache(TS,128,128,jungleData);

    // Stable all-Jungle canvas.
    const uint16_t jungleValue = ((cache.getTerrainTypeIsomValue(JUNGLE) << 4) | Chk::IsomRect::EditorFlag::Modified);
    scMap.isomRects.assign(scMap.getIsomWidth()*scMap.getIsomHeight(), Chk::IsomRect{jungleValue,jungleValue,jungleValue,jungleValue});
    cache.setAllChanged();
    scMap.updateTilesFromIsom(cache);
    cache.finalizeUndoableOperation();

    // Outer ocean around one continuous knee-shaped mainland.
    for ( int ix=0; ix<=64; ++ix )
    {
        int y=0; if ( (ix+y)&1 ) ++y;
        for ( ; y<=127; y+=2 )
        {
            const double x = double(ix*2);
            if ( !isLand(x,double(y)) )
                if ( !stamp(scMap,cache,size_t(ix),size_t(y),WATER) ) return 10;
        }
    }

    // Real second-floor terrain: 12/6 mains, patella, sesamoids, and the two thin ridges.
    for ( int ix=0; ix<=64; ++ix )
    {
        int y=0; if ( (ix+y)&1 ) ++y;
        for ( ; y<=127; y+=2 )
        {
            const double x = double(ix*2);
            if ( isLand(x,double(y)) && anyHigh(x,double(y)) )
                if ( !stamp(scMap,cache,size_t(ix),size_t(y),HIGH) ) return 11;
        }
    }

    copyFromScMap(*mapFile,scMap);

    // Exact functional ramp patches. Source orientation was selected by probing
    // the source map's ground-height transition:
    // comp5: high on north -> low on south
    // comp7: low on north -> high on south
    // comp11: high on west -> low on east (compact)
    // comp6 lower half: low on west -> high on east.
    const std::vector<Patch> patches = {
        {185,40,17,15, 56,15, "P1_MAIN_WIDE_RAMP"},
        {202,73,17,14, 56,98, "P2_MAIN_WIDE_RAMP"},

        {202,73,17,14, 56,47, "PATELLA_NORTH_RAMP"},
        {185,40,17,15, 56,66, "PATELLA_SOUTH_RAMP"},

        {129,102,8,8, 19,52, "LEFT_SESAMOID_UPPER"},
        {129,102,8,8, 19,68, "LEFT_SESAMOID_LOWER"},

        {201,112,15,8, 94,52, "RIGHT_SESAMOID_UPPER"},
        {201,112,15,8, 94,68, "RIGHT_SESAMOID_LOWER"}
    };

    for ( const Patch & p : patches )
        if ( !copyPatch(*mapFile,rampSource,p) ) return 12;

    configure1v1(*mapFile);
    // Starts pushed close to the north/south ends to maximize main-base use.
    mapFile->addUnit(makeStart(64,7,0));
    mapFile->addUnit(makeStart(64,120,1));
    mapFile->setScenarioName(RawString("Patellar Luxation v1.5 REAL RAMP TEST"));
    mapFile->setScenarioDescription(RawString("Terrain-only 1v1 test. Expanded edge mains; High Jungle 2F; exact functional ramp MTXM patches from Jungle Custom Ramps; thin trochlear ridges. No resources."));

    printCounts("BEFORE SAVE", countTerrain(mapFile->tiles,jungleData));

    if ( !mapFile->save(argv[3],true,false,false,true) ) return 20;

    MapFile verify(argv[3]);
    if ( verify.empty() || verify.getTileWidth()!=128 || verify.getTileHeight()!=128 || verify.getTileset()!=TS ) return 21;

    size_t starts=0;
    for ( size_t i=0; i<verify.numUnits(); ++i )
        if ( verify.getUnit(i).type==Sc::Unit::Type::StartLocation ) ++starts;
    if ( starts != 2 || verify.numUnits() != 2 ) {
        std::cerr << "start/unit verification failed" << std::endl;
        return 22;
    }

    for ( const Patch & p : patches )
    {
        if ( !patchMatches(verify,rampSource,p) ) {
            std::cerr << "Ramp patch changed after save: " << p.name << std::endl;
            return 23;
        }
    }

    printCounts("AFTER REOPEN", countTerrain(verify.tiles,jungleData));
    std::cout << "Verified exact ramp MTXM patches=" << patches.size()
              << " starts=" << starts << " units=" << verify.numUnits() << std::endl;
    return 0;
}
