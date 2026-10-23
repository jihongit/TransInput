// SPDX-License-Identifier: AGPL-3.0-only
// Fixed fictional vocabulary, private temporary files and owned broker only.
#include "../broker/user_words_store.h"
#include "../broker/learning_store.h"
#include "../shared/client.h"
#include "words_fixture.h"
#include <cstdio>
#include <thread>
namespace {
using namespace transinput;
void Need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Api {
    HMODULE module{};
    decltype(&ti_engine_create) create{};decltype(&ti_engine_destroy) destroy{};decltype(&ti_engine_query) query{};decltype(&ti_shutdown) shutdown{};
    decltype(&ti_words_create) words{};decltype(&ti_words_destroy) destroy_words{};decltype(&ti_words_clone) clone{};decltype(&ti_words_publish) publish{};
    decltype(&ti_words_info) info{};decltype(&ti_words_get) get{};decltype(&ti_words_edit) edit{};decltype(&ti_words_import) import{};decltype(&ti_words_export) export_words{};decltype(&ti_engine_set_words) attach{};
    explicit Api(const wchar_t* path){module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);Need(module!=nullptr,"word load core");
#define BIND(field,name) field=std::bit_cast<decltype(field)>(GetProcAddress(module,name));Need(field!=nullptr,"word core symbol")
        BIND(create,"ti_engine_create");BIND(destroy,"ti_engine_destroy");BIND(query,"ti_engine_query");BIND(shutdown,"ti_shutdown");
        BIND(words,"ti_words_create");BIND(destroy_words,"ti_words_destroy");BIND(clone,"ti_words_clone");BIND(publish,"ti_words_publish");BIND(info,"ti_words_info");BIND(get,"ti_words_get");
        BIND(edit,"ti_words_edit");BIND(import,"ti_words_import");BIND(export_words,"ti_words_export");BIND(attach,"ti_engine_set_words");
#undef BIND
    }
    std::string First(UserWordsStore& model,const std::string& input,LearningStore* learning=nullptr){
        const auto engine=create();Need(engine!=0,"word engine");TiSnapshot snapshot{};const auto status=model.Attach(engine);
        if(learning)Need(learning->Attach(engine)==0,"personal learning attach");
        const auto queried=status?status:query(engine,reinterpret_cast<const uint8_t*>(input.data()),static_cast<uint32_t>(input.size()),static_cast<uint32_t>(input.size()),&snapshot,sizeof snapshot);
        const auto ended=destroy(engine);Need(!queried&&!ended&&snapshot.candidate_count,"word candidate query");return {reinterpret_cast<const char*>(snapshot.candidates[0].bytes),snapshot.candidates[0].length};
    }
    ~Api(){if(module&&shutdown&&shutdown()==0)FreeLibrary(module);}
};
void Abi(Api& api){
    const auto words=api.words();Need(words!=0&&api.shutdown()==-4,"word handle unload guard");
    const std::string spelling="xing he yan jiu suo",text="星河研究所";
    Need(api.edit(words,0,reinterpret_cast<const uint8_t*>(spelling.data()),static_cast<uint32_t>(spelling.size()),reinterpret_cast<const uint8_t*>(text.data()),static_cast<uint32_t>(text.size()))==0,"word ABI add");
    std::vector<uint8_t> info_buffer(sizeof(TiWordInfo)+2,0xa5),row_buffer(sizeof(TiWordRow)+2,0xa5);
    Need(api.info(words,reinterpret_cast<TiWordInfo*>(info_buffer.data()+1),sizeof(TiWordInfo))==0,"unaligned word info");TiWordInfo info{};memcpy(&info,info_buffer.data()+1,sizeof info);
    Need(info.count==1&&info.abi_version==1&&info.struct_size==sizeof info&&info_buffer.front()==0xa5&&info_buffer.back()==0xa5,"word info bounds");
    Need(api.get(words,0,reinterpret_cast<TiWordRow*>(row_buffer.data()+1),sizeof(TiWordRow))==0,"unaligned word row");TiWordRow row{};memcpy(&row,row_buffer.data()+1,sizeof row);
    Need(std::string(reinterpret_cast<const char*>(row.text.bytes),row.text.length)==text&&row_buffer.front()==0xa5&&row_buffer.back()==0xa5,"owned word output");
    const auto copy=api.clone(words);Need(copy!=0,"word stage clone");Need(api.edit(copy,1,reinterpret_cast<const uint8_t*>(spelling.data()),static_cast<uint32_t>(spelling.size()),reinterpret_cast<const uint8_t*>(text.data()),static_cast<uint32_t>(text.size()))==0,"word stage delete");
    Need(api.info(words,&info,sizeof info)==0&&info.count==1,"unpublished stage changed original");Need(api.publish(words,copy)==0&&api.info(words,&info,sizeof info)==0&&info.count==0,"prepared word publish");
    Need(api.get(words,0,&row,sizeof row)==-1&&api.info(words,nullptr,sizeof info)==-1&&api.info(words,&info,sizeof info-1)==-1,"word ABI invalid buffer");
    const std::array<uint8_t,4> corrupt{'T','I','W','2'};Need(api.import(words,corrupt.data(),4)==-1&&api.info(words,&info,sizeof info)==0&&info.count==0,"invalid import changed model");
    uint32_t bytes{};Need(api.export_words(words,nullptr,0,&bytes)==0&&bytes==16,"empty word export");
    Need(api.destroy_words(copy)==0&&api.destroy_words(words)==0&&api.info(words,&info,sizeof info)==-2&&api.clone(words)==0,"stale word handle");
}
void Store(Api& api){
    testing::WordsFixture files(false);using namespace words_control;
    const Row first{"xing he yan jiu suo","星河研究所"},second{"yun duo gong zuo shi","云朵工作室"};
    {
        UserWordsStore model(api.module,files.path);Need(model.Info().total==0&&GetFileAttributesW(files.path.c_str())==INVALID_FILE_ATTRIBUTES,"read-only empty open created word file");
        Need(model.Submit(1,wire::WordsAdd,first).state==Pending,"word change not queued");Need(api.First(model,"nihao")=="你好","pending words blocked Chinese");model.RunControl();
        Need(model.Info(1).state==Succeeded&&api.First(model,"xingheyanjiusuo")==first.text,"word save failed candidate");
        auto encrypted=storage::Load(files.path);Need(encrypted&&encrypted->View().starts_with("TIW1"),"encrypted word persistence");
        Need(model.Submit(2,wire::WordsAdd,second).state==Pending,"second word queue");
        pipe::Handle deny_replace(CreateFileW(files.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));Need(bool(deny_replace),"owned word replace lock");model.RunControl();
        Need(model.Info(2).state==Failed&&model.Info().total==1,"failed disk write changed live dictionary");deny_replace.Reset();
        auto original=storage::Load(files.path);Need(original&&original->View()==encrypted->View(),"failed write replaced original word file");
        model.Submit(3,wire::WordsAdd,second);model.RunControl();Need(model.Info(3).state==Succeeded&&model.Info().total==2,"word retry with new identity");
        model.Submit(4,wire::WordsRemove,first);model.RunControl();model.Submit(5,wire::WordsAdd,first);model.RunControl();
        Need(model.Submit(4,wire::WordsRemove,first).state==Succeeded,"duplicate word removal lost identity");model.RunControl();Need(model.Info().total==2,"duplicate removal erased new word");
        Need(model.Submit(4,wire::WordsAdd,first).state==Failed,"conflicting word identity accepted");
        std::atomic<unsigned> reads{};std::vector<std::jthread> readers;
        for(unsigned i=0;i<4;++i)readers.emplace_back([&]{for(unsigned n=0;n<80;++n)if(api.First(model,"xingheyanjiusuo")==first.text)++reads;});
        for(uint64_t id=10;id<20;++id){model.Submit(id,id%2?wire::WordsAdd:wire::WordsRemove,second);model.RunControl();}
        for(auto& reader:readers)reader.join();Need(reads==320,"concurrent word snapshot mixed candidates");
        for(uint64_t id=100;id<108;++id)Need(model.Submit(id,wire::WordsAdd,second).state==Pending,"word queue capacity rejected early");
        Need(model.Submit(108,wire::WordsAdd,second).error==ERROR_BUSY,"word command queue unbounded");for(unsigned i=0;i<8;++i)model.RunControl();
        LearningStore learning(api.module,files.learning_path);learning.Submit(1,wire::LearningClear);learning.RunControl();Need(api.First(model,"xingheyanjiusuo")==first.text,"clearing preferences deleted personal words");
    }
    {UserWordsStore restored(api.module,files.path);Need(restored.Info().total==2&&api.First(restored,"xing'he'yan'jiu'suo")==first.text,"encrypted word restart");}
    {pipe::Handle file(CreateFileW(files.path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr));DWORD count{};Need(file&&WriteFile(file.value,"corrupt",7,&count,nullptr)&&count==7,"corrupt owned word file");}
    {UserWordsStore corrupt(api.module,files.path);Need(corrupt.Info().health==ReadFailed&&corrupt.Submit(200,wire::WordsAdd,first).state==Failed,"corrupt word file was writable");
        Need(api.First(corrupt,"nihao")=="你好","corrupt personal words broke base input");pipe::Handle file(CreateFileW(files.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));std::array<char,7> bytes{};DWORD count{};
        Need(file&&ReadFile(file.value,bytes.data(),7,&count,nullptr)&&count==7&&std::string(bytes.data(),7)=="corrupt","corrupt original not preserved");}
}
void Protocol(){
    using namespace words_control;testing::WordsFixture fixture;
    const auto request=[&](uint32_t offset=0,uint64_t id=0){return Query(fixture.endpoint,fixture.executable,nullptr,offset,id,wire::WordsStatus,{},fixture.pid);};
    auto result=request();Need(!result.status&&result.view.total==0,"word IPC initial status");
    if(result.view.health==Loading){Sleep(100);result=request();}
    Need(result.view.health==Ready,"word broker did not load private store");
    for(unsigned i=0;i<13;++i){const Row row{"ce shi","测试词"+std::to_string(i)};result=Change(fixture.endpoint,fixture.executable,nullptr,wire::WordsAdd,100+i,row,0,fixture.pid);Need(!result.status&&result.view.state==Succeeded,"word IPC add");}
    result=request();Need(result.view.total==13&&result.view.rows.size()==12,"word IPC first page");auto last=request(12);Need(!last.status&&last.view.rows.size()==1&&last.view.offset==12,"word IPC last page");
    const auto selected=last.view.rows[0];result=Change(fixture.endpoint,fixture.executable,nullptr,wire::WordsRemove,300,selected,12,fixture.pid);Need(!result.status&&result.view.state==Succeeded&&result.view.total==12,"word IPC remove by identity");
    result=Change(fixture.endpoint,fixture.executable,nullptr,wire::WordsAdd,301,selected,0,fixture.pid);Need(!result.status&&result.view.state==Succeeded,"word IPC re-add");
    result=Change(fixture.endpoint,fixture.executable,nullptr,wire::WordsRemove,300,selected,0,fixture.pid);Need(!result.status&&result.view.total==13,"replayed IPC removal changed new data");
    const auto query=QueryBroker(fixture.endpoint,fixture.executable,nullptr,99,1,1,"ceshi",5,fixture.pid);Need(!query.status&&query.snapshot.candidate_count==9,"personal words not in broker candidates");
    const Row invalid{"ni3hao","你好"};result=Change(fixture.endpoint,fixture.executable,nullptr,wire::WordsAdd,400,invalid,0,fixture.pid);Need(!result.status&&result.view.state==Failed&&result.view.error==ERROR_INVALID_PARAMETER,"invalid IPC spelling accepted");
    fixture.Restart();result=request();Need(!result.status&&result.view.total==13,"word IPC restart lost saved data");Need(request(0,300).view.state==words_control::Unknown,"restart invented past operation result");
    auto denied=Query(fixture.endpoint,fixture.executable,nullptr,0,0,wire::WordsStatus,{},GetCurrentProcessId());Need(denied.status!=0,"word IPC accepted wrong server identity");
}
void SelectionMemory(Api& api){
    testing::WordsFixture files(false);using namespace words_control;
    const Row first{"xing he yan jiu suo","星河研究所"},second{"xing he yan jiu suo","星河研修所"};
    const std::string input="xhyjs";
    {
        UserWordsStore words(api.module,files.path);LearningStore learning(api.module,files.learning_path);
        words.Submit(1,wire::WordsAdd,first);words.RunControl();words.Submit(2,wire::WordsAdd,second);words.RunControl();
        const auto original=api.First(words,input,&learning);const auto chosen=original==first.text?second.text:first.text;
        const auto record=[&](const std::string& text){return learning.Record({reinterpret_cast<const uint8_t*>(input.data()),input.size()},{reinterpret_cast<const uint8_t*>(text.data()),text.size()},words.Handle());};
        Need(record(chosen)==0&&api.First(words,input,&learning)==chosen,"personal initials selection not learned");
        Need(learning.Flush(true),"personal selection save");
        learning.Submit(10,wire::LearningEnable,false);learning.RunControl();Need(learning.Info(10).state==learning_control::Succeeded,"disable personal learning");
        Need(record(original)==0&&api.First(words,input,&learning)==chosen,"disabled learning changed preference");
    }
    {
        UserWordsStore words(api.module,files.path);LearningStore learning(api.module,files.learning_path);
        Need(learning.Info().count==1&&learning.Info().enabled==0,"personal choice/enabled state lost at restart");
        const auto chosen=api.First(words,input,&learning);const auto baseline=api.First(words,input);
        Need(chosen!=baseline,"personal learned order lost at restart");
        learning.Submit(11,wire::LearningClear);learning.RunControl();Need(learning.Info(11).state==learning_control::Succeeded&&api.First(words,input,&learning)==baseline&&words.Info().total==2,"clear did not restore personal order independently");
        learning.Submit(12,wire::LearningEnable,true);learning.RunControl();
        Need(learning.Record({reinterpret_cast<const uint8_t*>(input.data()),input.size()},{reinterpret_cast<const uint8_t*>(chosen.data()),chosen.size()},words.Handle())==0,"re-enable personal learning");
        words.Submit(3,wire::WordsRemove,chosen==first.text?first:second);words.RunControl();
        Need(api.First(words,input,&learning)!=chosen,"deleted personal word resurrected from memory");
    }
}
}
int wmain(int argc,wchar_t** argv){if(argc!=2)return 2;try{Api api(argv[1]);Abi(api);Store(api);SelectionMemory(api);Protocol();Need(api.shutdown()==0,"word handles prevented unload");
    std::puts("PASS: personal-word ABI/staging/unaligned buffers; encrypted save/restart; disk/corrupt protection; 320 concurrent queries; separate learning; bounded/deduplicated commands; paged verified-peer IPC and original word identity");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}
