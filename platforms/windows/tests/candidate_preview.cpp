// SPDX-License-Identifier: AGPL-3.0-only
// Draw only fixed fictional text into DIBs; no desktop capture or user settings.
#define NOMINMAX
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>
#include <bit>
#include <cstdio>
#include <string>
#include "../tsf/identity.h"
#include "candidate_test_interface.h"
using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t** argv){
    if(argc!=3)return 2;
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 3;
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module){CoUninitialize();return 4;}
    int result{};
    {
        using GetClass=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
        const auto get=std::bit_cast<GetClass>(GetProcAddress(module,"DllGetClassObject"));
        ComPtr<IClassFactory> factory;ComPtr<ITestCandidateView> view;
        if(!get||FAILED(get(transinput::ClassId,IID_PPV_ARGS(&factory)))||FAILED(factory->CreateInstance(nullptr,IID_PPV_ARGS(&view))))result=5;
        else {
            uint32_t placements{},paints{};const auto presented=view->CheckPresentation(&placements,&paints);
            if(FAILED(presented)){std::fprintf(stderr,"native presentation regression failed: 0x%08lx placements=%u paints=%u\n",static_cast<unsigned long>(presented),placements,paints);result=7;}
            else std::printf("PASS: 32 coalesced pending/ready cycles; one native placement/show, no hide, %u complete paints; delayed-frame expiry/cancel; owned offscreen windows only\n",paints);
        for(const uint32_t dpi:{96u,144u,192u})for(const BOOL bottom:{FALSE,TRUE}){
            const auto path=std::wstring(argv[2])+L"\\candidate-"+std::to_wstring(dpi)+(bottom?L"-bottom.png":L"-top.png");uint32_t content{},maximum{};
            const auto rendered=view->RenderPreview(path.c_str(),dpi,bottom,&content,&maximum);
            if(FAILED(rendered)||(bottom?(maximum==0||content<=maximum):maximum!=0)){std::fprintf(stderr,"candidate preview failed: 0x%08lx\n",static_cast<unsigned long>(rendered));result=6;}
            else std::printf("preview dpi=%u bottom=%d content=%u max_scroll=%u\n",dpi,bottom,content,maximum);
        }
        }
    }
    FreeLibrary(module);CoUninitialize();return result;
}
