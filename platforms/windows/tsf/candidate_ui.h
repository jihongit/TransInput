// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <utility>
namespace transinput {
enum class UiAction {Select,Finalize,Abort};
// STA object. End disconnects external references before releasing the document.
// Retained host references keep the DLL alive but cannot act on a later session.
class CandidateUi final:public ITfCandidateListUIElementBehavior {
    std::atomic<ULONG> refs_{1};std::atomic<long>* leases_;
    DWORD thread_{GetCurrentThreadId()};bool connected_{true};BOOL shown_{TRUE};
    std::vector<std::wstring> rows_;std::vector<UINT> pages_{0};UINT selected_{};uint64_t generation_{};
    Microsoft::WRL::ComPtr<ITfDocumentMgr> document_;
    std::function<HRESULT(UiAction,size_t,uint64_t)> action_;
    std::function<void(BOOL)> show_;
    HRESULT Check()const noexcept{return GetCurrentThreadId()!=thread_?RPC_E_WRONG_THREAD:connected_?S_OK:CO_E_OBJNOTCONNECTED;}
public:
    CandidateUi(std::atomic<long>* leases,std::function<HRESULT(UiAction,size_t,uint64_t)> action,std::function<void(BOOL)> show):leases_(leases),action_(std::move(action)),show_(std::move(show)){++*leases_;}
    ~CandidateUi(){--*leases_;}
    void Update(std::vector<std::wstring> rows,UINT selected,uint64_t generation,ITfDocumentMgr* document){rows_=std::move(rows);selected_=rows_.empty()?0:std::min(selected,static_cast<UINT>(rows_.size()-1));generation_=generation;document_=document;pages_={0};}
    void Disconnect()noexcept{connected_=false;action_={};show_={};document_.Reset();rows_.clear();}
    BOOL Allowed()const noexcept{return connected_&&shown_;}
    void InitialVisibility(BOOL shown)noexcept{shown_=shown;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** value)override{
        if(!value)return E_POINTER;*value=nullptr;
        if(iid==IID_IUnknown||iid==IID_ITfUIElement||iid==IID_ITfCandidateListUIElement||iid==IID_ITfCandidateListUIElementBehavior)*value=static_cast<ITfCandidateListUIElementBehavior*>(this);else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto count=--refs_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR* value)override{if(!value)return E_POINTER;*value=SysAllocString(L"TransInput 候选");return *value?S_OK:E_OUTOFMEMORY;}
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* value)override{if(!value)return E_POINTER;*value={0x9d9ae887,0x12ae,0x44b2,{0x83,0xa5,0xd1,0x74,0x5a,0xe0,0x16,0x79}};return S_OK;}
    HRESULT STDMETHODCALLTYPE Show(BOOL value)override{try{Microsoft::WRL::ComPtr<CandidateUi> lifetime(this);const auto status=Check();if(FAILED(status))return status;shown_=value!=FALSE;auto callback=show_;if(callback)callback(shown_);return S_OK;}catch(...){return E_OUTOFMEMORY;}}
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* value)override{if(!value)return E_POINTER;const auto status=Check();if(FAILED(status))return status;*value=shown_;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* value)override{if(!value)return E_POINTER;const auto status=Check();if(FAILED(status))return status;*value=TF_CLUIE_DOCUMENTMGR|TF_CLUIE_COUNT|TF_CLUIE_SELECTION|TF_CLUIE_STRING|TF_CLUIE_PAGEINDEX|TF_CLUIE_CURRENTPAGE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** value)override{if(!value)return E_POINTER;*value=nullptr;const auto status=Check();if(FAILED(status))return status;return document_.CopyTo(value);}
    HRESULT STDMETHODCALLTYPE GetCount(UINT* value)override{if(!value)return E_POINTER;const auto status=Check();if(FAILED(status))return status;*value=static_cast<UINT>(rows_.size());return S_OK;}
    HRESULT STDMETHODCALLTYPE GetSelection(UINT* value)override{if(!value)return E_POINTER;const auto status=Check();if(FAILED(status))return status;*value=selected_;return rows_.empty()?S_FALSE:S_OK;}
    HRESULT STDMETHODCALLTYPE GetString(UINT index,BSTR* value)override{if(!value)return E_POINTER;*value=nullptr;const auto status=Check();if(FAILED(status))return status;if(index>=rows_.size())return E_INVALIDARG;*value=SysAllocStringLen(rows_[index].data(),static_cast<UINT>(rows_[index].size()));return *value?S_OK:E_OUTOFMEMORY;}
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* indexes,UINT capacity,UINT* count)override{if(!count||(!indexes&&capacity))return E_POINTER;const auto status=Check();if(FAILED(status))return status;*count=static_cast<UINT>(pages_.size());for(UINT i=0;i<capacity&&i<pages_.size();++i)indexes[i]=pages_[i];return capacity<pages_.size()?S_FALSE:S_OK;}
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT* indexes,UINT count)override{const auto status=Check();if(FAILED(status))return status;if(!indexes||!count||count>rows_.size()||indexes[0]!=0)return E_INVALIDARG;
        for(UINT i=0;i<count;++i)if(indexes[i]>=rows_.size()||(i&&indexes[i]<=indexes[i-1]))return E_INVALIDARG;
        try{pages_.assign(indexes,indexes+count);return S_OK;}catch(...){return E_OUTOFMEMORY;}}
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* value)override{if(!value)return E_POINTER;const auto status=Check();if(FAILED(status))return status;*value=static_cast<UINT>(std::upper_bound(pages_.begin(),pages_.end(),selected_)-pages_.begin()-1);return S_OK;}
    HRESULT STDMETHODCALLTYPE SetSelection(UINT value)override{try{Microsoft::WRL::ComPtr<CandidateUi> lifetime(this);const auto status=Check();if(FAILED(status))return status;if(value>=rows_.size())return E_INVALIDARG;auto callback=action_;const auto result=callback?callback(UiAction::Select,value,generation_):CO_E_OBJNOTCONNECTED;if(SUCCEEDED(result)&&connected_)selected_=value;return result;}catch(...){return E_OUTOFMEMORY;}}
    HRESULT STDMETHODCALLTYPE Finalize()override{try{Microsoft::WRL::ComPtr<CandidateUi> lifetime(this);const auto status=Check();if(FAILED(status))return status;if(rows_.empty())return E_UNEXPECTED;auto callback=action_;return callback?callback(UiAction::Finalize,selected_,generation_):CO_E_OBJNOTCONNECTED;}catch(...){return E_OUTOFMEMORY;}}
    HRESULT STDMETHODCALLTYPE Abort()override{try{Microsoft::WRL::ComPtr<CandidateUi> lifetime(this);const auto status=Check();if(FAILED(status))return status;auto callback=action_;return callback?callback(UiAction::Abort,0,generation_):CO_E_OBJNOTCONNECTED;}catch(...){return E_OUTOFMEMORY;}}
};
class CandidateUiSession {
    Microsoft::WRL::ComPtr<ITfUIElementMgr> manager_;
    Microsoft::WRL::ComPtr<CandidateUi> element_;DWORD id_{};bool active_{},allowed_{true},updating_{};
public:
    void Manager(IUnknown* manager){manager_.Reset();if(manager)manager->QueryInterface(IID_PPV_ARGS(&manager_));}
    bool Allowed()const noexcept{return element_?element_->Allowed()!=FALSE:allowed_;}
    void End()noexcept{auto element=std::move(element_);const auto active=std::exchange(active_,false);const auto id=id_;allowed_=true;if(element)element->Disconnect();if(active&&manager_)manager_->EndUIElement(id);}
    ~CandidateUiSession(){End();}
    bool Publish(std::vector<std::wstring> rows,UINT selected,uint64_t generation,ITfDocumentMgr* document,std::atomic<long>* leases,
        const std::function<HRESULT(UiAction,size_t,uint64_t)>& action,const std::function<void(BOOL)>& show){
        // A query in flight has no actionable rows, but is still the same
        // composition. Preserve the host's visibility decision and UI identity.
        // Only the composition owner may End; never expose stale rows meanwhile.
        if(!manager_||updating_)return Allowed();if(rows.empty()&&!active_)return Allowed();
        updating_=true;struct Reset {bool& flag;~Reset(){flag=false;}} reset{updating_};
        if(!element_)element_.Attach(new CandidateUi(leases,action,show));auto current=element_;
        current->Update(std::move(rows),selected,generation,document);
        if(!active_){BOOL allowed=TRUE;DWORD id{};const auto result=manager_->BeginUIElement(current.Get(),&allowed,&id);
            if(current.Get()!=element_.Get()){if(SUCCEEDED(result))manager_->EndUIElement(id);return false;}
            allowed_=SUCCEEDED(result)&&allowed;if(FAILED(result)){current->Disconnect();element_.Reset();return false;}current->InitialVisibility(allowed);id_=id;active_=true;
        }
        manager_->UpdateUIElement(id_);return Allowed();
    }
};
}
