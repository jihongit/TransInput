// SPDX-License-Identifier: AGPL-3.0-only
// TSF integration fixture only: fixed fictional translations, no HTTP or keys.
#include "../translator/protocol.h"
#include <atomic>
#include <thread>
namespace {
using namespace transinput;
struct Current {DWORD process{};uint64_t session{},revision{},started{};std::string source;};
pipe::Handle MakePipe(const std::wstring& name,pipe::Security& security){
    return pipe::Handle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,16384,16384,0,&security.attributes));
}
bool Accept(HANDLE channel,HANDLE stop){
    pipe::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;
    OVERLAPPED operation{};operation.hEvent=event.value;DWORD count{};
    if(ConnectNamedPipe(channel,&operation))return true;const auto error=GetLastError();
    return error==ERROR_PIPE_CONNECTED||(error==ERROR_IO_PENDING&&pipe::Complete(channel,stop,operation,INFINITE,count)==0);
}
void Run(HANDLE channel,HANDLE stop,Current& current,uint32_t& mode,uint32_t& observations){
    std::array<uint8_t,wire::HeaderSize> data{};if(pipe::Read(channel,stop,data)!=0)return;const auto request=wire::DecodeHeader(data);
    if(request.length>804)return;std::vector<uint8_t> body(request.length);if(pipe::Read(channel,stop,body)!=0)return;
    ULONG process{};if(!GetNamedPipeClientProcessId(channel,&process))return;
    if(request.kind==31){if(request.revision>3)return;mode=static_cast<uint32_t>(request.revision);current={};observations=0;}
    else if(request.kind==translation::Observe){
        wire::Reader reader{body};const auto length=reader.U32();if(length>800||body.size()!=4+length)return;const auto text=reader.Data(length);
        current={process,request.session,request.revision,GetTickCount64(),{reinterpret_cast<const char*>(text.data()),text.size()}};++observations;
    }else if(request.kind==translation::Clear){if(current.process==process&&current.session==request.session&&current.revision==request.revision)current={};}
    else if(request.kind==translation::Retry){if(current.process==process&&current.session==request.session&&current.revision==request.revision)mode=1;}
    else if(request.kind!=translation::ReadState&&request.kind!=32&&request.kind!=33&&request.kind!=34)return;
    translation::View view{mode,mode?1u:0u,mode?1u:0u,0,{}};
    if(mode&&current.process==process&&current.session==request.session&&current.revision==request.revision){
        if(mode==2&&GetTickCount64()-current.started<650)view.status=3;
        else if(mode==3){view.status=5;view.failure=2;}
        else {view.status=4;view.output=current.source=="你好"?"Hello":current.source=="北京"?"Beijing":"Fixture translation";}
    }
    if(request.kind==32){view.status=4;view.output=std::to_string(observations);}
    if(request.kind==34){view.status=4;view.output=current.source;}
    wire::Writer response;response.U32(0);response.U64(view.configuration);response.U32(view.enabled);response.U32(view.status);response.U32(view.failure);response.U32(static_cast<uint32_t>(view.output.size()));
    response.Data({reinterpret_cast<const uint8_t*>(view.output.data()),view.output.size()});
    const auto header=wire::EncodeHeader({request.kind|wire::Reply,static_cast<uint32_t>(response.bytes.size()),request.session,request.revision,request.request});
    if(pipe::Write(channel,stop,header)!=0||pipe::Write(channel,stop,response.bytes)!=0)return;
    std::array<uint8_t,1> ack{};pipe::Read(channel,stop,ack,100);
    if(request.kind==33)SetEvent(stop);
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2||!std::wstring(argv[1]).starts_with(L"--parent="))return 2;
    try{
        const auto parent_id=std::stoul(std::wstring(argv[1]).substr(9));pipe::Handle parent(OpenProcess(SYNCHRONIZE,FALSE,parent_id));if(!parent)return 3;
        pipe::Security security;const auto name=translation::Endpoint(std::to_wstring(parent_id)+L"f");auto channel=MakePipe(name,security);if(!channel)return 4;
        pipe::Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));std::atomic<ULONGLONG> activity{GetTickCount64()};
        std::thread worker([&]{Current current;uint32_t mode{},observations{};
            while(WaitForSingleObject(stop.value,0)!=WAIT_OBJECT_0){try{if(Accept(channel.value,stop.value)){activity.store(GetTickCount64());Run(channel.value,stop.value,current,mode,observations);}}catch(...){ }
                DisconnectNamedPipe(channel.value);
            }
        });
        const HANDLE exits[]{parent.value,stop.value};
        while(WaitForMultipleObjects(2,exits,FALSE,100)==WAIT_TIMEOUT&&GetTickCount64()-activity.load()<15000){}
        SetEvent(stop.value);worker.join();return 0;
    }catch(...){return 1;}
}
