// SPDX-License-Identifier: AGPL-3.0-only
// Fixed fictional choices and owned temporary files only; no user profile path.
#include "../broker/learning_store.h"
#include <cstdio>
#include <thread>
namespace {
using namespace transinput;
void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Api {
    HMODULE module{};
    decltype(&ti_engine_create) create{};decltype(&ti_engine_destroy) destroy{};
    decltype(&ti_engine_query) query{};decltype(&ti_shutdown) shutdown{};
    explicit Api(const wchar_t* path){
        module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);Require(module!=nullptr,"learning load core");
        create=std::bit_cast<decltype(create)>(GetProcAddress(module,"ti_engine_create"));destroy=std::bit_cast<decltype(destroy)>(GetProcAddress(module,"ti_engine_destroy"));
        query=std::bit_cast<decltype(query)>(GetProcAddress(module,"ti_engine_query"));shutdown=std::bit_cast<decltype(shutdown)>(GetProcAddress(module,"ti_shutdown"));
        Require(create&&destroy&&query&&shutdown,"learning core symbols");
    }
    std::string First(LearningStore& model,const std::string& input){
        const auto engine=create();Require(engine!=0,"learning engine");TiSnapshot snapshot{};
        const auto attached=model.Attach(engine);
        const auto status=attached?attached:query(engine,reinterpret_cast<const uint8_t*>(input.data()),static_cast<uint32_t>(input.size()),static_cast<uint32_t>(input.size()),&snapshot,sizeof snapshot);
        const auto destroyed=destroy(engine);Require(!status&&!destroyed&&snapshot.candidate_count>0,"learning query");
        return {reinterpret_cast<const char*>(snapshot.candidates[0].bytes),snapshot.candidates[0].length};
    }
    ~Api(){if(module&&shutdown&&shutdown()==0)FreeLibrary(module);}
};
int32_t Record(LearningStore& model,const std::string& input,const std::string& text){return model.Record(
    {reinterpret_cast<const uint8_t*>(input.data()),input.size()},{reinterpret_cast<const uint8_t*>(text.data()),text.size()});}
struct Files {
    std::wstring root,path,unsupported,missing,controlled;
    Files(){
        wchar_t temporary[MAX_PATH]{};const auto length=GetTempPathW(MAX_PATH,temporary);Require(length>0&&length<MAX_PATH,"learning temp parent");GUID guid{};
        Require(SUCCEEDED(CoCreateGuid(&guid)),"learning temp GUID");wchar_t suffix[40]{};Require(StringFromGUID2(guid,suffix,40)>0,"learning temp name");
        root=std::wstring(temporary)+L"TransInputLearning-"+suffix;storage::UserSecurity security;
        Require(CreateDirectoryW(root.c_str(),&security.attributes)!=FALSE,"learning temp directory");
        path=root+L"\\fixed.bin";unsupported=root+L"\\unsupported.bin";missing=root+L"\\absent\\child\\unwritable.bin";controlled=root+L"\\controlled.bin";
    }
    ~Files(){DeleteFileW(path.c_str());DeleteFileW(unsupported.c_str());DeleteFileW(controlled.c_str());DeleteFileW((controlled+L".settings").c_str());RemoveDirectoryW(root.c_str());}
};
void Corrupt(const std::wstring& path){
    pipe::Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));Require(bool(file),"corrupt owned fixture");DWORD count{};
    Require(WriteFile(file.value,"corrupt",7,&count,nullptr)&&count==7,"write corrupt fixture");
}
void Controls(Api& api,const Files& files){
    using namespace learning_control;
    {
        LearningStore model(api.module,files.controlled);Require(model.Info().count==0&&model.Info().enabled==1,"initial control status");
        Require(!Record(model,"bj","北京"),"controlled selection");
        Require(model.Submit(1,wire::LearningEnable,false).state==Pending,"toggle not queued");model.RunControl();
        Require(model.Info(1).state==Succeeded&&model.Info().enabled==0,"disable did not complete");
        Require(!Record(model,"bj","比较")&&api.First(model,"bj")=="北京"&&model.Info().count==1,"disabled learning recorded or removed old preference");
        Require(model.Submit(1,wire::LearningClear).state==Failed,"identity reused with different action");
        Require(model.Submit(2,wire::LearningClear).state==Pending,"clear not queued");model.RunControl();
        Require(model.Info(2).state==Succeeded&&model.Info().count==0&&api.First(model,"bj")=="比较","clear changed base dictionary or retained history");
        model.Submit(3,wire::LearningEnable,true);model.RunControl();Require(!Record(model,"bj","北京"),"record after enable");
        Require(model.Submit(2,wire::LearningClear).state==Succeeded,"duplicate clear not recognized");model.RunControl();
        Require(model.Info().count==1&&api.First(model,"bj")=="北京","duplicate clear erased newer selection");
        for(uint64_t id=100;id<108;++id)Require(model.Submit(id,wire::LearningEnable,true).state==Pending,"control capacity rejected early");
        Require(model.Submit(108,wire::LearningEnable,true).error==ERROR_BUSY,"control queue was unbounded");
        Require(api.First(model,"bj")=="北京","pending controls blocked query");
        for(unsigned n=0;n<8;++n)model.RunControl();Require(model.Flush(true),"control persistence");
        model.Submit(200,wire::LearningEnable,false);model.RunControl();
    }
    {LearningStore restored(api.module,files.controlled);Require(restored.Info().enabled==0&&restored.Info().count==1,"control settings failed restart");
        Require(!Record(restored,"bj","比较")&&api.First(restored,"bj")=="北京","restart ignored disabled learning");}
    const std::string future="TIP2";storage::Save(files.controlled+L".settings",{reinterpret_cast<const uint8_t*>(future.data()),future.size()},L"TransInput fixed preference fixture");
    {LearningStore invalid(api.module,files.controlled);Require(invalid.Info().health==5&&invalid.Info().enabled==0,"unreadable preference did not stop recording");
        invalid.Submit(300,wire::LearningEnable,true);invalid.RunControl();Require(invalid.Info(300).state==Succeeded&&invalid.Info().enabled==1,"explicit preference recovery failed");}
    {LearningStore missing(api.module,files.missing);Require(!Record(missing,"bj","北京"),"missing store record");
        missing.Submit(400,wire::LearningClear);missing.RunControl();Require(missing.Info(400).state==Failed&&missing.Info().count==1,"failed clear removed in-memory data");
        missing.Submit(401,wire::LearningEnable,false);missing.RunControl();Require(missing.Info(401).state==Failed&&missing.Info().enabled==1,"failed toggle silently changed state");}
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    try{
        Api api(argv[1]);Files files;
        {
            LearningStore model(api.module,files.path);
            Require(GetFileAttributesW(files.path.c_str())==INVALID_FILE_ATTRIBUTES,"opening empty store created data");
            Require(api.First(model,"bj")=="比较","empty preference changed ranking");
            Require(Record(model,"bj","北京")==0&&api.First(model,"bj")=="北京","explicit Chinese preference failed");
            Require(Record(model,"bj","arbitrary English")==-1&&Record(model,"bj","不存在的候选")==-1,"unknown choice was learned");
            Require(api.First(model,"zg")=="这个","preference crossed pinyin context");
            Require(model.Flush(true),"learning encrypted atomic save");
        }
        {
            LearningStore restored(api.module,files.path);Require(restored.Status()==LearningStore::Health::Ready&&api.First(restored,"bj")=="北京","encrypted restart recovery failed");
            std::atomic<unsigned> passed{};std::vector<std::jthread> readers;
            for(unsigned i=0;i<4;++i)readers.emplace_back([&]{for(unsigned n=0;n<100;++n){const auto first=api.First(restored,"bj");if(first=="北京"||first=="比较")++passed;}});
            for(unsigned n=0;n<40;++n){Require(!Record(restored,"bj",n%2?"比较":"北京"),"concurrent learning record");Require(restored.Flush(true),"concurrent learning save");}
            for(auto& reader:readers)reader.join();Require(passed==400,"concurrent preference query mixed data");
        }
        Corrupt(files.path);
        {
            LearningStore corrupt(api.module,files.path);Require(corrupt.Status()==LearningStore::Health::ReadFailed,"corrupt data not classified");
            Require(!Record(corrupt,"bj","北京")&&api.First(corrupt,"bj")=="北京","corrupt persistence blocked local input ranking");
            Require(corrupt.Flush(true)&&corrupt.Status()==LearningStore::Health::ReadFailed,"corrupt store was overwritten");
            pipe::Handle file(CreateFileW(files.path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));std::array<char,7> bytes{};DWORD size{};
            Require(file&&ReadFile(file.value,bytes.data(),7,&size,nullptr)&&size==7&&std::string(bytes.data(),size)=="corrupt","corrupt original was lost");
        }
        const std::string future="TIL2\n";storage::Save(files.unsupported,{reinterpret_cast<const uint8_t*>(future.data()),future.size()},L"TransInput fixed test");
        {LearningStore future_model(api.module,files.unsupported);Require(future_model.Status()==LearningStore::Health::ReadFailed,"unsupported version accepted");
            auto original=storage::Load(files.unsupported);Require(original&&original->View()==future,"future file was silently replaced");
            future_model.Submit(500,wire::LearningClear);future_model.RunControl();
            Require(future_model.Info(500).state==learning_control::Succeeded&&future_model.Info().count==0&&future_model.Status()==LearningStore::Health::Ready,"explicit clear failed to recover unsupported store");}
        {LearningStore missing(api.module,files.missing);Require(!Record(missing,"bj","北京")&&!missing.Flush(true)&&missing.Status()==LearningStore::Health::SaveFailed&&api.First(missing,"bj")=="北京","disk failure blocked memory ranking");}
        Controls(api,files);
        Require(api.shutdown()==0,"learning handles or queries leaked");
        std::puts("PASS: bounded learning ABI, explicit choices, independent pinyin, DPAPI/atomic restart, 400 concurrent queries, corrupt/version preservation, disk failure isolation and shutdown");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
}
