// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../shared/pipe.h"
#include "../shared/protected_file.h"
#include "../shared/wire.h"
#include <wincrypt.h>
#include <bcrypt.h>
#include <shlobj.h>
namespace transinput::translation {
using storage::Secret;
using storage::ProtectedMemory;
using storage::UserSecurity;
using storage::UserSid;
struct Settings {bool enabled{};uint64_t identity{};Secret key;};
inline std::wstring SettingsPath(){return storage::DataDirectory()+L"\\settings.bin";}
inline Settings LoadSettings(const std::wstring& path){
    auto plaintext=storage::Load(path,65536);if(!plaintext)return {};
    wire::Reader reader{plaintext->bytes};
    if(reader.U32()!=0x31435449u||reader.U32()!=1)throw std::runtime_error("settings version");
    const auto enabled=reader.U32();const auto identity=reader.U64();const auto key_size=reader.U32();
    if(enabled>1||!identity||key_size>512||plaintext->bytes.size()!=24+key_size)throw std::runtime_error("settings content");
    const auto key=reader.Data(key_size);
    if(!std::all_of(key.begin(),key.end(),[](uint8_t c){return c>=33&&c<=126;})||(enabled&&key.empty()))throw std::runtime_error("settings key format");
    Settings result;result.enabled=enabled!=0;result.identity=identity;result.key.bytes.assign(key.begin(),key.end());return result;
}
inline uint64_t SaveSettings(const std::wstring& path,bool enabled,std::string_view key){
    if(key.size()>512||(enabled&&key.empty())||!std::all_of(key.begin(),key.end(),[](unsigned char c){return c>=33&&c<=126;}))
        throw std::runtime_error("settings key format");
    uint64_t identity{};
    while(!identity)if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&identity),sizeof identity,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("settings identity");
    wire::Writer writer;writer.U32(0x31435449u);writer.U32(1);writer.U32(enabled?1:0);writer.U64(identity);writer.U32(static_cast<uint32_t>(key.size()));
    writer.Data({reinterpret_cast<const uint8_t*>(key.data()),key.size()});
    Secret plaintext;plaintext.bytes=std::move(writer.bytes);storage::Save(path,plaintext.bytes,L"TransInput settings");
    return identity;
}
}
