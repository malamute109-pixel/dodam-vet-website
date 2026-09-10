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
    for ( char & c : s ) {
        if ( c == ' ' || c == '.' || c == '-' ) c = '_';
    }
    return s;
}

static void dumpGrid(const MapFile & map, const Sc::Terrain_::Tiles & data, const std::string & stem)
{
    const int w = int(map.getTileWidth()), h = int(map.getTileHeight());
    std::ofstream csv(stem + "_grid.csv");
    csv << "x,y,tile,group,index,terrainType,groundHeight,buildability\n";
    for ( int y=0; y<h; ++y ) for ( int x=0; x<w; ++x )
    {
        const u16 tile = map.tiles[size_t(y)*w+x];
        const size_t group = size_t(Sc::Terrain::getTileGroup(tile));
        const unsigned idx = unsigned(tile & 0x0F);
        unsigned tt=999, gh=999, b=999;
        if ( group < data.tileGroups.size() ) {
            tt = unsigned(data.tileGroups[group].terrainType);
            gh = unsigned(data.tileGroups[group].groundHeight);
            b  = unsigned(data.tileGroups[group].buildability);
        }
        csv << x << ',' << y << ',' << unsigned(tile) << ',' << group << ',' << idx << ',' << tt << ',' << gh << ',' << b << '\n';
    }

    // Quick categorical PPM: 4x4 pixels per StarCraft tile.
    // green=normal Jungle low, tan=Raised Jungle, blue=water,
    // magenta/cyan/yellow=custom ramp-ish group metadata, gray=other.
    const int S=4;
    std::ofstream ppm(stem + "_layout.ppm", std::ios::binary);
    ppm << "P6\n" << w*S << ' ' << h*S << "\n255\n";
    for ( int y=0; y<h; ++y )
    {
        for ( int sy=0; sy<S; ++sy )
        {
            for ( int x=0; x<w; ++x )
            {
                const u16 tile = map.tiles[size_t(y)*w+x];
                const size_t group = size_t(Sc::Terrain::getTileGroup(tile));
                unsigned tt=999, gh=999;
                if ( group < data.tileGroups.size() ) { tt=unsigned(data.tileGroups[group].terrainType); gh=unsigned(data.tileGroups[group].groundHeight); }
                unsigned char r=45,g=45,b=45;
                if ( tile==0 ) { r=8; g=8; b=8; }
                else if ( tt==5 ) { r=15; g=55; b=125; }
                else if ( tt==8 ) { r=45; g=115; b=45; }
                else if ( tt==9 ) { r=155; g=115; b=55; }
                else if ( group>=1024 && gh==19 ) { r=255; g=40; b=220; }
                else if ( group>=1024 && (gh==16 || gh==17 || gh==18) ) { r=40; g=235; b=245; }
                else if ( group>=1024 && gh==1 ) { r=255; g=230; b=30; }
                else if ( group>=1024 ) { r=220; g=80; b=60; }
                else if ( gh>=2 ) { r=170; g=125; b=70; }
                for ( int sx=0; sx<S; ++sx ) { ppm.put(char(r)); ppm.put(char(g)); ppm.put(char(b)); }
            }
        }
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

    std::map<unsigned,size_t> doodads;
    for ( const auto & d : map.doodads ) doodads[unsigned(d.type)]++;
    std::cout << "DOODAD_TYPE_COUNTS";
    for ( const auto & kv : doodads ) std::cout << ' ' << kv.first << ':' << kv.second;
    std::cout << "\n";

    std::map<size_t,size_t> groups;
    for ( u16 tile : map.tiles ) groups[size_t(Sc::Terrain::getTileGroup(tile))]++;
    size_t customTiles=0, unusualHeight=0;
    for ( const auto & kv : groups ) if ( kv.first >= 1024 ) customTiles += kv.second;
    for ( u16 tile : map.tiles ) {
        const size_t gr=size_t(Sc::Terrain::getTileGroup(tile));
        if ( gr<data.tileGroups.size() ) {
            const unsigned gh=unsigned(data.tileGroups[gr].groundHeight);
            if ( gh!=0 && gh!=2 && gh!=4 ) ++unusualHeight;
        }
    }
    std::cout << "customGroupTiles=" << customTiles << " unusualGroundHeightTiles=" << unusualHeight << "\n";

    dumpGrid(map,data,baseName(path));
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
