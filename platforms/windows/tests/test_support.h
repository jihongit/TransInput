// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include "../tsf/identity.h"
#include "../tsf/async_broker.h"
#include "../shared/protected_file.h"
#include "../shared/words_control.h"
#include "../shared/learning_control.h"
#include "../translator/client.h"
namespace transinput::testing {
using Microsoft::WRL::ComPtr;
inline void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
inline void Check(HRESULT hr, const char* message) {
    if (FAILED(hr)) { std::fprintf(stderr, "%s: 0x%08lx\n", message, static_cast<unsigned long>(hr)); throw std::runtime_error(message); }
}

inline void Pump() {
    const auto deadline = GetTickCount64() + 100;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjectsEx(0, nullptr, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
}

// The class and profile are local to this test process. No HKCU/HKLM registration,
// desktop-wide activation, keystroke injection, or default-language changes.
class LocalFactory final : public IClassFactory {
    std::atomic<ULONG> references_{1};
    ComPtr<IClassFactory> factory_;
public:
    ComPtr<ITfTextInputProcessorEx> created;
    explicit LocalFactory(IClassFactory* factory) : factory_(factory) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *value = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references_; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** value) override {
        const auto hr = factory_->CreateInstance(outer, iid, value);
        if (SUCCEEDED(hr) && value && *value) {
            static_cast<IUnknown*>(*value)->QueryInterface(IID_PPV_ARGS(&created));
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { return factory_->LockServer(lock); }
};

struct CandidateFixture {
    transinput::pipe::Handle process,stop;
    std::wstring endpoint,executable,root,path,stop_name,test_id;DWORD pid{};uint64_t request{1};
    CandidateFixture(){
        using namespace transinput;
        test_id=std::to_wstring(GetCurrentProcessId())+L"c";endpoint=pipe::Endpoint(test_id);
        executable=AsyncBroker::Path(GetModuleHandleW(nullptr));
        GUID guid{};Require(SUCCEEDED(CoCreateGuid(&guid)),"candidate fixture GUID");wchar_t suffix[40]{};
        Require(StringFromGUID2(guid,suffix,40)>0,"candidate fixture suffix");
        root=pipe::ParentPath(pipe::ModulePath())+L"\\test-learning-"+suffix;storage::UserSecurity security;
        Require(CreateDirectoryW(root.c_str(),&security.attributes)!=FALSE,"candidate fixture directory");path=root+L"\\choices.bin";
        stop_name=L"Local\\TransInput.Candidate.Test."+test_id+suffix;
        stop.Reset(CreateEventW(&security.attributes,TRUE,FALSE,stop_name.c_str()));Require(bool(stop),"candidate fixture stop event");Start();
    }
    void Start(){
        using namespace transinput;ResetEvent(stop.value);
        auto command=L"\""+executable+L"\" --parent="+std::to_wstring(GetCurrentProcessId())+L" --test-id="+test_id+
            L" --stop-event=\""+stop_name+L"\" --learning-path=\""+path+L"\" --words-path=\""+path+L".words\" --idle-ms=60000";
        STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        Require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,pipe::ParentPath(executable).c_str(),&startup,&info)!=FALSE,"start owned learning broker");
        process.Reset(info.hProcess);CloseHandle(info.hThread);pid=info.dwProcessId;
        auto channel=pipe::Connect(endpoint,executable,nullptr,pid,3000);Require(bool(channel),"owned learning broker readiness");
    }
    void Stop(){if(process){SetEvent(stop.value);Require(WaitForSingleObject(process.value,3000)==WAIT_OBJECT_0,"owned learning broker shutdown");process.Reset();}}
    void Restart(bool learning=true){Stop();const auto saved=learning?path:path+L".words";Require(GetFileAttributesW(saved.c_str())!=INVALID_FILE_ATTRIBUTES,"owned profile was not persisted before restart");Start();
        // Basic candidate pipes intentionally become ready before disk profiles.
        // A persistence assertion must wait for the specific profile, not infer
        // it from pipe readiness (which could legitimately return an empty set).
        if(learning){
            const auto deadline=GetTickCount64()+3000;transinput::learning_control::Result ready;
            do{ready=transinput::learning_control::Query(endpoint,executable,nullptr,transinput::wire::LearningStatus,0,false,pid);if(!ready.status&&ready.view.health!=0)break;Pump();}while(GetTickCount64()<deadline);
            Require(!ready.status&&ready.view.health==2,"restarted owned learning profile not ready");
        }
        if(GetFileAttributesW((path+L".words").c_str())!=INVALID_FILE_ATTRIBUTES){
            const auto deadline=GetTickCount64()+3000;transinput::words_control::Result ready;
            do{ready=transinput::words_control::Query(endpoint,executable,nullptr,0,0,transinput::wire::WordsStatus,{},pid);if(!ready.status&&ready.view.health!=transinput::words_control::Loading)break;Pump();}while(GetTickCount64()<deadline);
            Require(!ready.status&&ready.view.health==transinput::words_control::Ready,"restarted owned personal profile not ready");
        }
    }
    std::string First(const std::string& input){
        const auto result=transinput::QueryBroker(endpoint,executable,nullptr,999,1,request++,input,static_cast<uint32_t>(input.size()),pid);
        if(result.status||!result.snapshot.candidate_count)std::fprintf(stderr,"fixed candidate fixture query: status=%lu rows=%u input=%s\n",result.status,result.snapshot.candidate_count,input.c_str());
        Require(result.status==0&&result.snapshot.candidate_count>0,"owned learning query");
        return {reinterpret_cast<const char*>(result.snapshot.candidates[0].bytes),result.snapshot.candidates[0].length};
    }
    std::vector<std::wstring> Candidates(const std::string& input,uint32_t page=0){
        const auto result=transinput::QueryBroker(endpoint,executable,nullptr,999,1,request++,input,static_cast<uint32_t>(input.size()),pid,500,page);
        Require(result.status==0,"digit baseline query");std::vector<std::wstring> output;
        for(uint32_t i=0;i<result.snapshot.candidate_count;++i){
            const auto& text=result.snapshot.candidates[i];
            const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(text.bytes),static_cast<int>(text.length),nullptr,0);
            Require(size>0,"digit baseline UTF-8");std::wstring wide(size,L'\0');
            Require(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(text.bytes),static_cast<int>(text.length),wide.data(),size)==size,"digit baseline decode");output.push_back(std::move(wide));
        }
        return output;
    }
    ~CandidateFixture(){
        if(process){SetEvent(stop.value);if(WaitForSingleObject(process.value,3000)!=WAIT_OBJECT_0)TerminateProcess(process.value,99);process.Reset();}
        DeleteFileW(path.c_str());DeleteFileW((path+L".words").c_str());RemoveDirectoryW(root.c_str());
    }
};
struct TranslationFixture {
    transinput::pipe::Handle process;std::wstring endpoint,executable;DWORD pid{};
    TranslationFixture(){
        using namespace transinput;
        endpoint=translation::Endpoint(std::to_wstring(GetCurrentProcessId())+L"f");
        executable=pipe::ParentPath(pipe::ModulePath())+L"\\TransInputTranslationFixture.exe";
        auto command=L"\""+executable+L"\" --parent="+std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        Require(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,pipe::ParentPath(executable).c_str(),&startup,&info)!=FALSE,"start no-network translation fixture");
        process.Reset(info.hProcess);CloseHandle(info.hThread);pid=info.dwProcessId;
        auto channel=pipe::Connect(endpoint,executable,nullptr,pid,3000);Require(bool(channel),"translation fixture readiness");
    }
    ~TranslationFixture(){if(process){transinput::translation::Query(endpoint,executable,nullptr,33,0,0,1,{},pid);
        if(WaitForSingleObject(process.value,2000)!=WAIT_OBJECT_0)TerminateProcess(process.value,99);}}
    void Mode(uint64_t mode){const auto reply=transinput::translation::Query(endpoint,executable,nullptr,31,0,mode,2,{},pid);Require(reply.status==0,"fixture mode control");}
    unsigned Count(){const auto reply=transinput::translation::Query(endpoint,executable,nullptr,32,0,0,3,{},pid);Require(reply.status==0,"fixture count");return std::stoul(reply.view.output);}
    std::string Source(){const auto reply=transinput::translation::Query(endpoint,executable,nullptr,34,0,0,4,{},pid);Require(reply.status==0,"fixture source");return reply.view.output;}
};

}
