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

struct TerrainCounts {
    size_t water = 0;
    size_t jungle = 0;
    size_t other = 0;
};

static TerrainCounts countTerrain(const std::vector<u16> & tiles, const Sc::Terrain_::Tiles & jungleData)
{
    TerrainCounts c {};
    for ( u16 tile : tiles )
    {
        const size_t group = size_t(Sc::Terrain::getTileGroup(tile));
        if ( group >= jungleData.tileGroups.size() ) {
            ++c.other;
            continue;
        }
        const auto tt = size_t(jungleData.tileGroups[group].terrainType);
        if ( tt == Sc::Isom::Brush::Jungle::Water ) ++c.water;
        else if ( tt == Sc::Isom::Brush::Jungle::Jungle_ ) ++c.jungle;
        else ++c.other;
    }
    return c;
}

static void printCounts(const char * label, const TerrainCounts & c)
{
    std::cout << label << ": water=" << c.water << " jungle=" << c.jungle << " other=" << c.other << std::endl;
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

    Sc::Terrain_::Tiles jungleData;
    if ( !loadCv5(argv[1], jungleData) ) return 3;

    auto mapFile = std::make_unique<MapFile>(TS,128,128);
    mapFile->setSaveType(SaveType::ExpansionScx);
    ScMap scMap = copyToScMap(*mapFile);
    Chk::IsomCache cache(TS,128,128,jungleData);

    // Start from the v1.2-proven all-Jungle base.
    const uint16_t jungleValue = ((cache.getTerrainTypeIsomValue(JUNGLE) << 4) | Chk::IsomRect::EditorFlag::Modified);
    scMap.isomRects.assign(scMap.getIsomWidth()*scMap.getIsomHeight(), Chk::IsomRect{jungleValue,jungleValue,jungleValue,jungleValue});
    cache.setAllChanged();
    scMap.updateTilesFromIsom(cache);
    cache.finalizeUndoableOperation();
    printCounts("BASE", countTerrain(scMap.tiles,jungleData));

    // v1.3: carve a thick Water frame around one large central rectangular Jungle island.
    // Critical fix vs failed v1.1: placeIsomTerrain resets cache.changedArea each call,
    // therefore tiles MUST be updated after every brush stamp (not once after the whole loop).
    size_t waterStamps = 0;
    for ( int ix=0; ix<=64; ++ix )
    {
        int y = 0;
        if ( (ix+y)&1 ) ++y; // valid ISOM diamond parity
        for ( ; y<=127; y+=2 )
        {
            const int tileX = ix*2;
            const bool outside = tileX < 28 || tileX > 100 || y < 14 || y > 114;
            if ( outside )
            {
                if ( !scMap.placeIsomTerrain({size_t(ix),size_t(y)}, WATER, 1, cache) ) {
                    std::cerr << "Water carve failed at " << ix << "," << y << std::endl;
                    return 10;
                }
                scMap.updateTilesFromIsom(cache);
                cache.finalizeUndoableOperation();
                ++waterStamps;
            }
        }
    }
    std::cout << "Water carve stamps: " << waterStamps << std::endl;

    const TerrainCounts beforeSave = countTerrain(scMap.tiles,jungleData);
    printCounts("BEFORE SAVE", beforeSave);
    if ( beforeSave.jungle < 3000 || beforeSave.water < 3000 ) {
        std::cerr << "Expected a clear Jungle/Water mix before save" << std::endl;
        return 11;
    }

    copyFromScMap(*mapFile,scMap);
    configure1v1(*mapFile);
    mapFile->addUnit(makeStart(64,28,0));
    mapFile->addUnit(makeStart(64,100,1));
    mapFile->setScenarioName(RawString("Patellar Luxation v1.3 WATER FRAME TEST"));
    mapFile->setScenarioDescription(RawString("Diagnostic step 2: one large central Jungle island surrounded by a thick Water frame. No resources, cliffs, or ramps yet."));

    if ( !mapFile->save(argv[2],true,false,false,true) ) return 20;

    MapFile verify(argv[2]);
    if ( verify.empty() || verify.getTileWidth()!=128 || verify.getTileHeight()!=128 || verify.getTileset()!=TS ) return 21;
    const TerrainCounts afterSave = countTerrain(verify.tiles,jungleData);
    printCounts("AFTER REOPEN", afterSave);
    size_t starts=0;
    for ( size_t i=0; i<verify.numUnits(); ++i )
        if ( verify.getUnit(i).type==Sc::Unit::Type::StartLocation ) ++starts;
    std::cout << "starts=" << starts << " units=" << verify.numUnits() << std::endl;
    if ( starts != 2 || verify.numUnits() != 2 || afterSave.jungle < 3000 || afterSave.water < 3000 ) return 22;

    return 0;
}
