// SPDX-License-Identifier: AGPL-3.0-only
// This process owns HTTPS and credentials. It never handles keyboard/TSF objects.
#include "actor.h"
#include "protocol.h"
#include <bit>
#include <cstdio>
#include <thread>
namespace {
using namespace transinput;
constexpr unsigned WorkerCount=4;
pipe::Handle MakePipe(const std::wstring& name,pipe::Security& security,bool first=false){
    return pipe::Handle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|(first?FILE_FLAG_FIRST_PIPE_INSTANCE:0),
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,WorkerCount,16384,16384,0,&security.attributes));
}
bool Accept(HANDLE channel,HANDLE stop){
    pipe::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;
    OVERLAPPED operation{};operation.hEvent=event.value;DWORD count{};
    if(ConnectNamedPipe(channel,&operation))return true;
    const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return true;
    return error==ERROR_IO_PENDING&&pipe::Complete(channel,stop,operation,INFINITE,count)==ERROR_SUCCESS;
}
translation::Client Client(HANDLE channel,uint64_t session){
    ULONG pid{};if(!GetNamedPipeClientProcessId(channel,&pid))throw std::runtime_error("translation peer");
    pipe::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));FILETIME created{},exited{},kernel{},user{};
    if(!process||!GetProcessTimes(process.value,&created,&exited,&kernel,&user))throw std::runtime_error("translation peer identity");
    return {pid,(static_cast<uint64_t>(created.dwHighDateTime)<<32)|created.dwLowDateTime,session};
}
void Serve(translation::Actor& actor,HANDLE channel,HANDLE stop,std::atomic<ULONGLONG>& activity){
    std::array<uint8_t,wire::HeaderSize> bytes{};if(pipe::Read(channel,stop,bytes)!=ERROR_SUCCESS)return;
    const auto request=wire::DecodeHeader(bytes);
    if(request.kind<translation::Observe||request.kind>translation::Retry||request.length>TI_TRANSLATION_SOURCE+4)return;
    if((request.kind==translation::Observe&&request.length<4)||(request.kind!=translation::Observe&&request.length!=0))return;
    std::vector<uint8_t> body(request.length);if(pipe::Read(channel,stop,body)!=ERROR_SUCCESS)return;
    activity.store(GetTickCount64());const auto client=Client(channel,request.session);
    int32_t status{};translation::View view;
    try{
        if(request.kind==translation::Observe){
            wire::Reader reader{body};const auto length=reader.U32();if(length>TI_TRANSLATION_SOURCE||request.length!=4+length)return;
            const auto text=reader.Data(length);view=actor.Observe(client,request.revision,{reinterpret_cast<const char*>(text.data()),text.size()});
        }else if(request.kind==translation::Clear)view=actor.Clear(client,request.revision);
        else if(request.kind==translation::Retry)view=actor.Retry(client,request.revision);
        else {if(request.kind==translation::Reload)actor.Reload();view=actor.State(client,request.revision);}
    }catch(...){status=-1;}
    wire::Writer response;response.U32(std::bit_cast<uint32_t>(status));
    if(!status){response.U64(view.configuration);response.U32(view.enabled);response.U32(view.status);response.U32(view.failure);response.U32(static_cast<uint32_t>(view.output.size()));
        response.Data({reinterpret_cast<const uint8_t*>(view.output.data()),view.output.size()});}
    const auto header=wire::EncodeHeader({request.kind|wire::Reply,static_cast<uint32_t>(response.bytes.size()),request.session,request.revision,request.request});
    if(pipe::Write(channel,stop,header)!=ERROR_SUCCESS||pipe::Write(channel,stop,response.bytes)!=ERROR_SUCCESS)return;
    std::array<uint8_t,1> ack{};pipe::Read(channel,stop,ack,100);
}
struct Threads {
    HANDLE stop;std::vector<std::thread> workers;
    void Close(){SetEvent(stop);for(auto& worker:workers)if(worker.joinable())worker.join();workers.clear();}
    ~Threads(){Close();}
};
}
int wmain(int argc,wchar_t** argv){
    try{
        DWORD parent_id{},idle_ms=15000;std::wstring test_id,stop_name,settings_path;
        for(int i=1;i<argc;++i){const std::wstring arg=argv[i];
            if(arg.starts_with(L"--test-id="))test_id=arg.substr(10);
            else if(arg.starts_with(L"--parent="))parent_id=std::stoul(arg.substr(9));
            else if(arg.starts_with(L"--stop-event="))stop_name=arg.substr(13);
            else if(arg.starts_with(L"--settings-path="))settings_path=arg.substr(16);
            else if(arg.starts_with(L"--idle-ms="))idle_ms=std::stoul(arg.substr(10));else return 2;
        }
        if(idle_ms<100||idle_ms>60000||((!stop_name.empty()||!settings_path.empty())&&(!parent_id||test_id.empty())))return 2;
        if(settings_path.empty())settings_path=translation::SettingsPath();
        const auto endpoint=translation::Endpoint(test_id);pipe::Security security;
        auto first=MakePipe(endpoint,security,true);if(!first)return 3;
        pipe::Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!stop)return 4;
        pipe::Handle parent(parent_id?OpenProcess(SYNCHRONIZE,FALSE,parent_id):INVALID_HANDLE_VALUE);
        pipe::Handle test_stop(stop_name.empty()?INVALID_HANDLE_VALUE:OpenEventW(SYNCHRONIZE,FALSE,stop_name.c_str()));
        if((parent_id&&!parent)||(!stop_name.empty()&&!test_stop))return 4;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        {
            translation::Actor actor(pipe::ParentPath(pipe::ModulePath())+L"\\transinput_core.dll",settings_path);
            actor.Tick();std::atomic<ULONGLONG> activity{GetTickCount64()};Threads threads{stop.value,{}};
            const auto run=[&](pipe::Handle initial){auto channel=std::move(initial);
                while(WaitForSingleObject(stop.value,0)!=WAIT_OBJECT_0){
                    try{if(!channel)channel=MakePipe(endpoint,security);if(!channel)break;if(Accept(channel.value,stop.value))Serve(actor,channel.value,stop.value,activity);}catch(...){ }
                    if(channel)DisconnectNamedPipe(channel.value);channel.Reset();
                }
            };
            threads.workers.emplace_back(run,std::move(first));for(unsigned i=1;i<WorkerCount;++i)threads.workers.emplace_back(run,pipe::Handle{});
            HANDLE exits[]{stop.value,nullptr,nullptr};DWORD count=1;
            if(parent)exits[count++]=parent.value;if(test_stop)exits[count++]=test_stop.value;
            while(WaitForMultipleObjects(count,exits,FALSE,10)==WAIT_TIMEOUT){actor.Tick();if(GetTickCount64()-activity.load()>=idle_ms)break;}
            threads.Close();
        }
        winrt::uninit_apartment();return 0;
    }catch(const winrt::hresult_error& error){std::fprintf(stderr,"translator HRESULT 0x%08lx\n",static_cast<unsigned long>(error.code().value));}
    catch(const std::exception& error){std::fprintf(stderr,"translator: %s\n",error.what());}
    return 1;
}
