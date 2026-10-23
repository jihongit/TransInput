// SPDX-License-Identifier: AGPL-3.0-only
// Windows SDK transport host only; dictionary and candidate logic live in Rust.
#include "../shared/pipe.h"
#include "../shared/wire.h"
#include "learning_store.h"
#include "user_words_store.h"
#include <atomic>
#include <bit>
#include <cstdio>
#include <thread>

namespace {
using namespace transinput;
constexpr unsigned WorkerCount=8;
struct Core {
    HMODULE module{};
    decltype(&ti_engine_create) create{};
    decltype(&ti_engine_destroy) destroy{};
    decltype(&ti_engine_query) query{};
    decltype(&ti_engine_page) page{};
    decltype(&ti_shutdown) shutdown{};
    std::unique_ptr<LearningStore> learning;
    std::unique_ptr<UserWordsStore> words;
    Core(std::wstring learning_path,std::wstring words_path,bool user_path){
        const auto path=pipe::ParentPath(pipe::ModulePath())+L"\\transinput_core.dll";
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module)throw std::runtime_error("load core");
        create=std::bit_cast<decltype(create)>(GetProcAddress(module,"ti_engine_create"));
        destroy=std::bit_cast<decltype(destroy)>(GetProcAddress(module,"ti_engine_destroy"));
        query=std::bit_cast<decltype(query)>(GetProcAddress(module,"ti_engine_query"));
        page=std::bit_cast<decltype(page)>(GetProcAddress(module,"ti_engine_page"));
        shutdown=std::bit_cast<decltype(shutdown)>(GetProcAddress(module,"ti_shutdown"));
        const auto version=std::bit_cast<decltype(&ti_abi_version)>(GetProcAddress(module,"ti_abi_version"));
        const auto size=std::bit_cast<decltype(&ti_snapshot_size)>(GetProcAddress(module,"ti_snapshot_size"));
        if(!create||!destroy||!query||!page||!shutdown||!version||!size||version()!=TI_ABI_VERSION||size()!=sizeof(TiSnapshot)){
            FreeLibrary(module);module=nullptr;throw std::runtime_error("core ABI");
        }
        const auto warm=create();if(!warm){FreeLibrary(module);module=nullptr;throw std::runtime_error("core initialization");}
        destroy(warm);
        learning=std::make_unique<LearningStore>(module,std::move(learning_path),true,user_path);
        words=std::make_unique<UserWordsStore>(module,std::move(words_path),user_path,true);
    }
    bool Close()noexcept {if(!module)return true;if(learning){learning->Flush(true);learning.reset();}words.reset();if(shutdown()!=0)return false;const auto old=std::exchange(module,nullptr);return FreeLibrary(old)!=FALSE;}
    ~Core(){Close();}
};
pipe::Handle MakePipe(const std::wstring& name,pipe::Security& security,bool first=false){
    return pipe::Handle(CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|(first?FILE_FLAG_FIRST_PIPE_INSTANCE:0),
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,WorkerCount,16384,16384,0,&security.attributes));
}
bool Accept(HANDLE channel,HANDLE stop){
    pipe::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return false;
    OVERLAPPED operation{};operation.hEvent=event.value;DWORD transferred{};
    if(ConnectNamedPipe(channel,&operation))return true;
    const auto error=GetLastError();if(error==ERROR_PIPE_CONNECTED)return true;
    return error==ERROR_IO_PENDING&&pipe::Complete(channel,stop,operation,INFINITE,transferred)==ERROR_SUCCESS;
}
void Serve(Core& core,HANDLE channel,HANDLE stop,DWORD stall,std::atomic<ULONGLONG>& activity){
    std::array<uint8_t,wire::HeaderSize> header_bytes{};
    if(pipe::Read(channel,stop,header_bytes)!=ERROR_SUCCESS)return;
    const auto request=wire::DecodeHeader(header_bytes);
    const bool control=request.kind==wire::LearningStatus||request.kind==wire::LearningEnable||request.kind==wire::LearningClear;
    const bool words=request.kind==wire::WordsStatus||request.kind==wire::WordsAdd||request.kind==wire::WordsRemove;
    if(request.kind!=wire::Query&&request.kind!=wire::QueryPage&&request.kind!=wire::Ping&&request.kind!=wire::Learn&&!control&&!words)return;
    if((request.kind==wire::Query&&(request.length<8||request.length>136))||(request.kind==wire::Ping&&request.length!=0)||
        (request.kind==wire::Learn&&(request.length<8||request.length>648))||
        (request.kind==wire::QueryPage&&(request.length<12||request.length>140))||
        (control&&request.length!=(request.kind==wire::LearningEnable?4u:0u))||
        (words&&(request.kind==wire::WordsStatus?request.length!=4:request.length<14||request.length>268)))return;
    std::vector<uint8_t> body(request.length);
    if(pipe::Read(channel,stop,body)!=ERROR_SUCCESS)return;
    activity.store(GetTickCount64());
    if(stall&&WaitForSingleObject(stop,stall)==WAIT_OBJECT_0)return;
    wire::Writer response;int32_t status{};TiSnapshot snapshot{};TiCandidatePage candidate_page{};learning_control::View control_view;
    words_control::View words_view;
    if(request.kind==wire::Query||request.kind==wire::QueryPage){
        wire::Reader reader{body};const auto cursor=reader.U32(),length=reader.U32();
        const auto page=request.kind==wire::QueryPage?reader.U32():0;
        if(length>128||cursor>length||page>=TI_MAX_PAGES||request.length!=(request.kind==wire::QueryPage?12:8)+length)return;
        const auto source=reader.Data(length);
        const auto engine=core.create();if(!engine)return;
        status=core.learning->Attach(engine);
        if(!status)status=core.words->Attach(engine);
        if(!status)status=core.query(engine,source.data(),length,cursor,&snapshot,sizeof snapshot);
        if(!status&&request.kind==wire::QueryPage){status=core.page(engine,page,&candidate_page,sizeof candidate_page);snapshot=candidate_page.snapshot;}
        core.destroy(engine);
    }else if(words){
        wire::Reader reader{body};const auto offset=reader.U32();if(offset>TI_MAX_WORDS||request.revision)return;
        if(request.kind==wire::WordsStatus)words_view=core.words->Info(request.session,offset);
        else{
            const auto pinyin_size=reader.U32(),text_size=reader.U32();if(!pinyin_size||pinyin_size>128||!text_size||text_size>128||request.length!=12+pinyin_size+text_size)return;
            const auto pinyin=reader.Data(pinyin_size),text=reader.Data(text_size);
            words_view=core.words->Submit(request.session,request.kind,{{reinterpret_cast<const char*>(pinyin.data()),pinyin.size()},{reinterpret_cast<const char*>(text.data()),text.size()}},offset);
        }
    }else if(control){
        bool enabled{};if(request.kind==wire::LearningEnable){wire::Reader reader{body};const auto flag=reader.U32();if(flag>1)return;enabled=flag!=0;}
        control_view=request.kind==wire::LearningStatus?core.learning->Info(request.session):core.learning->Submit(request.session,request.kind,enabled);
    }else if(request.kind==wire::Learn){
        wire::Reader reader{body};const auto input_size=reader.U32(),text_size=reader.U32();
        if(input_size>128||text_size>512||request.length!=8+input_size+text_size)return;
        const auto input=reader.Data(input_size),text=reader.Data(text_size);status=core.learning->Record(input,text,core.words->Handle());
    }
    response.U32(std::bit_cast<uint32_t>(status));
    if(status==0&&request.kind==wire::Query)response.Data(wire::EncodeSnapshot(snapshot));
    if(status==0&&request.kind==wire::QueryPage){response.U32(candidate_page.page);response.U32(candidate_page.total);response.Data(wire::EncodeSnapshot(snapshot));}
    if(control){response.U32(control_view.enabled);response.U32(control_view.count);response.U32(control_view.health);response.U32(control_view.state);response.U32(control_view.error);}
    if(words)words_control::Encode(response,words_view);
    const auto header=wire::EncodeHeader({request.kind|wire::Reply,static_cast<uint32_t>(response.bytes.size()),request.session,request.revision,request.request});
    if(pipe::Write(channel,stop,header)!=ERROR_SUCCESS||pipe::Write(channel,stop,response.bytes)!=ERROR_SUCCESS)return;
    // An explicit bounded ACK replaces FlushFileBuffers, which could otherwise
    // block forever on a stalled client. No text-edit side effect occurs here.
    std::array<uint8_t,1> ack{};pipe::Read(channel,stop,ack,100);
}
struct Threads {
    HANDLE stop;
    std::vector<std::thread> workers;
    void Close(){SetEvent(stop);for(auto& worker:workers)if(worker.joinable())worker.join();workers.clear();}
    ~Threads(){Close();}
};
}
int wmain(int argc,wchar_t** argv){
    try {
        DWORD parent_id{},stall{},warmup{},learning_delay{},idle_ms=15000;std::wstring test_id,stop_name,learning_path,learning_event,words_path;
        for(int i=1;i<argc;++i){
            const std::wstring arg=argv[i];
            if(arg.starts_with(L"--test-id="))test_id=arg.substr(10);
            else if(arg.starts_with(L"--parent="))parent_id=std::stoul(arg.substr(9));
            else if(arg.starts_with(L"--stall-ms="))stall=std::stoul(arg.substr(11));
            else if(arg.starts_with(L"--warmup-ms="))warmup=std::stoul(arg.substr(12));
            else if(arg.starts_with(L"--idle-ms="))idle_ms=std::stoul(arg.substr(10));
            else if(arg.starts_with(L"--stop-event="))stop_name=arg.substr(13);
            else if(arg.starts_with(L"--learning-path="))learning_path=arg.substr(16);
            else if(arg.starts_with(L"--learning-load-ms="))learning_delay=std::stoul(arg.substr(19));
            else if(arg.starts_with(L"--learning-load-event="))learning_event=arg.substr(22);
            else if(arg.starts_with(L"--words-path="))words_path=arg.substr(13);
            else return 2;
        }
        if(idle_ms<100||idle_ms>60000||stall>5000||warmup>2000||learning_delay>2000||
            ((stall||warmup||learning_delay||!learning_event.empty()||!stop_name.empty()||!learning_path.empty()||!words_path.empty())&&(!parent_id||test_id.empty())))return 2;
        const auto name=pipe::Endpoint(test_id);pipe::Security security;
        auto first=MakePipe(name,security,true);if(!first)return 3;
        if(warmup)Sleep(warmup); // bounded, owned-process fault fixture only
        Core core(std::move(learning_path),std::move(words_path),test_id.empty());
        pipe::Handle stop(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!stop)return 4;
        pipe::Handle parent(parent_id?OpenProcess(SYNCHRONIZE,FALSE,parent_id):INVALID_HANDLE_VALUE);
        if(parent_id&&!parent)return 4;
        pipe::Handle test_stop(stop_name.empty()?INVALID_HANDLE_VALUE:OpenEventW(SYNCHRONIZE,FALSE,stop_name.c_str()));
        if(!stop_name.empty()&&!test_stop)return 4;
        std::atomic<ULONGLONG> activity{GetTickCount64()};Threads threads{stop.value,{}};
        const auto run=[&](pipe::Handle initial){
            auto channel=std::move(initial);
            while(WaitForSingleObject(stop.value,0)!=WAIT_OBJECT_0){
                try {
                    if(!channel)channel=MakePipe(name,security);
                    if(!channel)break;
                    if(Accept(channel.value,stop.value))Serve(core,channel.value,stop.value,stall,activity);
                }catch(...){ /* Reject malformed frames without logging input. */ }
                if(channel)DisconnectNamedPipe(channel.value);
                channel.Reset();
            }
        };
        threads.workers.emplace_back(run,std::move(first));
        for(unsigned i=1;i<WorkerCount;++i)threads.workers.emplace_back(run,pipe::Handle{});
        if(!learning_event.empty()){
            pipe::Handle loaded(OpenEventW(EVENT_MODIFY_STATE,FALSE,learning_event.c_str()));if(!loaded)return 4;SetEvent(loaded.value);
        }
        core.learning->Load(learning_delay);
        core.words->Load();
        HANDLE exits[]{stop.value,nullptr,nullptr};DWORD exit_count=1;
        if(parent)exits[exit_count++]=parent.value;
        if(test_stop)exits[exit_count++]=test_stop.value;
        while(WaitForMultipleObjects(exit_count,exits,FALSE,250)==WAIT_TIMEOUT){
            core.learning->RunControl();
            core.words->RunControl();
            core.learning->Flush();
            if(GetTickCount64()-activity.load()>=idle_ms)break;
        }
        threads.Close();return core.Close()?0:6;
    }catch(const std::exception& error){std::fprintf(stderr,"broker: %s\n",error.what());return 1;}
}
