// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../shared/protected_file.h"
#include "transinput_learning.h"
#include "../shared/learning_control.h"
#include <atomic>
#include <bit>
#include <deque>
#include <memory>
#include <mutex>
namespace transinput {
class LearningStore {
    decltype(&ti_learning_create) create_{};
    decltype(&ti_learning_destroy) destroy_{};
    decltype(&ti_engine_set_learning) attach_{};
    decltype(&ti_learning_record) record_{};
    decltype(&ti_learning_record_with_words) record_words_{};
    decltype(&ti_learning_import) import_{};
    decltype(&ti_learning_export) export_{};
    decltype(&ti_learning_count) count_{};
    uint64_t handle_{};
    std::wstring path_;
    std::wstring preference_path_;
    bool writable_{true};
    bool user_path_{};
    std::atomic<bool> loaded_{};
    std::atomic<bool> enabled_{true},preference_error_{};
    std::mutex records_mutex_,controls_mutex_;
    struct Command {uint64_t id;uint32_t kind;bool enabled;uint32_t state{learning_control::Pending},error{};};
    std::deque<std::shared_ptr<Command>> work_,history_;
    std::atomic<uint64_t> generation_{},last_record_{};
    uint64_t saved_{},last_attempt_{GetTickCount64()};
public:
    enum class Health {Loading,MemoryOnly,Ready,ReadFailed,SaveFailed};
private:
    std::atomic<Health> health_{Health::Loading};
public:
    LearningStore(HMODULE module,std::wstring path,bool deferred=false,bool user_path=false):path_(std::move(path)),user_path_(user_path){
        create_=std::bit_cast<decltype(create_)>(GetProcAddress(module,"ti_learning_create"));
        destroy_=std::bit_cast<decltype(destroy_)>(GetProcAddress(module,"ti_learning_destroy"));
        attach_=std::bit_cast<decltype(attach_)>(GetProcAddress(module,"ti_engine_set_learning"));
        record_=std::bit_cast<decltype(record_)>(GetProcAddress(module,"ti_learning_record"));
        record_words_=std::bit_cast<decltype(record_words_)>(GetProcAddress(module,"ti_learning_record_with_words"));
        import_=std::bit_cast<decltype(import_)>(GetProcAddress(module,"ti_learning_import"));
        export_=std::bit_cast<decltype(export_)>(GetProcAddress(module,"ti_learning_export"));
        count_=std::bit_cast<decltype(count_)>(GetProcAddress(module,"ti_learning_count"));
        if(!create_||!destroy_||!attach_||!record_||!record_words_||!import_||!export_||!count_)throw std::runtime_error("learning ABI");
        handle_=create_();if(!handle_)throw std::runtime_error("learning handle");
        if(!deferred)Load();
    }
    // Broker main thread, after query workers are accepting requests. A slow
    // profile/DPAPI read cannot hold Chinese candidate readiness hostage.
    void Load(DWORD fixture_delay=0)noexcept{
        if(loaded_.load())return;
        if(fixture_delay&&fixture_delay<=2000)Sleep(fixture_delay);
        try{
            if(user_path_)path_=storage::DataDirectory()+L"\\learning.bin";
            if(!path_.empty()){
                preference_path_=path_+L".settings";
                try{auto data=storage::Load(preference_path_,16);
                    if(data){if(data->bytes.size()!=5||std::string_view(reinterpret_cast<const char*>(data->bytes.data()),4)!="TIP1"||data->bytes[4]>1)throw std::runtime_error("learning preference format");enabled_=data->bytes[4]!=0;}
                }catch(...){enabled_=false;preference_error_=true;}
            }
            if(path_.empty())health_=Health::MemoryOnly;
            else{
                auto data=storage::Load(path_);
                if(data&&import_(handle_,data->bytes.data(),static_cast<uint32_t>(data->bytes.size()))!=0)throw std::runtime_error("learning format");
                health_=Health::Ready;
            }
        }catch(...){writable_=false;health_=Health::ReadFailed;}
        loaded_.store(true);
    }
    ~LearningStore(){if(handle_)destroy_(handle_);}
    LearningStore(const LearningStore&)=delete;LearningStore& operator=(const LearningStore&)=delete;
    int32_t Attach(uint64_t engine){return attach_(engine,handle_);}
    int32_t Record(std::span<const uint8_t> input,std::span<const uint8_t> text,uint64_t words=0){
        // Early selections can be skipped as best effort; never overwrite a
        // loaded snapshot with records made before that snapshot was imported.
        if(!loaded_.load())return -4;
        std::lock_guard lock(records_mutex_);
        if(!enabled_.load())return 0;
        if(input.size()>128||text.size()>512)return -1;
        const auto result=record_words_(handle_,words,input.data(),static_cast<uint32_t>(input.size()),text.data(),static_cast<uint32_t>(text.size()));
        if(!result){last_record_.store(GetTickCount64());generation_.fetch_add(1);}return result;
    }
    // Main broker thread only. Disk work never holds a query/engine/UI lock.
    bool Flush(bool force=false)noexcept{
        const auto generation=generation_.load(),now=GetTickCount64();
        if(!loaded_.load()||path_.empty()||!writable_||generation==saved_)return true;
        if(!force&&((now-last_record_.load()<400&&now-last_attempt_<5000)||
            (health_==Health::SaveFailed&&now-last_attempt_<5000)))return true;
        last_attempt_=now;
        try{
            storage::Secret snapshot;snapshot.bytes.resize(TI_MAX_LEARNING_BYTES);uint32_t size{};
            if(export_(handle_,snapshot.bytes.data(),static_cast<uint32_t>(snapshot.bytes.size()),&size)!=0||size>snapshot.bytes.size())throw std::runtime_error("learning snapshot");
            snapshot.bytes.resize(size);storage::Save(path_,snapshot.bytes,L"TransInput local selections");
            saved_=generation;health_=Health::Ready;return true;
        }catch(...){health_=Health::SaveFailed;return false;}
    }
    Health Status()const noexcept{return health_.load();}
    learning_control::View Info(uint64_t id=0){
        learning_control::View view;
        if(id){std::lock_guard lock(controls_mutex_);const auto found=std::find_if(history_.begin(),history_.end(),[&](const auto& value){return value->id==id;});
            if(found==history_.end()){view.state=learning_control::Unknown;view.error=ERROR_NOT_FOUND;}else{view.state=(*found)->state;view.error=(*found)->error;}}
        view.enabled=enabled_.load()?1u:0u;view.health=preference_error_.load()?5u:static_cast<uint32_t>(health_.load());
        if(count_(handle_,&view.count)!=0)view.health=static_cast<uint32_t>(Health::ReadFailed);
        return view;
    }
    learning_control::View Submit(uint64_t id,uint32_t kind,bool enabled=false){
        DWORD rejected{};
        {std::lock_guard lock(controls_mutex_);
            const auto found=std::find_if(history_.begin(),history_.end(),[&](const auto& value){return value->id==id;});
            if(!id||(kind!=wire::LearningEnable&&kind!=wire::LearningClear))rejected=ERROR_INVALID_PARAMETER;
            else if(found!=history_.end()){if((*found)->kind!=kind||(*found)->enabled!=enabled)rejected=ERROR_INVALID_DATA;}
            else if(work_.size()>=8)rejected=ERROR_BUSY;
            else{
                if(history_.size()==32){if(history_.front()->state!=learning_control::Pending)history_.pop_front();else rejected=ERROR_BUSY;}
                if(!rejected){auto command=std::make_shared<Command>(Command{id,kind,enabled});history_.push_back(command);try{work_.push_back(command);}catch(...){history_.pop_back();throw;}}
            }
        }
        auto view=Info(id);if(rejected){view.state=learning_control::Failed;view.error=rejected;}return view;
    }
    // Main thread only: file work is never executed by a query worker or while
    // holding the model/record mutex. The IPC acknowledgement above is pending.
    void RunControl()noexcept{
        if(!loaded_.load())return;
        std::shared_ptr<Command> command;
        {std::lock_guard lock(controls_mutex_);if(work_.empty())return;command=work_.front();work_.pop_front();}
        DWORD error{};
        try{
            if(command->kind==wire::LearningEnable){
                if(preference_path_.empty())throw std::runtime_error("memory-only preference");
                storage::Secret data("TIP1");data.bytes.push_back(command->enabled?1u:0u);
                storage::Save(preference_path_,data.bytes,L"TransInput learning preference");
                std::lock_guard lock(records_mutex_);enabled_=command->enabled;preference_error_=false;
            }else{
                const auto temporary=create_();if(!temporary)throw std::runtime_error("empty model");
                struct Temporary {uint64_t handle;decltype(destroy_) destroy;~Temporary(){destroy(handle);}} owned{temporary,destroy_};
                uint32_t size{};if(export_(temporary,nullptr,0,&size)!=0||size>TI_MAX_LEARNING_BYTES)throw std::runtime_error("empty model size");
                storage::Secret empty;empty.bytes.resize(size);
                if(export_(temporary,empty.bytes.data(),size,&size)!=0)throw std::runtime_error("empty model export");
                if(!path_.empty())storage::Save(path_,empty.bytes,L"TransInput local selections");
                std::lock_guard lock(records_mutex_);
                if(import_(handle_,empty.bytes.data(),size)!=0){writable_=false;throw std::runtime_error("empty model import");}
                saved_=generation_.fetch_add(1)+1;writable_=true;health_=path_.empty()?Health::MemoryOnly:Health::Ready;
            }
        }catch(...){error=ERROR_WRITE_FAULT;}
        {std::lock_guard lock(controls_mutex_);command->error=error;command->state=error?learning_control::Failed:learning_control::Succeeded;}
    }
};
}
