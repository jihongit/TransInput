// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "core.h"
#include "http.h"
#include "settings.h"
#include "protocol.h"
#include "request_gate.h"
#include <functional>
#include <mutex>
namespace transinput::translation {
struct Client {uint32_t process{};uint64_t created{},session{};bool operator==(const Client&)const=default;};
class Actor {
    struct Session {Client client;uint64_t global{},revision{},touched{};};
    Core core_;
    std::mutex mutex_;
    std::vector<Session> sessions_;
    uint64_t next_session_{1},active_job_{};
    std::wstring path_;
    Settings settings_;
    std::shared_ptr<HttpSession> pool_;
    RequestGate gate_;
    std::unique_ptr<Http> http_;
    std::atomic<bool> reload_{true};
    std::string prompt_;
#ifdef TI_HTTP_TEST
    std::function<std::shared_ptr<HttpSession>()> make_pool_;
#endif
    auto Find(const Client& client){return std::find_if(sessions_.begin(),sessions_.end(),[&](const auto& value){return value.client==client;});}
    View Read(uint64_t global,uint64_t revision){
        const auto state=core_.State();View out{state.configuration,state.status!=0,state.status==0?0u:1u,0,{}};
        if(state.session==global&&state.revision==revision){
            out.status=state.status;out.failure=state.failure;
            if(state.output_length>TI_TRANSLATION_OUTPUT)throw std::runtime_error("translation output layout");
            out.output.assign(reinterpret_cast<const char*>(state.output),state.output_length);
        }return out;
    }
    void Finish(const HttpResult& result){
        std::lock_guard lock(mutex_);
        if(core_.finish(core_.handle,active_job_,reinterpret_cast<const uint8_t*>(result.text.data()),static_cast<uint32_t>(result.text.size()),result.failure)!=0)
            core_.finish(core_.handle,active_job_,nullptr,0,5);
        active_job_=0;
        gate_.Release();
    }
public:
    Actor(const std::wstring& core_path,std::wstring settings_path):core_(core_path),path_(std::move(settings_path)),prompt_(core_.Prompt()){}
#ifdef TI_HTTP_TEST
    Actor(const std::wstring& core_path,std::wstring settings_path,std::function<std::shared_ptr<HttpSession>()> factory):
        Actor(core_path,std::move(settings_path)){make_pool_=std::move(factory);}
#endif
    Actor(const Actor&)=delete;Actor& operator=(const Actor&)=delete;
    ~Actor(){
        if(http_){while(!http_->Poll(true))Sleep(1);const auto result=http_->Result();http_.reset();Finish(result);}
    }
    void Reload()noexcept {reload_.store(true);}
    // Observe/read/clear run on bounded pipe workers. They never touch WinHTTP.
    View Observe(const Client& client,uint64_t revision,std::string_view source){
        if(!client.process||!client.created||!client.session||!revision||source.size()>TI_TRANSLATION_SOURCE)throw std::runtime_error("translation context");
        std::lock_guard lock(mutex_);auto found=Find(client);
        if(found==sessions_.end()){
            if(sessions_.size()>=256){
                const auto current=core_.State().session;
                auto oldest=sessions_.end();
                for(auto it=sessions_.begin();it!=sessions_.end();++it)if(it->global!=current&&(oldest==sessions_.end()||it->touched<oldest->touched))oldest=it;
                if(oldest==sessions_.end())throw std::runtime_error("translation context capacity");sessions_.erase(oldest);
            }
            if(next_session_==UINT64_MAX)throw std::runtime_error("translation session exhaustion");
            sessions_.push_back({client,next_session_++,0,0});found=std::prev(sessions_.end());
        }
        if(revision>found->revision){
            if(core_.observe(core_.handle,found->global,revision,reinterpret_cast<const uint8_t*>(source.data()),static_cast<uint32_t>(source.size()),GetTickCount64())!=0)
                throw std::runtime_error("translation source");
            found->revision=revision;found->touched=GetTickCount64();
        }
        return Read(found->global,revision);
    }
    View State(const Client& client,uint64_t revision){
        std::lock_guard lock(mutex_);const auto found=Find(client);return Read(found==sessions_.end()?0:found->global,revision);
    }
    View Clear(const Client& client,uint64_t revision){
        std::lock_guard lock(mutex_);const auto found=Find(client);
        if(found!=sessions_.end()&&revision==found->revision)core_.clear(core_.handle,found->global,revision);
        return Read(found==sessions_.end()?0:found->global,revision);
    }
    View Retry(const Client& client,uint64_t revision){
        std::lock_guard lock(mutex_);const auto found=Find(client);const auto state=core_.State();
        if(found!=sessions_.end()&&state.session==found->global&&state.revision==revision)core_.retry(core_.handle,GetTickCount64());
        return Read(found==sessions_.end()?0:found->global,revision);
    }
    // One dedicated actor thread calls Tick. The network runs outside mutex_.
    void Tick(){
        if(reload_.exchange(false)){
            Settings next;try{next=LoadSettings(path_);}catch(...){ /* invalid configuration fails closed */ }
            {std::lock_guard lock(mutex_);core_.configure(core_.handle,next.identity,next.enabled&&!next.key.bytes.empty()?1:0);}
            settings_=std::move(next);
        }
        if(http_){
            bool cancel{};{std::lock_guard lock(mutex_);cancel=core_.State().cancel_active!=0;}
            if(http_->Poll(cancel)){const auto result=http_->Result();http_.reset();Finish(result);}
        }
        if(http_)return;
        if(!gate_.TryAcquire())return;
        TiTranslation job{};{std::lock_guard lock(mutex_);job=core_.State(true);}
        if(!job.job){gate_.Release();return;}
        active_job_=job.job;
        try{
            if(!pool_){
#ifdef TI_HTTP_TEST
                if(make_pool_)pool_=make_pool_();else
#endif
                pool_=std::make_shared<HttpSession>();
            }
            http_=std::make_unique<Http>(pool_,prompt_,std::string_view(reinterpret_cast<const char*>(job.source),job.source_length),settings_.key.View());
        }catch(...){Finish({1,{}});}
    }
};
}
