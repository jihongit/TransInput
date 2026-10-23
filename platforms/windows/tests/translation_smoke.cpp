// SPDX-License-Identifier: AGPL-3.0-only
#include <winsock2.h>
#include <ws2tcpip.h>
#include "../translator/core.h"
#include "../translator/http.h"
#include "../translator/settings.h"
#include "../translator/actor.h"
#include <cstdio>
#include <thread>
namespace {
using namespace transinput::translation;
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
constexpr char Reply[]=R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"Hello, how's it going?"}}]})";
enum class Mode {Success,Fragmented,Auth,Limited,Redirect,Invalid,Huge,Truncated,Stall,StallBody};
class Fixture {
    SOCKET listener_{INVALID_SOCKET};
    std::thread worker_;
    std::atomic<bool> stop_{};
    std::atomic<unsigned> connections_{};
    Mode mode_;
    std::string received_;
    static bool Send(SOCKET socket,std::string_view bytes){
        while(!bytes.empty()){
            const auto count=send(socket,bytes.data(),static_cast<int>(bytes.size()),0);
            if(count<=0)return false;bytes.remove_prefix(count);
        }return true;
    }
    void Run(SOCKET listener)noexcept {
        while(!stop_.load()){
        const auto client=accept(listener,nullptr,nullptr);if(client==INVALID_SOCKET)return;
        ++connections_;
        DWORD timeout=1000;setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof timeout);
        try{
            received_.clear();std::array<char,4096> buffer{};size_t wanted=1;
            while(received_.size()<wanted&&received_.size()<16384){
                const auto count=recv(client,buffer.data(),static_cast<int>(buffer.size()),0);if(count<=0)break;
                received_.append(buffer.data(),count);
                const auto end=received_.find("\r\n\r\n");
                if(end==std::string::npos){wanted=received_.size()+1;continue;}
                const auto field=received_.find("Content-Length:");
                if(field==std::string::npos)break;
                wanted=end+4+std::stoul(received_.substr(field+15));
            }
            if(mode_==Mode::Stall){while(!stop_.load())Sleep(2);}
            else {
                const auto body=mode_==Mode::Invalid?std::string("invalid JSON"):mode_==Mode::Huge?std::string(MaxBody+1,' '):std::string(Reply);
                const auto status=mode_==Mode::Auth?"401 Unauthorized":mode_==Mode::Limited?"429 Too Many Requests":mode_==Mode::Redirect?"307 Temporary Redirect":"200 OK";
                const auto header=std::string("HTTP/1.1 ")+status+"\r\nContent-Type: application/json\r\nConnection: close\r\nLocation: http://127.0.0.1:"+std::to_string(port)+"/not-allowed\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n";
                if(Send(client,header)){
                    if(mode_==Mode::StallBody){while(!stop_.load())Sleep(2);}
                    else if(mode_==Mode::Fragmented){for(size_t offset=0;offset<body.size();offset+=4){if(!Send(client,std::string_view(body).substr(offset,4)))break;Sleep(1);}}
                    else if(mode_==Mode::Truncated)Send(client,std::string_view(body).substr(0,12));
                    else Send(client,body);
                }
            }
        }catch(...){ }
        shutdown(client,SD_BOTH);closesocket(client);
        }
    }
public:
    INTERNET_PORT port{};
    explicit Fixture(Mode mode):mode_(mode){
        listener_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(listener_==INVALID_SOCKET)throw std::runtime_error("fixture socket");
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if(bind(listener_,reinterpret_cast<sockaddr*>(&address),sizeof address)==SOCKET_ERROR||listen(listener_,1)==SOCKET_ERROR){closesocket(listener_);throw std::runtime_error("fixture bind");}
        int length=sizeof address;getsockname(listener_,reinterpret_cast<sockaddr*>(&address),&length);port=ntohs(address.sin_port);
        worker_=std::thread([this,listener=listener_]{Run(listener);});
    }
    void Stop(){stop_.store(true);if(listener_!=INVALID_SOCKET){closesocket(std::exchange(listener_,INVALID_SOCKET));}if(worker_.joinable())worker_.join();}
    ~Fixture(){Stop();}
    unsigned Count()const{return connections_.load();}
    std::string Body(){Stop();const auto end=received_.find("\r\n\r\n");Require(end!=std::string::npos,"fixture incomplete request");return received_.substr(end+4);}
};
HttpResult RoundTrip(std::string_view prompt,Mode mode,bool cancel=false,DWORD budget=1500){
    Fixture fixture(mode);auto session=std::make_shared<HttpSession>(fixture.port);Http http(session,prompt,"你好，你最近怎么样？","fixture-key",budget);
    const auto start=GetTickCount64();
    while(!http.Poll(cancel&&GetTickCount64()-start>30)){
        Require(GetTickCount64()-start<budget+1500,"HTTP completion or cancellation acknowledgement hung");Sleep(2);
    }
    const auto result=http.Result();
    if(mode==Mode::Redirect)Require(fixture.Count()==1,"redirect repeated request with credentials");
    if(result.failure)std::printf("fixture mode=%u failure=%lu elapsed=%llu\n",static_cast<unsigned>(mode),static_cast<unsigned long>(result.failure),static_cast<unsigned long long>(GetTickCount64()-start));
    if(mode==Mode::Success){
        const auto body=fixture.Body();const auto request=json::JsonObject::Parse(winrt::to_hstring(body));
        const auto messages=request.GetNamedArray(L"messages");Require(messages.Size()==2,"conversation history leaked");
        Require(messages.GetObjectAt(0).GetNamedString(L"role")==L"system"&&messages.GetObjectAt(0).GetNamedString(L"content")==winrt::to_hstring(prompt),"system prompt differs");
        Require(messages.GetObjectAt(1).GetNamedString(L"role")==L"user"&&messages.GetObjectAt(1).GetNamedString(L"content")==L"你好，你最近怎么样？","wrong source message");
        Require(request.GetNamedString(L"model")==Model&&request.GetNamedObject(L"thinking").GetNamedString(L"type")==L"disabled"&&!request.GetNamedBoolean(L"stream"),"model or thinking mode");
        Require(!request.HasKey(L"tools")&&!request.HasKey(L"user_id"),"unnecessary remote metadata");
    }
    return result;
}
void JsonChecks(std::string_view prompt){
    const auto source="中文 \"quote\" \\path\n下一行\t🙂";
    const auto request=json::JsonObject::Parse(winrt::to_hstring(RequestBody(prompt,source)));
    Require(request.GetNamedArray(L"messages").GetObjectAt(1).GetNamedString(L"content")==winrt::to_hstring(source),"JSON escaping changed source");
    Require(ParseReply(Reply)=="Hello, how's it going?","reply parse");
    for(const auto body:{"{}","not JSON",R"({"choices":[]})",
        R"({"choices":[{"index":0,"finish_reason":"length","message":{"role":"assistant","content":"cut off"}}]})",
        R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"hello","tool_calls":[{}]}}]})",
        R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"hello","reasoning_content":"thinking"}}]})",
        R"({"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"bad\u0000text"}}]})"}){
        bool rejected{};try{ParseReply(body);}catch(...){rejected=true;}Require(rejected,"malformed response accepted");
    }
}
void AbiChecks(Core& core){
    Require(core.State().status==0,"new actor enabled without configuration");
    Require(core.shutdown()==-4,"unload ignored active translator");
    Require(core.configure(core.handle,7,1)==0,"configure");
    const auto source=reinterpret_cast<const uint8_t*>("你好");
    Require(core.observe(core.handle,99,1,source,6,0)==0,"observe");
    Require(core.State(false,1000).job==0,"getter consumed work");
    auto job=core.State(true,160);Require(job.job!=0&&job.session==99&&job.source_length==6,"poll and C/Rust layout");
    Require(core.configure(core.handle,8,0)==0&&core.State().cancel_active==1,"disable did not request cancellation");
    Require(core.configure(core.handle,9,1)==0&&core.observe(core.handle,100,1,source,6,200)==0,"reconfigure");
    Require(core.State(true,1000).job==0,"old network slot released without acknowledgement");
    Require(core.finish(core.handle,job.job,nullptr,0,1)==0,"cancel acknowledgement");
    job=core.State(true,1000);Require(job.job!=0&&job.session==100,"new job after cancellation");
    Require(core.finish(core.handle,job.job,reinterpret_cast<const uint8_t*>("Hello"),5,0)==0,"finish");
    Require(core.State().status==4&&core.State().output_length==5,"translation result");
    Require(core.clear(core.handle,99,1)==0&&core.State().status==4,"old focus cleared new result");
    Require(core.clear(core.handle,100,1)==0&&core.State().status==1,"clear current context");
}
void SettingsChecks(){
    const auto path=transinput::pipe::ParentPath(transinput::pipe::ModulePath())+L"\\settings-fixture-"+std::to_wstring(GetCurrentProcessId())+L".bin";
    // Synthetic credential only; no user settings directory or real key is read.
    Require(!LoadSettings(path).enabled,"missing config enabled translation");
    const auto identity=SaveSettings(path,true,"fixture-key-never-a-real-key");
    auto loaded=LoadSettings(path);Require(loaded.enabled&&loaded.identity==identity&&loaded.key.View()=="fixture-key-never-a-real-key","DPAPI round trip");
    {
        transinput::pipe::Handle encrypted(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
        std::array<char,2048> bytes{};DWORD count{};Require(encrypted&&ReadFile(encrypted.value,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr),"encrypted fixture read");
        Require(std::string_view(bytes.data(),count).find("fixture-key-never-a-real-key")==std::string_view::npos,"key written in plaintext");
    }
    const auto next=SaveSettings(path,false,loaded.key.View());
    loaded=LoadSettings(path);Require(!loaded.enabled&&next!=identity,"save configuration invalidation");
    bool rejected{};try{SaveSettings(path,true,"bad\r\nheader");}catch(...){rejected=true;}Require(rejected,"unsafe key accepted");
    Require(LoadSettings(path).identity==next,"failed save changed configuration");
    transinput::pipe::Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,0,nullptr));
    DWORD count{};const uint8_t bad=255;Require(file&&WriteFile(file.value,&bad,1,&count,nullptr),"corrupt fixture write");file.Reset();
    rejected=false;try{LoadSettings(path);}catch(...){rejected=true;}Require(rejected,"corrupt config silently accepted");
    Require(DeleteFileW(path.c_str())!=FALSE,"settings fixture cleanup");
}
void ActorChecks(const std::wstring& core_path){
    const auto path=transinput::pipe::ParentPath(transinput::pipe::ModulePath())+L"\\actor-fixture-"+std::to_wstring(GetCurrentProcessId())+L".bin";
    SaveSettings(path,true,"fixture-key-never-a-real-key");
    {
        Fixture fixture(Mode::Success);Actor actor(core_path,path,[&]{return std::make_shared<HttpSession>(fixture.port);});actor.Tick();
        const Client first{111,100,1},second{222,200,1};
        actor.Observe(first,1,"你好");
        const auto wait=[&](const Client& client,uint64_t revision,uint32_t status){
            const auto deadline=GetTickCount64()+2000;
            while(actor.State(client,revision).status!=status){actor.Tick();Require(GetTickCount64()<deadline,"actor state deadline");Sleep(2);}
        };
        wait(first,1,4);Require(actor.State(first,1).output=="Hello, how's it going?","actor HTTP result");
        Require(actor.State(second,1).output.empty(),"different process observed another translation");
        const auto before=fixture.Count();actor.Observe(second,1,"你好");actor.Clear(first,1);wait(second,1,4);
        Require(fixture.Count()==before&&actor.State(first,1).output.empty(),"context isolation or cache reuse failed");
        for(uint64_t i=2;i<=1000;++i)actor.Observe(second,i,"测试"+std::to_string(i));
        actor.Observe(second,998,"过期的候选"); // lower revision cannot replace latest
        wait(second,1000,4);Require(fixture.Count()==before+1,"latest pending work was not coalesced");
        Require(actor.State(second,998).output.empty(),"old revision returned current text");
        actor.Clear(second,1000);actor.Observe(second,1000,"重复请求");
        for(unsigned i=0;i<20;++i){actor.Tick();Sleep(1);}
        Require(actor.State(second,1000).status==1&&fixture.Count()==before+1,"cleared revision resurrected");
        SaveSettings(path,false,"fixture-key-never-a-real-key");actor.Reload();actor.Tick();
        actor.Observe(second,1001,"中文模式");
        for(unsigned i=0;i<20;++i){actor.Tick();Sleep(1);}
        Require(actor.State(second,1001).status==0&&fixture.Count()==before+1,"disabled actor sent a request");
    }
    Require(DeleteFileW(path.c_str())!=FALSE,"actor fixture cleanup");
}
void GateChecks(){
    using namespace transinput;
    const auto id=std::to_wstring(GetCurrentProcessId())+L"a";translation::RequestGate first(id);Require(first.TryAcquire(),"initial request gate");
    std::atomic<bool> acquired{};
    {std::thread other([&]{translation::RequestGate second(id);acquired=second.TryAcquire();});other.join();}
    Require(!acquired.load(),"parallel request slot admitted");first.Release();
    {std::thread other([&]{translation::RequestGate second(id);acquired=second.TryAcquire();});other.join();}
    Require(acquired.load(),"request slot not released");
    // Simulate natural worker failure while holding the mutex, without killing
    // a process/thread or touching the production logon's request object.
    {std::thread abandoned([&]{pipe::Security security;pipe::Handle mutex(CreateMutexW(&security.attributes,FALSE,translation::RequestGate::Name(id).c_str()));
        if(mutex)WaitForSingleObject(mutex.value,0);});abandoned.join();}
    Require(first.TryAcquire(),"abandoned request slot not recovered");first.Release();
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    WSADATA winsock{};if(WSAStartup(MAKEWORD(2,2),&winsock)!=0)return 3;
    try{
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        {
            Core core(argv[1]);const auto prompt=core.Prompt();
            Require(prompt.find("American English")!=std::string::npos,"prompt missing American style");
            AbiChecks(core);JsonChecks(prompt);SettingsChecks();GateChecks();
            Require(RoundTrip(prompt,Mode::Success).failure==0,"HTTP success");
            Require(RoundTrip(prompt,Mode::Fragmented).failure==0,"fragmented HTTP");
            Require(RoundTrip(prompt,Mode::Auth).failure==3,"authentication classification");
            Require(RoundTrip(prompt,Mode::Limited).failure==4,"rate classification");
            Require(RoundTrip(prompt,Mode::Redirect).failure!=0,"redirect followed");
            Require(RoundTrip(prompt,Mode::Invalid).failure==5,"invalid JSON classification");
            Require(RoundTrip(prompt,Mode::Huge).failure==5,"oversized HTTP accepted");
            Require(RoundTrip(prompt,Mode::Truncated).failure!=0,"truncated HTTP accepted");
            Require(RoundTrip(prompt,Mode::Stall,false,200).failure==2,"deadline not enforced");
            Require(RoundTrip(prompt,Mode::StallBody,false,200).failure==2,"body deadline not enforced");
            const auto cancel=GetTickCount64();Require(RoundTrip(prompt,Mode::Stall,true).failure==1,"cancel failed");
            Require(GetTickCount64()-cancel<500,"cancel acknowledgement too slow");
            DWORD before{},after{};
            Fixture fixture(Mode::Success);auto session=std::make_shared<HttpSession>(fixture.port);
            // The long-lived actor reuses one HTTPS session. Messages stay independent.
            const auto run=[&]{Http http(session,prompt,"你好","fixture-key");const auto deadline=GetTickCount64()+3000;
                while(!http.Poll()){Require(GetTickCount64()<deadline,"reused HTTP hung");Sleep(2);}Require(http.Result().failure==0,"repeated lifecycle");};
            run();GetProcessHandleCount(GetCurrentProcess(),&before);
            for(unsigned i=0;i<40;++i)run();
            GetProcessHandleCount(GetCurrentProcess(),&after);
            std::printf("HTTP handles immediate %lu -> %lu\n",before,after);
            const auto settled=GetTickCount64()+1000;
            while(after>before+8&&GetTickCount64()<settled){Sleep(10);GetProcessHandleCount(GetCurrentProcess(),&after);}
            std::printf("HTTP handles settled %lu -> %lu\n",before,after);
            Require(after<=before+8,"HTTP handles grew");
            std::printf("translation: ABI, stateless JSON, 11 network cases, 40 lifecycles; handles %lu -> %lu\n",before,after);
        }
        ActorChecks(argv[1]);
        winrt::uninit_apartment();WSACleanup();return 0;
    }catch(const winrt::hresult_error& error){std::fprintf(stderr,"translation HRESULT 0x%08lx\n",static_cast<unsigned long>(error.code().value));}
    catch(const std::exception& error){std::fprintf(stderr,"translation: %s\n",error.what());}
    WSACleanup();return 1;
}
