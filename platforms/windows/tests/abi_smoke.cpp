// SPDX-License-Identifier: AGPL-3.0-only
#include <windows.h>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "transinput_core.h"

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> Function Symbol(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    Require(address != nullptr, "missing C ABI symbol");
    return std::bit_cast<Function>(address);
}
struct Api {
    decltype(&ti_abi_version) version;
    decltype(&ti_snapshot_size) size;
    decltype(&ti_engine_create) create;
    decltype(&ti_engine_destroy) destroy;
    decltype(&ti_shutdown) shutdown;
    decltype(&ti_engine_apply) apply;
    decltype(&ti_engine_snapshot) snapshot;
    decltype(&ti_engine_query) query;
    decltype(&ti_engine_page) page;
    explicit Api(HMODULE module)
        : version(Symbol<decltype(version)>(module, "ti_abi_version")),
          size(Symbol<decltype(size)>(module, "ti_snapshot_size")),
          create(Symbol<decltype(create)>(module, "ti_engine_create")),
          destroy(Symbol<decltype(destroy)>(module, "ti_engine_destroy")),
          shutdown(Symbol<decltype(shutdown)>(module, "ti_shutdown")),
          apply(Symbol<decltype(apply)>(module, "ti_engine_apply")),
          snapshot(Symbol<decltype(snapshot)>(module, "ti_engine_snapshot")),
          query(Symbol<decltype(query)>(module, "ti_engine_query")),
          page(Symbol<decltype(page)>(module, "ti_engine_page")) {}
};
std::string Text(const TiText& text) {
    Require(text.length <= TI_TEXT_CAPACITY, "invalid text length");
    return {reinterpret_cast<const char*>(text.bytes), text.length};
}
void RoundTrip(const Api& api) {
    const auto handle = api.create();
    Require(handle != 0, "create failed");
    TiSnapshot value{};
    for (const char c : std::string("nihao")) {
        const auto status = api.apply(handle, TI_LETTER, static_cast<uint32_t>(c), &value, sizeof value);
        if (status != 0) std::fprintf(stderr, "apply returned %ld; handle=%llu size=%zu align=%zu pointer-mod8=%zu\n",
            static_cast<long>(status), static_cast<unsigned long long>(handle), sizeof value, alignof(TiSnapshot),
            reinterpret_cast<uintptr_t>(&value) % 8);
        Require(status == 0, "apply failed");
    }
    Require(value.abi_version == TI_ABI_VERSION, "wrong snapshot version");
    Require(value.struct_size == sizeof value, "wrong snapshot size");
    Require(value.composing == 1 && Text(value.preedit) == "nihao", "wrong composition");
    Require(value.candidate_count > 0 && Text(value.candidates[0]) == "你好", "wrong Chinese candidate");
    const auto captured = value;
    Require(api.apply(handle, TI_SPACE, 0, &value, sizeof value) == 0, "commit failed");
    Require(value.outcome == 2 && Text(value.commit) == "你好" && value.composing == 0, "wrong commit");
    Require(api.snapshot(handle, &value, sizeof value) == 0 && value.commit.length == 0, "commit replayed");
    Require(api.destroy(handle) == 0, "destroy failed");
    Require(api.destroy(handle) == -2, "stale handle accepted");
    Require(Text(captured.candidates[0]) == "你好", "snapshot depends on destroyed engine");
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::fputs("Usage: abi-smoke.exe absolute-path-to-core.dll\n", stderr); return 2; }
    const auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { std::fprintf(stderr, "LoadLibraryEx failed: %lu\n", GetLastError()); return 3; }
    try {
        Api api(module);
        Require(api.version() == TI_ABI_VERSION && api.size() == sizeof(TiSnapshot), "C/Rust layout mismatch");
        Require(offsetof(TiSnapshot, revision) == 8, "unexpected revision alignment");
        Require(api.snapshot(0, nullptr, 0) == -1, "null buffer not rejected");
        const auto busy = api.create();
        Require(busy != 0 && api.shutdown() == -4, "shutdown ignored active engine");
        Require(api.destroy(busy) == 0, "busy handle cleanup failed");
        const auto query_handle=api.create();TiSnapshot query_snapshot{};
        const auto source=reinterpret_cast<const uint8_t*>("nihao");
        Require(api.query(query_handle,source,5,3,&query_snapshot,sizeof query_snapshot)==0,"whole composition query");
        Require(Text(query_snapshot.preedit)=="nihao"&&query_snapshot.cursor==3&&query_snapshot.commit.length==0,"query edited/committed wrong snapshot");
        Require(api.query(query_handle,source,5,6,&query_snapshot,sizeof query_snapshot)==-1,"invalid query cursor");
        Require(api.query(query_handle,nullptr,1,0,&query_snapshot,sizeof query_snapshot)==-1,"null query source");
        Require(api.snapshot(query_handle,&query_snapshot,sizeof query_snapshot)==0&&Text(query_snapshot.preedit)=="nihao","invalid query mutated state");
        Require(api.destroy(query_handle)==0,"query handle cleanup");
        const auto paging=api.create();TiCandidatePage page{};
        Require(sizeof page==5728&&offsetof(TiCandidatePage,snapshot)==16,"page ABI layout");
        Require(api.query(paging,reinterpret_cast<const uint8_t*>("n"),1,1,&query_snapshot,sizeof query_snapshot)==0,"page query source");
        Require(api.page(paging,1,&page,sizeof page)==0&&page.page==1&&page.total==90&&page.snapshot.candidate_count==9,"native second page");
        const auto choice=Text(page.snapshot.candidates[0]);
        Require(api.page(paging,10,&page,sizeof page)==-1&&api.page(paging,0,&page,1)==-1,"invalid page buffers");
        Require(api.apply(paging,TI_SELECT,0,&query_snapshot,sizeof query_snapshot)==0&&Text(query_snapshot.commit)==choice,"page-local index committed wrong candidate");
        Require(api.destroy(paging)==0,"page cleanup");
        for (unsigned round = 0; round < 1000; ++round) RoundTrip(api);
        std::vector<std::thread> threads;
        std::array<bool, 4> successful{};
        for (size_t i = 0; i < successful.size(); ++i) {
            threads.emplace_back([&api, &successful, i] {
                try { for (unsigned round = 0; round < 100; ++round) RoundTrip(api); successful[i] = true; }
                catch (...) { successful[i] = false; }
            });
        }
        for (auto& thread : threads) thread.join();
        for (const bool passed : successful) Require(passed, "concurrent independent handle test failed");
        Require(api.shutdown() == 0, "library state not drained");
        Require(FreeLibrary(module) != FALSE, "FreeLibrary failed");
        Require(GetModuleHandleW(argv[1]) == nullptr, "DLL remained mapped");
        std::puts("PASS: C/Rust layout, UTF-8, 1400 sessions, stale handles, snapshot ownership, concurrent clients, module unload");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        FreeLibrary(module);
        return 1;
    }
}
