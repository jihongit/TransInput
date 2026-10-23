// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "../shared/pipe.h"
#include "transinput_core.h"
#include "transinput_translation.h"
#include <bit>
namespace transinput::translation {
struct Core {
    HMODULE module{};
    uint64_t handle{};
    decltype(&ti_translator_create) create{};
    decltype(&ti_translator_destroy) destroy{};
    decltype(&ti_translator_configure) configure{};
    decltype(&ti_translator_observe) observe{};
    decltype(&ti_translator_clear) clear{};
    decltype(&ti_translator_poll) poll{};
    decltype(&ti_translator_state) state{};
    decltype(&ti_translator_finish) finish{};
    decltype(&ti_translator_retry) retry{};
    decltype(&ti_translation_prompt) prompt{};
    decltype(&ti_shutdown) shutdown{};
    template<class Function> Function Symbol(const char* name) {
        const auto address=GetProcAddress(module,name);
        if(!address)throw std::runtime_error("translation ABI symbol");
        return std::bit_cast<Function>(address);
    }
    explicit Core(const std::wstring& path) {
        module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module)throw std::runtime_error("translation core load");
        try {
            if(Symbol<decltype(&ti_abi_version)>("ti_abi_version")()!=TI_ABI_VERSION)throw std::runtime_error("translation ABI version");
#define TI_BIND(name) name=Symbol<decltype(name)>("ti_translator_" #name)
            TI_BIND(create);TI_BIND(destroy);TI_BIND(configure);TI_BIND(observe);TI_BIND(clear);
            TI_BIND(poll);TI_BIND(state);TI_BIND(finish);TI_BIND(retry);
#undef TI_BIND
            prompt=Symbol<decltype(prompt)>("ti_translation_prompt");shutdown=Symbol<decltype(shutdown)>("ti_shutdown");
            handle=create();if(!handle)throw std::runtime_error("translation actor create");
        }catch(...){FreeLibrary(module);module=nullptr;throw;}
    }
    Core(const Core&)=delete;
    Core& operator=(const Core&)=delete;
    ~Core(){if(module){if(handle)destroy(handle);if(shutdown()==0)FreeLibrary(module);}}
    TiTranslation State(bool take_job=false,uint64_t now=GetTickCount64())const {
        TiTranslation value{};
        const auto status=take_job?poll(handle,now,&value,sizeof value):state(handle,&value,sizeof value);
        if(status||value.abi_version!=TI_ABI_VERSION||value.struct_size!=sizeof value)throw std::runtime_error("translation state");
        return value;
    }
    std::string Prompt()const {
        const auto length=prompt(nullptr,0);if(!length||length>4096)throw std::runtime_error("translation prompt size");
        std::string text(length,'\0');prompt(reinterpret_cast<uint8_t*>(text.data()),length);return text;
    }
};
}
