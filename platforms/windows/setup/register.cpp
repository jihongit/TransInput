// SPDX-License-Identifier: AGPL-3.0-only
// Official TSF deployment helper. Mutating actions are unavailable to unsigned
// development builds and are not executed on the current company computer.
#define NOMINMAX
#include <windows.h>
#include <msctf.h>
#include <shlobj.h>
#include <wintrust.h>
#include <softpub.h>
#include <wrl/client.h>
#include <algorithm>
#include <bit>
#include <cstdio>
#include <functional>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <iterator>
#include <vector>
#include "../tsf/identity.h"
using Microsoft::WRL::ComPtr;
namespace {
constexpr LANGID Language=0x0804;
const std::wstring ClassKey=L"SOFTWARE\\Classes\\CLSID\\{40E2B4B8-B417-47D3-AACF-F49A77858E40}";
constexpr wchar_t Layout[]=L"0x0804:{40E2B4B8-B417-47D3-AACF-F49A77858E40}{AE1C8AF6-3CE7-45A5-B256-8B6FE28037F3}";
struct Step {std::function<HRESULT()> apply,undo;};
HRESULT Run(std::vector<Step>& steps,unsigned& undo_failures){
    for(size_t i=0;i<steps.size();++i){const auto result=steps[i].apply();if(FAILED(result)){for(size_t j=i+1;j>0;--j)if(FAILED(steps[j-1].undo()))++undo_failures;return result;}}
    return S_OK;
}
LONG Trusted(const std::wstring& path){WINTRUST_FILE_INFO file{sizeof file};file.pcwszFilePath=path.c_str();WINTRUST_DATA data{sizeof data};data.dwUIChoice=WTD_UI_NONE;data.fdwRevocationChecks=WTD_REVOKE_WHOLECHAIN;data.dwUnionChoice=WTD_CHOICE_FILE;data.pFile=&file;data.dwStateAction=WTD_STATEACTION_VERIFY;data.dwProvFlags=WTD_CACHE_ONLY_URL_RETRIEVAL|WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;GUID policy=WINTRUST_ACTION_GENERIC_VERIFY_V2;const auto result=WinVerifyTrust(nullptr,&policy,&data);data.dwStateAction=WTD_STATEACTION_CLOSE;WinVerifyTrust(nullptr,&policy,&data);return result;}
bool Product(const std::wstring& path){DWORD unused{};const auto size=GetFileVersionInfoSizeW(path.c_str(),&unused);if(!size||size>1024*1024)return false;std::vector<uint8_t> data(size);if(!GetFileVersionInfoW(path.c_str(),0,size,data.data()))return false;void* text{};UINT length{};return VerQueryValueW(data.data(),L"\\StringFileInfo\\040904b0\\ProductName",&text,&length)&&length&&std::wstring(static_cast<const wchar_t*>(text))==L"TransInput";}
std::wstring Module(){wchar_t path[32768]{};const auto length=GetModuleFileNameW(nullptr,path,32768);if(!length||length>=32768)throw std::runtime_error("module path");return path;}
bool Elevated(){HANDLE token{};if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))return false;TOKEN_ELEVATION value{};DWORD size{};const auto read=GetTokenInformation(token,TokenElevation,&value,sizeof value,&size);CloseHandle(token);return read&&value.TokenIsElevated;}
bool ProtectedLocation(const std::wstring& path){PWSTR root{};auto result=SHGetKnownFolderPath(FOLDERID_ProgramFilesX64,0,nullptr,&root);if(FAILED(result))result=SHGetKnownFolderPath(FOLDERID_ProgramFiles,0,nullptr,&root);if(FAILED(result)||!root)return false;const auto prefix=std::wstring(root)+L"\\TransInput\\";CoTaskMemFree(root);return path.size()>prefix.size()&&_wcsnicmp(path.c_str(),prefix.c_str(),prefix.size())==0;}
HRESULT ClassPath(std::wstring& value){wchar_t text[32768]{};DWORD size=sizeof text;const auto result=RegGetValueW(HKEY_LOCAL_MACHINE,(ClassKey+L"\\InprocServer32").c_str(),nullptr,RRF_RT_REG_SZ,nullptr,text,&size);if(result!=ERROR_SUCCESS)return HRESULT_FROM_WIN32(result);value=text;return S_OK;}
HRESULT HasCategory(ITfCategoryMgr* manager,REFGUID category,bool& found){ComPtr<IEnumGUID> values;const auto result=manager->EnumCategoriesInItem(transinput::ClassId,&values);if(FAILED(result))return result;GUID value{};ULONG count{};found=false;for(unsigned i=0;i<128;++i){const auto next=values->Next(1,&value,&count);if(next!=S_OK)return next==S_FALSE?S_OK:next;if(value==category){found=true;return S_OK;}}return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}
HRESULT Install(const std::wstring& dll,unsigned& undo_failures){
    ComPtr<ITfInputProcessorProfileMgr> profiles;ComPtr<ITfCategoryMgr> categories;
    auto result=CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&profiles));if(FAILED(result))return result;
    result=CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&categories));if(FAILED(result))return result;
    std::wstring previous;const auto read=ClassPath(previous);const bool existing=SUCCEEDED(read);
    if(existing&&_wcsicmp(previous.c_str(),dll.c_str())!=0)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if(FAILED(read)&&read!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)&&read!=HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))return read;
    TF_INPUTPROCESSORPROFILE old{};const bool profile_exists=profiles->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR,Language,transinput::ClassId,transinput::ProfileId,nullptr,&old)==S_OK;
    if(profile_exists&&!existing)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    const GUID wanted[]{GUID_TFCAT_TIP_KEYBOARD,GUID_TFCAT_TIPCAP_UIELEMENTENABLED};bool category_exists[2]{};
    for(size_t i=0;i<2;++i){result=HasCategory(categories.Get(),wanted[i],category_exists[i]);if(FAILED(result))return result;}
    bool class_created{},profile_attempted{},category_attempted[2]{};std::vector<Step> steps;
    steps.push_back({[&]()->HRESULT{if(existing)return S_OK;HKEY key{};DWORD disposition{};auto status=RegCreateKeyExW(HKEY_LOCAL_MACHINE,ClassKey.c_str(),0,nullptr,0,KEY_WRITE,nullptr,&key,&disposition);if(status)return HRESULT_FROM_WIN32(status);class_created=disposition==REG_CREATED_NEW_KEY;RegCloseKey(key);if(!class_created)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        status=RegCreateKeyExW(HKEY_LOCAL_MACHINE,(ClassKey+L"\\InprocServer32").c_str(),0,nullptr,0,KEY_WRITE,nullptr,&key,nullptr);if(status)return HRESULT_FROM_WIN32(status);
        status=RegSetValueExW(key,nullptr,0,REG_SZ,reinterpret_cast<const BYTE*>(dll.c_str()),static_cast<DWORD>((dll.size()+1)*sizeof(wchar_t)));
        const wchar_t model[]=L"Apartment";if(!status)status=RegSetValueExW(key,L"ThreadingModel",0,REG_SZ,reinterpret_cast<const BYTE*>(model),sizeof model);RegCloseKey(key);return HRESULT_FROM_WIN32(status);},
        [&]()->HRESULT{return class_created?HRESULT_FROM_WIN32(RegDeleteTreeW(HKEY_LOCAL_MACHINE,ClassKey.c_str())):S_OK;}});
    steps.push_back({[&]()->HRESULT{if(profile_exists)return S_OK;profile_attempted=true;return profiles->RegisterProfile(transinput::ClassId,Language,transinput::ProfileId,L"TransInput",10,dll.c_str(),static_cast<ULONG>(dll.size()),0,nullptr,0,FALSE,0);},
        [&]()->HRESULT{return profile_attempted?profiles->UnregisterProfile(transinput::ClassId,Language,transinput::ProfileId,0):S_OK;}});
    for(size_t i=0;i<2;++i)steps.push_back({[&,i]()->HRESULT{if(category_exists[i])return S_OK;category_attempted[i]=true;return categories->RegisterCategory(transinput::ClassId,wanted[i],transinput::ClassId);},[&,i]()->HRESULT{return category_attempted[i]?categories->UnregisterCategory(transinput::ClassId,wanted[i],transinput::ClassId):S_OK;}});
    return Run(steps,undo_failures);
}
HRESULT Uninstall(const std::wstring& dll){
    std::wstring existing;const auto read=ClassPath(existing);if(read==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)||read==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))return S_OK;if(FAILED(read))return read;
    if(_wcsicmp(existing.c_str(),dll.c_str())!=0)return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    ComPtr<ITfInputProcessorProfileMgr> profiles;ComPtr<ITfCategoryMgr> categories;
    auto result=CoCreateInstance(CLSID_TF_InputProcessorProfiles,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&profiles));if(FAILED(result))return result;
    result=CoCreateInstance(CLSID_TF_CategoryMgr,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&categories));if(FAILED(result))return result;
    TF_INPUTPROCESSORPROFILE profile{};if(profiles->GetProfile(TF_PROFILETYPE_INPUTPROCESSOR,Language,transinput::ClassId,transinput::ProfileId,nullptr,&profile)==S_OK){result=profiles->UnregisterProfile(transinput::ClassId,Language,transinput::ProfileId,0);if(FAILED(result))return result;}
    for(const auto& value:{GUID_TFCAT_TIPCAP_UIELEMENTENABLED,GUID_TFCAT_TIP_KEYBOARD}){bool found{};result=HasCategory(categories.Get(),value,found);if(FAILED(result))return result;if(found){result=categories->UnregisterCategory(transinput::ClassId,value,transinput::ClassId);if(FAILED(result))return result;}}
    return HRESULT_FROM_WIN32(RegDeleteTreeW(HKEY_LOCAL_MACHINE,ClassKey.c_str()));
}
HRESULT Enable(bool enabled){const auto module=LoadLibraryExW(L"input.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!module)return HRESULT_FROM_WIN32(GetLastError());using Install=BOOL(WINAPI*)(LPCWSTR,DWORD);const auto install=std::bit_cast<Install>(GetProcAddress(module,"InstallLayoutOrTip"));const auto result=install&&install(Layout,enabled?0u:1u);FreeLibrary(module);return result?S_OK:E_FAIL;}
bool SelfCheck(){
    for(size_t fail=0;fail<4;++fail){bool values[4]{};std::vector<Step> steps;unsigned failures{};for(size_t i=0;i<4;++i)steps.push_back({[&,i](){values[i]=true;return i==fail?E_FAIL:S_OK;},[&,i](){values[i]=false;return S_OK;}});if(Run(steps,failures)!=E_FAIL||failures||std::any_of(std::begin(values),std::end(values),[](bool value){return value;}))return false;}
    return true;
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;const std::wstring action=argv[1];
    if(action==L"--self-check"){if(!SelfCheck())return 1;std::puts("PASS: transaction failure at each stage rolls back partial/current and earlier changes; no system registration performed");return 0;}
    if(action==L"--plan"){std::puts("{\"schema\":1,\"machine_actions\":[\"own COM class\",\"official TSF RegisterProfile\",\"keyboard and UIElement categories\"],\"user_enable_separate\":true,\"sets_default\":false,\"creates_start_menu\":false,\"preserves_user_data\":true,\"executed\":false}");return 0;}
    if(action!=L"--register-signed"&&action!=L"--unregister-signed"&&action!=L"--enable-signed"&&action!=L"--disable-signed")return 2;
    try{
        const auto self=Module(),dll=self.substr(0,self.find_last_of(L"\\/"))+L"\\TransInputPrototype.dll";
        if(!ProtectedLocation(self)||Trusted(self)!=ERROR_SUCCESS||Trusted(dll)!=ERROR_SUCCESS||!Product(self)||!Product(dll)){std::fputs("BLOCKED: trusted signed TransInput deployment under Program Files is required; no registration performed\n",stderr);return 5;}
        const bool machine=action==L"--register-signed"||action==L"--unregister-signed";if(machine&&!Elevated()){std::fputs("BLOCKED: use the company-approved deployment channel; no elevation requested\n",stderr);return 6;}
        const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 7;unsigned rollback_failures{};
        const auto result=action==L"--register-signed"?Install(dll,rollback_failures):action==L"--unregister-signed"?Uninstall(dll):Enable(action==L"--enable-signed");CoUninitialize();
        std::printf("{\"hresult\":\"0x%08lx\",\"rollback_failures\":%u,\"sets_default\":false}\n",static_cast<unsigned long>(result),rollback_failures);return SUCCEEDED(result)?0:1;
    }catch(...){std::fputs("FAIL: deployment helper error; inspect project-owned registration before retrying\n",stderr);return 1;}
}
