// SPDX-License-Identifier: AGPL-3.0-only
// Own native editor host; process-local TSF only. No system registration, network or saved user input.
#define NOMINMAX
#include "test_support.h"
#include "candidate_test_interface.h"
#include "resource_probe.h"
#include <array>
#include <bit>
#include <thread>
#include <utility>
#include <richedit.h>
#include <shellapi.h>
using Microsoft::WRL::ComPtr;
using namespace transinput::testing;

namespace {
constexpr wchar_t ManualClass[]=L"TransInput.Manual.RichEdit.40e2b4b8";
constexpr wchar_t Instructions[]=L"仅本窗口使用自研 TransInput，不改变系统输入法；内容不保存，翻译关闭。\r\n试试 nihao + 空格、n + PgDn + 2；Enter 提交拼音，Esc 取消。\r\nCaps Lock 输入英文；未提交中文会先上屏。Ctrl+A / Delete 清空，请用虚构文字。";
struct RichHost {
    HINSTANCE instance{GetModuleHandleW(nullptr)};HMODULE module{},rich_edit{};HWND window{},editor{},notice{};HFONT font{};
    ComPtr<IClassFactory> factory;ComPtr<LocalFactory> local;
    ComPtr<ITfThreadMgrEx> manager;ComPtr<ITfKeystrokeMgr> keyboard;ComPtr<ITfInputProcessorProfileMgr> profiles;
    ComPtr<ITfTextInputProcessorEx> service;ComPtr<ITfKeyEventSink> input;ComPtr<ITfDocumentMgr> document;ComPtr<ITfContext> context;
    ComPtr<ITestCandidateView> test_keys;
    std::unique_ptr<CandidateFixture> candidates;std::unique_ptr<TranslationFixture> translations;
    TfClientId client{};DWORD registration{};LANGID language{};bool registered{},checking{},not_ready{};UINT dpi{96};std::thread keep_alive;const char* phase{"startup"};
    void Setup(bool self_check){
        checking=self_check;
        const auto directory=transinput::pipe::ParentPath(transinput::pipe::ModulePath());
        const auto dll=directory+L"\\TransInputPrototypeTest.dll";
        module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);Require(module!=nullptr,"manual test DLL");
        using GetClass=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
        const auto get=std::bit_cast<GetClass>(GetProcAddress(module,"DllGetClassObject"));Require(get!=nullptr,"manual class export");
        Check(get(transinput::ClassId,IID_PPV_ARGS(&factory)),"manual class factory");
        candidates=std::make_unique<CandidateFixture>();translations=std::make_unique<TranslationFixture>();translations->Mode(0);
        Check(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&manager)),"manual thread manager");
        Check(manager->ActivateEx(&client,TF_TMAE_NOACTIVATETIP),"manual isolated activation");
        local.Attach(new LocalFactory(factory.Get()));Check(CoRegisterClassObject(transinput::ClassId,local.Get(),CLSCTX_INPROC_SERVER,REGCLS_MULTIPLEUSE,&registration),"manual local class");
        Check(CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&profiles)),"manual profile manager");
        language=LOWORD(reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)));
        Check(profiles->RegisterProfile(transinput::ClassId,language,transinput::ProfileId,transinput::PrototypeName,
            static_cast<ULONG>(std::size(transinput::PrototypeName)-1),nullptr,0,0,nullptr,0,TRUE,TF_RP_LOCALPROCESS|TF_RP_HIDDENINSETTINGUI),"manual local profile");registered=true;
        Check(profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR,language,transinput::ClassId,transinput::ProfileId,nullptr,TF_IPPMF_FORPROCESS),"manual process activation");
        // Keep NOACTIVATETIP for this diagnostic host. It explicitly activates
        // and routes to our service below; restarting with flags=0 needlessly
        // admits unrelated installed TIPs into this native-editor test process.
        Check(manager.As(&keyboard),"manual keyboard manager");
        rich_edit=LoadLibraryExW(L"msftedit.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);Require(rich_edit!=nullptr,"system RichEdit");
        WNDCLASSEXW wc{sizeof wc};wc.hInstance=instance;wc.lpszClassName=ManualClass;wc.lpfnWndProc=Procedure;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        Require(RegisterClassExW(&wc)!=0,"manual window class");
        const int left=checking?GetSystemMetrics(SM_XVIRTUALSCREEN)-4096:CW_USEDEFAULT;
        const int top=checking?GetSystemMetrics(SM_YVIRTUALSCREEN)-4096:CW_USEDEFAULT;
        window=CreateWindowExW(checking?(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE):0,ManualClass,L"TransInput 输入测试 · 中英混输 · 仅本窗口，不安装、不联网",WS_OVERLAPPEDWINDOW,left,top,780,520,nullptr,nullptr,instance,this);
        Require(window!=nullptr,"manual frame");
        notice=CreateWindowExW(0,L"STATIC",Instructions,WS_CHILD|WS_VISIBLE,16,14,720,82,window,nullptr,instance,nullptr);
        editor=CreateWindowExW(WS_EX_CLIENTEDGE,MSFTEDIT_CLASS,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|ES_MULTILINE|ES_AUTOVSCROLL|ES_NOHIDESEL,16,88,720,360,window,nullptr,instance,nullptr);
        Require(notice&&editor,"manual editor controls");
        UpdateFont();
        SendMessageW(editor,EM_SETTEXTMODE,TM_PLAINTEXT,0);SendMessageW(editor,EM_EXLIMITTEXT,0,65536);
        SendMessageW(editor,EM_SETEDITSTYLE,SES_USECTF,SES_USECTF|SES_NOIME|SES_CTFNOLOCK);
        
        // A hidden RichEdit can reject focus and never create its native TSF
        // document. SDK tests therefore use a visible offscreen tool window,
        // without foreground activation; popup rendering is suppressed below.
        Layout();
        if(checking){
            ShowWindow(window,SW_SHOWNOACTIVATE);
            if(!IsWindowVisible(window))ShowWindow(window,SW_SHOWNOACTIVATE); // STARTF_USESHOWWINDOW may hide the first call.
            Require(IsWindowVisible(window)!=FALSE,"native offscreen editor visibility");
            RECT bounds{};Require(GetWindowRect(window,&bounds)!=FALSE,"native offscreen bounds");
            Require(bounds.right<=GetSystemMetrics(SM_XVIRTUALSCREEN)||bounds.bottom<=GetSystemMetrics(SM_YVIRTUALSCREEN),"SDK tool window entered the visible desktop");
        }else ShowWindow(window,SW_SHOWNORMAL);
        if(!checking)SetFocus(editor);SendMessageW(editor,WM_SETFOCUS,0,0);
        const auto deadline=GetTickCount64()+2000;
        do{
            Pump();document.Reset();context.Reset();
            if(SUCCEEDED(manager->GetFocus(&document))&&document)document->GetTop(&context);
            if(!context){
                // A hidden native editor can have a TSF document association
                // while its thread is not the foreground TSF thread. Read and
                // restore only our own editor's association, then explicitly
                // focus that native document inside this SDK test manager.
                ComPtr<ITfDocumentMgr> associated,temporary;
                const auto association=manager->AssociateFocus(editor,nullptr,&associated);
                if(SUCCEEDED(association))Check(manager->AssociateFocus(editor,associated.Get(),&temporary),"restore own native focus association");
                if(associated){
                    document=associated;document->GetTop(&context);
                    if(context){Check(manager->SetFocus(document.Get()),"focus own associated native document");if(checking)std::puts("Native SDK startup: used own editor document association");}
                }
            }
        }while(!context&&GetTickCount64()<deadline);
        if(!context){BOOL thread_focus{};manager->IsThreadFocus(&thread_focus);
            std::fprintf(stderr,"Native SDK startup: own_focus=%d thread_focus=%d edit_style=0x%08lx\n",GetFocus()==editor,thread_focus,static_cast<unsigned long>(SendMessageW(editor,EM_GETEDITSTYLE,0,0)));}
        Require(context!=nullptr,"native RichEdit did not create its TSF context within 2 seconds");
        service=local->created;
        if(!service){
            Check(factory->CreateInstance(nullptr,IID_PPV_ARGS(&service)),"manual explicit TIP");ComPtr<ITfClientId> ids;TfClientId id{};
            Check(manager.As(&ids),"manual client IDs");Check(ids->GetClientId(transinput::ClassId,&id),"manual TIP client");Check(service->ActivateEx(manager.Get(),id,0),"manual TIP activation");
        }
        Check(service.As(&input),"manual own input sink");
        Check(service.As(&test_keys),"manual fixed callback sink");
        if(checking){
            Check(test_keys->SuppressWindow(TRUE),"suppress offscreen SDK candidate popup");
            Require(GetForegroundWindow()!=window,"offscreen SDK editor unexpectedly became foreground");
        }
        Layout();if(!checking){
            // Keep only this owned fixture alive while the user tests the pad.
            // No personal text, UI call, or paid request is used by this thread.
            keep_alive=std::thread([fixture=candidates.get()]{
                uint64_t request=1;
                while(WaitForSingleObject(fixture->stop.value,20000)==WAIT_TIMEOUT){
                    try{transinput::QueryBroker(fixture->endpoint,fixture->executable,fixture->stop.value,997,0,request++,{},0,fixture->pid,500);}catch(...){}
                }
            });
            SetFocus(editor);
        }
    }
    int Scale(int value)const{return MulDiv(value,static_cast<int>(dpi),96);}
    void UpdateFont(){
        dpi=GetDpiForWindow(window);if(!dpi)dpi=96;
        const auto next=CreateFontW(-Scale(18),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
        if(!next)return;
        SendMessageW(notice,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);SendMessageW(editor,WM_SETFONT,reinterpret_cast<WPARAM>(next),TRUE);
        if(font)DeleteObject(font);font=next;
    }
    void Layout(){if(!editor)return;RECT bounds{};GetClientRect(window,&bounds);MoveWindow(notice,Scale(16),Scale(14),std::max(1L,bounds.right-Scale(32)),Scale(82),TRUE);MoveWindow(editor,Scale(16),Scale(108),std::max(1L,bounds.right-Scale(32)),std::max(1L,bounds.bottom-Scale(124)),TRUE);}
    std::wstring Text()const{const auto length=GetWindowTextLengthW(editor);std::wstring value(static_cast<size_t>(length)+1,L'\0');GetWindowTextW(editor,value.data(),length+1);value.resize(length);return value;}
    bool Handle(const MSG& message){
        if(GetFocus()!=editor)return false;
        return HandleEditor(message);
    }
    bool HandleEditor(const MSG& message){
        if(message.message!=WM_KEYDOWN&&message.message!=WM_KEYUP&&message.message!=WM_CHAR)return false;
        // This diagnostic app routes its own editor messages explicitly. A
        // process-local profile is not a system-installed keyboard category;
        // forwarding first to the global key manager could test another IME.
        ComPtr<ITfDocumentMgr> current_document;ComPtr<ITfContext> current_context;
        if(SUCCEEDED(manager->GetFocus(&current_document))&&current_document)current_document->GetTop(&current_context);
        return RouteEditor(message,current_context.Get());
    }
    bool RouteEditor(const MSG& message,ITfContext* current_context){
        BOOL eaten{};
        if(!current_context){
            // Never let another IME fill this diagnostic editor and produce a
            // false positive when our own TSF route is unavailable.
            not_ready=true;SetWindowTextW(notice,L"输入测试通道未就绪，请重新点击编辑区；如仍无法输入，请关闭窗口后重试。");
            if(message.message==WM_KEYUP)input->OnKeyUp(context.Get(),message.wParam,message.lParam,&eaten);
            return true;
        }
        if(not_ready){not_ready=false;SetWindowTextW(notice,Instructions);}
        if(message.message==WM_KEYDOWN&&input->OnTestKeyDown(current_context,message.wParam,message.lParam,&eaten)==S_OK&&eaten){
            const auto result=input->OnKeyDown(current_context,message.wParam,message.lParam,&eaten);
            if(FAILED(result)||!eaten)SetWindowTextW(notice,L"当前测试接入失败，请关闭窗口后重试。不要把本窗口结果当作系统输入法已安装。");return true;
        }
        if(message.message==WM_KEYUP&&input->OnTestKeyUp(current_context,message.wParam,message.lParam,&eaten)==S_OK&&eaten){input->OnKeyUp(current_context,message.wParam,message.lParam,&eaten);return true;}
        return false;
    }
    void Key(WPARAM key,uint32_t modifiers=0){
        BOOL eaten{};Check(test_keys->Key(context.Get(),static_cast<uint32_t>(key),modifiers,&eaten,nullptr),"native fixed SDK callback");
        if(!eaten){TF_STATUS status{};const auto hr=context->GetStatus(&status);
            std::fprintf(stderr,"native fixed callback rejected: phase=%s key=%llu status=0x%08lx flags=%lu\n",phase,static_cast<unsigned long long>(key),static_cast<unsigned long>(hr),status.dwDynamicFlags);}
        Require(eaten!=FALSE,"native own sink did not consume fixed key");
    }
    void Wait(const std::wstring& expected){const auto deadline=GetTickCount64()+2000;while(Text()!=expected&&GetTickCount64()<deadline)Pump();
        if(Text()!=expected)std::fprintf(stderr,"Native fixed text mismatch: phase=%s expected_length=%zu actual_length=%zu\n",phase,expected.size(),Text().size());
        Require(Text()==expected,"native RichEdit fixed text mismatch");}
    void Type(const char* value){for(;*value;++value)Key(static_cast<WPARAM>(*value-'a'+'A'));}
    void ReadOnly(bool value){
        SendMessageW(editor,EM_SETREADONLY,value?TRUE:FALSE,0);TF_STATUS status{};const auto deadline=GetTickCount64()+2000;
        do{Pump();Check(context->GetStatus(&status),"native editor readonly notification");}
        while(((status.dwDynamicFlags&TF_SD_READONLY)!=0)!=value&&GetTickCount64()<deadline);
        Require(((status.dwDynamicFlags&TF_SD_READONLY)!=0)==value,"native readonly notification missed deadline");
    }
    void CheckInput(){
        phase="initial";Type("nihao");Key(VK_SPACE);Wait(L"你好");
        phase="cancel/edit/raw";
        Type("ceshi");Key(VK_ESCAPE);Wait(L"你好");
        Type("niho");Key(VK_LEFT);Type("a");Key(VK_RIGHT);Key(VK_SPACE);Wait(L"你好你好");
        Type("abc");Key(VK_RETURN);Wait(L"你好你好abc");
        Type("ceshi");Key(VK_BACK);Key(VK_DELETE);Key(VK_ESCAPE);Wait(L"你好你好abc");
        phase="page";const auto second=candidates->Candidates("n",1);Require(second.size()==9,"native page baseline");
        Type("n");Key(VK_NEXT);Key('2');Wait(L"你好你好abc"+second[1]);
        // A real RichEdit selection must be replaced, not appended elsewhere.
        phase="selection";SendMessageW(editor,EM_SETSEL,0,2);Pump();Type("ceshi");Key(VK_SPACE);Wait(L"测试你好abc"+second[1]);
        SendMessageW(editor,EM_SETSEL,static_cast<WPARAM>(-1),-1);Pump();
        auto expected=Text();
        phase="burst";for(unsigned batch=0;batch<8;++batch){
            for(unsigned i=0;i<4;++i){Type("nihao");Key(VK_SPACE);expected+=L"你好";}
            Wait(expected);
        }
        phase="readonly";ReadOnly(true);BOOL eaten{};
        Check(input->OnTestKeyDown(context.Get(),'N',0,&eaten),"native readonly test");Require(!eaten,"native readonly editor consumed input");
        ReadOnly(false);Type("nihao");Key(VK_SPACE);expected+=L"你好";Wait(expected);
        phase="mixed";Check(test_keys->HoldCandidates(TRUE),"native hold mixed candidates");
        Type("nihao");Key(VK_SPACE);Key('0');Key('G',TiTestCaps);Key('O',TiTestCaps|TiTestShift);Key('2',TiTestCaps);Key('0',TiTestCaps);Key('2',TiTestCaps);Key('6',TiTestCaps);
        Type("ceshi");Key(VK_SPACE);Check(test_keys->HoldCandidates(FALSE),"native release mixed candidates");expected+=L"你好0Go2026测试";Wait(expected);
        phase="focus";Type("ceshi");Pump();Check(input->OnSetFocus(FALSE),"native focus loss");Wait(expected);
        Check(input->OnSetFocus(TRUE),"native focus restore");Type("nihao");Key(VK_SPACE);expected+=L"你好";Wait(expected);
        phase="unavailable route";
        MSG blocked{};blocked.hwnd=editor;blocked.message=WM_KEYDOWN;blocked.wParam='N';
        Require(RouteEditor(blocked,nullptr),"unavailable native route leaked a key to another input path");Pump();Wait(expected);
        Type("nihao");Key(VK_SPACE);expected+=L"你好";Wait(expected);
        Require(translations->Count()==0,"native Chinese pad invoked translation");
        std::puts("PASS: native RichEdit edit/cancel/raw/page/selection/readonly/focus/mixed and 32 rapid commits; explicit own-sink callbacks, no OS key injection");
    }
    void Soak(unsigned seconds){
        // Only this app's fixed fictional text is typed and cleared. Never
        // interact with a foreign window, document, installed IME or network.
        phase="soak";
        const HANDLE processes[]{GetCurrentProcess(),candidates->process.value,translations->process.value};
        const char* names[]{"native-editor","candidate-broker","translation-fixture"};
        std::array<ResourceSample,3> baseline{};
        for(size_t i=0;i<baseline.size();++i)baseline[i]=ResourceSample::Read(processes[i]);
        const auto started=GetTickCount64();auto sampled=started;unsigned batches{},commits{};
        std::puts("METRIC,elapsed_ms,owned_process,private_bytes,handles,gdi,user,cpu_ms");
        for(size_t i=0;i<baseline.size();++i)baseline[i].Print(0,names[i]);
        const auto sample=[&]{
            for(size_t i=0;i<baseline.size();++i){const auto value=ResourceSample::Read(processes[i]);value.Print(GetTickCount64()-started,names[i]);value.CheckGrowth(baseline[i]);}
        };
        while(GetTickCount64()-started<static_cast<uint64_t>(seconds)*1000){
            Require(SetWindowTextW(editor,L"")!=FALSE,"clear own fixed soak text");Pump();Require(Text().empty(),"native soak reset failed");
            std::wstring expected;
            for(unsigned i=0;i<4;++i){Type("nihao");Key(VK_SPACE);expected+=L"你好";++commits;}
            Wait(expected);Type("ceshi");Key(VK_ESCAPE);Wait(expected);++batches;
            // Alternate bursts and idle so replies/cancellation and cleanup
            // keep running instead of testing only one saturated tight loop.
            if(batches%5==0)for(unsigned i=0;i<5;++i)Pump();
            if(GetTickCount64()-sampled>=5000){sample();sampled=GetTickCount64();}
        }
        sample();Require(translations->Count()==0,"soak invoked translation");
        std::printf("PASS: native soak duration_ms=%llu batches=%u commits=%u; fixed text correctness and bounded warm resources\n",
            static_cast<unsigned long long>(GetTickCount64()-started),batches,commits);
    }
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam)noexcept{
        if(message==WM_NCCREATE){auto app=static_cast<RichHost*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(app));}
        auto app=reinterpret_cast<RichHost*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(app){if(message==WM_SIZE){app->Layout();return 0;}if(message==WM_SETFOCUS&&app->editor){if(!app->checking)SetFocus(app->editor);return 0;}
            if(message==WM_DPICHANGED){const auto bounds=reinterpret_cast<RECT*>(lparam);app->UpdateFont();SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);app->Layout();return 0;}
            if(message==WM_GETMINMAXINFO){auto limits=reinterpret_cast<MINMAXINFO*>(lparam);limits->ptMinTrackSize={app->Scale(640),app->Scale(360)};return 0;}
            if(message==WM_CLOSE){DestroyWindow(window);return 0;}if(message==WM_DESTROY){app->window=nullptr;PostQuitMessage(0);return 0;}}
        return DefWindowProcW(window,message,wparam,lparam);
    }
    ~RichHost(){
        if(service)service->Deactivate();if(manager)manager->SetFocus(nullptr);test_keys.Reset();input.Reset();service.Reset();keyboard.Reset();context.Reset();document.Reset();
        if(window)DestroyWindow(window);if(profiles&&registered){profiles->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR,language,transinput::ClassId,transinput::ProfileId,nullptr,TF_IPPMF_FORPROCESS);profiles->UnregisterProfile(transinput::ClassId,language,transinput::ProfileId,TF_URP_LOCALPROCESS);}
        if(manager)manager->Deactivate();manager.Reset();profiles.Reset();if(registration)CoRevokeClassObject(registration);if(local)local->created.Reset();local.Reset();factory.Reset();
        if(candidates)SetEvent(candidates->stop.value);if(keep_alive.joinable())keep_alive.join();
        translations.reset();candidates.reset();Pump();if(font)DeleteObject(font);if(rich_edit)FreeLibrary(rich_edit);
        if(module){using CanUnload=HRESULT(WINAPI*)();const auto can=std::bit_cast<CanUnload>(GetProcAddress(module,"DllCanUnloadNow"));if(can&&can()==S_OK)FreeLibrary(module);}
        UnregisterClassW(ManualClass,instance);
    }
};
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int){
    int count{};auto args=CommandLineToArgvW(GetCommandLineW(),&count);if(!args)return 2;
    const std::wstring option=count==2?args[1]:L"";unsigned seconds{};
    if(option.starts_with(L"--soak-seconds=")){
        const auto number=option.substr(15);if(number.empty()||number.size()>5){LocalFree(args);return 2;}
        for(const auto digit:number){if(digit<L'0'||digit>L'9'){LocalFree(args);return 2;}seconds=seconds*10+static_cast<unsigned>(digit-L'0');}
        if(seconds<10||seconds>28800){LocalFree(args);return 2;}
    }
    const bool checking=count==2&&(option==L"--self-check"||seconds);
    const bool valid=count==1||checking;LocalFree(args);if(!valid){std::fputs("Usage: TransInputTestHost.exe [--self-check | --soak-seconds=10..28800]\n",stderr);return 2;}
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 3;
    int result{};try{RichHost app;app.Setup(checking);if(checking){app.CheckInput();if(seconds)app.Soak(seconds);std::puts("PASS: native RichEdit process-local TSF startup; offscreen/no-activate fixture; no registry installation");}
        else{MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){if(app.Handle(message))continue;TranslateMessage(&message);DispatchMessageW(&message);}}}
    catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());if(!checking)MessageBoxA(nullptr,error.what(),"TransInput test host",MB_OK|MB_ICONERROR);result=1;}
    CoUninitialize();return result;
}
