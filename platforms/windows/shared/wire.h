// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>
#include "transinput_core.h"
namespace transinput::wire {
constexpr uint32_t Magic = 0x31514954; // TIQ1, little endian
constexpr uint32_t Version = 1;
constexpr uint32_t Query = 1;
constexpr uint32_t Ping = 2;
constexpr uint32_t Learn = 3;
constexpr uint32_t QueryPage = 4;
constexpr uint32_t LearningStatus = 5,LearningEnable = 6,LearningClear = 7;
constexpr uint32_t WordsStatus=8,WordsAdd=9,WordsRemove=10;
constexpr uint32_t Reply = 0x80000000;
constexpr size_t HeaderSize = 40;
constexpr size_t MaxPayload = 8192;
constexpr size_t SnapshotSize = 5712;
static_assert(sizeof(TiSnapshot) == SnapshotSize);
struct Header { uint32_t kind{}, length{}; uint64_t session{}, revision{}, request{}; };
struct Writer {
    std::vector<uint8_t> bytes;
    void U32(uint32_t value) { for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<uint8_t>(value>>(8*i))); }
    void U64(uint64_t value) { for (unsigned i=0;i<8;++i) bytes.push_back(static_cast<uint8_t>(value>>(8*i))); }
    void Data(std::span<const uint8_t> value) { bytes.insert(bytes.end(),value.begin(),value.end()); }
};
struct Reader {
    std::span<const uint8_t> bytes;
    size_t position{};
    std::span<const uint8_t> Data(size_t count) {
        if (count > bytes.size()-position) throw std::runtime_error("truncated frame");
        const auto value=bytes.subspan(position,count); position+=count; return value;
    }
    uint32_t U32() { const auto v=Data(4); uint32_t n{}; for(unsigned i=0;i<4;++i)n|=uint32_t(v[i])<<(8*i); return n; }
    uint64_t U64() { const auto v=Data(8); uint64_t n{}; for(unsigned i=0;i<8;++i)n|=uint64_t(v[i])<<(8*i); return n; }
};
inline std::vector<uint8_t> EncodeHeader(Header h) {
    if(h.length>MaxPayload) throw std::runtime_error("oversize frame");
    Writer w; w.U32(Magic);w.U32(Version);w.U32(h.kind);w.U32(h.length);
    w.U64(h.session);w.U64(h.revision);w.U64(h.request); return w.bytes;
}
inline Header DecodeHeader(std::span<const uint8_t> bytes) {
    if(bytes.size()!=HeaderSize) throw std::runtime_error("header size");
    Reader r{bytes}; if(r.U32()!=Magic||r.U32()!=Version)throw std::runtime_error("protocol version");
    Header h;h.kind=r.U32();h.length=r.U32();h.session=r.U64();h.revision=r.U64();h.request=r.U64();
    if(h.length>MaxPayload)throw std::runtime_error("oversize frame"); return h;
}
inline void Text(Writer& w,const TiText& text) {
    if(text.length>TI_TEXT_CAPACITY)throw std::runtime_error("text size");
    w.U32(text.length);w.Data(text.bytes);
}
inline void Text(Reader& r,TiText& text) {
    text.length=r.U32(); if(text.length>TI_TEXT_CAPACITY)throw std::runtime_error("text size");
    const auto data=r.Data(TI_TEXT_CAPACITY);std::copy(data.begin(),data.end(),text.bytes);
}
inline std::vector<uint8_t> EncodeSnapshot(const TiSnapshot& s) {
    Writer w;w.U32(s.abi_version);w.U32(s.struct_size);w.U64(s.revision);w.U32(s.composing);
    w.U32(s.cursor);w.U32(s.selected);w.U32(s.candidate_count);Text(w,s.preedit);
    for(const auto& text:s.candidates)Text(w,text);Text(w,s.commit);w.U32(s.outcome);return w.bytes;
}
inline TiSnapshot DecodeSnapshot(std::span<const uint8_t> bytes) {
    if(bytes.size()!=SnapshotSize)throw std::runtime_error("snapshot size");
    Reader r{bytes};TiSnapshot s{};s.abi_version=r.U32();s.struct_size=r.U32();s.revision=r.U64();s.composing=r.U32();
    s.cursor=r.U32();s.selected=r.U32();s.candidate_count=r.U32();Text(r,s.preedit);
    for(auto& text:s.candidates)Text(r,text);Text(r,s.commit);s.outcome=r.U32();
    if(s.abi_version!=TI_ABI_VERSION||s.struct_size!=SnapshotSize||s.candidate_count>9||s.selected>=9||s.cursor>s.preedit.length)
        throw std::runtime_error("snapshot fields");
    return s;
}
}
