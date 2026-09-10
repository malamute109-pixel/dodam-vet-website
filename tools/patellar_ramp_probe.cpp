#include "IsomApi.h"
#include "../MappingCoreLib/MappingCore.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
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

static std::string baseName(const std::string & path)
{
    const auto p = path.find_last_of("/\\");
    std::string s = p == std::string::npos ? path : path.substr(p+1);
    for ( char & c : s ) if ( c==' ' || c=='.' || c=='-' ) c='_';
    return s;
}

static void dumpPatch(const MapFile & map, const Sc::Terrain_::Tiles & data, const std::string & stem, size_t idx, int cx, int cy)
{
    const int w=int(map.getTileWidth()), h=int(map.getTileHeight());
    const int rx=10, ry=10;
    std::ofstream csv(stem + "_doodad_" + std::to_string(idx) + "_patch.csv");
    csv << "dx,dy,x,y,tile,group,index,terrainType,groundHeight,buildability\n";
    for ( int y=std::max(0,cy-ry); y<=std::min(h-1,cy+ry); ++y )
    for ( int x=std::max(0,cx-rx); x<=std::min(w-1,cx+rx); ++x )
    {
        const u16 tile=map.tiles[size_t(y)*w+x];
        const size_t g=size_t(Sc::Terrain::getTileGroup(tile));
        unsigned tt=999,gh=999,b=999;
        if ( g<data.tileGroups.size() ) { tt=unsigned(data.tileGroups[g].terrainType); gh=unsigned(data.tileGroups[g].groundHeight); b=unsigned(data.tileGroups[g].buildability); }
        csv << (x-cx) << ',' << (y-cy) << ',' << x << ',' << y << ',' << unsigned(tile) << ',' << g << ',' << unsigned(tile&0xF) << ',' << tt << ',' << gh << ',' << b << '\n';
    }
}

static void probeMap(const std::string & path, const Sc::Terrain_::Tiles & data)
{
    MapFile map(path);
    std::cout << "=== MAP " << path << " ===\n";
    if ( map.empty() ) { std::cout << "OPEN_FAILED\n"; return; }
    const std::string stem=baseName(path);
    std::cout << "dim=" << map.getTileWidth() << "x" << map.getTileHeight() << " tileset=" << int(map.getTileset())
              << " doodads=" << map.doodads.size() << " sprites=" << map.sprites.size() << " units=" << map.units.size() << "\n";

    for ( size_t i=0; i<map.doodads.size(); ++i )
    {
        const auto & d=map.doodads[i];
        const int tx=int(d.xc/32), ty=int(d.yc/32);
        std::cout << "DOODAD i=" << i << " type=" << unsigned(d.type) << " px=" << d.xc << ',' << d.yc
                  << " tile=" << tx << ',' << ty << " owner=" << unsigned(d.owner) << " enabled=" << unsigned(d.enabled) << "\n";
        dumpPatch(map,data,stem,i,tx,ty);
    }
}

int main(int argc, char ** argv)
{
    if ( argc < 3 ) { std::cerr << "usage: RampProbe.exe <jungle.cv5> <map1> [map2 ...]\n"; return 2; }
    Sc::Terrain_::Tiles data;
    if ( !loadCv5(argv[1],data) ) return 3;
    std::cout << "CV5 groups=" << data.tileGroups.size() << "\n";
    for ( int i=2; i<argc; ++i ) probeMap(argv[i],data);
    return 0;
}
