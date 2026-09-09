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

    // v0.9 terrain prototype uses a deliberately small, bounded number of real ISOM
    // brush operations. Overlap creates the knee-joint silhouette without thousands
    // of independent radial ISOM repairs.
    const std::vector<BrushOp> landOps = {
        {LOW,32,14,14,"upper shaft"},
        {LOW,24,28,14,"upper-left condyle"},
        {LOW,40,28,14,"upper-right condyle"},
        {LOW,20,46,13,"left trochlear shoulder"},
        {LOW,44,46,13,"right trochlear shoulder"},
        {LOW,32,62,18,"joint center"},
        {LOW,20,78,13,"left lower shoulder"},
        {LOW,44,78,13,"right lower shoulder"},
        {LOW,24,96,14,"lower-left condyle"},
        {LOW,40,96,14,"lower-right condyle"},
        {LOW,32,112,14,"lower shaft"},
        // Sesamoid islands: low outer ring first so raised terrain has a legal base.
        {LOW,6,62,7,"left sesamoid island"},
        {LOW,58,62,7,"right sesamoid island"}
    };

    for ( const auto & op : landOps )
        if ( !applyBrush(scMap, cache, op) ) return 10;

    const std::vector<BrushOp> highOps = {
        // Four symmetric elevated starting plateaus.
        {HIGH,22,18,7,"P1 high main"},
        {HIGH,42,18,7,"P2 high main"},
        {HIGH,22,108,7,"P3 high main"},
        {HIGH,42,108,7,"P4 high main"},
        // Central patella and paired trochlear ridges.
        {HIGH,32,62,9,"patella"},
        {HIGH,20,62,7,"left trochlear ridge"},
        {HIGH,44,62,7,"right trochlear ridge"},
        // Raised centers of the two sesamoid islands.
        {HIGH,6,62,3,"left sesamoid high core"},
        {HIGH,58,62,3,"right sesamoid high core"}
    };

    for ( const auto & op : highOps )
        if ( !applyBrush(scMap, cache, op) ) return 11;

    // Low-ground notches create approach/ramp-like slots into elevated anatomy.
    const std::vector<BrushOp> cutOps = {
        {LOW,32,52,3,"upper patella approach"},
        {LOW,32,72,3,"lower patella approach"},
        {LOW,26,62,3,"left patella approach"},
        {LOW,38,62,3,"right patella approach"},
        {LOW,22,26,3,"P1 natural exit"},
        {LOW,42,26,3,"P2 natural exit"},
        {LOW,22,100,3,"P3 natural exit"},
        {LOW,42,100,3,"P4 natural exit"}
    };

    for ( const auto & op : cutOps )
        if ( !applyBrush(scMap, cache, op) ) return 12;

    std::cout << "POST copyFromScMap BEGIN" << std::endl;
    copyFromScMap(*mapFile, scMap);
    std::cout << "POST copyFromScMap DONE" << std::endl;

    std::cout << "POST metadata BEGIN" << std::endl;
    mapFile->setScenarioName(RawString("Patellar Luxation v0.9 ISOM Terrain"));
    mapFile->setScenarioDescription(RawString("ISOM-generated Jungle terrain prototype; terrain only, no resources yet."));
    std::cout << "POST metadata DONE" << std::endl;

    std::cout << "SAVE BEGIN" << std::endl;
    // No custom MPQ assets exist in this terrain-only prototype, so there is no
    // reason to update a listfile while packaging the SCX.
    if ( !mapFile->save(argv[2], true, false, false, true) )
    {
        std::cerr << "Failed to save map" << std::endl;
        return 4;
    }
    std::cout << "SAVE DONE" << std::endl;

    // Re-open the exact MPQ/CHK that was written. This catches malformed SCX output.
    std::cout << "REOPEN BEGIN" << std::endl;
    MapFile verify(argv[2]);
    std::cout << "REOPEN DONE" << std::endl;
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

    size_t zeroTiles = std::count(verify.tiles.begin(), verify.tiles.end(), uint16_t(0));
    std::cout << "Saved and reopened: " << argv[2] << std::endl;
    std::cout << "ISOM=" << verify.isomRects.size()
              << " TILE=" << verify.editorTiles.size()
              << " MTXM=" << verify.tiles.size()
              << " zeroMTXM=" << zeroTiles << std::endl;
    return 0;
}
