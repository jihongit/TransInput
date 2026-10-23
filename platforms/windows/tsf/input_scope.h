// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <msctf.h>
#include <initguid.h>
#include <inputscope.h>
#include <wrl/client.h>
namespace transinput {
enum class Scope {Unknown,Allowed,Blocked};
inline bool TranslatableScope(InputScope scope)noexcept{
    switch(scope){
    case IS_DEFAULT:case IS_TEXT:case IS_CHAT:case IS_CHAT_WITHOUT_EMOJI:
    case IS_SEARCH:case IS_SEARCH_INCREMENTAL:case IS_CHINESE_HALFWIDTH:case IS_CHINESE_FULLWIDTH:case IS_NATIVE_SCRIPT:return true;
    default:return false;
    }
}
inline Scope ReadScope(ITfContext* context,ITfRange* range,TfEditCookie cookie)noexcept{
    if(!context||!range)return Scope::Unknown;
    Microsoft::WRL::ComPtr<ITfContextView> view;
    if(SUCCEEDED(context->GetActiveView(&view))&&view){
        HWND window{};wchar_t name[64]{};
        if(SUCCEEDED(view->GetWnd(&window))&&window&&GetClassNameW(window,name,64)){
            const bool edit=lstrcmpiW(name,L"Edit")==0||CompareStringOrdinal(name,8,L"RichEdit",8,TRUE)==CSTR_EQUAL;
            if(edit&&(GetWindowLongPtrW(window,GWL_STYLE)&ES_PASSWORD))return Scope::Blocked;
        }
    }
    Microsoft::WRL::ComPtr<ITfReadOnlyProperty> property;
    if(FAILED(context->GetAppProperty(GUID_PROP_INPUTSCOPE,&property))||!property)return Scope::Unknown;
    VARIANT value{};const auto status=property->GetValue(cookie,range,&value);
    Scope result=Scope::Unknown;
    if(SUCCEEDED(status)){
        // An empty app property is the unmarked/default scope. Applications
        // that omit sensitivity metadata cannot be identified with certainty.
        if(value.vt==VT_EMPTY)result=Scope::Allowed;
        else if(value.vt==VT_UNKNOWN&&value.punkVal){
            Microsoft::WRL::ComPtr<ITfInputScope> scopes;
            if(SUCCEEDED(value.punkVal->QueryInterface(IID_PPV_ARGS(&scopes)))){
                InputScope* items{};UINT count{};
                if(SUCCEEDED(scopes->GetInputScopes(&items,&count))&&count<=256&&(!count||items)){
                    result=Scope::Allowed;for(UINT i=0;i<count;++i)if(!TranslatableScope(items[i])){result=Scope::Blocked;break;}
                }
                CoTaskMemFree(items);
            }
        }
    }
    VariantClear(&value);return result;
}
}
