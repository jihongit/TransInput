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
#include "preview_png.h"
#include "../tsf/commit_presentation.h"
using Microsoft::WRL::ComPtr;
bool CommitPresentationChecks(){
    const auto instance=GetModuleHandleW(nullptr);
    WNDCLASSW cls{};cls.hInstance=instance;cls.lpszClassName=L"Scintilla";cls.lpfnWndProc=DefWindowProcW;
    if(!RegisterClassW(&cls))return false;
    const auto window=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Owned commit drawing fixture",WS_POPUP,
        GetSystemMetrics(SM_XVIRTUALSCREEN)-8192,GetSystemMetrics(SM_YVIRTUALSCREEN)-8192,32,32,nullptr,nullptr,instance,nullptr);
    if(!window){UnregisterClassW(cls.lpszClassName,instance);return false;}
    auto frame=std::make_shared<transinput::CommitPresentation>(instance);
    bool good=!frame->Begin(window)&&!IsWindowVisible(window);
    ShowWindow(window,SW_SHOWNOACTIVATE);
    {transinput::CommitPresentation::Call outer(frame.get());good=good&&frame->Begin(window)&&GetPropW(window,L"SysSetRedraw");
        good=good&&!frame->Begin(window);
        const auto deadline=GetTickCount64()+32;
        while(GetTickCount64()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}
        good=good&&frame->Active(window)&&GetPropW(window,L"SysSetRedraw");}
    good=good&&frame->Active(window); // The key callback has returned; legacy IME work may still follow.
    const auto deadline=GetTickCount64()+500;
    while(!frame->Empty()&&GetTickCount64()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}
    good=good&&frame->Empty();
    good=good&&IsWindowVisible(window)&&!GetPropW(window,L"SysSetRedraw");
    try{transinput::CommitPresentation::Call failure(frame.get());frame->Begin(window);throw 1;}catch(...){good=good&&frame->Empty()&&IsWindowVisible(window)&&!GetPropW(window,L"SysSetRedraw");}
    frame->Begin(window);frame.reset();good=good&&IsWindowVisible(window)&&!GetPropW(window,L"SysSetRedraw");
    ShowWindow(window,SW_HIDE);DestroyWindow(window);UnregisterClassW(cls.lpszClassName,instance);
    return good;
}
int wmain(int argc,wchar_t** argv){
    if(argc!=3)return 2;
    if(!CommitPresentationChecks()){std::puts("FAIL: commit frame restoration/hidden/nested/error/shutdown path");return 9;}
    std::puts("PASS: owned Scintilla commit frame covers legacy work after callback; nested loops defer restore; timer/error/shutdown restore; hidden windows stay hidden");
    const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 3;
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!module){CoUninitialize();return 4;}
    int result{};
    for(int resource=1;resource<=3;++resource){
        const auto name=resource==1?L"TI":resource==2?L"chinese":L"english";
        for(int size:{16,48}){
            const auto icon=static_cast<HICON>(LoadImageW(module,MAKEINTRESOURCEW(resource),IMAGE_ICON,size,size,0));
            transinput::testing::PreviewBuffer buffer(size,size);
            RECT bounds{0,0,size,size};const auto background=CreateSolidBrush(RGB(32,32,32));FillRect(buffer.dc,&bounds,background);DeleteObject(background);
            const auto path=std::wstring(argv[2])+L"\\indicator-"+name+L"-"+std::to_wstring(size)+L".png";
            if(!icon||!DrawIconEx(buffer.dc,0,0,icon,size,size,0,nullptr,DI_NORMAL)||!buffer.Save(path.c_str()))result=8;
            if(icon)DestroyIcon(icon);
        }
    }
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
