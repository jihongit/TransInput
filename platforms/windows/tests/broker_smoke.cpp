// SPDX-License-Identifier: AGPL-3.0-only
#include "../shared/client.h"
#include "../shared/learning_control.h"
#include "../tsf/async_broker.h"
#include <atomic>
#include <cstdio>
#include <thread>
namespace {
using namespace transinput;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string Text(const TiText& text){Require(text.length<=512,"text length");return {reinterpret_cast<const char*>(text.bytes),text.length};}
struct Server {
    pipe::Handle process,stop;
    std::wstring executable,endpoint;
    DWORD pid{};
    Server(const std::wstring& path,DWORD stall=0,DWORD warmup=0,DWORD learning_delay=0,const std::wstring& learning_path={}):executable(path){
        const auto id=std::to_wstring(GetCurrentProcessId())+L"a"+std::to_wstring(GetTickCount64())+(stall?L"b":L"a");
        endpoint=pipe::Endpoint(id);const auto stop_name=L"Local\\TransInput.BrokerTest."+id;
        pipe::Security security;stop.Reset(CreateEventW(&security.attributes,TRUE,FALSE,stop_name.c_str()));Require(bool(stop),"test stop event");
        pipe::Handle loading;const auto loading_name=L"Local\\TransInput.LearningTest."+id;
        if(learning_delay){loading.Reset(CreateEventW(&security.attributes,TRUE,FALSE,loading_name.c_str()));Require(bool(loading),"learning-ready event");}
        auto command=L"\""+path+L"\" --test-id="+id+L" --parent="+std::to_wstring(GetCurrentProcessId())+
            L" --stop-event="+stop_name+L" --stall-ms="+std::to_wstring(stall)+L" --warmup-ms="+std::to_wstring(warmup)+L" --idle-ms=10000";
        if(learning_delay)command+=L" --learning-load-ms="+std::to_wstring(learning_delay)+L" --learning-load-event="+loading_name;
        if(!learning_path.empty())command+=L" --learning-path=\""+learning_path+L"\"";
        STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        Require(CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,
            pipe::ParentPath(path).c_str(),&startup,&info)!=FALSE,"start owned broker");
        process.Reset(info.hProcess);CloseHandle(info.hThread);pid=info.dwProcessId;
        auto channel=pipe::Connect(endpoint,executable,nullptr,pid,3000);Require(bool(channel),"owned broker did not become ready");
        if(learning_delay)Require(WaitForSingleObject(loading.value,3000)==WAIT_OBJECT_0,"query workers not ready before profile load");
    }
    ~Server(){
        if(process){SetEvent(stop.value);if(WaitForSingleObject(process.value,3000)!=WAIT_OBJECT_0)TerminateProcess(process.value,99);}
    }
    void Close(){
        SetEvent(stop.value);Require(WaitForSingleObject(process.value,3000)==WAIT_OBJECT_0,"broker shutdown timeout");
        DWORD code{};Require(GetExitCodeProcess(process.value,&code)&&code==0,"broker did not drain engine/cache");process.Reset();
    }
};
struct ControlFiles {
    std::wstring root,path;
    ControlFiles(){
        wchar_t temporary[MAX_PATH]{};const auto size=GetTempPathW(MAX_PATH,temporary);Require(size>0&&size<MAX_PATH,"control temp directory");
        root=std::wstring(temporary)+L"TransInputControl-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        pipe::Security security;Require(CreateDirectoryW(root.c_str(),&security.attributes)!=FALSE,"control fixture directory");path=root+L"\\fixed.bin";
    }
    ~ControlFiles(){DeleteFileW(path.c_str());DeleteFileW((path+L".settings").c_str());RemoveDirectoryW(root.c_str());}
};
void Controls(const std::wstring& executable){
    using namespace learning_control;ControlFiles files;
    {
        Server server(executable,0,0,0,files.path);
        auto state=Query(server.endpoint,server.executable,nullptr,wire::LearningStatus,0,false,server.pid);
        const auto deadline=GetTickCount64()+2000;
        while(!state.status&&state.view.health==0&&GetTickCount64()<deadline){Sleep(10);state=Query(server.endpoint,server.executable,nullptr,wire::LearningStatus,0,false,server.pid);}
        Require(!state.status&&state.view.enabled==1&&state.view.count==0&&state.view.health==2,"initial control IPC status");
        Require(!RecordSelection(server.endpoint,server.executable,nullptr,8,1,500,"bj","北京",server.pid).status,"control fixture record");
        state=Change(server.endpoint,server.executable,nullptr,wire::LearningEnable,10,false,server.pid);
        Require(!state.status&&state.view.state==Succeeded&&state.view.enabled==0,"disable control IPC");
        Require(!RecordSelection(server.endpoint,server.executable,nullptr,8,1,501,"bj","比较",server.pid).status,"disabled record reply");
        auto candidate=QueryBroker(server.endpoint,server.executable,nullptr,8,1,502,"bj",2,server.pid);
        Require(!candidate.status&&Text(candidate.snapshot.candidates[0])=="北京","disabled control removed or changed saved rank");
        state=Change(server.endpoint,server.executable,nullptr,wire::LearningClear,11,false,server.pid);
        Require(!state.status&&state.view.state==Succeeded&&state.view.count==0,"clear control IPC");
        state=Change(server.endpoint,server.executable,nullptr,wire::LearningEnable,12,true,server.pid);
        Require(!state.status&&state.view.state==Succeeded&&state.view.enabled==1,"enable control IPC");
        Require(!RecordSelection(server.endpoint,server.executable,nullptr,8,1,503,"bj","北京",server.pid).status,"new record after clear");
        state=Query(server.endpoint,server.executable,nullptr,wire::LearningClear,11,false,server.pid);
        Require(!state.status&&state.view.state==Succeeded&&state.view.count==1,"duplicate clear erased a newer record");
        state=Query(server.endpoint,server.executable,nullptr,wire::LearningClear,12,false,server.pid);
        Require(!state.status&&state.view.state==Failed&&state.view.error==ERROR_INVALID_DATA,"control identity accepted a different operation");
        server.Close();
    }
    {
        Server restored(executable,0,0,0,files.path);auto state=Query(restored.endpoint,restored.executable,nullptr,wire::LearningStatus,0,false,restored.pid);
        const auto deadline=GetTickCount64()+2000;while(!state.status&&state.view.health==0&&GetTickCount64()<deadline){Sleep(10);state=Query(restored.endpoint,restored.executable,nullptr,wire::LearningStatus,0,false,restored.pid);}
        Require(!state.status&&state.view.count==1&&state.view.enabled==1,"clear/relearn restart state");
        state=Change(restored.endpoint,restored.executable,nullptr,wire::LearningEnable,13,false,restored.pid);
        Require(!state.status&&state.view.state==Succeeded,"persistent disable");restored.Close();
    }
    {
        Server restored(executable,0,0,0,files.path);auto state=Query(restored.endpoint,restored.executable,nullptr,wire::LearningStatus,0,false,restored.pid);
        const auto deadline=GetTickCount64()+2000;while(!state.status&&state.view.health==0&&GetTickCount64()<deadline){Sleep(10);state=Query(restored.endpoint,restored.executable,nullptr,wire::LearningStatus,0,false,restored.pid);}
        Require(!state.status&&state.view.count==1&&state.view.enabled==0,"disabled preference failed restart");restored.Close();
    }
}
void Codec(){
    auto bytes=wire::EncodeHeader({wire::Query,8,0x123456789abcdef0ULL,77,88});
    const auto header=wire::DecodeHeader(bytes);Require(header.session==0x123456789abcdef0ULL&&header.revision==77&&header.request==88,"header roundtrip");
    for(unsigned field:{0u,4u,12u}){
        auto bad=bytes;bad[field]=0xff;bad[field+1]=0xff;bad[field+2]=0xff;bad[field+3]=0xff;
        bool rejected{};try{wire::DecodeHeader(bad);}catch(...){rejected=true;}Require(rejected,"invalid header accepted");
    }
    TiSnapshot snapshot{};snapshot.abi_version=TI_ABI_VERSION;snapshot.struct_size=sizeof snapshot;
    auto encoded=wire::EncodeSnapshot(snapshot);Require(wire::DecodeSnapshot(encoded).candidate_count==0,"snapshot roundtrip");
    encoded.pop_back();bool rejected{};try{wire::DecodeSnapshot(encoded);}catch(...){rejected=true;}Require(rejected,"truncated snapshot");
}
void Pages(const Server& server){
    std::vector<std::string> words;
    const auto legacy=QueryBroker(server.endpoint,server.executable,nullptr,88,1,400,"n",1,server.pid);
    Require(legacy.status==0&&legacy.snapshot.candidate_count==9,"legacy query layout changed");
    for(uint32_t page=0;page<10;++page){
        const auto result=QueryBroker(server.endpoint,server.executable,nullptr,88,page,401+page,"n",1,server.pid,500,page);
        Require(result.status==0&&result.page==page&&result.total==90&&result.snapshot.candidate_count==9,"page query identity or bounds");
        for(uint32_t row=0;row<result.snapshot.candidate_count;++row){
            const auto word=Text(result.snapshot.candidates[row]);
            Require(std::find(words.begin(),words.end(),word)==words.end(),"duplicate across pages");words.push_back(word);
            if(page==0)Require(word==Text(legacy.snapshot.candidates[row]),"first page differs from legacy query");
        }
    }
    Require(QueryBroker(server.endpoint,server.executable,nullptr,88,1,430,"n",1,server.pid,500,10).status==ERROR_INVALID_PARAMETER,"invalid page not rejected");
    const auto empty=QueryBroker(server.endpoint,server.executable,nullptr,88,1,431,"qqqqqqqq",8,server.pid,500,9);
    Require(empty.status==0&&empty.page==0&&empty.total==0&&empty.snapshot.candidate_count==0,"empty page did not clamp");
    Require(RecordSelection(server.endpoint,server.executable,nullptr,88,1,432,"n",words[9],server.pid).status==0,"second-page selection rejected by learning");
    const auto promoted=QueryBroker(server.endpoint,server.executable,nullptr,88,1,433,"n",1,server.pid,500,0);
    Require(promoted.status==0&&Text(promoted.snapshot.candidates[0])==words[9],"off-page preference did not reach first page");
}
void Fragmented(const Server& server){
    auto channel=pipe::Connect(server.endpoint,server.executable,nullptr,server.pid);Require(bool(channel),"fragment connect");
    wire::Writer body;body.U32(3);body.U32(5);const std::string input="nihao";
    body.Data({reinterpret_cast<const uint8_t*>(input.data()),input.size()});
    const auto header=wire::EncodeHeader({wire::Query,static_cast<uint32_t>(body.bytes.size()),5,6,7});
    for(const auto byte:header)Require(pipe::Write(channel.value,nullptr,std::vector<uint8_t>{byte})==0,"fragmented header write");
    for(const auto byte:body.bytes)Require(pipe::Write(channel.value,nullptr,std::vector<uint8_t>{byte})==0,"fragmented body write");
    std::array<uint8_t,wire::HeaderSize> incoming{};Require(pipe::Read(channel.value,nullptr,incoming)==0,"fragment header read");
    const auto response=wire::DecodeHeader(incoming);Require(response.session==5&&response.revision==6&&response.request==7,"response identity");
    std::vector<uint8_t> payload(response.length);Require(pipe::Read(channel.value,nullptr,payload)==0,"fragment body read");
    wire::Reader reader{payload};Require(reader.U32()==0,"fragment status");
    const auto snapshot=wire::DecodeSnapshot(reader.Data(payload.size()-4));
    Require(Text(snapshot.candidates[0])=="你好"&&snapshot.cursor==3&&snapshot.commit.length==0,"fragment candidate/cursor");
    pipe::Write(channel.value,nullptr,std::vector<uint8_t>{6});
}
void PumpUntil(const std::function<bool()>& ready){
    const auto deadline=GetTickCount64()+3000;
    while(!ready()&&GetTickCount64()<deadline){
        MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
        MsgWaitForMultipleObjectsEx(0,nullptr,1,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    Require(ready(),"async callback/teardown deadline");
}
void Async(const Server& server){
    std::atomic<long> leases{},channels{};std::vector<uint64_t> commits;uint64_t latest{};
    {
        AsyncBroker broker(GetModuleHandleW(nullptr),&leases,&channels,[&](BrokerReply reply){
            Require(reply.result.status==0,"async query failed");
            if(reply.job.commit){Require(Text(reply.result.snapshot.candidates[0])=="北京","commit result mixed");commits.push_back(reply.job.request);}
            else {Require(Text(reply.result.snapshot.candidates[0])=="你好","preview result mixed");latest=std::max(latest,reply.job.request);}
        },{server.endpoint,server.executable,server.pid});
        for(uint64_t i=1;i<=100;++i)Require(broker.Submit({1,i,i,"nihao",5,false}),"latest preview submission");
        for(uint64_t i=0;i<8;++i)Require(broker.Submit({2,i,1000+i,"beijing",7,true}),"ordered commit submission");
        PumpUntil([&]{return commits.size()==8&&latest==100;});
        for(size_t i=0;i<8;++i)Require(commits[i]==1000+i,"commit work coalesced or reordered");
    }
    PumpUntil([&]{return leases==0&&channels==0;});
    Require(AsyncBroker::Unregister(GetModuleHandleW(nullptr)),"reply class leak");
}
void StopAsync(const Server& stalled){
    std::atomic<long> leases{},channels{};
    {
        AsyncBroker broker(GetModuleHandleW(nullptr),&leases,&channels,[](BrokerReply){},
            {stalled.endpoint,stalled.executable,stalled.pid});
        unsigned accepted{};for(uint64_t i=1;i<=64;++i)if(broker.Submit({1,i,i,"nihao",5,true}))++accepted;
        Require(accepted<=33,"commit queue unbounded");
        Sleep(10);const auto started=GetTickCount64();broker.Close();
        Require(GetTickCount64()-started<100,"UI close waited on a stalled peer");
    }
    PumpUntil([&]{return leases==0&&channels==0;});
    Require(AsyncBroker::Unregister(GetModuleHandleW(nullptr)),"stalled reply class leak");
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    try{
        Codec();Server server(argv[1]);Fragmented(server);
        for(unsigned i=0;i<32;++i){auto probe=pipe::Connect(server.endpoint,server.executable,nullptr,server.pid);Require(bool(probe),"worker lost after disconnected probe");}
        const auto bad_peer=QueryBroker(server.endpoint,server.executable,nullptr,1,1,1,"nihao",5,server.pid+1);
        Require(bad_peer.status!=0,"unverified peer accepted");
        const auto bad_input=QueryBroker(server.endpoint,server.executable,nullptr,1,1,2,std::string(1,'\xff'),1,server.pid);
        Require(bad_input.status!=0,"invalid UTF-8 accepted");
        std::atomic<unsigned> passed{};std::vector<std::thread> clients;
        for(unsigned i=0;i<6;++i)clients.emplace_back([&,i]{
            for(unsigned round=0;round<80;++round){
                const auto response=QueryBroker(server.endpoint,server.executable,nullptr,i+1,round,round+3,"nihao",5,server.pid);
                if(response.status==0&&Text(response.snapshot.candidates[0])=="你好"&&Text(response.snapshot.preedit)=="nihao")++passed;
            }
        });
        for(auto& client:clients)client.join();Require(passed==480,"concurrent requests lost or mixed");Async(server);Pages(server);
        const auto learned=RecordSelection(server.endpoint,server.executable,nullptr,8,1,100,"bj","北京",server.pid);
        Require(learned.status==0&&learned.connected,"learning IPC record failed");
        const auto personalized=QueryBroker(server.endpoint,server.executable,nullptr,8,2,101,"bj",2,server.pid);
        Require(personalized.status==0&&Text(personalized.snapshot.candidates[0])=="北京","learning did not affect query");
        Require(RecordSelection(server.endpoint,server.executable,nullptr,8,3,102,"bj","English",server.pid).status!=0,"arbitrary learning IPC accepted");
        server.Close();
        Controls(argv[1]);
        {Server warming(argv[1],0,600);Async(warming);warming.Close();}
        {
            Server loading(argv[1],0,0,1000);const auto started=GetTickCount64();
            const auto candidate=QueryBroker(loading.endpoint,loading.executable,nullptr,9,1,201,"nihao",5,loading.pid);
            Require(candidate.status==0&&Text(candidate.snapshot.candidates[0])=="你好"&&GetTickCount64()-started<500,"profile load blocked Chinese readiness");
            auto recorded=RecordSelection(loading.endpoint,loading.executable,nullptr,9,1,202,"bj","北京",loading.pid);
            Require(recorded.connected&&recorded.status!=0,"record during import could overwrite loaded preferences");
            const auto deadline=GetTickCount64()+2000;uint64_t request=203;
            while(recorded.status!=0&&GetTickCount64()<deadline){Sleep(10);recorded=RecordSelection(loading.endpoint,loading.executable,nullptr,9,1,request++,"bj","北京",loading.pid);}
            Require(recorded.status==0,"learning did not become available after profile load");loading.Close();
        }
        Server stalled(argv[1],600);const auto started=GetTickCount64();
        const auto response=QueryBroker(stalled.endpoint,stalled.executable,nullptr,1,1,3,"nihao",5,stalled.pid);
        Require(response.status==ERROR_TIMEOUT&&GetTickCount64()-started<1500,"stalled peer did not time out");StopAsync(stalled);stalled.Close();
        std::puts("PASS: framed IPC, 480 concurrent queries, ordered commits, local learning, slow profile readiness isolation, cold retry, bounded queues, peer checks, cancellation, nonblocking close and clean shutdown");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
