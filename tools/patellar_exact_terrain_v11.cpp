#include "IsomApi.h"
#include "../MappingCoreLib/MappingCore.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

static bool inside(const Poly & poly, double x, double y)
{
    bool c = false;
    for ( size_t i=0, j=poly.size()-1; i<poly.size(); j=i++ )
    {
        const auto & a = poly[i];
        const auto & b = poly[j];
        const bool crossing = ((a.y > y) != (b.y > y)) &&
            (x < (b.x-a.x)*(y-a.y)/(b.y-a.y) + a.x);
        if ( crossing ) c = !c;
    }
    return c;
}

static Poly mirrorY(const Poly & p)
{
    Poly r; r.reserve(p.size());
    for ( const auto & q : p ) r.push_back({q.x, 128.0-q.y});
    std::reverse(r.begin(), r.end());
    return r;
}

static Poly mirrorX(const Poly & p)
{
    Poly r; r.reserve(p.size());
    for ( const auto & q : p ) r.push_back({128.0-q.x, q.y});
    std::reverse(r.begin(), r.end());
    return r;
}

static bool stampPolygon(ScMap & map, Chk::IsomCache & cache, size_t terrainType,
                         const Poly & poly, size_t extent, const char * label)
{
    double minX=128, maxX=0, minY=128, maxY=0;
    for ( const auto & q : poly ) {
        minX=std::min(minX,q.x); maxX=std::max(maxX,q.x);
        minY=std::min(minY,q.y); maxY=std::max(maxY,q.y);
    }

    const int ix0 = std::max(0, int(std::floor(minX/2.0))-1);
    const int ix1 = std::min(64, int(std::ceil(maxX/2.0))+1);
    const int y0 = std::max(0, int(std::floor(minY))-1);
    const int y1 = std::min(127, int(std::ceil(maxY))+1);
    size_t count=0;

    for ( int ix=ix0; ix<=ix1; ++ix )
    {
        int y = y0;
        if ( (ix+y)&1 ) ++y;
        for ( ; y<=y1; y+=2 )
        {
            const double tileX = double(ix*2);
            const double tileY = double(y);
            if ( inside(poly, tileX, tileY) )
            {
                if ( !map.placeIsomTerrain({size_t(ix),size_t(y)}, terrainType, extent, cache) )
                {
                    std::cerr << "placeIsomTerrain failed in " << label << " at " << ix << "," << y << std::endl;
                    return false;
                }
                ++count;
            }
        }
    }

    map.updateTilesFromIsom(cache);
    cache.finalizeUndoableOperation();
    std::cout << "Stamped " << label << ": " << count << " ISOM centers" << std::endl;
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

int main(int argc, char ** argv)
{
    if ( argc < 3 ) {
        std::cerr << "usage: IsomTerrain.exe <jungle.cv5> <output.scx>" << std::endl;
        return 2;
    }

    Sc::Terrain_::Tiles jungleData;
    if ( !loadCv5(argv[1], jungleData) ) return 3;

    constexpr auto TS = Sc::Terrain::Tileset::Jungle;
    constexpr size_t WATER = Sc::Isom::Brush::Jungle::Water;
    constexpr size_t LOW = Sc::Isom::Brush::Jungle::Jungle_;
    constexpr size_t HIGH = Sc::Isom::Brush::Jungle::RaisedJungle;

    auto mapFile = std::make_unique<MapFile>(TS,128,128);
    mapFile->setSaveType(SaveType::ExpansionScx);
    ScMap scMap = copyToScMap(*mapFile);
    Chk::IsomCache cache(TS,128,128,jungleData);

    const uint16_t waterValue = ((cache.getTerrainTypeIsomValue(WATER) << 4) | Chk::IsomRect::EditorFlag::Modified);
    scMap.isomRects.assign(scMap.getIsomWidth()*scMap.getIsomHeight(), Chk::IsomRect{waterValue,waterValue,waterValue,waterValue});
    cache.setAllChanged();
    scMap.updateTilesFromIsom(cache);

    const std::vector<Pt> leftTop = {
        {39,0},{37,6},{36,13},{32,18},{28,23},{25,29},{23,36},{24,43},
        {27,48},{24,52},{22,59},{22,64}
    };
    Poly left = leftTop;
    for ( int i=int(leftTop.size())-2; i>=0; --i ) left.push_back({leftTop[i].x,128-leftTop[i].y});
    Poly mainland = left;
    for ( int i=int(left.size())-1; i>=0; --i ) mainland.push_back({128-left[i].x,left[i].y});

    const Poly leftIsland = {{4,50},{8,47},{15,46},{20,49},{23,55},{23,68},{20,74},{15,77},{8,76},{4,72},{2,66},{2,56}};
    const Poly rightIsland = mirrorX(leftIsland);

    if ( !stampPolygon(scMap,cache,LOW,mainland,1,"approved mainland") ) return 10;
    if ( !stampPolygon(scMap,cache,LOW,leftIsland,1,"left sesamoid shelf") ) return 11;
    if ( !stampPolygon(scMap,cache,LOW,rightIsland,1,"right sesamoid shelf") ) return 12;

    const Poly p1Main = {{39,1},{88,1},{91,6},{92,13},{88,19},{81,22},{71,22},{68,19},{60,19},{57,22},{47,23},{40,21},{35,16},{36,8}};
    const Poly p1Nat = {{53,22},{75,22},{80,25},{80,31},{76,35},{69,37},{59,37},{52,35},{48,31},{49,25}};
    const Poly patella = {{55,51},{73,51},{78,54},{80,60},{79,67},{74,72},{55,72},{50,68},{48,61},{50,55}};
    const Poly lTrochlea = {{35,39},{44,39},{48,43},{49,48},{46,54},{44,58},{46,63},{44,68},{47,74},{48,78},{45,83},{38,83},{34,79},{35,73},{38,67},{38,59},{35,53},{32,47}};
    const Poly rTrochlea = mirrorX(lTrochlea);
    const Poly p2Main = mirrorY(p1Main);
    const Poly p2Nat = mirrorY(p1Nat);

    if ( !stampPolygon(scMap,cache,HIGH,p1Main,1,"P1 main high ground") ) return 20;
    if ( !stampPolygon(scMap,cache,HIGH,p1Nat,1,"P1 natural high ground") ) return 21;
    if ( !stampPolygon(scMap,cache,HIGH,patella,1,"patella high ground") ) return 22;
    if ( !stampPolygon(scMap,cache,HIGH,lTrochlea,1,"left trochlear ridge") ) return 23;
    if ( !stampPolygon(scMap,cache,HIGH,rTrochlea,1,"right trochlear ridge") ) return 24;
    if ( !stampPolygon(scMap,cache,HIGH,p2Nat,1,"P2 natural high ground") ) return 25;
    if ( !stampPolygon(scMap,cache,HIGH,p2Main,1,"P2 main high ground") ) return 26;

    const Poly leftIslandCore = {{6,52},{10,49},{16,49},{20,52},{21,58},{21,67},{18,72},{12,74},{7,71},{5,65},{5,57}};
    const Poly rightIslandCore = mirrorX(leftIslandCore);
    if ( !stampPolygon(scMap,cache,HIGH,leftIslandCore,1,"left raised sesamoid") ) return 27;
    if ( !stampPolygon(scMap,cache,HIGH,rightIslandCore,1,"right raised sesamoid") ) return 28;

    const std::vector<std::pair<const char*,Poly>> ramps = {
        {"P1 main ramp", {{59,17},{69,17},{69,25},{67,27},{61,27},{59,25}}},
        {"P1 natural ramp", {{59,31},{69,31},{69,40},{67,42},{61,42},{59,40}}},
        {"north patellar ligament", {{59,47},{69,47},{69,56},{67,58},{61,58},{59,56}}},
        {"south patellar ligament", {{59,68},{69,68},{69,78},{67,80},{61,80},{59,78}}},
        {"P2 natural ramp", {{59,86},{69,86},{69,97},{67,99},{61,99},{59,97}}},
        {"P2 main ramp", {{59,101},{69,101},{69,111},{67,113},{61,113},{59,111}}},
        {"left upper trochlea ramp", {{28,39},{38,39},{38,48},{36,50},{30,50},{28,48}}},
        {"left lower trochlea ramp", {{28,75},{38,75},{38,85},{36,87},{30,87},{28,85}}},
        {"right upper trochlea ramp", {{90,39},{100,39},{100,48},{98,50},{92,50},{90,48}}},
        {"right lower trochlea ramp", {{90,75},{100,75},{100,85},{98,87},{92,87},{90,85}}}
    };
    for ( const auto & ramp : ramps )
        if ( !stampPolygon(scMap,cache,LOW,ramp.second,1,ramp.first) ) return 30;

    copyFromScMap(*mapFile,scMap);
    configure1v1(*mapFile);
    mapFile->addUnit(makeStart(64,14,0));
    mapFile->addUnit(makeStart(64,113,1));
    mapFile->setScenarioName(RawString("Patellar Luxation v1.1 Exact Terrain Test"));
    mapFile->setScenarioDescription(RawString("Terrain-only 1v1 reference-match test. No resources. Verify knee silhouette, coastline, elevated plateaus, and all ramp routes first."));

    if ( !mapFile->save(argv[2],true,false,false,true) ) return 40;

    MapFile verify(argv[2]);
    if ( verify.empty() || verify.getTileWidth()!=128 || verify.getTileHeight()!=128 || verify.getTileset()!=TS ) return 41;
    size_t starts=0;
    for ( size_t i=0; i<verify.numUnits(); ++i ) if ( verify.getUnit(i).type==Sc::Unit::Type::StartLocation ) ++starts;
    const size_t zeroTiles = std::count(verify.tiles.begin(),verify.tiles.end(),uint16_t(0));
    std::cout << "VALIDATED exact terrain: ISOM=" << verify.isomRects.size()
              << " TILE=" << verify.editorTiles.size() << " MTXM=" << verify.tiles.size()
              << " tileIndexZero=" << zeroTiles << " starts=" << starts << " units=" << verify.numUnits() << std::endl;
    // Tile value 0 is a legal Jungle megatile index, so it is diagnostic only.
    if ( starts!=2 || verify.numUnits()!=2 ) return 42;
    return 0;
}
