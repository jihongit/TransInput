// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <stdexcept>
namespace transinput {
// No DLL-lifetime counter: unloading and reloading in the same host must not
// restart IDs while the translator still retains old client revisions. IDs are
// opaque 64-bit nonces; pipe identity/access control remains independent.
inline uint64_t NewSessionId(){
    for(unsigned attempt=0;attempt<2;++attempt){
        uint64_t value{};
        if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&value),sizeof value,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("session identity unavailable");
        if(value)return value;
    }
    throw std::runtime_error("session identity exhausted");
}
}
