// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../shared/protected_file.h"
#include "../shared/words_control.h"
#include <atomic>
#include <bit>
#include <deque>
#include <memory>
#include <mutex>
namespace transinput {
class UserWordsStore {
    decltype(&ti_words_create) create_{};decltype(&ti_words_clone) clone_{};decltype(&ti_words_destroy) destroy_{};
    decltype(&ti_engine_set_words) attach_{};decltype(&ti_words_publish) publish_{};decltype(&ti_words_info) info_{};
    decltype(&ti_words_get) get_{};decltype(&ti_words_edit) edit_{};decltype(&ti_words_import) import_{};decltype(&ti_words_export) export_{};
    uint64_t handle_{};std::wstring path_;bool user_path_{};std::atomic<uint32_t> health_{words_control::Loading};
    struct Command {uint64_t id;uint32_t kind;words_control::Row row;uint32_t state{words_control::Pending},error{};};
    std::mutex controls_mutex_;std::deque<std::shared_ptr<Command>> work_,history_;
    struct Temporary {uint64_t handle;decltype(destroy_) destroy;~Temporary(){if(handle)destroy(handle);}};
public:
    UserWordsStore(HMODULE module,std::wstring path,bool user_path=false,bool deferred=false):path_(std::move(path)),user_path_(user_path){
#define TI_WORD_BIND(field,name) field=std::bit_cast<decltype(field)>(GetProcAddress(module,name));if(!field)throw std::runtime_error("personal word ABI")
        TI_WORD_BIND(create_,"ti_words_create");TI_WORD_BIND(clone_,"ti_words_clone");TI_WORD_BIND(destroy_,"ti_words_destroy");TI_WORD_BIND(attach_,"ti_engine_set_words");
        TI_WORD_BIND(publish_,"ti_words_publish");TI_WORD_BIND(info_,"ti_words_info");TI_WORD_BIND(get_,"ti_words_get");TI_WORD_BIND(edit_,"ti_words_edit");TI_WORD_BIND(import_,"ti_words_import");TI_WORD_BIND(export_,"ti_words_export");
#undef TI_WORD_BIND
        handle_=create_();if(!handle_)throw std::runtime_error("personal word handle");if(!deferred)Load();
    }
    ~UserWordsStore(){if(handle_)destroy_(handle_);}
    UserWordsStore(const UserWordsStore&)=delete;UserWordsStore& operator=(const UserWordsStore&)=delete;
    void Load()noexcept{
        if(health_.load()!=words_control::Loading)return;
        try{
            if(user_path_)path_=storage::DataDirectory()+L"\\user-words.bin";
            if(path_.empty()){health_=words_control::MemoryOnly;return;}
            auto data=storage::Load(path_,TI_MAX_WORD_FILE_BYTES);
            if(data&&import_(handle_,data->bytes.data(),static_cast<uint32_t>(data->bytes.size()))!=0)throw std::runtime_error("personal word format");
            health_=words_control::Ready;
        }catch(...){health_=words_control::ReadFailed;}
    }
    int32_t Attach(uint64_t engine){return attach_(engine,handle_);}
    uint64_t Handle()const noexcept{return handle_;}
    words_control::View Info(uint64_t id=0,uint32_t offset=0){
        words_control::View view;view.health=health_.load();
        if(id){std::lock_guard lock(controls_mutex_);const auto found=std::find_if(history_.begin(),history_.end(),[&](const auto& command){return command->id==id;});
            if(found==history_.end()){view.state=words_control::Unknown;view.error=ERROR_NOT_FOUND;}else{view.state=(*found)->state;view.error=(*found)->error;}}
        // Stable page from one immutable snapshot, even when a mutation publishes.
        Temporary snapshot{clone_(handle_),destroy_};if(!snapshot.handle)throw std::runtime_error("word page snapshot");
        TiWordInfo info{};if(info_(snapshot.handle,&info,sizeof info)!=0)throw std::runtime_error("word page info");
        view.total=info.count;view.revision=info.revision;view.offset=std::min(offset,view.total?((view.total-1)/words_control::PageSize)*words_control::PageSize:0);
        for(uint32_t i=view.offset;i<view.total&&i<view.offset+words_control::PageSize;++i){
            TiWordRow row{};if(get_(snapshot.handle,i,&row,sizeof row)!=0||row.pinyin.length>128||row.text.length>128)throw std::runtime_error("word page row");
            view.rows.push_back({{reinterpret_cast<const char*>(row.pinyin.bytes),row.pinyin.length},{reinterpret_cast<const char*>(row.text.bytes),row.text.length}});
        }
        return view;
    }
    words_control::View Submit(uint64_t id,uint32_t kind,const words_control::Row& row,uint32_t offset=0){
        DWORD error{};
        {std::lock_guard lock(controls_mutex_);const auto found=std::find_if(history_.begin(),history_.end(),[&](const auto& command){return command->id==id;});
            if(!id||(kind!=wire::WordsAdd&&kind!=wire::WordsRemove)||row.pinyin.empty()||row.pinyin.size()>128||row.text.empty()||row.text.size()>128)error=ERROR_INVALID_PARAMETER;
            else if(found!=history_.end()){if((*found)->kind!=kind||(*found)->row!=row)error=ERROR_INVALID_DATA;}
            else if(health_.load()!=words_control::Ready)error=health_.load()==words_control::Loading?ERROR_BUSY:ERROR_FILE_CORRUPT;
            else if(work_.size()>=8)error=ERROR_BUSY;
            else{
                if(history_.size()==32){if(history_.front()->state!=words_control::Pending)history_.pop_front();else error=ERROR_BUSY;}
                if(!error){auto command=std::make_shared<Command>(Command{id,kind,row});history_.push_back(command);try{work_.push_back(command);}catch(...){history_.pop_back();throw;}}
            }
        }
        auto view=Info(id,offset);if(error){view.state=words_control::Failed;view.error=error;}return view;
    }
    // Broker main thread only. Stage/serialize/encrypt/write outside the query
    // model lock; publish a prepared immutable snapshot only after disk success.
    void RunControl()noexcept{
        std::shared_ptr<Command> command;
        {std::lock_guard lock(controls_mutex_);if(work_.empty())return;command=work_.front();work_.pop_front();}
        DWORD error{};
        try{
            Temporary next{clone_(handle_),destroy_};if(!next.handle)throw std::runtime_error("word stage");
            const auto& row=command->row;
            const auto edited=edit_(next.handle,command->kind==wire::WordsRemove?1u:0u,reinterpret_cast<const uint8_t*>(row.pinyin.data()),static_cast<uint32_t>(row.pinyin.size()),reinterpret_cast<const uint8_t*>(row.text.data()),static_cast<uint32_t>(row.text.size()));
            if(edited)error=edited==-5?ERROR_TOO_MANY_NAMES:ERROR_INVALID_PARAMETER;
            else{
                uint32_t size{};if(export_(next.handle,nullptr,0,&size)!=0||size>TI_MAX_WORD_FILE_BYTES)throw std::runtime_error("word export size");
                storage::Secret bytes;bytes.bytes.resize(size);if(export_(next.handle,bytes.bytes.data(),size,&size)!=0)throw std::runtime_error("word export");
                storage::Save(path_,bytes.bytes,L"TransInput personal vocabulary");
                if(publish_(handle_,next.handle)!=0){health_=words_control::ReadFailed;throw std::runtime_error("word publish");}
            }
        }catch(...){error=ERROR_WRITE_FAULT;}
        {std::lock_guard lock(controls_mutex_);command->error=error;command->state=error?words_control::Failed:words_control::Succeeded;}
    }
};
}
