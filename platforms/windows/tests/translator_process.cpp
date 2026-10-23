// SPDX-License-Identifier: AGPL-3.0-only
#include "../translator/client.h"
#include "../tsf/async_translation.h"
#include <atomic>
#include <cstdio>
#include <thread>
namespace {
using namespace transinput;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Server {
    pipe::Handle process,stop;std::wstring executable,endpoint;DWORD pid{};
    explicit Server(const std::wstring& path):executable(path){
        const auto id=std::to_wstring(GetCurrentProcessId())+L"a"+std::to_wstring(GetTickCount64());
        endpoint=translation::Endpoint(id);const auto stop_name=L"Local\\TransInput.TranslatorTest."+id;
        pipe::Security security;stop.Reset(CreateEventW(&security.attributes,TRUE,FALSE,stop_name.c_str()));Require(bool(stop),"stop event");
        // A missing fixture settings file means no key and no HTTP. Never read user configuration.
        const auto settings=pipe::ParentPath(path)+L"\\missing-settings-"+id+L".bin";
        auto command=L"\""+path+L"\" --test-id="+id+L" --parent="+std::to_wstring(GetCurrentProcessId())+
            L" --stop-event="+stop_name+L" --settings-path=\""+settings+L"\" --idle-ms=10000";
        STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        Require(CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,pipe::ParentPath(path).c_str(),&startup,&info)!=FALSE,"start translator");
        process.Reset(info.hProcess);CloseHandle(info.hThread);pid=info.dwProcessId;
        auto channel=pipe::Connect(endpoint,executable,nullptr,pid,3000);Require(bool(channel),"translator readiness");
    }
    ~Server(){if(process){SetEvent(stop.value);if(WaitForSingleObject(process.value,3000)!=WAIT_OBJECT_0)TerminateProcess(process.value,99);}}
    void Close(){SetEvent(stop.value);Require(WaitForSingleObject(process.value,3000)==WAIT_OBJECT_0,"translator shutdown");DWORD code{};
        Require(GetExitCodeProcess(process.value,&code)&&code==0,"translator exit code");process.Reset();}
};
void PumpUntil(const std::function<bool()>& ready){
    const auto deadline=GetTickCount64()+3000;
    while(!ready()&&GetTickCount64()<deadline){MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_INPUTAVAILABLE);}
    Require(ready(),"translation callback/worker deadline");
}
void Async(const Server& server){
    std::atomic<long> leases{},channels{};uint64_t latest{};bool initial{};
    {
        AsyncTranslation client(GetModuleHandleW(nullptr),&leases,&channels,[&](TranslationReply reply){
            Require(reply.result.status==0&&reply.result.view.status==0,"unconfigured async result");
            if(reply.update.session==700)latest=std::max(latest,reply.update.revision);else initial=true;
        },{server.endpoint,server.executable,server.pid});
        PumpUntil([&]{return initial;});Require(!client.Enabled(),"unconfigured client mode");
        for(uint64_t i=1;i<=1000;++i)Require(client.Submit({translation::Observe,700,i,"固定虚构文本"}),"async submit");
        PumpUntil([&]{return latest==1000;});
        const auto start=GetTickCount64();client.Close();Require(GetTickCount64()-start<100,"translation UI close blocked");
    }
    PumpUntil([&]{return leases==0&&channels==0;});Require(AsyncTranslation::Unregister(GetModuleHandleW(nullptr)),"translation reply class leak");
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    try{
        Server server(argv[1]);
        for(unsigned i=0;i<32;++i){auto probe=pipe::Connect(server.endpoint,server.executable,nullptr,server.pid);Require(bool(probe),"translator worker lost after disconnected probe");}
        auto response=translation::Query(server.endpoint,server.executable,nullptr,translation::ReadState,1,1,1,{},server.pid);
        Require(response.status==0&&response.view.status==0&&!response.view.enabled&&response.view.output.empty(),"unconfigured mode");
        response=translation::Query(server.endpoint,server.executable,nullptr,translation::Observe,1,2,2,"你好",server.pid);
        Require(response.status==0&&response.view.status==0,"unconfigured observation");
        response=translation::Query(server.endpoint,server.executable,nullptr,translation::Observe,1,3,3,std::string(1,'\xff'),server.pid);
        Require(response.status!=0,"invalid UTF-8 accepted");
        response=translation::Query(server.endpoint,server.executable,nullptr,translation::ReadState,1,3,4,{},server.pid+1);
        Require(response.status!=0,"wrong server PID accepted");
        std::atomic<unsigned> passed{};std::vector<std::thread> clients;
        for(unsigned i=0;i<4;++i)clients.emplace_back([&,i]{for(uint64_t revision=1;revision<=80;++revision){
            const auto value=translation::Query(server.endpoint,server.executable,nullptr,translation::Observe,100+i,revision,revision,"固定虚构文本",server.pid);
            if(value.status==0&&value.view.status==0&&value.view.output.empty())++passed;
        }});
        for(auto& client:clients)client.join();Require(passed==320,"concurrent translator IPC");
        Async(server);server.Close();std::puts("translator process: current-logon IPC, no-key mode, invalid data, verified peer, 320 concurrent queries, 1000 coalesced observations, nonblocking UI close, clean exit");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"translator process: %s\n",error.what());return 1;}
}
