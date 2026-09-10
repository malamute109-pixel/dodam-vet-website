#include "IsomApi.h"
#include "../MappingCoreLib/MappingCore.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool loadCv5(const std::string & path, Sc::Terrain_::Tiles & out)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto size = size_t(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size == 0 || size % sizeof(Sc::Isom::TileGroup) != 0) return false;
    std::vector<uint8_t> bytes(size);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) return false;
    const size_t groups = size / sizeof(Sc::Isom::TileGroup);
    const auto * raw = reinterpret_cast<const Sc::Isom::TileGroup*>(bytes.data());
    out.tileGroups.assign(raw, raw + groups);
    out.loadIsom(size_t(Sc::Terrain::Tileset::Jungle));
    return true;
}

struct Region { const char * name; int x1,y1,x2,y2; };

static void dumpRegion(const MapFile & map, const Sc::Terrain_::Tiles & data, const Region & r)
{
    std::ofstream out(std::string(r.name)+".csv");
    out << "x,y,dx,dy,tile,group,index,terrainType,groundHeight,buildability\n";
    const int w=int(map.getTileWidth());
    for (int y=r.y1; y<=r.y2; ++y)
    for (int x=r.x1; x<=r.x2; ++x)
    {
        const u16 tile=map.tiles[size_t(y)*w+x];
        const size_t g=size_t(Sc::Terrain::getTileGroup(tile));
        unsigned tt=999, gh=999, b=999;
        if (g<data.tileGroups.size()) {
            tt=unsigned(data.tileGroups[g].terrainType);
            gh=unsigned(data.tileGroups[g].groundHeight);
            b=unsigned(data.tileGroups[g].buildability);
        }
        out << x << ',' << y << ',' << (x-r.x1) << ',' << (y-r.y1) << ','
            << unsigned(tile) << ',' << g << ',' << unsigned(tile&0xF) << ','
            << tt << ',' << gh << ',' << b << '\n';
    }
}

static void printHeightGrid(const MapFile & map, const Sc::Terrain_::Tiles & data, const Region & r)
{
    const int w=int(map.getTileWidth());
    std::cout << "=== " << r.name << " " << r.x1 << ',' << r.y1 << ".." << r.x2 << ',' << r.y2 << " ===\n";
    for (int y=r.y1; y<=r.y2; ++y) {
        for (int x=r.x1; x<=r.x2; ++x) {
            const u16 tile=map.tiles[size_t(y)*w+x];
            const size_t g=size_t(Sc::Terrain::getTileGroup(tile));
            unsigned gh=9;
            if (g<data.tileGroups.size()) gh=unsigned(data.tileGroups[g].groundHeight);
            std::cout << gh;
        }
        std::cout << '\n';
    }
}

int main(int argc, char ** argv)
{
    if (argc < 3) { std::cerr << "usage: RampProbe.exe <jungle.cv5> <custom.scx>\n"; return 2; }
    Sc::Terrain_::Tiles data;
    if (!loadCv5(argv[1], data)) return 3;
    MapFile map(argv[2]);
    if (map.empty()) return 4;

    const std::vector<Region> regions = {
        {"comp4_stairs",205,10,221,24},
        {"comp5_ramp",185,40,201,54},
        {"comp11_stairs",129,102,136,109},
        {"comp6_candidate",201,104,215,119},
        {"comp7_candidate",202,73,218,86}
    };
    for (const auto & r: regions) {
        dumpRegion(map,data,r);
        printHeightGrid(map,data,r);
    }
    return 0;
}
