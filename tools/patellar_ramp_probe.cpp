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

static std::vector<uint8_t> loadBytes(const std::string & path)
{
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f) return {};
    auto n=size_t(f.tellg()); f.seekg(0);
    std::vector<uint8_t> b(n); if(n && !f.read((char*)b.data(),std::streamsize(n))) return {};
    return b;
}

static bool loadCv5(const std::string & path, Sc::Terrain_::Tiles & out)
{
    auto bytes=loadBytes(path);
    if(bytes.empty() || bytes.size()%sizeof(Sc::Isom::TileGroup)!=0) return false;
    const size_t groups=bytes.size()/sizeof(Sc::Isom::TileGroup);
    const auto * raw=reinterpret_cast<const Sc::Isom::TileGroup*>(bytes.data());
    out.tileGroups.assign(raw,raw+groups);
    out.loadIsom(size_t(Sc::Terrain::Tileset::Jungle));
    return true;
}

static std::string baseName(const std::string & path)
{
    const auto p=path.find_last_of("/\\");
    std::string s=p==std::string::npos?path:path.substr(p+1);
    for(char & c:s) if(c==' '||c=='.'||c=='-') c='_';
    return s;
}

static uint16_t rd16(const std::vector<uint8_t>& b,size_t p)
{
    return p+1<b.size()?uint16_t(b[p]|(uint16_t(b[p+1])<<8)):0;
}

static void renderMap(const MapFile & map,const Sc::Terrain_::Tiles & cv5,
                      const std::vector<uint8_t> & vx4,const std::vector<uint8_t> & vr4,const std::vector<uint8_t> & wpe,
                      const std::string & out)
{
    const int tw=int(map.getTileWidth()), th=int(map.getTileHeight());
    const int W=tw*32,H=th*32;
    std::ofstream ppm(out,std::ios::binary); ppm<<"P6\n"<<W<<' '<<H<<"\n255\n";
    std::vector<uint8_t> row(size_t(W)*3);
    for(int ty=0;ty<th;++ty){
        for(int py=0;py<32;++py){
            std::fill(row.begin(),row.end(),0);
            for(int tx=0;tx<tw;++tx){
                const uint16_t tile=map.tiles[size_t(ty)*tw+tx];
                const size_t group=size_t(Sc::Terrain::getTileGroup(tile));
                const unsigned idx=unsigned(tile&0x0F);
                uint16_t mega=0;
                if(group<cv5.tileGroups.size()) mega=cv5.tileGroups[group].megaTileIndex[idx];
                const int suby=py/8, inY=py%8;
                for(int subx=0;subx<4;++subx){
                    const uint16_t v=rd16(vx4,size_t(mega)*32+size_t(suby*4+subx)*2);
                    const size_t mini=size_t(v>>1); const bool flip=(v&1)!=0;
                    for(int i=0;i<8;++i){
                        const int srcx=flip?7-i:i;
                        const size_t rp=mini*64+size_t(inY)*8+srcx;
                        const uint8_t pal=rp<vr4.size()?vr4[rp]:0;
                        const size_t wp=size_t(pal)*4;
                        uint8_t r=0,g=0,b=0;
                        if(wp+2<wpe.size()){ r=wpe[wp]; g=wpe[wp+1]; b=wpe[wp+2]; }
                        const int dx=tx*32+subx*8+i;
                        row[size_t(dx)*3]=r; row[size_t(dx)*3+1]=g; row[size_t(dx)*3+2]=b;
                    }
                }
            }
            ppm.write((char*)row.data(),std::streamsize(row.size()));
        }
    }
}

static void probeMap(const std::string & path,const Sc::Terrain_::Tiles & data,
                     const std::vector<uint8_t>&vx4,const std::vector<uint8_t>&vr4,const std::vector<uint8_t>&wpe)
{
    MapFile map(path);
    std::cout<<"=== MAP "<<path<<" ===\n";
    if(map.empty()){ std::cout<<"OPEN_FAILED\n"; return; }
    std::cout<<"dim="<<map.getTileWidth()<<'x'<<map.getTileHeight()<<" doodads="<<map.doodads.size()<<"\n";
    for(size_t i=0;i<map.doodads.size();++i){ const auto &d=map.doodads[i];
        std::cout<<"DOODAD i="<<i<<" type="<<unsigned(d.type)<<" tile="<<(d.xc/32)<<','<<(d.yc/32)<<" enabled="<<unsigned(d.enabled)<<"\n"; }
    renderMap(map,data,vx4,vr4,wpe,baseName(path)+"_exact.ppm");
}

int main(int argc,char **argv)
{
    if(argc<6){ std::cerr<<"usage: probe <cv5> <vx4> <vr4> <wpe> <map...>\n"; return 2; }
    Sc::Terrain_::Tiles data; if(!loadCv5(argv[1],data)) return 3;
    auto vx4=loadBytes(argv[2]), vr4=loadBytes(argv[3]), wpe=loadBytes(argv[4]);
    if(vx4.empty()||vr4.empty()||wpe.empty()) return 4;
    for(int i=5;i<argc;++i) probeMap(argv[i],data,vx4,vr4,wpe);
    return 0;
}
