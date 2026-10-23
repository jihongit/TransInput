// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../shared/pipe.h"
#include "../shared/wire.h"
namespace transinput::translation {
struct View {uint64_t configuration{};uint32_t enabled{},status{},failure{};std::string output;};
enum : uint32_t {Observe=16,ReadState=17,Clear=18,Reload=19,Retry=20};
inline std::wstring Endpoint(const std::wstring& test_id=L""){
    auto name=pipe::Endpoint(test_id);const auto index=name.find(L".Prototype.");
    if(index==std::wstring::npos)throw std::runtime_error("translation endpoint");name.replace(index,11,L".Translate.");return name;
}
}
