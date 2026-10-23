// SPDX-License-Identifier: AGPL-3.0-only
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include "../tsf/identity.h"
#include "candidate_test_interface.h"

using Microsoft::WRL::ComPtr;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using CanUnload = HRESULT(WINAPI*)();
using GetClass = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { std::fprintf(stderr, "load error %lu\n", GetLastError()); return 3; }
    try {
        const auto can_unload = std::bit_cast<CanUnload>(GetProcAddress(module, "DllCanUnloadNow"));
        const auto get_class = std::bit_cast<GetClass>(GetProcAddress(module, "DllGetClassObject"));
        Require(can_unload && get_class, "missing COM exports");
        Require(can_unload() == S_OK, "fresh module cannot unload");
        std::unordered_set<uint64_t> sessions;
        const bool fixture=std::wstring(argv[1]).find(L"TransInputPrototypeTest.dll")!=std::wstring::npos;
        for (unsigned round = 0; round < 1000; ++round) {
            ComPtr<IClassFactory> factory;
            Require(get_class(transinput::ClassId, IID_PPV_ARGS(&factory)) == S_OK, "factory unavailable");
            Require(can_unload() == S_FALSE, "factory not counted");
            Require(factory->LockServer(TRUE) == S_OK, "server lock failed");
            Require(factory->LockServer(FALSE) == S_OK, "server unlock failed");
            Require(factory->LockServer(FALSE) == E_UNEXPECTED, "server lock underflow accepted");
            ComPtr<ITfTextInputProcessorEx> service;
            Require(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service)) == S_OK, "TIP unavailable");
            ComPtr<ITestCandidateView> test_view;
            const auto test_interface=service.As(&test_view);
            Require(fixture?test_interface==S_OK:test_interface==E_NOINTERFACE,"production/test candidate interface boundary incorrect");
            if(fixture){uint64_t identity{};Require(test_view->FreshSession(&identity)==S_OK&&identity&&sessions.insert(identity).second,"session identity reused within DLL");}
            test_view.Reset();
            factory.Reset();
            Require(can_unload() == S_FALSE, "TIP not counted");
            Require(service->Activate(nullptr, TF_CLIENTID_NULL) == E_INVALIDARG, "invalid activation accepted");
            ComPtr<ITfKeyEventSink> keys;
            Require(service.As(&keys) == S_OK, "key sink unavailable");
            BOOL eaten = TRUE;
            Require(keys->OnTestKeyDown(nullptr, 'A', 0, &eaten) == S_OK && eaten == FALSE, "inactive TIP consumed input");
            Require(keys->OnKeyDown(nullptr, 'A', 0, &eaten) == S_OK && eaten == FALSE, "null context consumed input");
            Require(keys->OnKeyDown(nullptr, 'A', 0, nullptr) == E_POINTER, "null output accepted");
            Require(keys->OnTestKeyUp(nullptr, VK_SPACE, 0, &eaten) == S_OK && eaten == FALSE, "inactive keyup consumed");
            Require(service->Deactivate() == S_OK && service->Deactivate() == S_OK, "deactivate not idempotent");
            keys.Reset(); service.Reset();
            Require(can_unload() == S_OK, "COM object/reference leaked");
        }
        Require(FreeLibrary(std::exchange(module,nullptr)) != FALSE, "FreeLibrary failed");
        Require(GetModuleHandleW(argv[1]) == nullptr, "module still mapped");
        if(fixture){
            module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);Require(module!=nullptr,"reload test DLL");
            const auto reloaded=std::bit_cast<GetClass>(GetProcAddress(module,"DllGetClassObject"));
            const auto unloaded=std::bit_cast<CanUnload>(GetProcAddress(module,"DllCanUnloadNow"));Require(reloaded&&unloaded,"reload exports");
            {ComPtr<IClassFactory> factory;Require(reloaded(transinput::ClassId,IID_PPV_ARGS(&factory))==S_OK,"reload factory");
                ComPtr<ITestCandidateView> view;Require(factory->CreateInstance(nullptr,IID_PPV_ARGS(&view))==S_OK,"reload candidate test interface");
                uint64_t identity{};Require(view->FreshSession(&identity)==S_OK&&identity&&sessions.insert(identity).second,"DLL reload reused a prior session identity");}
            Require(unloaded()==S_OK,"reloaded module leaked");Require(FreeLibrary(std::exchange(module,nullptr))!=FALSE,"reload FreeLibrary");
        }
        std::puts("PASS: 1000 COM lifecycles, production/test interfaces, invalid input, lock balance, inactive keys, unload/reload and fresh session identity");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        if(module)FreeLibrary(module); return 1;
    }
}
