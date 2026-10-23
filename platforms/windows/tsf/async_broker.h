// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../shared/client.h"
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
namespace transinput {
struct BrokerJob {
    uint64_t session{},revision{},request{};
    std::string source;
    uint32_t cursor{};
    bool commit{};
    uint32_t page{};
};
struct BrokerReply { BrokerJob job; QueryResult result; };
struct SelectionLearn {uint64_t session{},revision{},request{};std::string input,text;};
class AsyncBroker {
public:
    // Internal dependency injection for isolated transport tests. No registry,
    // environment variable, or exported service API can override the target.
    struct Target {std::wstring endpoint,executable;DWORD expected_pid{};std::wstring arguments;};
private:
    static constexpr wchar_t ClassName[]=L"TransInput.Prototype.Replies.40e2b4b8";
    static constexpr UINT ReadyMessage=WM_APP+0x180;
    struct State {
        std::mutex mutex;
        std::deque<BrokerJob> commits;
        std::optional<BrokerJob> preview;
        std::deque<SelectionLearn> learning;
        uint64_t next_learning{1};
        std::deque<BrokerReply> results;
        pipe::Handle stop{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        pipe::Handle wake{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
        std::atomic<HWND> window{};
        std::atomic<long>* leases;
        std::atomic<long>* channels;
        size_t outstanding{};
        bool failed{};
        bool failure_notified{};
        HMODULE module{};
        Target target;
        State(std::atomic<long>* objects,std::atomic<long>* count):leases(objects),channels(count){
            if(!stop||!wake)throw std::runtime_error("broker events");
            if(channels->fetch_add(1)>=8){--*channels;throw std::runtime_error("broker channel limit");}
            ++*leases;
        }
        ~State(){--*channels;--*leases;}
    };
    struct Work {std::shared_ptr<State> state;HMODULE module{};~Work(){if(module)FreeLibrary(module);}};
    std::shared_ptr<State> state_;
    HWND window_{};
    std::function<void(BrokerReply)> callback_;
    static std::wstring Executable(HMODULE module){
        const auto directory=pipe::ParentPath(pipe::ModulePath(module));
        const auto shared=pipe::ParentPath(directory)+L"\\shared\\TransInputBroker.exe";
        if(GetFileAttributesW(shared.c_str())!=INVALID_FILE_ATTRIBUTES)return shared;
        return directory+L"\\TransInputBroker.exe";
    }
    static bool Launch(const std::wstring& executable,const std::wstring& arguments={}){
        static std::atomic<ULONGLONG> last{};
        const auto now=GetTickCount64();auto previous=last.load();
        if(now-previous<2000||!last.compare_exchange_strong(previous,now))return false;
        auto command=L"\""+executable+L"\""+(arguments.empty()?L"":L" "+arguments);
        STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        if(!CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,
            pipe::ParentPath(executable).c_str(),&startup,&info))return false;
        CloseHandle(info.hProcess);CloseHandle(info.hThread);return true;
    }
    static QueryResult Query(State& state,const BrokerJob& job,const std::wstring& name,const std::wstring& executable,DWORD timeout=100){
        auto result=QueryBroker(name,executable,state.stop.value,job.session,job.revision,job.request,job.source,job.cursor,state.target.expected_pid,timeout,job.page);
        if(!state.target.expected_pid&&result.status==ERROR_PIPE_NOT_CONNECTED&&WaitForSingleObject(state.stop.value,0)!=WAIT_OBJECT_0&&Launch(executable,state.target.arguments)){
            // Pure candidate queries have no remote edit side effects; only this
            // local calculation may be retried. Paid model calls are separate.
            result=QueryBroker(name,executable,state.stop.value,job.session,job.revision,job.request,job.source,job.cursor,0,500,job.page);
        }else if((result.status==ERROR_TIMEOUT||result.status==ERROR_BROKEN_PIPE||result.status==ERROR_PIPE_NOT_CONNECTED)&&
            WaitForSingleObject(state.stop.value,0)!=WAIT_OBJECT_0){
            // One bounded retry of a pure local calculation also covers a pipe
            // reserved while the broker is still warming its dictionary. This
            // must never be applied to edits, learning, or paid model requests.
            result=QueryBroker(name,executable,state.stop.value,job.session,job.revision,job.request,job.source,job.cursor,state.target.expected_pid,500,job.page);
        }
        return result;
    }
    static void Notify(State& state){if(const auto window=state.window.load())PostMessageW(window,ReadyMessage,0,0);}
    static void CALLBACK Run(PTP_CALLBACK_INSTANCE instance,void* pointer)noexcept {
        std::unique_ptr<Work> work(static_cast<Work*>(pointer));
        // The OS owns a real module reference until this callback has returned,
        // even after UI teardown. UI code never joins/waits for this worker.
        FreeLibraryWhenCallbackReturns(instance,std::exchange(work->module,nullptr));CallbackMayRunLong(instance);
        auto& state=*work->state;
        try{
            const auto name=state.target.endpoint.empty()?pipe::Endpoint():state.target.endpoint;
            const auto executable=state.target.executable.empty()?Executable(state.module):state.target.executable;
            Query(state,{},name,executable,1); // prewarm away from key callbacks
            const HANDLE events[]{state.stop.value,state.wake.value};
            while(WaitForMultipleObjects(2,events,FALSE,INFINITE)==WAIT_OBJECT_0+1){
                BrokerJob job;
                std::optional<SelectionLearn> learning;
                {
                    std::lock_guard lock(state.mutex);
                    if(!state.commits.empty()){job=std::move(state.commits.front());state.commits.pop_front();}
                    else if(state.preview){job=std::move(*state.preview);state.preview.reset();}
                    else if(!state.learning.empty()){learning=std::move(state.learning.front());state.learning.pop_front();}
                    else {ResetEvent(state.wake.value);continue;}
                    if(state.commits.empty()&&!state.preview&&state.learning.empty())ResetEvent(state.wake.value);
                }
                if(learning){
                    auto result=RecordSelection(name,executable,state.stop.value,learning->session,learning->revision,learning->request,learning->input,learning->text,state.target.expected_pid);
                    // A failed connection can launch before any frame was sent.
                    // Never replay a record once a verified connection existed.
                    if(!result.connected&&!state.target.expected_pid&&result.status==ERROR_PIPE_NOT_CONNECTED&&
                        WaitForSingleObject(state.stop.value,0)!=WAIT_OBJECT_0&&Launch(executable,state.target.arguments)){
                        RecordSelection(name,executable,state.stop.value,learning->session,learning->revision,learning->request,learning->input,learning->text,0,500);
                    }
                    continue;
                }
                auto result=Query(state,job,name,executable);
                if(WaitForSingleObject(state.stop.value,0)==WAIT_OBJECT_0)break;
                {std::lock_guard lock(state.mutex);state.results.push_back({std::move(job),result});}
                Notify(state);
            }
        }catch(...){
            try{
                std::lock_guard lock(state.mutex);state.failed=true;
                while(!state.commits.empty()){state.results.push_back({std::move(state.commits.front()),{}});state.commits.pop_front();}
                if(state.preview){state.results.push_back({std::move(*state.preview),{}});state.preview.reset();}
            }catch(...){ /* Allocation failure cannot escape an OS callback. */ }
            Notify(state);
        }
    }
    static LRESULT CALLBACK Procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam)noexcept {
        if(message==WM_NCCREATE){const auto create=reinterpret_cast<CREATESTRUCTW*>(lparam);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(create->lpCreateParams));}
        auto self=reinterpret_cast<AsyncBroker*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCDESTROY)SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);
        if(message==ReadyMessage&&self){
            try{
                BrokerReply result;bool have{},more{};
                auto callback=self->callback_;
                {std::lock_guard lock(self->state_->mutex);
                    if(!self->state_->results.empty()){
                        result=std::move(self->state_->results.front());self->state_->results.pop_front();
                        --self->state_->outstanding;have=true;
                    }else if(self->state_->failed&&!self->state_->failure_notified){
                        self->state_->failure_notified=true;have=true;
                    }
                    more=!self->state_->results.empty()||(self->state_->failed&&!self->state_->failure_notified);
                }
                if(more)PostMessageW(hwnd,ReadyMessage,0,0);
                // A callback may synchronously deactivate/destroy this window.
                // No access to self is permitted after invoking the local copy.
                if(have&&callback)callback(std::move(result));
            }catch(...){ /* no exceptions across WndProc */ }
            return 0;
        }
        return DefWindowProcW(hwnd,message,wparam,lparam);
    }
public:
    static std::wstring Path(HMODULE module){return Executable(module);}
    std::function<bool(SelectionLearn)> LearningHook()const{
        const std::weak_ptr<State> weak=state_;
        return [weak](SelectionLearn selection)noexcept{
            try{
                auto state=weak.lock();if(!state)return false;
                std::lock_guard lock(state->mutex);
                if(state->failed||WaitForSingleObject(state->stop.value,0)==WAIT_OBJECT_0||state->learning.size()>=64)return false;
                selection.request=state->next_learning++;
                state->learning.push_back(std::move(selection));SetEvent(state->wake.value);return true;
            }catch(...){return false;}
        };
    }
    AsyncBroker(HMODULE module,std::atomic<long>* leases,std::atomic<long>* channels,std::function<void(BrokerReply)> callback,Target target={}):
        state_(std::make_shared<State>(leases,channels)),callback_(std::move(callback)) {
        try{
            state_->target=std::move(target);
            WNDCLASSEXW wc{sizeof wc};wc.hInstance=module;wc.lpfnWndProc=Procedure;wc.lpszClassName=ClassName;
            if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("reply class");
            window_=CreateWindowExW(0,ClassName,L"TransInput replies",0,0,0,0,0,HWND_MESSAGE,nullptr,module,this);
            if(!window_)throw std::runtime_error("reply window");
            state_->window.store(window_);state_->module=module;
            auto work=std::make_unique<Work>();work->state=state_;
            if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,reinterpret_cast<LPCWSTR>(module),&work->module))throw std::runtime_error("worker module lease");
            if(!TrySubmitThreadpoolCallback(Run,work.get(),nullptr))throw std::runtime_error("worker start");
            work.release();
        }catch(...){Close();throw;}
    }
    ~AsyncBroker(){Close();}
    bool Submit(BrokerJob job)noexcept{try{
        std::lock_guard lock(state_->mutex);
        if(state_->failed)return false;
        if(!job.commit&&state_->preview){state_->preview=std::move(job);SetEvent(state_->wake.value);return true;}
        if(state_->outstanding>=64||(job.commit&&state_->commits.size()>=32))return false;
        if(job.commit)state_->commits.push_back(std::move(job));else state_->preview=std::move(job);
        ++state_->outstanding;SetEvent(state_->wake.value);return true;
      }catch(...){return false;}
    }
    void Close()noexcept{
        callback_={};
        if(state_){state_->window.store(nullptr);SetEvent(state_->stop.value);}
        if(window_)DestroyWindow(std::exchange(window_,nullptr));
        state_.reset();
    }
    static bool Unregister(HMODULE module)noexcept {
        return UnregisterClassW(ClassName,module)!=FALSE||GetLastError()==ERROR_CLASS_DOES_NOT_EXIST;
    }
};
}
