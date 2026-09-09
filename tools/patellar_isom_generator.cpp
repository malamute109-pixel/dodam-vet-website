#include "IsomApi.h"
#include "../MappingCoreLib/MappingCore.h"
#include <algorithm>
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
    auto size = size_t(f.tellg());
    f.seekg(0, std::ios::beg);
    if ( size == 0 || size % sizeof(Sc::Isom::TileGroup) != 0 ) return false;
    std::vector<uint8_t> bytes(size);
    if ( !f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size)) ) return false;
    size_t groups = size / sizeof(Sc::Isom::TileGroup);
    const auto * raw = reinterpret_cast<const Sc::Isom::TileGroup*>(bytes.data());
    out.tileGroups.assign(raw, raw + groups);
    out.loadIsom(size_t(Sc::Terrain::Tileset::Jungle));
    std::cout << "Loaded Jungle CV5 groups: " << groups << std::endl;
    return true;
}

struct BrushOp
{
    size_t terrainType;
    size_t x;
    size_t y;
    size_t extent;
    const char * label;
};

static bool applyBrush(ScMap & map, Chk::IsomCache & cache, const BrushOp & op)
{
    if ( (op.x + op.y) % 2 != 0 )
    {
        std::cerr << "Invalid ISOM parity for " << op.label << std::endl;
        return false;
    }

    std::cout << "BEGIN " << op.label << " type=" << op.terrainType
              << " x=" << op.x << " y=" << op.y << " extent=" << op.extent << std::endl;

    if ( !map.placeIsomTerrain({op.x, op.y}, op.terrainType, op.extent, cache) )
    {
        std::cerr << "placeIsomTerrain failed for " << op.label << std::endl;
        return false;
    }

    map.updateTilesFromIsom(cache);
    cache.finalizeUndoableOperation();
    std::cout << "DONE  " << op.label << std::endl;
    return true;
}

static Chk::Unit makeUnit(Sc::Unit::Type type, int tileX, int tileY, u8 owner, u32 resourceAmount = 0)
{
    static u32 nextClassId = 1;
    Chk::Unit unit {};
    unit.classId = nextClassId++;
    unit.xc = u16(tileX*32 + 16);
    unit.yc = u16(tileY*32 + 16);
    unit.type = type;
    unit.relationFlags = 0;
    unit.validStateFlags = 0;
    unit.validFieldFlags = resourceAmount > 0 ? u16(0x0011) : u16(0x0001); // owner + optional resources
    unit.owner = owner;
    unit.hitpointPercent = 100;
    unit.shieldPercent = 100;
    unit.energyPercent = 100;
    unit.resourceAmount = resourceAmount;
    unit.hangerAmount = 0;
    unit.stateFlags = 0;
    unit.unused = 0;
    unit.relationClassId = 0;
    return unit;
}

static void addResource(MapFile & mapFile, Sc::Unit::Type type, int tileX, int tileY, u32 amount)
{
    constexpr u8 NeutralOwner = 11;
    mapFile.addUnit(makeUnit(type, tileX, tileY, NeutralOwner, amount));
}

static void addMinerals(MapFile & mapFile, const std::vector<std::pair<int,int>> & positions, u32 amount)
{
    for ( const auto & pos : positions )
        addResource(mapFile, Sc::Unit::Type::MineralFieldType1, pos.first, pos.second, amount);
}

static void configureOneVsOnePlayers(MapFile & mapFile)
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

static void addOneVsOneResources(MapFile & mapFile)
{
    // 12 / 6 o'clock starts, exactly mirrored across the horizontal center line.
    mapFile.addUnit(makeUnit(Sc::Unit::Type::StartLocation, 64, 18, 0));
    mapFile.addUnit(makeUnit(Sc::Unit::Type::StartLocation, 64, 109, 1));

    // Main bases: 8 minerals + 1 gas each.
    std::vector<std::pair<int,int>> topMain;
    for ( int x=55; x<=69; x+=2 ) topMain.push_back({x,11});
    std::vector<std::pair<int,int>> bottomMain;
    for ( const auto & p : topMain ) bottomMain.push_back({p.first,127-p.second});
    addMinerals(mapFile, topMain, 1500);
    addMinerals(mapFile, bottomMain, 1500);
    addResource(mapFile, Sc::Unit::Type::VespeneGeyser, 74, 18, 5000);
    addResource(mapFile, Sc::Unit::Type::VespeneGeyser, 74, 109, 5000);

    // Natural bases: same mining value and same distance from each start.
    std::vector<std::pair<int,int>> topNatural;
    for ( int x=55; x<=69; x+=2 ) topNatural.push_back({x,32});
    std::vector<std::pair<int,int>> bottomNatural;
    for ( const auto & p : topNatural ) bottomNatural.push_back({p.first,127-p.second});
    addMinerals(mapFile, topNatural, 1500);
    addMinerals(mapFile, bottomNatural, 1500);
    addResource(mapFile, Sc::Unit::Type::VespeneGeyser, 74, 39, 5000);
    addResource(mapFile, Sc::Unit::Type::VespeneGeyser, 74, 88, 5000);

    // Rich patella base: the high-risk center gets deliberately more resources.
    const std::vector<std::pair<int,int>> patellaUpper = {
        {56,56},{59,55},{62,54},{65,54},{68,55},{71,56}
    };
    std::vector<std::pair<int,int>> patellaLower;
    for ( const auto & p : patellaUpper ) patellaLower.push_back({127-p.first,127-p.second});
    addMinerals(mapFile, patellaUpper, 2000);
    addMinerals(mapFile, patellaLower, 2000);
    addResource(mapFile, Sc::Unit::Type::VespeneGeyser, 52, 63, 6000);
    addResource(mapFile, Sc::Unit::Type::VespeneGeyser, 75, 64, 6000);

    // Sesamoid island expansions: smaller but fully usable island bases.
    const std::vector<std::pair<int,int>> leftIsland = {
        {8,58},{8,60},{8,62},{8,65},{8,67},{8,69}
    };
    std::vector<std::pair<int,int>> rightIsland;
    for ( const auto & p : leftIsland ) rightIsland.push_back({127-p.first,127-p.second});
    addMinerals(mapFile, leftIsland, 1500);
    addMinerals(mapFile, rightIsland, 1500);
    addResource(mapFile, Sc::Unit::Type::VespeneGeyser, 24, 64, 5000);
    addResource(mapFile, Sc::Unit::Type::VespeneGeyser, 103, 63, 5000);
}

int main(int argc, char ** argv)
{
    if ( argc < 3 )
    {
        std::cerr << "usage: IsomTerrain.exe <jungle.cv5> <output.scx>" << std::endl;
        return 2;
    }

    Sc::Terrain_::Tiles jungleData;
    if ( !loadCv5(argv[1], jungleData) )
    {
        std::cerr << "Failed to load jungle.cv5" << std::endl;
        return 3;
    }

    constexpr auto TS = Sc::Terrain::Tileset::Jungle;
    constexpr size_t WATER = Sc::Isom::Brush::Jungle::Water;
    constexpr size_t LOW = Sc::Isom::Brush::Jungle::Jungle_;
    constexpr size_t HIGH = Sc::Isom::Brush::Jungle::RaisedJungle;

    auto mapFile = std::make_unique<MapFile>(TS, 128, 128);
    mapFile->setSaveType(SaveType::ExpansionScx);
    ScMap scMap = copyToScMap(*mapFile);
    Chk::IsomCache cache(TS, 128, 128, jungleData);

    // Clean legal water canvas.
    uint16_t waterValue = ((cache.getTerrainTypeIsomValue(WATER) << 4) | Chk::IsomRect::EditorFlag::Modified);
    scMap.isomRects.assign(scMap.getIsomWidth()*scMap.getIsomHeight(), Chk::IsomRect{waterValue,waterValue,waterValue,waterValue});
    cache.setAllChanged();
    scMap.updateTilesFromIsom(cache);
    std::cout << "Water canvas initialized" << std::endl;

    // 1v1 knee-joint silhouette. Every top-side mainland operation has an exact
    // bottom-side mirror around y=64; left/right anatomy is mirrored around x=32.
    const std::vector<BrushOp> landOps = {
        {LOW,32,12,12,"upper shaft"},
        {LOW,24,28,14,"upper-left condyle"},
        {LOW,40,28,14,"upper-right condyle"},
        {LOW,20,46,13,"upper-left trochlear shoulder"},
        {LOW,44,46,13,"upper-right trochlear shoulder"},
        {LOW,32,64,18,"joint center"},
        {LOW,20,82,13,"lower-left trochlear shoulder"},
        {LOW,44,82,13,"lower-right trochlear shoulder"},
        {LOW,24,100,14,"lower-left condyle"},
        {LOW,40,100,14,"lower-right condyle"},
        {LOW,32,116,12,"lower shaft"},
        {LOW,8,64,8,"left sesamoid island"},
        {LOW,56,64,8,"right sesamoid island"}
    };

    for ( const auto & op : landOps )
        if ( !applyBrush(scMap, cache, op) ) return 10;

    const std::vector<BrushOp> highOps = {
        // Two elevated mains at 12 and 6 o'clock.
        {HIGH,32,18,9,"P1 high main"},
        {HIGH,32,110,9,"P2 high main"},
        // Elevated naturals between each main and the joint.
        {HIGH,32,38,8,"P1 high natural"},
        {HIGH,32,90,8,"P2 high natural"},
        // Patella and trochlear ridges.
        {HIGH,32,64,10,"patella"},
        {HIGH,20,64,7,"left trochlear ridge"},
        {HIGH,44,64,7,"right trochlear ridge"},
        // Raised island cores large enough to function as expansions.
        {HIGH,8,64,6,"left sesamoid high core"},
        {HIGH,56,64,6,"right sesamoid high core"}
    };

    for ( const auto & op : highOps )
        if ( !applyBrush(scMap, cache, op) ) return 11;

    // Low-ground cuts form natural-looking legal ramp approaches while preserving
    // left/right bypass routes around the patella.
    const std::vector<BrushOp> cutOps = {
        {LOW,32,26,3,"P1 main exit"},
        {LOW,32,46,3,"P1 natural exit"},
        {LOW,32,54,3,"upper patellar ligament ramp"},
        {LOW,32,74,3,"lower patellar ligament ramp"},
        {LOW,32,82,3,"P2 natural exit"},
        {LOW,32,102,3,"P2 main exit"},
        {LOW,26,64,3,"left patella approach"},
        {LOW,38,64,3,"right patella approach"}
    };

    for ( const auto & op : cutOps )
        if ( !applyBrush(scMap, cache, op) ) return 12;

    copyFromScMap(*mapFile, scMap);
    configureOneVsOnePlayers(*mapFile);
    addOneVsOneResources(*mapFile);

    mapFile->setScenarioName(RawString("Patellar Luxation v1.0 1v1 Playtest"));
    mapFile->setScenarioDescription(RawString("128x128 Jungle 1v1; 12/6 starts, raised patella rich center, symmetric naturals, sesamoid island expansions."));

    if ( !mapFile->save(argv[2], true, false, false, true) )
    {
        std::cerr << "Failed to save map" << std::endl;
        return 4;
    }

    // Re-open the exact MPQ/CHK that was written and validate core melee data.
    MapFile verify(argv[2]);
    if ( verify.empty() || verify.getTileWidth() != 128 || verify.getTileHeight() != 128 || verify.getTileset() != TS )
    {
        std::cerr << "Verification reopen failed" << std::endl;
        return 5;
    }

    const size_t expectedTiles = 128u*128u;
    const size_t expectedIsom = (128u/2u+1u)*(128u+1u);
    if ( verify.tiles.size() != expectedTiles || verify.editorTiles.size() != expectedTiles || verify.isomRects.size() != expectedIsom )
    {
        std::cerr << "Verification section sizes are invalid" << std::endl;
        return 6;
    }

    size_t startCount = 0;
    size_t mineralCount = 0;
    size_t geyserCount = 0;
    for ( size_t i=0; i<verify.numUnits(); ++i )
    {
        const auto & u = verify.getUnit(i);
        if ( u.type == Sc::Unit::Type::StartLocation ) ++startCount;
        else if ( u.type == Sc::Unit::Type::MineralFieldType1 ||
                  u.type == Sc::Unit::Type::MineralFieldType2 ||
                  u.type == Sc::Unit::Type::MineralFieldType3 ) ++mineralCount;
        else if ( u.type == Sc::Unit::Type::VespeneGeyser ) ++geyserCount;
    }

    size_t openSlots = 0;
    for ( size_t i=0; i<Sc::Player::TotalSlots; ++i )
        if ( verify.slotTypes[i] == Sc::Player::SlotType::GameOpen ) ++openSlots;

    if ( startCount != 2 || mineralCount != 56 || geyserCount != 8 || openSlots != 2 )
    {
        std::cerr << "1v1 data verification failed: starts=" << startCount
                  << " minerals=" << mineralCount
                  << " geysers=" << geyserCount
                  << " openSlots=" << openSlots << std::endl;
        return 7;
    }

    size_t zeroTiles = std::count(verify.tiles.begin(), verify.tiles.end(), uint16_t(0));
    std::cout << "Saved and reopened: " << argv[2] << std::endl;
    std::cout << "ISOM=" << verify.isomRects.size()
              << " TILE=" << verify.editorTiles.size()
              << " MTXM=" << verify.tiles.size()
              << " zeroMTXM=" << zeroTiles
              << " starts=" << startCount
              << " minerals=" << mineralCount
              << " geysers=" << geyserCount
              << " openSlots=" << openSlots << std::endl;
    return 0;
}
