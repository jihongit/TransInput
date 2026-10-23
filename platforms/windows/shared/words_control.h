// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "pipe.h"
#include "wire.h"
#include "transinput_words.h"
#include <atomic>
#include <string>
namespace transinput::words_control {
constexpr uint32_t PageSize=12;
enum State:uint32_t {None,Pending,Succeeded,Failed,Unknown};
enum Health:uint32_t {Loading,Ready,ReadFailed,MemoryOnly};
struct Row {std::string pinyin,text;bool operator==(const Row&)const=default;};
struct View {uint64_t revision{};uint32_t total{},health{},state{},error{},offset{};std::vector<Row> rows;};
struct Result {DWORD status{ERROR_GEN_FAILURE};View view;};
inline void Encode(wire::Writer& response,const View& view){
    response.U64(view.revision);response.U32(view.total);response.U32(view.health);response.U32(view.state);response.U32(view.error);response.U32(view.offset);response.U32(static_cast<uint32_t>(view.rows.size()));
    for(const auto& row:view.rows){response.U32(static_cast<uint32_t>(row.pinyin.size()));response.U32(static_cast<uint32_t>(row.text.size()));
        response.Data({reinterpret_cast<const uint8_t*>(row.pinyin.data()),row.pinyin.size()});response.Data({reinterpret_cast<const uint8_t*>(row.text.data()),row.text.size()});}
}
inline Result Query(const std::wstring& endpoint,const std::wstring& executable,HANDLE stop,uint32_t offset=0,uint64_t id=0,uint32_t kind=wire::WordsStatus,const Row& row={},DWORD expected_pid=0){
    static std::atomic<uint64_t> sequence{1};const auto request=sequence.fetch_add(1);Result result;
    try{
        if((kind!=wire::WordsStatus&&kind!=wire::WordsAdd&&kind!=wire::WordsRemove)||offset>TI_MAX_WORDS||(kind!=wire::WordsStatus&&(!id||row.pinyin.empty()||row.pinyin.size()>128||row.text.empty()||row.text.size()>128))){result.status=ERROR_INVALID_PARAMETER;return result;}
        auto channel=pipe::Connect(endpoint,executable,stop,expected_pid,500);if(!channel){result.status=GetLastError();return result;}
        wire::Writer body;body.U32(offset);
        if(kind!=wire::WordsStatus){body.U32(static_cast<uint32_t>(row.pinyin.size()));body.U32(static_cast<uint32_t>(row.text.size()));
            body.Data({reinterpret_cast<const uint8_t*>(row.pinyin.data()),row.pinyin.size()});body.Data({reinterpret_cast<const uint8_t*>(row.text.data()),row.text.size()});}
        const auto header=wire::EncodeHeader({kind,static_cast<uint32_t>(body.bytes.size()),id,0,request});
        auto status=pipe::Write(channel.value,stop,header);if(!status)status=pipe::Write(channel.value,stop,body.bytes);
        std::array<uint8_t,wire::HeaderSize> reply_bytes{};if(!status)status=pipe::Read(channel.value,stop,reply_bytes);if(status){result.status=status;return result;}
        const auto reply=wire::DecodeHeader(reply_bytes);
        if(reply.kind!=(kind|wire::Reply)||reply.session!=id||reply.revision||reply.request!=request||reply.length<36||reply.length>4096)throw std::runtime_error("word control reply");
        std::vector<uint8_t> payload(reply.length);status=pipe::Read(channel.value,stop,payload);if(status){result.status=status;return result;}
        wire::Reader reader{payload};if(reader.U32()!=0)throw std::runtime_error("word control status");
        auto& view=result.view;view.revision=reader.U64();view.total=reader.U32();view.health=reader.U32();view.state=reader.U32();view.error=reader.U32();view.offset=reader.U32();const auto count=reader.U32();
        if(view.total>TI_MAX_WORDS||view.health>MemoryOnly||view.state>Unknown||view.offset>view.total||count>PageSize||count>view.total-view.offset)throw std::runtime_error("word control bounds");
        for(uint32_t i=0;i<count;++i){const auto pinyin_size=reader.U32(),text_size=reader.U32();if(!pinyin_size||pinyin_size>128||!text_size||text_size>128)throw std::runtime_error("word row bounds");
            const auto pinyin=reader.Data(pinyin_size),text=reader.Data(text_size);view.rows.push_back({{reinterpret_cast<const char*>(pinyin.data()),pinyin.size()},{reinterpret_cast<const char*>(text.data()),text.size()}});}
        if(reader.position!=payload.size())throw std::runtime_error("word control trailing data");
        pipe::Write(channel.value,stop,std::vector<uint8_t>{6},100);result.status=0;return result;
    }catch(...){result.status=ERROR_INVALID_DATA;return result;}
}
// Submit once; uncertainty is resolved only by polling the same identity.
inline Result Change(const std::wstring& endpoint,const std::wstring& executable,HANDLE stop,uint32_t kind,uint64_t id,const Row& row,uint32_t offset=0,DWORD expected_pid=0){
    auto result=Query(endpoint,executable,stop,offset,id,kind,row,expected_pid);const auto deadline=GetTickCount64()+4000;
    while(!result.status&&result.view.state==Pending&&GetTickCount64()<deadline){
        if(stop&&WaitForSingleObject(stop,50)==WAIT_OBJECT_0){result.status=ERROR_OPERATION_ABORTED;return result;}
        if(!stop)Sleep(50);result=Query(endpoint,executable,stop,offset,id,wire::WordsStatus,{},expected_pid);
    }
    if(!result.status&&result.view.state==Pending)result.status=ERROR_TIMEOUT;return result;
}
}
