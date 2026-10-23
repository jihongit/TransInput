// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <gdiplus.h>
#include <cstdint>
#include <vector>
namespace transinput::testing {
struct PreviewBuffer {
    HDC dc{};HBITMAP bitmap{};HGDIOBJ previous{};ULONG_PTR runtime{};
    PreviewBuffer(int width,int height){
        Gdiplus::GdiplusStartupInput input;
        if(Gdiplus::GdiplusStartup(&runtime,&input,nullptr)!=Gdiplus::Ok)runtime=0;
        dc=CreateCompatibleDC(nullptr);BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* bits{};bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(dc&&bitmap)previous=SelectObject(dc,bitmap);
    }
    ~PreviewBuffer(){if(previous)SelectObject(dc,previous);if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);if(runtime)Gdiplus::GdiplusShutdown(runtime);}
    bool Save(const wchar_t* path){
        if(!runtime||!dc||!bitmap)return false;
        Gdiplus::Bitmap image(bitmap,nullptr);UINT count{},size{};
        if(Gdiplus::GetImageEncodersSize(&count,&size)!=Gdiplus::Ok||!size||size>1024*1024)return false;
        std::vector<uint8_t> data(size);auto codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(data.data());
        if(Gdiplus::GetImageEncoders(count,size,codecs)!=Gdiplus::Ok)return false;
        for(UINT i=0;i<count;++i)if(wcscmp(codecs[i].MimeType,L"image/png")==0)return image.Save(path,&codecs[i].Clsid)==Gdiplus::Ok;
        return false;
    }
};
}
