// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
// Isolated translator actor only. Never instantiate or poll in a TSF/UI thread.
#include "../shared/pipe.h"
#include "json.h"
#include <winhttp.h>
#include <atomic>
namespace transinput::translation {
struct HttpResult {uint32_t failure{};std::string text;};
class HttpSession {
    friend class Http;
    HINTERNET session_{},connection_{};
    pipe::Handle closed_{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    bool secure_{};
    static void CALLBACK Closing(HINTERNET,DWORD_PTR context,DWORD status,void*,DWORD)noexcept {
        if(context&&status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)SetEvent(reinterpret_cast<HttpSession*>(context)->closed_.value);
    }
    void Close(HINTERNET& handle)noexcept {
        if(!handle)return;ResetEvent(closed_.value);auto context=reinterpret_cast<DWORD_PTR>(this);
        const auto registered=WinHttpSetOption(handle,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof context)&&
            WinHttpSetStatusCallback(handle,Closing,WINHTTP_CALLBACK_FLAG_HANDLES,0)!=WINHTTP_INVALID_STATUS_CALLBACK;
        WinHttpCloseHandle(std::exchange(handle,nullptr));
        if(registered)WaitForSingleObject(closed_.value,INFINITE);
    }
    HttpSession(const wchar_t* host,INTERNET_PORT port,bool secure,bool testing):secure_(secure){
        try{
            if(!closed_)throw std::runtime_error("translation session close event");
            session_=WinHttpOpen(L"TransInput/0.1",testing?WINHTTP_ACCESS_TYPE_NO_PROXY:WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
            if(!session_||!WinHttpSetTimeouts(session_,1000,1000,1000,1000))throw std::runtime_error("translation HTTP session");
            connection_=WinHttpConnect(session_,host,port,0);if(!connection_)throw std::runtime_error("translation HTTP connection");
        }catch(...){Close(connection_);Close(session_);throw;}
    }
public:
    HttpSession():HttpSession(L"api.deepseek.com",INTERNET_DEFAULT_HTTPS_PORT,true,false){}
#ifdef TI_HTTP_TEST
    explicit HttpSession(INTERNET_PORT loopback):HttpSession(L"127.0.0.1",loopback,false,true){}
#endif
    HttpSession(const HttpSession&)=delete;HttpSession& operator=(const HttpSession&)=delete;
    ~HttpSession(){Close(connection_);Close(session_);}
};
class Http {
    enum class Phase {Send,Headers,Available,Read,Closing,Done};
    static constexpr DWORD Sent=1,Headers=2,Available=4,Read=8,Error=16;
    std::shared_ptr<HttpSession> session_;
    HINTERNET request_{};
    pipe::Handle closed_{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    std::atomic<DWORD> notifications_{},count_{},error_{};
    bool callback_registered_{};
    Phase phase_{Phase::Send};
    std::string body_,response_;
    std::wstring headers_;
    std::array<uint8_t,8192> buffer_{};
    DWORD expected_{};
    ULONGLONG started_{GetTickCount64()};
    DWORD budget_{3000};
    HttpResult result_;
    static void CALLBACK Notify(HINTERNET,DWORD_PTR context,DWORD status,void* info,DWORD length)noexcept {
        const auto self=reinterpret_cast<Http*>(context);if(!self)return;
        DWORD flag{};
        switch(status){
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:flag=Sent;break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:flag=Headers;break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
            if(length!=sizeof(DWORD)||!info){self->error_.store(ERROR_INVALID_DATA);flag=Error;break;}
            self->count_.store(*static_cast<DWORD*>(info));flag=Available;break;
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:self->count_.store(length);flag=Read;break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
            self->error_.store(info&&length==sizeof(WINHTTP_ASYNC_RESULT)?static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError:ERROR_INVALID_DATA);
            flag=Error;break;
        case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:SetEvent(self->closed_.value);return;
        default:return;
        }
        self->notifications_.fetch_or(flag,std::memory_order_release);
    }
    void Check(BOOL ok){if(!ok){const auto code=GetLastError();if(code!=ERROR_IO_PENDING)throw std::runtime_error("translation HTTP operation");}}
    void Initialize() {
        if(!closed_||!session_)throw std::runtime_error("translation session required");
        request_=WinHttpOpenRequest(session_->connection_,L"POST",L"/chat/completions",nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,session_->secure_?WINHTTP_FLAG_SECURE:0);
        if(!request_)throw std::runtime_error("translation HTTP request");
        DWORD disabled=WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_AUTHENTICATION;
        Check(WinHttpSetOption(request_,WINHTTP_OPTION_DISABLE_FEATURE,&disabled,sizeof disabled));
        DWORD autologon=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        Check(WinHttpSetOption(request_,WINHTTP_OPTION_AUTOLOGON_POLICY,&autologon,sizeof autologon));
        auto context=reinterpret_cast<DWORD_PTR>(this);
        Check(WinHttpSetOption(request_,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof context));
        if(WinHttpSetStatusCallback(request_,Notify,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK)
            throw std::runtime_error("translation HTTP callback");
        callback_registered_=true;
        Check(WinHttpSendRequest(request_,headers_.c_str(),static_cast<DWORD>(headers_.size()),body_.data(),
            static_cast<DWORD>(body_.size()),static_cast<DWORD>(body_.size()),context));
    }
    void CloseRequest()noexcept {
        if(request_){WinHttpCloseHandle(std::exchange(request_,nullptr));if(!callback_registered_)SetEvent(closed_.value);}
        else if(!callback_registered_)SetEvent(closed_.value);
    }
    void Complete(uint32_t failure,std::string text={}) {
        result_={failure,std::move(text)};phase_=Phase::Closing;CloseRequest();
    }
    void Dispose()noexcept {
        CloseRequest();
        // This wait is only in the isolated translator actor. HANDLE_CLOSING is
        // the SDK's final callback acknowledgement; buffers cannot be freed early.
        if(callback_registered_)WaitForSingleObject(closed_.value,INFINITE);
        if(!headers_.empty())SecureZeroMemory(headers_.data(),headers_.size()*sizeof(wchar_t));
    }
public:
    Http(std::shared_ptr<HttpSession> session,std::string_view prompt,std::string_view source,std::string_view key,DWORD budget=3000):session_(std::move(session)),body_(RequestBody(prompt,source)),budget_(budget) {
        if(key.empty()||key.size()>512||!std::all_of(key.begin(),key.end(),[](unsigned char c){return c>=33&&c<=126;}))
            throw std::runtime_error("translation key format");
        headers_=L"Content-Type: application/json; charset=utf-8\r\nAccept: application/json\r\nAuthorization: Bearer "+std::wstring(winrt::to_hstring(key).c_str())+L"\r\n";
        try{Initialize();}catch(...){Dispose();throw;}
    }
    Http(const Http&)=delete;
    Http& operator=(const Http&)=delete;
    ~Http(){Dispose();}
    bool Poll(bool cancel=false) {
        if(phase_==Phase::Done)return true;
        if(phase_==Phase::Closing){if(WaitForSingleObject(closed_.value,0)==WAIT_OBJECT_0)phase_=Phase::Done;return phase_==Phase::Done;}
        if(cancel||GetTickCount64()-started_>=budget_){Complete(cancel?1u:2u);return false;}
        const auto flags=notifications_.exchange(0,std::memory_order_acquire);
        if(flags&Error){Complete(error_.load()==ERROR_WINHTTP_TIMEOUT?2u:1u);return false;}
        try {
            if(phase_==Phase::Send&&(flags&Sent)){phase_=Phase::Headers;Check(WinHttpReceiveResponse(request_,nullptr));}
            else if(phase_==Phase::Headers&&(flags&Headers)){
                DWORD status{},length=sizeof status;
                Check(WinHttpQueryHeaders(request_,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX));
                if(status!=200){Complete(status==401||status==403?3u:status==429?4u:1u);return false;}
                phase_=Phase::Available;Check(WinHttpQueryDataAvailable(request_,nullptr));
            }else if(phase_==Phase::Available&&(flags&Available)){
                const auto count=count_.load();
                if(!count){Complete(0,ParseReply(response_));return false;}
                if(count>MaxBody-response_.size()){Complete(5);return false;}
                expected_=std::min<DWORD>(count,static_cast<DWORD>(buffer_.size()));
                phase_=Phase::Read;Check(WinHttpReadData(request_,buffer_.data(),expected_,nullptr));
            }else if(phase_==Phase::Read&&(flags&Read)){
                const auto count=count_.load();if(!count||count>expected_||count>MaxBody-response_.size()){Complete(5);return false;}
                response_.append(reinterpret_cast<const char*>(buffer_.data()),count);
                phase_=Phase::Available;Check(WinHttpQueryDataAvailable(request_,nullptr));
            }
        }catch(const winrt::hresult_error&){Complete(5);}catch(const std::exception&){Complete(phase_==Phase::Available?5u:1u);}
        return false;
    }
    const HttpResult& Result()const {if(phase_!=Phase::Done)throw std::runtime_error("translation result still active");return result_;}
};
}
