#include "IsomApi.h"
#include "../MappingCoreLib/MappingCore.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

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
    return true;
}

static void dumpWindow(const MapFile & map, const Sc::Terrain_::Tiles & data, int cx, int cy, int rx=8, int ry=8)
{
    const int w = int(map.getTileWidth()), h = int(map.getTileHeight());
    std::cout << "WINDOW centerTile=" << cx << "," << cy << " radius=" << rx << "x" << ry << "\n";
    for ( int y=std::max(0,cy-ry); y<=std::min(h-1,cy+ry); ++y )
    {
        std::cout << "y=" << y << ":";
        for ( int x=std::max(0,cx-rx); x<=std::min(w-1,cx+rx); ++x )
        {
            const u16 tile = map.tiles[size_t(y)*w+x];
            const size_t g = size_t(Sc::Terrain::getTileGroup(tile));
            size_t tt = 999, gh = 999;
            if ( g < data.tileGroups.size() ) { tt = data.tileGroups[g].terrainType; gh = data.tileGroups[g].groundHeight; }
            std::cout << " " << x << "=" << tile << "/g" << g << "/t" << tt << "/h" << gh;
        }
        std::cout << "\n";
    }
}

static void probeMap(const std::string & path, const Sc::Terrain_::Tiles & data)
{
    MapFile map(path);
    std::cout << "=== MAP " << path << " ===\n";
    if ( map.empty() ) { std::cout << "OPEN_FAILED\n"; return; }
    std::cout << "dim=" << map.getTileWidth() << "x" << map.getTileHeight()
              << " tileset=" << int(map.getTileset())
              << " doodads=" << map.doodads.size()
              << " sprites=" << map.sprites.size()
              << " units=" << map.units.size() << "\n";

    std::map<unsigned,size_t> counts;
    for ( const auto & d : map.doodads ) counts[unsigned(d.type)]++;
    std::cout << "DOODAD_TYPE_COUNTS";
    for ( const auto & kv : counts ) std::cout << " " << kv.first << ":" << kv.second;
    std::cout << "\n";

    for ( size_t i=0; i<map.doodads.size(); ++i )
    {
        const auto & d = map.doodads[i];
        std::cout << "DOODAD i=" << i << " type=" << unsigned(d.type)
                  << " px=" << d.xc << "," << d.yc
                  << " tile=" << (d.xc/32) << "," << (d.yc/32)
                  << " owner=" << unsigned(d.owner)
                  << " enabled=" << unsigned(d.enabled) << "\n";
        // SCM Draft calls the hidden raised-Jungle ramp 'Jungle #4'.
        // Dump a generous window around numeric type 4, plus the first few doodads
        // so the database layout remains inspectable even if numbering differs.
        if ( unsigned(d.type)==4 || i<12 )
            dumpWindow(map,data,int(d.xc/32),int(d.yc/32),7,7);
    }

    // Summarize tile groups that have mixed/transition ground-height metadata.
    std::map<size_t,size_t> groups;
    for ( u16 tile : map.tiles ) groups[size_t(Sc::Terrain::getTileGroup(tile))]++;
    std::cout << "RARE_GROUPS";
    for ( const auto & kv : groups )
    {
        if ( kv.second <= 32 && kv.first < data.tileGroups.size() )
        {
            const auto & tg = data.tileGroups[kv.first];
            std::cout << " g" << kv.first << "(n=" << kv.second << ",t=" << unsigned(tg.terrainType)
                      << ",h=" << unsigned(tg.groundHeight) << ")";
        }
    }
    std::cout << "\n";
}

int main(int argc, char ** argv)
{
    if ( argc < 3 ) {
        std::cerr << "usage: RampProbe.exe <jungle.cv5> <map1> [map2 ...]\n";
        return 2;
    }
    Sc::Terrain_::Tiles data;
    if ( !loadCv5(argv[1],data) ) return 3;
    std::cout << "CV5 groups=" << data.tileGroups.size() << "\n";
    for ( int i=2; i<argc; ++i ) probeMap(argv[i],data);
    return 0;
}
