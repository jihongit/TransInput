// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>
#include <msctf.h>
#include <ctfutb.h>
#include <ctffunc.h>
#include <olectl.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <utility>
#include "identity.h"

namespace transinput {
// Official TSF input-mode item. External references retain a module lease but
// become inert on deactivation; no service ownership or background callbacks.
class LanguageButton final:public ITfLangBarItemButton,public ITfSource {
    std::atomic<ULONG> refs_{1};std::atomic<long>* leases_;HMODULE module_;
    DWORD thread_{GetCurrentThreadId()};bool connected_{true},english_{},shown_{true};
    Microsoft::WRL::ComPtr<ITfLangBarItemSink> sink_;
    std::function<HRESULT()> toggle_;
    HRESULT Check()const noexcept{return GetCurrentThreadId()!=thread_?RPC_E_WRONG_THREAD:connected_?S_OK:CO_E_OBJNOTCONNECTED;}
    HRESULT Text(BSTR* value,const wchar_t* text){if(!value)return E_POINTER;*value=nullptr;const auto hr=Check();if(FAILED(hr))return hr;*value=SysAllocString(text);return *value?S_OK:E_OUTOFMEMORY;}
public:
    LanguageButton(std::atomic<long>* leases,HMODULE module,std::function<HRESULT()> toggle):leases_(leases),module_(module),toggle_(std::move(toggle)){++*leases_;}
    ~LanguageButton(){--*leases_;}
    void Disconnect()noexcept{connected_=false;toggle_={};sink_.Reset();}
    void English(bool value)noexcept{if(english_==value)return;english_=value;auto sink=sink_;if(sink)sink->OnUpdate(TF_LBI_TEXT|TF_LBI_TOOLTIP);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(iid==IID_IUnknown||iid==IID_ITfLangBarItem||iid==IID_ITfLangBarItemButton)*out=static_cast<ITfLangBarItemButton*>(this);
        else if(iid==IID_ITfSource)*out=static_cast<ITfSource*>(this);else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto count=--refs_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE GetInfo(TF_LANGBARITEMINFO* info)override{
        if(!info)return E_POINTER;*info={};const auto hr=Check();if(FAILED(hr))return hr;
        info->clsidService=ClassId;info->guidItem=GUID_LBI_INPUTMODE;info->dwStyle=TF_LBI_STYLE_BTN_BUTTON|TF_LBI_STYLE_SHOWNINTRAY;
        wcscpy_s(info->szDescription,L"TransInput TI");return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(DWORD* value)override{if(!value)return E_POINTER;*value=shown_?0:TF_LBI_STATUS_HIDDEN;return Check();}
    HRESULT STDMETHODCALLTYPE Show(BOOL show)override{const auto hr=Check();if(FAILED(hr))return hr;const bool value=show!=FALSE;if(shown_!=value){shown_=value;auto sink=sink_;if(sink)sink->OnUpdate(TF_LBI_STATUS);}return S_OK;}
    HRESULT STDMETHODCALLTYPE GetTooltipString(BSTR* value)override{return Text(value,english_?L"TransInput · 英文 · 单按 Shift 切换中文":L"TransInput · 中文 · 单按 Shift 切换英文");}
    HRESULT STDMETHODCALLTYPE OnClick(TfLBIClick click,POINT,const RECT*)override{const auto hr=Check();if(FAILED(hr))return hr;if(click!=TF_LBI_CLK_LEFT)return S_OK;try{return toggle_?toggle_():S_FALSE;}catch(...){return E_FAIL;}}
    HRESULT STDMETHODCALLTYPE InitMenu(ITfMenu*)override{return Check();}
    HRESULT STDMETHODCALLTYPE OnMenuSelect(UINT)override{return Check();}
    HRESULT STDMETHODCALLTYPE GetIcon(HICON* icon)override{
        if(!icon)return E_POINTER;*icon=nullptr;const auto hr=Check();if(FAILED(hr))return hr;
        *icon=static_cast<HICON>(LoadImageW(module_,MAKEINTRESOURCEW(1),IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),0));return *icon?S_OK:E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE GetText(BSTR* value)override{return Text(value,L"TI");}
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid,IUnknown* object,DWORD* cookie)override{
        if(!cookie)return E_POINTER;*cookie=TF_INVALID_COOKIE;const auto hr=Check();if(FAILED(hr))return hr;
        if(iid!=IID_ITfLangBarItemSink)return CONNECT_E_CANNOTCONNECT;if(!object)return E_INVALIDARG;if(sink_)return CONNECT_E_ADVISELIMIT;
        const auto result=object->QueryInterface(IID_PPV_ARGS(&sink_));if(SUCCEEDED(result))*cookie=1;return result;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD cookie)override{const auto hr=Check();if(FAILED(hr))return hr;if(cookie!=1||!sink_)return CONNECT_E_NOCONNECTION;sink_.Reset();return S_OK;}
};
}
