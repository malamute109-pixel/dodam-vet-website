#include "../CrossCutLib/Logger.h"
#include "../MappingCoreLib/MappingCore.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// IsomTerrain's low-level batch-edit helpers are private implementation details.
// This generator intentionally exposes them only in this translation unit so a whole
// polygon can be written first and repaired with ONE radial ISOM pass, instead of
// running the expensive radial search once per diamond.
#define private public
#include "IsomApi.h"
#undef private

struct Pt { double x; double y; };
using Poly = std::vector<Pt>;

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
    std::cout << "Loaded Jungle CV5 groups: " << groups << "\n";
    return true;
}

static bool inside(const Poly & poly, double x, double y)
{
    bool c = false;
    for ( size_t i=0, j=poly.size()-1; i<poly.size(); j=i++ )
    {
        const auto & a = poly[i];
        const auto & b = poly[j];
        const bool intersect = ((a.y > y) != (b.y > y)) &&
            (x < (b.x-a.x)*(y-a.y)/(b.y-a.y + 1e-12) + a.x);
        if ( intersect ) c = !c;
    }
    return c;
}

static bool insideAny(const std::vector<Poly> & polys, double x, double y)
{
    for ( const auto & poly : polys )
    {
        if ( inside(poly, x, y) ) return true;
    }
    return false;
}

static Poly mirrorY(const Poly & p)
{
    Poly r; r.reserve(p.size());
    for ( const auto & q : p ) r.push_back({q.x, 128.0-q.y});
    return r;
}

static Poly mirrorX(const Poly & p)
{
    Poly r; r.reserve(p.size());
    for ( const auto & q : p ) r.push_back({128.0-q.x, q.y});
    return r;
}

static Poly rect(double x1,double y1,double x2,double y2)
{
    return {{x1,y1},{x2,y1},{x2,y2},{x1,y2}};
}

static size_t paintPolysFast(ScMap & map, Chk::IsomCache & cache, const std::vector<Poly> & polys, size_t terrainType)
{
    const uint16_t isomValue = cache.getTerrainTypeIsomValue(terrainType);
    if ( isomValue == 0 || size_t(isomValue) >= cache.isomLinks.size() )
        return 0;

    cache.resetChangedArea();
    std::vector<Chk::IsomDiamond> painted;
    painted.reserve(map.getIsomWidth()*map.getIsomHeight()/3);

    for ( size_t y=0; y<map.getIsomHeight(); ++y )
    {
        for ( size_t ix=y%2; ix<map.getIsomWidth(); ix+=2 )
        {
            Chk::IsomDiamond d{ix,y};
            if ( !d.isValid() ) continue;
            const double tx = double(ix*2);
            const double ty = double(y);
            if ( insideAny(polys, tx, ty) )
            {
                map.setDiamondIsomValues(d, isomValue, false, cache);
                painted.push_back(d);
            }
        }
    }

    if ( painted.empty() ) return 0;

    // Only seed the radial repair with unmodified diamonds touching the freshly
    // painted area. The repair then propagates outward as far as required by the
    // legal Jungle ISOM transition graph.
    std::deque<Chk::IsomDiamond> frontier;
    std::vector<uint8_t> queued(map.getIsomWidth()*map.getIsomHeight(), 0);
    for ( const auto & d : painted )
    {
        for ( auto n : Chk::IsomDiamond::neighbors )
        {
            auto q = d.getNeighbor(n);
            if ( !map.diamondNeedsUpdate(q) ) continue;
            const size_t idx = q.y*map.getIsomWidth() + q.x;
            if ( idx < queued.size() && queued[idx] == 0 )
            {
                queued[idx] = 1;
                frontier.push_back(q);
            }
        }
    }

    map.radiallyUpdateTerrain(false, frontier, cache);
    map.updateTilesFromIsom(cache); // also clears ISOM modified/visited editor flags

    std::cout << "batch paint type " << terrainType << ": " << painted.size() << " diamonds\n";
    return painted.size();
}

int main(int argc, char ** argv)
{
    if ( argc < 3 )
    {
        std::cerr << "usage: IsomTerrain.exe <jungle.cv5> <output.scx>\n";
        return 2;
    }

    Sc::Terrain_::Tiles jungleData;
    if ( !loadCv5(argv[1], jungleData) )
    {
        std::cerr << "Failed to load jungle.cv5\n";
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

    // Initialize the whole field as legal ISOM water, create MTXM/TILE once,
    // then clear the Modified flags before beginning the batch operations.
    uint16_t waterValue = ((cache.getTerrainTypeIsomValue(WATER) << 4) | Chk::IsomRect::EditorFlag::Modified);
    scMap.isomRects.assign(scMap.getIsomWidth()*scMap.getIsomHeight(), Chk::IsomRect{waterValue,waterValue,waterValue,waterValue});
    cache.setAllChanged();
    scMap.updateTilesFromIsom(cache);

    // Mainland silhouette: symmetric knee-joint envelope.
    Poly leftTop = {
        {38,2},{36,6},{34,12},{32,18},{28,22},{24,28},{22,34},{22,42},
        {26,48},{22,54},{20,58},{18,64}
    };
    Poly left = leftTop;
    auto mirroredLeft = mirrorY(leftTop);
    for ( auto it=mirroredLeft.rbegin()+1; it!=mirroredLeft.rend(); ++it ) left.push_back(*it);
    Poly right = mirrorX(left);
    Poly mainland = left;
    for ( const auto & q : right ) mainland.push_back(q);
    paintPolysFast(scMap, cache, {mainland}, LOW);

    // True raised terrain: mains/naturals, central patella, trochlear ridges,
    // plus two isolated raised sesamoid islands.
    Poly topMain = {{38,2},{90,2},{94,8},{94,14},{90,20},{82,24},{72,24},{68,20},{60,20},{56,24},{46,24},{38,20},{34,14},{34,8}};
    Poly topNat = {{52,20},{76,20},{82,24},{82,30},{78,34},{70,38},{58,38},{50,34},{46,30},{46,24}};
    Poly patella = {{54,52},{74,52},{80,56},{82,62},{80,68},{74,74},{54,74},{48,68},{46,62},{48,56}};
    Poly leftTroch = {{28,34},{38,34},{44,38},{46,44},{44,50},{42,56},{42,60},{46,64},{42,68},{42,72},{44,78},{46,84},{44,90},{38,94},{28,94},{24,88},{24,82},{28,74},{30,68},{30,60},{28,54},{24,46},{24,40}};
    Poly leftIsland = {{4,50},{8,46},{16,46},{22,50},{24,56},{24,68},{22,74},{16,78},{8,76},{4,72},{2,66},{2,56}};

    std::vector<Poly> highs = {
        topMain, mirrorY(topMain), topNat, mirrorY(topNat), patella,
        leftTroch, mirrorX(leftTroch), leftIsland, mirrorX(leftIsland)
    };
    paintPolysFast(scMap, cache, highs, HIGH);

    // Symmetrical low-ground approach slots. ISOM generates legal transition/cliff
    // shapes around them rather than directly writing cliff tile IDs.
    std::vector<Poly> rampCuts = {
        rect(60,18,68,27), rect(60,31,68,40),
        rect(60,47,68,57), rect(60,70,68,81),
        rect(60,87,68,97), rect(60,101,68,111),
        rect(23,38,33,49), rect(23,79,33,90),
        rect(95,38,105,49), rect(95,79,105,90)
    };
    paintPolysFast(scMap, cache, rampCuts, LOW);

    // Final deterministic MTXM/TILE rebuild from the completed ISOM field.
    cache.setAllChanged();
    for ( auto & r : scMap.isomRects )
    {
        r.left |= Chk::IsomRect::EditorFlag::Modified;
        r.right |= Chk::IsomRect::EditorFlag::Modified;
    }
    scMap.updateTilesFromIsom(cache);
    copyFromScMap(*mapFile, scMap);

    mapFile->setScenarioName(RawString("Patellar Luxation v0.9 ISOM Terrain"));
    mapFile->setScenarioDescription(RawString("ISOM-generated Jungle terrain test. Terrain only."));

    if ( !mapFile->save(argv[2], true) )
    {
        std::cerr << "Failed to save map\n";
        return 4;
    }

    // Re-open what was actually written to disk so a corrupt MPQ/CHK never gets uploaded.
    MapFile verify(argv[2]);
    if ( verify.empty() || verify.getTileWidth() != 128 || verify.getTileHeight() != 128 || verify.getTileset() != TS )
    {
        std::cerr << "Verification reopen failed\n";
        return 5;
    }
    if ( verify.tiles.size() != 128u*128u || verify.editorTiles.size() != 128u*128u || verify.isomRects.size() != (128u/2u+1u)*(128u+1u) )
    {
        std::cerr << "Verification section sizes are invalid\n";
        return 6;
    }

    size_t zeroTiles = std::count(verify.tiles.begin(), verify.tiles.end(), uint16_t(0));
    std::cout << "Saved and reopened: " << argv[2] << "\n";
    std::cout << "ISOM=" << verify.isomRects.size() << " TILE=" << verify.editorTiles.size() << " MTXM=" << verify.tiles.size() << " zeroMTXM=" << zeroTiles << "\n";
    return 0;
}
