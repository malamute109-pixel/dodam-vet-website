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

// Tile-coordinate trace of the approved 128x128 reference silhouette.
// The polar ends deliberately return to x~=39..89 instead of the over-wide
// v1.7 caps; the enlarged main plateau comes from usable depth, not silhouette drift.
static double halfWidthAtY(double y)
{
    struct Knot { double y; double hw; };
    static const Knot k[] = {
        {0,25},{8,28},{16,30},{24,37},{32,41},{40,43},{48,40},
        {56,40},{64,42},{72,40},{80,43},{88,41},{96,37},{104,30},
        {112,28},{120,26},{127,25}
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
    // Deep, buildable end-cap plateau centered at the requested 12 o'clock edge.
    if ( y < 0 || y > 25 ) return false;
    double hw = 25.5;
    if ( y > 18 ) hw -= (y-18)*0.45;
    return std::abs(x-64.0) <= hw;
}
static bool bottomMainHigh(double x, double y) { return topMainHigh(x,127.0-y); }
static bool patellaHigh(double x, double y)
{
    // Anatomical patella: a clean, slightly north-south elongated oval.
    return ellipse(x,y,64.0,63.5,12.5,13.5);
}
static bool leftSesamoidHigh(double x, double y) { return ellipse(x,y,13.5,63.5,10.5,12.5); }
static bool rightSesamoidHigh(double x, double y) { return ellipse(x,y,114.5,63.5,10.5,12.5); }

// Very thin elongated trochlear ridges, as requested: visually a narrow vertical
// 2F line rather than the broad bulky ridges from v1.4.
static bool leftTrochleaHigh(double x, double y)
{
    if ( y < 40 || y > 87 ) return false;
    const double phase = (y-40.0)/(87.0-40.0);
    const double cx = 40.0 + 1.0*std::sin(phase*3.14159265358979323846);
    return std::abs(x-cx) <= 2.0;
}
static bool rightTrochleaHigh(double x, double y) { return leftTrochleaHigh(127.0-x,y); }

static bool forceLow(double x, double y)
{
    // Approach pockets. These keep the generated ISOM high ground from
    // occupying the low side of each copied ramp transition.
    const bool p1main = x>=57 && x<=72 && y>=20 && y<=29;
    const bool p2main = x>=56 && x<=71 && y>=98 && y<=107;
    const bool patN = x>=57 && x<=71 && y>=49 && y<=57;
    const bool patS = x>=56 && x<=72 && y>=70 && y<=79;

    const bool lsu = x>=19 && x<=29 && y>=51 && y<=59;
    const bool lsl = x>=19 && x<=29 && y>=67 && y<=75;
    const bool rsu = x>=98 && x<=109 && y>=51 && y<=59;
    const bool rsl = x>=98 && x<=109 && y>=67 && y<=75;
    return p1main || p2main || patN || patS || lsu || lsl || rsu || rsl;
}

static bool anyHigh(double x, double y)
{
    if ( forceLow(x,y) ) return false;
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

// A complete ramp is more than its central stair tiles.  Build a non-rectangular
// transfer mask from every donor transition/collision tile, then feather two tiles
// into matching low/high floor.  This preserves the proven walkable core while
// avoiding the rectangular v1.5 paste and the brittle terrainType-only v1.7 mask.
static bool isKnownRampTransitionType(size_t tt)
{
    switch ( tt )
    {
        case 8: case 12: case 23: case 25: case 26: case 34: case 35:
            return true;
        default:
            return false;
    }
}

static unsigned elevationNibble(uint8_t groundHeight)
{
    return unsigned(groundHeight & 0x0f);
}

struct RampCopyStats {
    size_t core = 0;
    size_t feather = 0;
    size_t copied = 0;
    size_t midOrFlagged = 0;
    size_t invalid = 0;
};

static RampCopyStats copyFeatheredRamp(MapFile & dest, const MapFile & src,
                                       const Sc::Terrain_::Tiles & data, const Patch & p)
{
    RampCopyStats stats {};
    const int sw = int(src.getTileWidth()), sh = int(src.getTileHeight());
    const int dw = int(dest.getTileWidth()), dh = int(dest.getTileHeight());
    if ( p.sx < 0 || p.sy < 0 || p.dx < 0 || p.dy < 0 ||
         p.sx+p.w > sw || p.sy+p.h > sh || p.dx+p.w > dw || p.dy+p.h > dh )
    {
        stats.invalid = 1;
        return stats;
    }

    const size_t count = size_t(p.w)*size_t(p.h);
    std::vector<uint8_t> core(count,0), mask(count,0);
    auto at = [&p](int x, int y) { return size_t(y)*size_t(p.w)+size_t(x); };

    for ( int y=0; y<p.h; ++y )
    for ( int x=0; x<p.w; ++x )
    {
        const u16 tile = src.tiles[size_t(p.sy+y)*size_t(sw)+size_t(p.sx+x)];
        const size_t group = size_t(Sc::Terrain::getTileGroup(tile));
        if ( group >= data.tileGroups.size() ) { ++stats.invalid; continue; }
        const auto & tg = data.tileGroups[group];
        const unsigned elevation = elevationNibble(tg.groundHeight);
        if ( isKnownRampTransitionType(size_t(tg.terrainType)) ||
             tg.groundHeight >= 16 || (elevation != 0 && elevation != 2) )
            core[at(x,y)] = 1;
    }

    // Diamond dilation makes an irregular transition envelope, never a rectangle.
    for ( int y=0; y<p.h; ++y )
    for ( int x=0; x<p.w; ++x )
    {
        if ( !core[at(x,y)] ) continue;
        for ( int oy=-2; oy<=2; ++oy )
        for ( int ox=-2; ox<=2; ++ox )
        {
            if ( std::abs(ox)+std::abs(oy) > 3 ) continue;
            const int nx=x+ox, ny=y+oy;
            if ( nx>=0 && nx<p.w && ny>=0 && ny<p.h ) mask[at(nx,ny)]=1;
        }
    }

    for ( int y=0; y<p.h; ++y )
    for ( int x=0; x<p.w; ++x )
    {
        const size_t local = at(x,y);
        if ( !mask[local] ) continue;

        const size_t si = size_t(p.sy+y)*size_t(sw)+size_t(p.sx+x);
        const size_t di = size_t(p.dy+y)*size_t(dw)+size_t(p.dx+x);
        const u16 sourceTile = src.tiles[si];
        const u16 destTile = dest.tiles[di];
        const size_t sourceGroup = size_t(Sc::Terrain::getTileGroup(sourceTile));
        const size_t destGroup = size_t(Sc::Terrain::getTileGroup(destTile));
        if ( sourceGroup >= data.tileGroups.size() || destGroup >= data.tileGroups.size() )
        {
            ++stats.invalid;
            continue;
        }

        const auto & sourceTg = data.tileGroups[sourceGroup];
        const auto & destTg = data.tileGroups[destGroup];
        const bool isCore = core[local] != 0;
        if ( !isCore )
        {
            const unsigned se = elevationNibble(sourceTg.groundHeight);
            const unsigned de = elevationNibble(destTg.groundHeight);
            // Feather only like-height flat floors.  It keeps the donor's narrow
            // Jungle/Dirt blend without overwriting the generated cliff topology.
            if ( (se != 0 && se != 2) || se != de ) continue;
        }

        dest.tiles[di] = sourceTile;
        dest.editorTiles[di] = sourceTile;
        ++stats.copied;
        if ( isCore ) ++stats.core; else ++stats.feather;
        const unsigned e = elevationNibble(sourceTg.groundHeight);
        if ( sourceTg.groundHeight >= 16 || (e != 0 && e != 2) ) ++stats.midOrFlagged;
    }

    std::cout << "FEATHERED_RAMP " << p.name
              << " core=" << stats.core << " feather=" << stats.feather
              << " copied=" << stats.copied << " midOrFlagged=" << stats.midOrFlagged
              << " invalid=" << stats.invalid << std::endl;
    return stats;
}

static size_t countInvalidTileGroups(const std::vector<u16> & tiles,
                                     const Sc::Terrain_::Tiles & data)
{
    size_t invalid = 0;
    for ( u16 tile : tiles )
        if ( size_t(Sc::Terrain::getTileGroup(tile)) >= data.tileGroups.size() )
            ++invalid;
    return invalid;
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
        // Proven vertical transitions from the custom-ramp probe.  Destination
        // rectangles are only search windows; copyFeatheredRamp emits an irregular mask.
        {185,40,17,15, 55,15, "P1_MAIN_WIDE"},
        {202,73,17,14, 55,99, "P2_MAIN_WIDE"},

        {202,73,17,14, 55,47, "PATELLA_NORTH"},
        {185,40,17,15, 55,66, "PATELLA_SOUTH"},

        {129,102,8,8,   20,51, "LEFT_SESAMOID_UPPER"},
        {129,102,8,8,   20,68, "LEFT_SESAMOID_LOWER"},
        {201,112,15,8,  94,51, "RIGHT_SESAMOID_UPPER"},
        {201,112,15,8,  94,68, "RIGHT_SESAMOID_LOWER"}
    };

    size_t totalCopied = 0, totalCore = 0, totalFeather = 0, totalMid = 0;
    for ( const Patch & p : patches )
    {
        const RampCopyStats s = copyFeatheredRamp(*mapFile,rampSource,jungleData,p);
        if ( s.invalid != 0 || s.core < 4 || s.copied <= s.core || s.midOrFlagged == 0 )
        {
            std::cerr << "Ramp integration validation failed: " << p.name << std::endl;
            return 12;
        }
        totalCopied += s.copied;
        totalCore += s.core;
        totalFeather += s.feather;
        totalMid += s.midOrFlagged;
    }
    if ( totalCopied < 160 || totalFeather < 24 || totalMid < 8 ) {
        std::cerr << "Too few complete feathered ramp tiles: copied=" << totalCopied
                  << " feather=" << totalFeather << " mid=" << totalMid << std::endl;
        return 13;
    }

    configure1v1(*mapFile);
    // Starts pushed close to the north/south ends to maximize main-base use.
    mapFile->addUnit(makeStart(64,7,0));
    mapFile->addUnit(makeStart(64,120,1));
    mapFile->setScenarioName(RawString("Patellar Luxation v1.7 FEATHERED RAMP TEST"));
    mapFile->setScenarioDescription(RawString("Terrain-only 1v1 test. Expanded edge mains; clean oval patella; thin trochlear ridges; proven ramp cores with like-height feathered borders blended into native Jungle terrain. No resources."));

    printCounts("BEFORE SAVE", countTerrain(mapFile->tiles,jungleData));

    if ( !mapFile->save(argv[3],true,false,false,true) ) return 20;

    MapFile verify(argv[3]);
    if ( verify.empty() || verify.getTileWidth()!=128 || verify.getTileHeight()!=128 || verify.getTileset()!=TS ) return 21;
    if ( verify.tiles.size()!=16384 || verify.editorTiles.size()!=16384 ) {
        std::cerr << "MTXM/TILE size verification failed" << std::endl;
        return 22;
    }
    const size_t invalidTiles = countInvalidTileGroups(verify.tiles,jungleData);
    if ( invalidTiles != 0 ) {
        std::cerr << "Invalid/black tile groups after reopen: " << invalidTiles << std::endl;
        return 23;
    }

    size_t starts=0, openSlots=0;
    for ( size_t i=0; i<Sc::Player::Total; ++i )
        if ( verify.slotTypes[i] == Sc::Player::SlotType::GameOpen ) ++openSlots;
    for ( size_t i=0; i<verify.numUnits(); ++i )
        if ( verify.getUnit(i).type==Sc::Unit::Type::StartLocation ) ++starts;
    if ( starts != 2 || verify.numUnits() != 2 || openSlots != 2 ) {
        std::cerr << "1v1/start/resource verification failed" << std::endl;
        return 24;
    }

    std::cout << "featheredRampTiles=" << totalCopied
              << " core=" << totalCore << " feather=" << totalFeather
              << " midOrFlagged=" << totalMid << std::endl;

    printCounts("AFTER REOPEN", countTerrain(verify.tiles,jungleData));
    std::cout << "Verified feathered ramp patches=" << patches.size()
              << " starts=" << starts << " units=" << verify.numUnits()
              << " openSlots=" << openSlots << " invalidTiles=" << invalidTiles << std::endl;
    return 0;
}
