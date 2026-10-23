// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "protocol.h"
#include <bit>
namespace transinput::translation {
struct Reply {int32_t status{};View view;};
inline Reply Query(const std::wstring& endpoint,const std::wstring& executable,HANDLE stop,uint32_t kind,uint64_t session,uint64_t revision,
    uint64_t request,std::string_view source={},DWORD expected_pid=0){
    try{
        wire::Writer payload;
        if(kind==Observe){if(source.size()>800)return {-1,{}};payload.U32(static_cast<uint32_t>(source.size()));payload.Data({reinterpret_cast<const uint8_t*>(source.data()),source.size()});}
        auto channel=pipe::Connect(endpoint,executable,stop,expected_pid);if(!channel)return {static_cast<int32_t>(GetLastError()),{}};
        const auto header=wire::EncodeHeader({kind,static_cast<uint32_t>(payload.bytes.size()),session,revision,request});
        auto code=pipe::Write(channel.value,stop,header);if(code)return {static_cast<int32_t>(code),{}};
        code=pipe::Write(channel.value,stop,payload.bytes);if(code)return {static_cast<int32_t>(code),{}};
        std::array<uint8_t,wire::HeaderSize> incoming{};code=pipe::Read(channel.value,stop,incoming);if(code)return {static_cast<int32_t>(code),{}};
        const auto response=wire::DecodeHeader(incoming);
        if(response.kind!=(kind|wire::Reply)||response.session!=session||response.revision!=revision||response.request!=request||response.length<4||response.length>4124)return {-1,{}};
        std::vector<uint8_t> body(response.length);code=pipe::Read(channel.value,stop,body);if(code)return {static_cast<int32_t>(code),{}};
        wire::Reader reader{body};Reply result;result.status=std::bit_cast<int32_t>(reader.U32());
        if(!result.status){
            result.view.configuration=reader.U64();result.view.enabled=reader.U32();result.view.status=reader.U32();result.view.failure=reader.U32();const auto length=reader.U32();
            if(result.view.enabled>1||result.view.status>5||result.view.failure>5||length>4096||body.size()!=28+length||
                (result.view.status==4?length==0:length!=0))return {-1,{}};
            const auto text=reader.Data(length);result.view.output.assign(reinterpret_cast<const char*>(text.data()),text.size());
            if(length&&(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,result.view.output.data(),static_cast<int>(length),nullptr,0)||
                std::any_of(text.begin(),text.end(),[](uint8_t c){return(c<32&&c!='\n'&&c!='\t')||c==127;})))return {-1,{}};
        }else if(body.size()!=4)return {-1,{}};
        pipe::Write(channel.value,stop,std::vector<uint8_t>{1},100);return result;
    }catch(...){return {-1,{}};}
}
}
