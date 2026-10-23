// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../shared/pipe.h"
namespace transinput::translation {
// The automatic actor and explicit settings connection test share one paid
// request slot for this logon. A crashed owner leaves an abandoned mutex that
// the next actor can recover, rather than a permanently depleted semaphore.
class RequestGate {
    pipe::Handle mutex_;
    bool owned_{};
public:
    static std::wstring Name(const std::wstring& test_id=L""){
        if(!test_id.empty())pipe::Endpoint(test_id); // reuse strict test-ID validation
        return L"Local\\TransInput.TranslateRequest."+pipe::LogonSid()+(test_id.empty()?L"":L".test."+test_id);
    }
    explicit RequestGate(const std::wstring& test_id=L""){pipe::Security security;const auto name=Name(test_id);
        mutex_.Reset(CreateMutexW(&security.attributes,FALSE,name.c_str()));if(!mutex_)throw std::runtime_error("translation request gate");}
    RequestGate(const RequestGate&)=delete;RequestGate& operator=(const RequestGate&)=delete;
    bool TryAcquire(){if(owned_)return true;const auto status=WaitForSingleObject(mutex_.value,0);
        owned_=status==WAIT_OBJECT_0||status==WAIT_ABANDONED;if(status==WAIT_FAILED)throw std::runtime_error("translation gate wait");return owned_;}
    void Release()noexcept{if(owned_){ReleaseMutex(mutex_.value);owned_=false;}}
    ~RequestGate(){Release();}
};
}
