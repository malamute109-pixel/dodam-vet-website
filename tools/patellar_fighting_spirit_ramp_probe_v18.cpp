#include "IsomApi.h"
#include "../MappingCoreLib/MappingCore.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool loadCv5(const std::string & path, Sc::Terrain_::Tiles & out)
{
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f) return false;
    const size_t n=size_t(f.tellg()); f.seekg(0);
    if(n==0 || n%sizeof(Sc::Isom::TileGroup)!=0) return false;
    std::vector<uint8_t> b(n);
    if(!f.read(reinterpret_cast<char*>(b.data()),std::streamsize(n))) return false;
    const auto * p=reinterpret_cast<const Sc::Isom::TileGroup*>(b.data());
    out.tileGroups.assign(p,p+n/sizeof(Sc::Isom::TileGroup));
    out.loadIsom(size_t(Sc::Terrain::Tileset::Jungle));
    return true;
}

struct Candidate {
    int x=0,y=0,score=0,special=0,mid=0,low=0,high=0;
};

static unsigned elevation(uint8_t h) { return unsigned(h&0x0f); }

static Candidate scoreAt(const MapFile & map,const Sc::Terrain_::Tiles & data,int cx,int cy)
{
    Candidate c; c.x=cx; c.y=cy;
    const int w=int(map.getTileWidth()),h=int(map.getTileHeight());
    for(int y=std::max(0,cy-7);y<=std::min(h-1,cy+7);++y)
    for(int x=std::max(0,cx-7);x<=std::min(w-1,cx+7);++x)
    {
        const u16 t=map.tiles[size_t(y)*size_t(w)+size_t(x)];
        const size_t g=size_t(Sc::Terrain::getTileGroup(t));
        if(g>=data.tileGroups.size()) continue;
        const auto & tg=data.tileGroups[g];
        const unsigned e=elevation(tg.groundHeight);
        if(e==0) ++c.low; else if(e==2) ++c.high;
        if(e==1 || e==3) ++c.mid;
        if(g>=1024 && tg.terrainType==1 && tg.groundHeight>=16) ++c.special;
    }
    c.score=c.special*12+c.mid*20+std::min(c.low,c.high);
    return c;
}

static char symbol(const MapFile & map,const Sc::Terrain_::Tiles & data,int x,int y)
{
    const int w=int(map.getTileWidth());
    const u16 t=map.tiles[size_t(y)*size_t(w)+size_t(x)];
    const size_t g=size_t(Sc::Terrain::getTileGroup(t));
    if(g>=data.tileGroups.size()) return '?';
    const auto & tg=data.tileGroups[g];
    const unsigned e=elevation(tg.groundHeight);
    if(g>=1024 && tg.terrainType==1) {
        if(tg.groundHeight==16) return 'f';
        if(tg.groundHeight==17) return 'r';
        if(tg.groundHeight==18) return 'H';
        if(tg.groundHeight==19) return 'R';
        if(e==1 || e==3) return 'm';
    }
    if(e==0) return '.';
    if(e==2) return '#';
    return '+';
}

static void dump(const MapFile & map,const Sc::Terrain_::Tiles & data,const Candidate & c,int index)
{
    const int w=int(map.getTileWidth()),h=int(map.getTileHeight());
    std::cout<<"CANDIDATE "<<index<<" center="<<c.x<<","<<c.y
             <<" score="<<c.score<<" special="<<c.special<<" mid="<<c.mid
             <<" low="<<c.low<<" high="<<c.high<<"\n";
    for(int y=std::max(0,c.y-9);y<=std::min(h-1,c.y+9);++y) {
        std::cout<<"y="<<y<<" ";
        for(int x=std::max(0,c.x-9);x<=std::min(w-1,c.x+9);++x)
            std::cout<<symbol(map,data,x,y);
        std::cout<<"\n";
    }
}

int main(int argc,char ** argv)
{
    if(argc<3){std::cerr<<"usage: probe <jungle.cv5> <FightingSpirit.scx>\n";return 2;}
    Sc::Terrain_::Tiles data;
    if(!loadCv5(argv[1],data)) return 3;
    MapFile map(argv[2]);
    if(map.empty() || map.getTileWidth()!=128 || map.getTileHeight()!=128 ||
       map.getTileset()!=Sc::Terrain::Tileset::Jungle) return 4;

    std::vector<Candidate> all;
    for(int y=7;y<121;++y)
    for(int x=7;x<121;++x) {
        Candidate c=scoreAt(map,data,x,y);
        if(c.special>=2 && c.mid>=2 && c.low>=12 && c.high>=12) all.push_back(c);
    }
    std::sort(all.begin(),all.end(),[](const Candidate&a,const Candidate&b){return a.score>b.score;});

    std::vector<Candidate> chosen;
    for(const Candidate & c:all) {
        bool near=false;
        for(const Candidate & q:chosen) {
            const int dx=c.x-q.x,dy=c.y-q.y;
            if(dx*dx+dy*dy<144){near=true;break;}
        }
        if(!near) chosen.push_back(c);
        if(chosen.size()==24) break;
    }

    std::cout<<"FIGHTING_SPIRIT_RAMP_CANDIDATES="<<chosen.size()<<"\n";
    for(size_t i=0;i<chosen.size();++i) dump(map,data,chosen[i],int(i));
    return chosen.size()>=4?0:5;
}
