// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "pipe.h"
#include <wincrypt.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <optional>
namespace transinput::storage {
constexpr size_t MaxPlaintext=512*1024,MaxEncrypted=1024*1024;
struct Secret {
    std::vector<uint8_t> bytes;
    Secret()=default;
    explicit Secret(std::string_view text):bytes(text.begin(),text.end()){}
    Secret(const Secret&)=delete;Secret& operator=(const Secret&)=delete;
    Secret(Secret&& other)noexcept:bytes(std::move(other.bytes)){}
    Secret& operator=(Secret&& other)noexcept{if(this!=&other){Clear();bytes=std::move(other.bytes);}return *this;}
    void Clear()noexcept{if(!bytes.empty())SecureZeroMemory(bytes.data(),bytes.size());bytes.clear();}
    ~Secret(){Clear();}
    std::string_view View()const noexcept{return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};}
};
struct ProtectedMemory {
    DATA_BLOB blob{};
    ~ProtectedMemory(){if(blob.pbData){SecureZeroMemory(blob.pbData,blob.cbData);LocalFree(blob.pbData);}}
};
inline std::wstring UserSid(){
    pipe::Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))throw std::runtime_error("profile user token");
    DWORD size{};GetTokenInformation(token.value,TokenUser,nullptr,0,&size);
    if(!size||size>65536)throw std::runtime_error("profile token size");
    std::vector<uint8_t> bytes(size);
    if(!GetTokenInformation(token.value,TokenUser,bytes.data(),size,&size))throw std::runtime_error("profile user identity");
    LPWSTR text{};if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text))throw std::runtime_error("profile user SID");
    pipe::LocalAllocation owned{text};return text;
}
struct UserSecurity {
    pipe::LocalAllocation descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES),nullptr,FALSE};
    UserSecurity(){
        const auto sddl=L"D:P(A;;GA;;;"+UserSid()+L")";PSECURITY_DESCRIPTOR value{};
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&value,nullptr))throw std::runtime_error("profile security descriptor");
        descriptor.value=value;attributes.lpSecurityDescriptor=value;
    }
};
inline std::wstring DataDirectory(){
    PWSTR directory{};if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&directory)))throw std::runtime_error("profile directory");
    std::wstring path(directory);CoTaskMemFree(directory);return path+L"\\TransInputNext";
}
inline std::optional<Secret> Load(const std::wstring& path,size_t max_plaintext=MaxPlaintext){
    if(!max_plaintext||max_plaintext>MaxPlaintext)throw std::runtime_error("profile size limit");
    pipe::Handle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file){if(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND)return {};throw std::runtime_error("profile open");}
    FILE_ATTRIBUTE_TAG_INFO tag{};LARGE_INTEGER length{};
    if(!GetFileInformationByHandleEx(file.value,FileAttributeTagInfo,&tag,sizeof tag)||(tag.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)||
        !GetFileSizeEx(file.value,&length)||length.QuadPart<=0||length.QuadPart>static_cast<LONGLONG>(MaxEncrypted))throw std::runtime_error("profile file format");
    std::vector<uint8_t> encrypted(static_cast<size_t>(length.QuadPart));DWORD count{};
    if(!ReadFile(file.value,encrypted.data(),static_cast<DWORD>(encrypted.size()),&count,nullptr)||count!=encrypted.size())throw std::runtime_error("profile read");
    DATA_BLOB input{count,encrypted.data()};ProtectedMemory plain;
    if(!CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain.blob))throw std::runtime_error("profile decryption");
    if(!plain.blob.cbData||plain.blob.cbData>max_plaintext)throw std::runtime_error("profile plaintext size");
    Secret result;result.bytes.assign(plain.blob.pbData,plain.blob.pbData+plain.blob.cbData);return result;
}
inline void Save(const std::wstring& path,std::span<const uint8_t> bytes,const wchar_t* description){
    if(bytes.empty()||bytes.size()>MaxPlaintext)throw std::runtime_error("profile plaintext size");
    DATA_BLOB input{static_cast<DWORD>(bytes.size()),const_cast<BYTE*>(bytes.data())};ProtectedMemory encrypted;
    if(!CryptProtectData(&input,description,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted.blob))throw std::runtime_error("profile encryption");
    if(encrypted.blob.cbData>MaxEncrypted)throw std::runtime_error("profile encrypted size");
    UserSecurity security;const auto directory=pipe::ParentPath(path);
    if(!CreateDirectoryW(directory.c_str(),&security.attributes)&&GetLastError()!=ERROR_ALREADY_EXISTS)throw std::runtime_error("profile create directory");
    GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("profile temporary identity");
    wchar_t suffix[40]{};if(!StringFromGUID2(guid,suffix,40))throw std::runtime_error("profile temporary name");
    const auto temporary=path+L".tmp-"+suffix;
    pipe::Handle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,&security.attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,nullptr));
    if(!file)throw std::runtime_error("profile temporary file");
    try{
        DWORD count{};
        if(!WriteFile(file.value,encrypted.blob.pbData,encrypted.blob.cbData,&count,nullptr)||count!=encrypted.blob.cbData||!FlushFileBuffers(file.value))throw std::runtime_error("profile atomic write");
        file.Reset();if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("profile atomic replace");
    }catch(...){file.Reset();DeleteFileW(temporary.c_str());throw;}
}
}
