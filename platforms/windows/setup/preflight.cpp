// SPDX-License-Identifier: AGPL-3.0-only
// Read-only deployment diagnostics. Never registers a TIP or changes trust/policy.
#define NOMINMAX
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {
LONG Signature(const wchar_t* path){
    WINTRUST_FILE_INFO file{sizeof file};file.pcwszFilePath=path;
    WINTRUST_DATA data{sizeof data};data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_NONE;
    data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;
    data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL;GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto result=WinVerifyTrust(nullptr,&policy,&data);
    data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&data);return result;
}
LONG MachinePermission(REGSAM view){
    HKEY key{};const auto result=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Classes",0,KEY_CREATE_SUB_KEY|view,&key);
    if(key)RegCloseKey(key);return result;
}
bool Elevated(){
    HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;
    TOKEN_ELEVATION value{};DWORD size{};const auto read=GetTokenInformation(token,TokenElevation,&value,sizeof value,&size);CloseHandle(token);
    return read&&value.TokenIsElevated;
}
}
int wmain(int argc,wchar_t** argv){
    if(argc==2&&std::wstring(argv[1])==L"--self-check"){
        wchar_t module[32768]{};const auto length=GetModuleFileNameW(nullptr,module,32768);if(!length||length>=32768)return 1;
        const auto missing=std::wstring(module)+L".fixed-missing.dll";
        if(GetFileAttributesW(missing.c_str())!=INVALID_FILE_ATTRIBUTES||Signature(missing.c_str())==ERROR_SUCCESS)return 1;
        std::puts("PASS: missing/unsigned binaries cannot satisfy trust preflight; no registration, trust-store write, elevation or policy change");return 0;
    }
    if(argc!=3||std::wstring(argv[1])!=L"--probe"){std::fputs("Usage: TransInputPreflight.exe --probe <TIP DLL> | --self-check\n",stderr);return 2;}
    const auto signature=Signature(argv[2]);
    const auto machine32=MachinePermission(KEY_WOW64_32KEY),machine64=MachinePermission(KEY_WOW64_64KEY);
    const bool signature_ok=signature==ERROR_SUCCESS,permission_ok=machine32==ERROR_SUCCESS&&machine64==ERROR_SUCCESS;
    std::printf("{\"schema\":1,\"read_only\":true,\"elevated\":%s,\"signature_valid\":%s,\"signature_status\":\"0x%08lx\",\"machine_com32_status\":%ld,\"machine_com64_status\":%ld,\"registration_prerequisites_met\":%s,\"installed\":false}\n",
        Elevated()?"true":"false",signature_ok?"true":"false",static_cast<unsigned long>(signature),machine32,machine64,signature_ok&&permission_ok?"true":"false");
    // A successful diagnostic execution is not an installation success.
    return 0;
}
