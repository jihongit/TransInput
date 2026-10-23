// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace transinput::pipe {
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){ Reset(); }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    Handle(Handle&& other)noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& other)noexcept {if(this!=&other){Reset();value=std::exchange(other.value,INVALID_HANDLE_VALUE);}return *this;}
    void Reset(HANDLE h=INVALID_HANDLE_VALUE)noexcept{if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);value=h;}
    explicit operator bool()const noexcept{return value&&value!=INVALID_HANDLE_VALUE;}
};
struct LocalAllocation {
    void* value{};
    ~LocalAllocation(){if(value)LocalFree(value);}
};
inline std::wstring LogonSid() {
    Handle token;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))throw std::runtime_error("token query");
    DWORD length{};GetTokenInformation(token.value,TokenGroups,nullptr,0,&length);
    if(!length||length>65536)throw std::runtime_error("token group size");
    std::vector<uint8_t> bytes(length);
    if(!GetTokenInformation(token.value,TokenGroups,bytes.data(),length,&length))throw std::runtime_error("token groups");
    const auto groups=reinterpret_cast<const TOKEN_GROUPS*>(bytes.data());
    for(DWORD i=0;i<groups->GroupCount;++i)if((groups->Groups[i].Attributes&SE_GROUP_LOGON_ID)==SE_GROUP_LOGON_ID){
        LPWSTR value{};
        if(!ConvertSidToStringSidW(groups->Groups[i].Sid,&value))break;
        LocalAllocation storage{value};return value;
    }
    throw std::runtime_error("logon SID unavailable");
}
inline std::wstring Endpoint(const std::wstring& test_id=L"") {
    if(test_id.size()>32||!std::all_of(test_id.begin(),test_id.end(),[](wchar_t c){return(c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f');}))
        throw std::runtime_error("invalid test identifier");
    auto name=L"\\\\.\\pipe\\TransInput.Prototype.v1."+LogonSid();
    if(!test_id.empty())name+=L".test."+test_id;
    return name;
}
struct Security {
    LocalAllocation descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
    Security(){
        const auto sddl=L"D:P(A;;GA;;;"+LogonSid()+L")";
        PSECURITY_DESCRIPTOR value{};
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&value,nullptr))
            throw std::runtime_error("pipe security descriptor");
        descriptor.value=value;attributes.lpSecurityDescriptor=value;
    }
};
inline std::wstring ModulePath(HMODULE module=nullptr) {
    std::wstring path(32768,L'\0');const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!length||length>=path.size())throw std::runtime_error("module path");path.resize(length);return path;
}
inline std::wstring ParentPath(const std::wstring& path) {
    const auto end=path.find_last_of(L"\\/");if(end==std::wstring::npos)throw std::runtime_error("absolute path required");
    return path.substr(0,end);
}
inline bool VerifyServer(HANDLE channel,const std::wstring& executable,DWORD expected_pid=0) {
    ULONG pid{};if(!GetNamedPipeServerProcessId(channel,&pid)||(expected_pid&&pid!=expected_pid))return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));if(!process)return false;
    DWORD length=32768;std::wstring actual(length,L'\0');
    if(!QueryFullProcessImageNameW(process.value,0,actual.data(),&length))return false;actual.resize(length);
    return CompareStringOrdinal(actual.data(),static_cast<int>(actual.size()),executable.data(),static_cast<int>(executable.size()),TRUE)==CSTR_EQUAL;
}
// Every I/O is overlapped. Cancellation is acknowledged before the OVERLAPPED
// and its storage are released. Call this on workers only, never on the UI thread.
inline DWORD Complete(HANDLE channel,HANDLE stop,OVERLAPPED& operation,DWORD timeout,DWORD& transferred) {
    const HANDLE handles[]{operation.hEvent,stop};
    const auto wait=WaitForMultipleObjects(stop?2u:1u,handles,FALSE,timeout);
    if(wait==WAIT_OBJECT_0){
        if(GetOverlappedResult(channel,&operation,&transferred,FALSE))return ERROR_SUCCESS;
        return GetLastError();
    }
    CancelIoEx(channel,&operation);
    DWORD ignored{};
    // Named-pipe kernel cancellation completes independently of the peer. This
    // wait stays on the worker and keeps operation/buffer storage alive.
    GetOverlappedResult(channel,&operation,&ignored,TRUE);
    return wait==WAIT_TIMEOUT?ERROR_TIMEOUT:ERROR_OPERATION_ABORTED;
}
inline DWORD Transfer(HANDLE channel,HANDLE stop,std::span<uint8_t> data,bool writing,DWORD timeout=250) {
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));if(!event)return GetLastError();
    const auto deadline=GetTickCount64()+timeout;size_t done{};
    while(done<data.size()){
        if(stop&&WaitForSingleObject(stop,0)==WAIT_OBJECT_0)return ERROR_OPERATION_ABORTED;
        OVERLAPPED operation{};operation.hEvent=event.value;ResetEvent(event.value);DWORD count{};
        const auto length=static_cast<DWORD>(data.size()-done);
        const auto ok=writing?WriteFile(channel,data.data()+done,length,&count,&operation):ReadFile(channel,data.data()+done,length,&count,&operation);
        if(!ok){
            auto status=GetLastError();if(status!=ERROR_IO_PENDING)return status;
            const auto now=GetTickCount64();
            status=Complete(channel,stop,operation,now<deadline?static_cast<DWORD>(deadline-now):0,count);
            if(status!=ERROR_SUCCESS)return status;
        }
        if(!count)return ERROR_BROKEN_PIPE;done+=count;
        if(done<data.size()&&GetTickCount64()>=deadline)return ERROR_TIMEOUT;
    }
    return ERROR_SUCCESS;
}
inline DWORD Write(HANDLE channel,HANDLE stop,const std::vector<uint8_t>& bytes,DWORD timeout=250) {
    // WriteFile's non-const SDK signature does not mutate the supplied bytes.
    return Transfer(channel,stop,{const_cast<uint8_t*>(bytes.data()),bytes.size()},true,timeout);
}
inline DWORD Read(HANDLE channel,HANDLE stop,std::span<uint8_t> bytes,DWORD timeout=250) {
    return Transfer(channel,stop,bytes,false,timeout);
}
inline Handle Connect(const std::wstring& name,const std::wstring& executable,HANDLE stop,DWORD expected_pid=0,DWORD timeout=500) {
    const auto deadline=GetTickCount64()+timeout;DWORD last_error=ERROR_FILE_NOT_FOUND;
    do {
        Handle channel(CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
        if(channel){if(!VerifyServer(channel.value,executable,expected_pid)){SetLastError(ERROR_ACCESS_DENIED);return {};}return channel;}
        const auto error=GetLastError();last_error=error;if(error!=ERROR_PIPE_BUSY&&error!=ERROR_FILE_NOT_FOUND)return {};
        if(stop&&WaitForSingleObject(stop,10)==WAIT_OBJECT_0){SetLastError(ERROR_OPERATION_ABORTED);return {};}
        if(!stop)Sleep(10);
    }while(GetTickCount64()<deadline);
    SetLastError(last_error==ERROR_FILE_NOT_FOUND?ERROR_PIPE_NOT_CONNECTED:ERROR_TIMEOUT);return {};
}
}
