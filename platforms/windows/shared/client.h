// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "pipe.h"
#include "wire.h"
#include <optional>
namespace transinput {
struct QueryResult { DWORD status{ERROR_GEN_FAILURE}; TiSnapshot snapshot{}; uint32_t page{},total{}; };
struct LearnResult {DWORD status{ERROR_GEN_FAILURE};bool connected{};};
inline LearnResult RecordSelection(const std::wstring& endpoint,const std::wstring& executable,HANDLE stop,
    uint64_t session,uint64_t revision,uint64_t request,const std::string& input,const std::string& text,DWORD expected_pid=0,DWORD timeout=100){
    LearnResult result;
    try{
        if(input.empty()||input.size()>128||text.empty()||text.size()>512){result.status=ERROR_INVALID_PARAMETER;return result;}
        auto channel=pipe::Connect(endpoint,executable,stop,expected_pid,timeout);
        if(!channel){result.status=GetLastError();return result;}result.connected=true;
        wire::Writer body;body.U32(static_cast<uint32_t>(input.size()));body.U32(static_cast<uint32_t>(text.size()));
        body.Data({reinterpret_cast<const uint8_t*>(input.data()),input.size()});body.Data({reinterpret_cast<const uint8_t*>(text.data()),text.size()});
        const auto header=wire::EncodeHeader({wire::Learn,static_cast<uint32_t>(body.bytes.size()),session,revision,request});
        auto status=pipe::Write(channel.value,stop,header);if(!status)status=pipe::Write(channel.value,stop,body.bytes);
        std::array<uint8_t,wire::HeaderSize> reply_bytes{};if(!status)status=pipe::Read(channel.value,stop,reply_bytes);
        if(status){result.status=status;return result;}
        const auto reply=wire::DecodeHeader(reply_bytes);
        if(reply.kind!=(wire::Learn|wire::Reply)||reply.session!=session||reply.revision!=revision||reply.request!=request||reply.length!=4)throw std::runtime_error("learning reply identity");
        std::array<uint8_t,4> payload{};status=pipe::Read(channel.value,stop,payload);if(status){result.status=status;return result;}
        wire::Reader reader{payload};result.status=reader.U32()==0?ERROR_SUCCESS:ERROR_INVALID_DATA;
        pipe::Write(channel.value,stop,std::vector<uint8_t>{6},100);return result;
    }catch(...){result.status=ERROR_INVALID_DATA;return result;}
}
inline QueryResult QueryBroker(const std::wstring& endpoint,const std::wstring& executable,HANDLE stop,
    uint64_t session,uint64_t revision,uint64_t request,const std::string& source,uint32_t cursor,DWORD expected_pid=0,DWORD connect_timeout=500,std::optional<uint32_t> page={}){
    QueryResult result;
    try {
        if(source.size()>128||cursor>source.size()||(page&&*page>=TI_MAX_PAGES)){result.status=ERROR_INVALID_PARAMETER;return result;}
        auto channel=pipe::Connect(endpoint,executable,stop,expected_pid,connect_timeout);if(!channel){result.status=GetLastError();return result;}
        wire::Writer body;body.U32(cursor);body.U32(static_cast<uint32_t>(source.size()));
        if(page)body.U32(*page);
        body.Data({reinterpret_cast<const uint8_t*>(source.data()),source.size()});
        const auto kind=page?wire::QueryPage:wire::Query;
        const auto header=wire::EncodeHeader({kind,static_cast<uint32_t>(body.bytes.size()),session,revision,request});
        auto status=pipe::Write(channel.value,stop,header);
        if(status==ERROR_SUCCESS)status=pipe::Write(channel.value,stop,body.bytes);
        std::array<uint8_t,wire::HeaderSize> reply_bytes{};
        if(status==ERROR_SUCCESS)status=pipe::Read(channel.value,stop,reply_bytes);
        if(status!=ERROR_SUCCESS){result.status=status;return result;}
        const auto reply=wire::DecodeHeader(reply_bytes);
        if(reply.kind!=(kind|wire::Reply)||reply.session!=session||reply.revision!=revision||reply.request!=request||reply.length<4)
            throw std::runtime_error("reply identity");
        std::vector<uint8_t> payload(reply.length);
        status=pipe::Read(channel.value,stop,payload);if(status!=ERROR_SUCCESS){result.status=status;return result;}
        wire::Reader reader{payload};const auto core_status=reader.U32();
        if(core_status!=0){result.status=ERROR_INVALID_DATA;return result;}
        if(page){result.page=reader.U32();result.total=reader.U32();}
        result.snapshot=wire::DecodeSnapshot(reader.Data(payload.size()-(page?12:4)));
        if(page){
            const auto last=result.total?((result.total-1)/TI_CANDIDATES):0;
            if(result.total>TI_CANDIDATES*TI_MAX_PAGES||result.page!=std::min(*page,last)||
                result.snapshot.candidate_count!=std::min<uint32_t>(TI_CANDIDATES,result.total-result.page*TI_CANDIDATES))throw std::runtime_error("page fields");
        }else result.total=result.snapshot.candidate_count;
        if(result.snapshot.preedit.length!=source.size()||!std::equal(source.begin(),source.end(),result.snapshot.preedit.bytes))
            throw std::runtime_error("reply source");
        std::vector<uint8_t> ack{6};pipe::Write(channel.value,stop,ack,100);
        result.status=ERROR_SUCCESS;return result;
    }catch(...){result.status=ERROR_INVALID_DATA;return result;}
}
}
