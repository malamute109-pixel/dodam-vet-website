#include "IsomApi.h"
#include "../MappingCoreLib/MappingCore.h"
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

static size_t countJungle(const std::vector<u16> & tiles, const Sc::Terrain_::Tiles & jungleData)
{
    size_t count = 0;
    for ( u16 tile : tiles )
    {
        const size_t group = size_t(Sc::Terrain::getTileGroup(tile));
        if ( group < jungleData.tileGroups.size() &&
             size_t(jungleData.tileGroups[group].terrainType) == Sc::Isom::Brush::Jungle::Jungle_ )
            ++count;
    }
    return count;
}

int main(int argc, char ** argv)
{
    if ( argc < 3 ) {
        std::cerr << "usage: IsomTerrain.exe <jungle.cv5> <output.scx>" << std::endl;
        return 2;
    }

    constexpr auto TS = Sc::Terrain::Tileset::Jungle;
    constexpr size_t JUNGLE = Sc::Isom::Brush::Jungle::Jungle_;

    Sc::Terrain_::Tiles jungleData;
    if ( !loadCv5(argv[1], jungleData) ) return 3;

    auto mapFile = std::make_unique<MapFile>(TS,128,128);
    mapFile->setSaveType(SaveType::ExpansionScx);
    ScMap scMap = copyToScMap(*mapFile);
    Chk::IsomCache cache(TS,128,128,jungleData);

    // v1.2 is intentionally the simplest possible test after v1.1 rendered as water:
    // fill EVERY tile with plain Jungle. No water, no cliffs, no ramps, no resources.
    // If this appears as normal land in StarCraft, the base terrain pipeline is proven.
    const uint16_t jungleValue = ((cache.getTerrainTypeIsomValue(JUNGLE) << 4) | Chk::IsomRect::EditorFlag::Modified);
    scMap.isomRects.assign(scMap.getIsomWidth()*scMap.getIsomHeight(), Chk::IsomRect{jungleValue,jungleValue,jungleValue,jungleValue});
    cache.setAllChanged();
    scMap.updateTilesFromIsom(cache);

    const size_t before = countJungle(scMap.tiles,jungleData);
    std::cout << "BEFORE SAVE jungle tiles=" << before << " / " << scMap.tiles.size() << std::endl;
    if ( before != 128u*128u ) return 10;

    copyFromScMap(*mapFile,scMap);
    configure1v1(*mapFile);
    mapFile->addUnit(makeStart(64,20,0));
    mapFile->addUnit(makeStart(64,108,1));
    mapFile->setScenarioName(RawString("Patellar Luxation v1.2 FLAT LAND TEST"));
    mapFile->setScenarioDescription(RawString("Diagnostic step: the entire 128x128 map must display as ordinary Jungle ground. No water, resources, high ground, or ramps yet."));

    if ( !mapFile->save(argv[2],true,false,false,true) ) return 20;

    MapFile verify(argv[2]);
    if ( verify.empty() || verify.getTileWidth()!=128 || verify.getTileHeight()!=128 || verify.getTileset()!=TS ) return 21;
    const size_t after = countJungle(verify.tiles,jungleData);
    size_t starts=0;
    for ( size_t i=0; i<verify.numUnits(); ++i )
        if ( verify.getUnit(i).type==Sc::Unit::Type::StartLocation ) ++starts;
    std::cout << "AFTER REOPEN jungle tiles=" << after << " / " << verify.tiles.size()
              << " starts=" << starts << " units=" << verify.numUnits() << std::endl;
    if ( after != 128u*128u || starts != 2 || verify.numUnits() != 2 ) return 22;

    return 0;
}
