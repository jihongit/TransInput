// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../translator/client.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
namespace transinput {
struct TranslationUpdate {uint32_t kind{translation::Observe};uint64_t session{},revision{};std::string source;};
struct TranslationReply {TranslationUpdate update;translation::Reply result;};
class AsyncTranslation {
public:
    struct Target {std::wstring endpoint,executable;DWORD expected_pid{};std::wstring arguments;};
private:
    static constexpr wchar_t ClassName[]=L"TransInput.Translation.Replies.40e2b4b8";
    static constexpr UINT Ready=WM_APP+0x181;
    struct State {
        std::mutex mutex;
        std::optional<TranslationUpdate> pending;
        std::optional<TranslationReply> result;
        pipe::Handle stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)},wake{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        std::atomic<HWND> window{};
        std::atomic<bool> enabled{};
        std::atomic<uint64_t> request{1};
        std::atomic<long>* leases;
        std::atomic<long>* channels;
        HMODULE module{};Target target;
        State(std::atomic<long>* owners,std::atomic<long>* count):leases(owners),channels(count){
            if(!stop||!wake)throw std::runtime_error("translation events");
            if(channels->fetch_add(1)>=8){--*channels;throw std::runtime_error("translation channel limit");}++*leases;
        }
        ~State(){--*channels;--*leases;}
    };
    struct Work {std::shared_ptr<State> state;HMODULE module{};~Work(){if(module)FreeLibrary(module);}};
    std::shared_ptr<State> state_;
    std::function<void(TranslationReply)> callback_;
    HWND window_{};
    static std::wstring Executable(HMODULE module){
        const auto directory=pipe::ParentPath(pipe::ModulePath(module));
        const auto shared=pipe::ParentPath(directory)+L"\\shared\\TransInputTranslator.exe";
        return GetFileAttributesW(shared.c_str())!=INVALID_FILE_ATTRIBUTES?shared:directory+L"\\TransInputTranslator.exe";
    }
    static bool Launch(const std::wstring& executable,const std::wstring& arguments){
        static std::atomic<ULONGLONG> last{};const auto now=GetTickCount64();auto previous=last.load();
        if(now-previous<2000||!last.compare_exchange_strong(previous,now))return false;
        auto command=L"\""+executable+L"\""+(arguments.empty()?L"":L" "+arguments);STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,pipe::ParentPath(executable).c_str(),&startup,&info))return false;
        CloseHandle(info.hProcess);CloseHandle(info.hThread);return true;
    }
    static translation::Reply Query(State& state,const TranslationUpdate& update,bool stopping=false){
        auto result=translation::Query(state.target.endpoint,state.target.executable,stopping?nullptr:state.stop.value,
            update.kind,update.session,update.revision,state.request.fetch_add(1),update.source,state.target.expected_pid);
        // PIPE_NOT_CONNECTED is exclusively a connect failure before a request
        // was written. Errors after delivery never retry a paid observation.
        if(!stopping&&!state.target.expected_pid&&result.status==ERROR_PIPE_NOT_CONNECTED&&WaitForSingleObject(state.stop.value,0)!=WAIT_OBJECT_0&&Launch(state.target.executable,state.target.arguments))
            result=translation::Query(state.target.endpoint,state.target.executable,state.stop.value,update.kind,update.session,update.revision,state.request.fetch_add(1),update.source);
        return result;
    }
    static void Publish(State& state,TranslationUpdate update,translation::Reply result){
        if(!result.status)state.enabled.store(result.view.enabled!=0);
        {std::lock_guard lock(state.mutex);state.result=TranslationReply{std::move(update),std::move(result)};}
        if(const auto window=state.window.load())PostMessageW(window,Ready,0,0);
    }
    static bool Submit(const std::shared_ptr<State>& state,TranslationUpdate update)noexcept{
        if(!state||WaitForSingleObject(state->stop.value,0)==WAIT_OBJECT_0)return false;
        try{std::lock_guard lock(state->mutex);state->pending=std::move(update);SetEvent(state->wake.value);return true;}catch(...){return false;}
    }
    static void CALLBACK Run(PTP_CALLBACK_INSTANCE instance,void* pointer)noexcept {
        std::unique_ptr<Work> work(static_cast<Work*>(pointer));
        FreeLibraryWhenCallbackReturns(instance,std::exchange(work->module,nullptr));CallbackMayRunLong(instance);
        auto& state=*work->state;std::optional<TranslationUpdate> watching,owned;
        try{
            Publish(state,{},Query(state,{translation::ReadState,0,0,{}}));
            const HANDLE events[]{state.stop.value,state.wake.value};
            while(true){
                const auto wait=WaitForMultipleObjects(2,events,FALSE,watching?40:1000);if(wait==WAIT_OBJECT_0)break;
                if(wait!=WAIT_OBJECT_0+1&&wait!=WAIT_TIMEOUT)break;
                std::optional<TranslationUpdate> update;
                {std::lock_guard lock(state.mutex);update=std::move(state.pending);state.pending.reset();ResetEvent(state.wake.value);}
                if(!update){if(watching){update=*watching;update->kind=translation::ReadState;}else update=TranslationUpdate{translation::ReadState,0,0,{}};}
                auto result=Query(state,*update);
                if(update->kind==translation::Observe||update->kind==translation::Retry){watching=*update;owned=*update;}
                if(update->kind==translation::Clear&&watching&&watching->session==update->session&&watching->revision==update->revision)watching.reset();
                if(update->kind==translation::Clear&&owned&&owned->session==update->session&&owned->revision==update->revision)owned.reset();
                if(watching&&update->session==watching->session&&update->revision==watching->revision&&
                    (result.status||result.view.status==0||result.view.status==1||result.view.status==4||result.view.status==5))watching.reset();
                Publish(state,std::move(*update),std::move(result));
            }
        }catch(...){try{Publish(state,watching.value_or(TranslationUpdate{}),{-1,{}});}catch(...){}}
        // Clear only our exact context after an in-flight IPC was cancelled.
        // This bounded worker cleanup never makes UI deactivation wait.
        if(owned){try{owned->kind=translation::Clear;Query(state,*owned,true);}catch(...){}}
    }
    static LRESULT CALLBACK Procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam)noexcept{
        if(message==WM_NCCREATE)SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        auto self=reinterpret_cast<AsyncTranslation*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCDESTROY)SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);
        if(message==Ready&&self){
            try{auto callback=self->callback_;std::optional<TranslationReply> reply;
                {std::lock_guard lock(self->state_->mutex);reply=std::move(self->state_->result);self->state_->result.reset();}
                if(reply&&callback)callback(std::move(*reply));
            }catch(...){ }return 0;
        }return DefWindowProcW(hwnd,message,wparam,lparam);
    }
public:
    AsyncTranslation(HMODULE module,std::atomic<long>* owners,std::atomic<long>* count,std::function<void(TranslationReply)> callback,Target target={}):
        state_(std::make_shared<State>(owners,count)),callback_(std::move(callback)){
        try{
            state_->target=std::move(target);state_->module=module;
            if(state_->target.endpoint.empty())state_->target.endpoint=translation::Endpoint();
            if(state_->target.executable.empty())state_->target.executable=Executable(module);
            WNDCLASSEXW wc{sizeof wc};wc.hInstance=module;wc.lpfnWndProc=Procedure;wc.lpszClassName=ClassName;
            if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("translation reply class");
            window_=CreateWindowExW(0,ClassName,L"TransInput translation replies",0,0,0,0,0,HWND_MESSAGE,nullptr,module,this);
            if(!window_)throw std::runtime_error("translation reply window");state_->window.store(window_);
            auto work=std::make_unique<Work>();work->state=state_;
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(module),&work->module))throw std::runtime_error("translation module lease");
            if(!TrySubmitThreadpoolCallback(Run,work.get(),nullptr))throw std::runtime_error("translation worker");work.release();
        }catch(...){Close();throw;}
    }
    ~AsyncTranslation(){Close();}
    bool Enabled()const noexcept{return state_&&state_->enabled.load();}
    bool Submit(TranslationUpdate update)noexcept{return Submit(state_,std::move(update));}
    std::function<bool(TranslationUpdate)> SubmitHook()const{
        std::weak_ptr<State> weak=state_;
        return [weak](TranslationUpdate update){if(auto state=weak.lock())return Submit(state,std::move(update));return false;};
    }
    std::function<void(uint64_t,uint64_t)> ClearHook()const{
        std::weak_ptr<State> weak=state_;
        return [weak](uint64_t session,uint64_t revision){if(auto state=weak.lock())Submit(state,{translation::Clear,session,revision,{}});};
    }
    void Close()noexcept{
        callback_={};if(state_){state_->window.store(nullptr);SetEvent(state_->stop.value);}
        if(window_)DestroyWindow(std::exchange(window_,nullptr));state_.reset();
    }
    static bool Unregister(HMODULE module)noexcept{return UnregisterClassW(ClassName,module)!=FALSE||GetLastError()==ERROR_CLASS_DOES_NOT_EXIST;}
};
}
