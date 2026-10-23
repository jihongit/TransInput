// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../shared/words_control.h"
#include "../shared/protected_file.h"
namespace transinput::testing {
// Own child broker and private temporary files. Never opens a real user store.
struct WordsFixture {
    pipe::Handle process,stop;std::wstring root,path,learning_path,endpoint,executable,test_id,stop_name;DWORD pid{};
    static void Need(bool value,const char* message){if(!value)throw std::runtime_error(message);}
    explicit WordsFixture(bool start=true){
        wchar_t temporary[MAX_PATH]{};const auto length=GetTempPathW(MAX_PATH,temporary);Need(length>0&&length<MAX_PATH,"word fixture temporary directory");
        GUID guid{};Need(SUCCEEDED(CoCreateGuid(&guid)),"word fixture GUID");wchar_t suffix[40]{};Need(StringFromGUID2(guid,suffix,40)>0,"word fixture suffix");
        root=std::wstring(temporary)+L"TransInputWords-"+suffix;storage::UserSecurity security;Need(CreateDirectoryW(root.c_str(),&security.attributes)!=FALSE,"word fixture directory");
        path=root+L"\\words.bin";learning_path=root+L"\\learning.bin";test_id=std::to_wstring(GetCurrentProcessId())+L"d";endpoint=pipe::Endpoint(test_id);
        executable=pipe::ParentPath(pipe::ModulePath())+L"\\TransInputBroker.exe";stop_name=std::wstring(L"Local\\TransInput.Word.Test.")+suffix;
        stop.Reset(CreateEventW(&security.attributes,TRUE,FALSE,stop_name.c_str()));Need(bool(stop),"word fixture stop event");if(start)Start();
    }
    void Start(){
        ResetEvent(stop.value);auto command=L"\""+executable+L"\" --parent="+std::to_wstring(GetCurrentProcessId())+L" --test-id="+test_id+
            L" --stop-event=\""+stop_name+L"\" --words-path=\""+path+L"\" --learning-path=\""+learning_path+L"\" --idle-ms=60000";
        STARTUPINFOW startup{sizeof startup};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};
        Need(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,pipe::ParentPath(executable).c_str(),&startup,&info)!=FALSE,"start owned word broker");
        process.Reset(info.hProcess);CloseHandle(info.hThread);pid=info.dwProcessId;
        auto channel=pipe::Connect(endpoint,executable,nullptr,pid,3000);Need(bool(channel),"word fixture broker ready");
        channel.Reset();const auto deadline=GetTickCount64()+3000;
        words_control::Result result;
        do{result=words_control::Query(endpoint,executable,nullptr,0,0,wire::WordsStatus,{},pid);if(!result.status&&result.view.health!=words_control::Loading)break;Sleep(10);}while(GetTickCount64()<deadline);
        Need(!result.status&&result.view.health==words_control::Ready,"word fixture profile ready");
    }
    void Stop(){if(process){SetEvent(stop.value);Need(WaitForSingleObject(process.value,3000)==WAIT_OBJECT_0,"word fixture broker close");process.Reset();}}
    void Restart(){Stop();Start();}
    ~WordsFixture(){
        if(process){SetEvent(stop.value);if(WaitForSingleObject(process.value,3000)!=WAIT_OBJECT_0)TerminateProcess(process.value,99);}
        DeleteFileW(path.c_str());DeleteFileW(learning_path.c_str());DeleteFileW((learning_path+L".settings").c_str());RemoveDirectoryW(root.c_str());
    }
};
}
