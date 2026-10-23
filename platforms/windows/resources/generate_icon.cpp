// SPDX-License-Identifier: AGPL-3.0-only
// Authored geometric input indicator, not a resample/edit of the user's PNG.
#define NOMINMAX
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <vector>
#include <array>
#include <algorithm>
#include <windows.h>
#pragma pack(push,1)
struct Header {uint16_t reserved{},kind{1},count{6};};
struct Entry {uint8_t width{},height{},colors{},reserved{};uint16_t planes{1},bits{32};uint32_t bytes{},offset{};};
#pragma pack(pop)
struct Segment {double ax,ay,bx,by;};
int wmain(int argc,wchar_t** argv){
    if(argc!=2&&argc!=3)return 2;
    const bool chinese=argc==3&&wcscmp(argv[2],L"chinese")==0,english=argc==3&&wcscmp(argv[2],L"english")==0;
    if(argc==3&&!chinese&&!english)return 2;
    FILE* file{};if(_wfopen_s(&file,argv[1],L"wb")||!file)return 3;
    const std::array<int,6> sizes{16,20,24,32,40,48};std::vector<std::vector<uint8_t>> images;std::array<Entry,6> entries{};uint32_t offset=sizeof(Header)+sizeof entries;
    // Deliberately simple TI letterforms stay legible at 16 pixels.
    std::vector<Segment> strokes{{4,7,17,7},{10.5,7,10.5,25},{21,7,28,7},{24.5,7,24.5,25},{21,25,28,25}};
    if(chinese)strokes={{6,10,26,10},{26,10,26,23},{26,23,6,23},{6,23,6,10},{16,4,16,29}};
    if(english)strokes={{4,6,28,6},{11,3,11,10},{21,3,21,10},{8,13,24,13},{8,13,8,21},{24,13,24,21},{3,21,29,21},{16,10,16,21},{16,21,6,29},{16,21,26,29}};
    for(size_t item=0;item<sizes.size();++item){
        const int size=sizes[item],mask_stride=((size+31)/32)*4;BITMAPINFOHEADER info{sizeof info,size,size*2,1,32,BI_RGB,static_cast<DWORD>(size*size*4),0,0,0,0};
        std::vector<uint8_t> bytes(sizeof info+static_cast<size_t>(size*size*4+mask_stride*size),0);memcpy(bytes.data(),&info,sizeof info);
        for(int y=0;y<size;++y)for(int x=0;x<size;++x){
            const bool mode=chinese||english;
            uint32_t pixel=mode?0u:(x==0||y==0||x==size-1||y==size-1)?0x80000000u:0xffffffffu;
            const double px=(x+0.5)*32/size,py=(y+0.5)*32/size;
            double distance=100.0;
            for(const auto& line:strokes){const auto dx=line.bx-line.ax,dy=line.by-line.ay;const auto t=std::clamp(((px-line.ax)*dx+(py-line.ay)*dy)/(dx*dx+dy*dy),0.0,1.0);distance=std::min(distance,std::hypot(px-line.ax-t*dx,py-line.ay-t*dy));}
            const auto width=std::max(1.0,16.0/size);
            if(distance<=width)pixel=mode?0xffffffffu:0xff000000u;
            else if(mode&&distance<=width*2)pixel=0x80000000u;
            memcpy(bytes.data()+sizeof info+static_cast<size_t>(((size-1-y)*size+x)*4),&pixel,4);
            if(mode&&pixel==0)bytes[sizeof info+static_cast<size_t>(size*size*4+(size-1-y)*mask_stride+x/8)]|=static_cast<uint8_t>(0x80u>>(x%8));
        }
        entries[item]={static_cast<uint8_t>(size),static_cast<uint8_t>(size),0,0,1,32,static_cast<uint32_t>(bytes.size()),offset};offset+=static_cast<uint32_t>(bytes.size());images.push_back(std::move(bytes));
    }
    const Header header{};bool good=fwrite(&header,sizeof header,1,file)==1&&fwrite(entries.data(),sizeof entries,1,file)==1;
    for(const auto& bytes:images)good=good&&fwrite(bytes.data(),bytes.size(),1,file)==1;
    good=fclose(file)==0&&good;return good?0:4;
}
