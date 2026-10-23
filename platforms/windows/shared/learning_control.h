// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "pipe.h"
#include "wire.h"
#include <atomic>
namespace transinput::learning_control {
enum State:uint32_t {None,Pending,Succeeded,Failed,Unknown};
struct View {uint32_t enabled{1},count{},health{},state{},error{};};
struct Result {DWORD status{ERROR_GEN_FAILURE};View view;};
inline Result Query(const std::wstring& endpoint,const std::wstring& executable,HANDLE stop,uint32_t kind=wire::LearningStatus,uint64_t id=0,bool enabled=false,DWORD expected_pid=0){
    static std::atomic<uint64_t> sequence{1};const auto request=sequence.fetch_add(1);Result result;
    try{
        if((kind!=wire::LearningStatus&&kind!=wire::LearningEnable&&kind!=wire::LearningClear)||(kind!=wire::LearningStatus&&!id)){result.status=ERROR_INVALID_PARAMETER;return result;}
        auto channel=pipe::Connect(endpoint,executable,stop,expected_pid,250);if(!channel){result.status=GetLastError();return result;}
        wire::Writer body;if(kind==wire::LearningEnable)body.U32(enabled?1u:0u);
        const auto header=wire::EncodeHeader({kind,static_cast<uint32_t>(body.bytes.size()),id,0,request});
        auto status=pipe::Write(channel.value,stop,header);if(!status)status=pipe::Write(channel.value,stop,body.bytes);
        std::array<uint8_t,wire::HeaderSize> reply_bytes{};if(!status)status=pipe::Read(channel.value,stop,reply_bytes);
        if(status){result.status=status;return result;}
        const auto reply=wire::DecodeHeader(reply_bytes);
        if(reply.kind!=(kind|wire::Reply)||reply.session!=id||reply.revision!=0||reply.request!=request||reply.length!=24)throw std::runtime_error("learning control reply");
        std::array<uint8_t,24> payload{};status=pipe::Read(channel.value,stop,payload);if(status){result.status=status;return result;}
        wire::Reader reader{payload};if(reader.U32()!=0)throw std::runtime_error("learning control status");
        auto& view=result.view;view.enabled=reader.U32();view.count=reader.U32();view.health=reader.U32();view.state=reader.U32();view.error=reader.U32();
        if(view.enabled>1||view.count>512||view.health>5||view.state>Unknown)throw std::runtime_error("learning control bounds");
        pipe::Write(channel.value,stop,std::vector<uint8_t>{6},100);result.status=0;return result;
    }catch(...){result.status=ERROR_INVALID_DATA;return result;}
}
// A mutation is submitted exactly once. Poll only its identity, never replay a
// timed-out clear; a broker restart can make the outcome unknown.
inline Result Change(const std::wstring& endpoint,const std::wstring& executable,HANDLE stop,uint32_t kind,uint64_t id,bool enabled=false,DWORD expected_pid=0){
    auto result=Query(endpoint,executable,stop,kind,id,enabled,expected_pid);const auto deadline=GetTickCount64()+4000;
    while(!result.status&&result.view.state==Pending&&GetTickCount64()<deadline){
        if(stop&&WaitForSingleObject(stop,50)==WAIT_OBJECT_0){result.status=ERROR_OPERATION_ABORTED;return result;}
        if(!stop)Sleep(50);result=Query(endpoint,executable,stop,wire::LearningStatus,id,false,expected_pid);
    }
    if(!result.status&&result.view.state==Pending)result.status=ERROR_TIMEOUT;return result;
}
}
