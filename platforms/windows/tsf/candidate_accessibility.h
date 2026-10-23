// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "candidate_ui.h"
namespace transinput::accessibility {
struct Model {
    DWORD thread{GetCurrentThreadId()};HWND window{};bool active{true},shown{};
    std::atomic<long>* leases{};uint64_t generation{};size_t selected{};RECT bounds{};
    std::vector<std::wstring> rows;std::vector<RECT> rectangles;
    std::function<HRESULT(UiAction,size_t,uint64_t)> action;
    inline static std::atomic<int> next{1};int identity{next.fetch_add(1)};
};
class Provider final:public IRawElementProviderSimple,public IRawElementProviderFragment,public IRawElementProviderFragmentRoot,public ISelectionProvider,public ISelectionItemProvider,public IInvokeProvider {
    std::atomic<ULONG> refs_{1};std::shared_ptr<Model> model_;int row_;uint64_t generation_;
    HRESULT Check()const noexcept{
        if(GetCurrentThreadId()!=model_->thread)return RPC_E_WRONG_THREAD;
        return !model_->active||(row_>=0&&(generation_!=model_->generation||static_cast<size_t>(row_)>=model_->rows.size()))?UIA_E_ELEMENTNOTAVAILABLE:S_OK;
    }
    template<class F> HRESULT Guard(F&& function)noexcept{try{const auto result=Check();return FAILED(result)?result:function();}catch(...){return E_OUTOFMEMORY;}}
    Provider* Make(int row){return new Provider(model_,row);}
    HRESULT Action(UiAction action){return Guard([&]()->HRESULT{
        Microsoft::WRL::ComPtr<Provider> lifetime(this);
        if(row_<0||!model_->shown)return UIA_E_ELEMENTNOTAVAILABLE;
        auto callback=model_->action;const auto result=callback?callback(action,static_cast<size_t>(row_),generation_):UIA_E_ELEMENTNOTAVAILABLE;
        if(SUCCEEDED(result)&&action==UiAction::Select&&model_->active&&model_->generation==generation_){model_->selected=static_cast<size_t>(row_);if(UiaClientsAreListening())UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(this),UIA_SelectionItem_ElementSelectedEventId);}
        return result;
    });}
public:
    Provider(std::shared_ptr<Model> model,int row):model_(std::move(model)),row_(row),generation_(model_->generation){++*model_->leases;}
    ~Provider(){--*model_->leases;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** value)override{
        if(!value)return E_POINTER;*value=nullptr;
        if(iid==IID_IUnknown||iid==__uuidof(IRawElementProviderSimple))*value=static_cast<IRawElementProviderSimple*>(this);
        else if(iid==__uuidof(IRawElementProviderFragment))*value=static_cast<IRawElementProviderFragment*>(this);
        else if(iid==__uuidof(IRawElementProviderFragmentRoot)&&row_<0)*value=static_cast<IRawElementProviderFragmentRoot*>(this);
        else if(iid==__uuidof(ISelectionProvider)&&row_<0)*value=static_cast<ISelectionProvider*>(this);
        else if(iid==__uuidof(ISelectionItemProvider)&&row_>=0)*value=static_cast<ISelectionItemProvider*>(this);
        else if(iid==__uuidof(IInvokeProvider)&&row_>=0)*value=static_cast<IInvokeProvider*>(this);
        else return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto count=--refs_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* value)override{if(!value)return E_POINTER;*value=static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider|ProviderOptions_UseComThreading|ProviderOptions_ProviderOwnsSetFocus);return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id,IUnknown** value)override{if(!value)return E_POINTER;*value=nullptr;return Guard([&]()->HRESULT{
        if(id==UIA_SelectionPatternId&&row_<0)return QueryInterface(__uuidof(ISelectionProvider),reinterpret_cast<void**>(value));
        if(id==UIA_SelectionItemPatternId&&row_>=0)return QueryInterface(__uuidof(ISelectionItemProvider),reinterpret_cast<void**>(value));
        if(id==UIA_InvokePatternId&&row_>=0)return QueryInterface(__uuidof(IInvokeProvider),reinterpret_cast<void**>(value));return S_OK;
    });}
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id,VARIANT* value)override{if(!value)return E_POINTER;VariantInit(value);return Guard([&]()->HRESULT{
        if(id==UIA_NamePropertyId||id==UIA_AutomationIdPropertyId||id==UIA_ClassNamePropertyId||id==UIA_FrameworkIdPropertyId){
            std::wstring text=id==UIA_NamePropertyId?(row_<0?L"TransInput 候选":model_->rows[static_cast<size_t>(row_)]):id==UIA_AutomationIdPropertyId?(row_<0?L"IME_Candidate_Window":L"candidate_"+std::to_wstring(row_)):id==UIA_ClassNamePropertyId?L"TransInput.Candidate":L"Win32";
            value->vt=VT_BSTR;value->bstrVal=SysAllocStringLen(text.data(),static_cast<UINT>(text.size()));return value->bstrVal?S_OK:E_OUTOFMEMORY;
        }
        if(id==UIA_ControlTypePropertyId){value->vt=VT_I4;value->lVal=row_<0?UIA_ListControlTypeId:UIA_ListItemControlTypeId;}
        else if(id==UIA_IsControlElementPropertyId||id==UIA_IsContentElementPropertyId||id==UIA_IsEnabledPropertyId||id==UIA_IsKeyboardFocusablePropertyId||id==UIA_HasKeyboardFocusPropertyId||id==UIA_IsOffscreenPropertyId||id==UIA_SelectionItemIsSelectedPropertyId){
            value->vt=VT_BOOL;bool result=id==UIA_IsControlElementPropertyId||id==UIA_IsContentElementPropertyId||id==UIA_IsEnabledPropertyId;
            if(id==UIA_IsOffscreenPropertyId)result=!model_->shown||(row_>=0&&static_cast<size_t>(row_)<model_->rectangles.size()&&IsRectEmpty(&model_->rectangles[static_cast<size_t>(row_)]));
            if(id==UIA_SelectionItemIsSelectedPropertyId)result=row_>=0&&model_->selected==static_cast<size_t>(row_);value->boolVal=result?VARIANT_TRUE:VARIANT_FALSE;
        }
        return S_OK;
    });}
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** value)override{if(!value)return E_POINTER;*value=nullptr;return Guard([&](){return row_<0&&model_->window?UiaHostProviderFromHwnd(model_->window,value):S_OK;});}
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction,IRawElementProviderFragment** value)override{if(!value)return E_POINTER;*value=nullptr;return Guard([&]()->HRESULT{
        int row=-2;if(row_<0){if(!model_->rows.empty()&&direction==NavigateDirection_FirstChild)row=0;else if(!model_->rows.empty()&&direction==NavigateDirection_LastChild)row=static_cast<int>(model_->rows.size()-1);}
        else if(direction==NavigateDirection_Parent)row=-1;else if(direction==NavigateDirection_NextSibling&&static_cast<size_t>(row_+1)<model_->rows.size())row=row_+1;else if(direction==NavigateDirection_PreviousSibling&&row_>0)row=row_-1;
        if(row!=-2)*value=static_cast<IRawElementProviderFragment*>(Make(row));return S_OK;
    });}
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** value)override{if(!value)return E_POINTER;*value=nullptr;return Guard([&]()->HRESULT{const LONG ids[]{UiaAppendRuntimeId,model_->identity,row_+1};*value=SafeArrayCreateVector(VT_I4,0,3);if(!*value)return E_OUTOFMEMORY;for(LONG i=0;i<3;++i){LONG item=ids[i];if(FAILED(SafeArrayPutElement(*value,&i,&item))){SafeArrayDestroy(*value);*value=nullptr;return E_FAIL;}}return S_OK;});}
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* value)override{if(!value)return E_POINTER;return Guard([&]()->HRESULT{const auto bounds=row_<0?model_->bounds:static_cast<size_t>(row_)<model_->rectangles.size()?model_->rectangles[static_cast<size_t>(row_)]:RECT{};*value={static_cast<double>(bounds.left),static_cast<double>(bounds.top),static_cast<double>(bounds.right-bounds.left),static_cast<double>(bounds.bottom-bounds.top)};return S_OK;});}
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** value)override{if(!value)return E_POINTER;*value=nullptr;return Check();}
    HRESULT STDMETHODCALLTYPE SetFocus()override{return UIA_E_NOTSUPPORTED;}
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** value)override{if(!value)return E_POINTER;*value=nullptr;return Guard([&]()->HRESULT{if(row_<0){*value=static_cast<IRawElementProviderFragmentRoot*>(this);AddRef();}else *value=static_cast<IRawElementProviderFragmentRoot*>(Make(-1));return S_OK;});}
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x,double y,IRawElementProviderFragment** value)override{if(!value)return E_POINTER;*value=nullptr;return Guard([&]()->HRESULT{for(size_t i=0;i<model_->rectangles.size();++i){const auto& r=model_->rectangles[i];if(x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom){*value=static_cast<IRawElementProviderFragment*>(Make(static_cast<int>(i)));break;}}return S_OK;});}
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** value)override{if(!value)return E_POINTER;*value=nullptr;return Check();}
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** value)override{if(!value)return E_POINTER;*value=nullptr;return Guard([&]()->HRESULT{const auto count=model_->rows.empty()?0u:1u;*value=SafeArrayCreateVector(VT_UNKNOWN,0,count);if(!*value)return E_OUTOFMEMORY;if(count){Microsoft::WRL::ComPtr<IRawElementProviderSimple> item;item.Attach(static_cast<IRawElementProviderSimple*>(Make(static_cast<int>(model_->selected))));LONG index{};const auto result=SafeArrayPutElement(*value,&index,item.Get());if(FAILED(result)){SafeArrayDestroy(*value);*value=nullptr;return result;}}return S_OK;});}
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* value)override{if(!value)return E_POINTER;*value=FALSE;return Check();}
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* value)override{if(!value)return E_POINTER;*value=TRUE;return Check();}
    HRESULT STDMETHODCALLTYPE Select()override{return Action(UiAction::Select);}
    HRESULT STDMETHODCALLTYPE AddToSelection()override{return Select();}
    HRESULT STDMETHODCALLTYPE RemoveFromSelection()override{return UIA_E_INVALIDOPERATION;}
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* value)override{if(!value)return E_POINTER;return Guard([&]()->HRESULT{*value=row_>=0&&model_->selected==static_cast<size_t>(row_);return S_OK;});}
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** value)override{if(!value)return E_POINTER;*value=nullptr;return Guard([&]()->HRESULT{*value=static_cast<IRawElementProviderSimple*>(Make(-1));return S_OK;});}
    HRESULT STDMETHODCALLTYPE Invoke()override{return Action(UiAction::Finalize);}
};
class Bridge {
    std::shared_ptr<Model> model_{std::make_shared<Model>()};Microsoft::WRL::ComPtr<Provider> root_;
public:
    ~Bridge(){Disconnect();}
    IRawElementProviderSimple* Root(std::atomic<long>* leases){if(!model_->active){model_=std::make_shared<Model>();root_.Reset();}model_->leases=leases;if(!root_)root_.Attach(new Provider(model_,-1));return static_cast<IRawElementProviderSimple*>(root_.Get());}
    void Update(HWND window,const RECT& bounds,std::vector<std::wstring> rows,std::vector<RECT> rectangles,size_t selected,uint64_t generation,const std::function<HRESULT(UiAction,size_t,uint64_t)>& action,std::atomic<long>* leases)noexcept{
        try{if(!model_->active){model_=std::make_shared<Model>();root_.Reset();}const auto old=model_->generation;const bool changed=old!=generation||model_->selected!=selected||model_->rows!=rows;model_->leases=leases;model_->window=window;model_->active=true;model_->shown=true;model_->bounds=bounds;model_->rows=std::move(rows);model_->rectangles=std::move(rectangles);model_->selected=selected;model_->generation=generation;model_->action=action;
            if(changed&&UiaClientsAreListening()){auto root=Root(leases);if(old!=generation)UiaRaiseStructureChangedEvent(root,StructureChangeType_ChildrenInvalidated,nullptr,0);if(!model_->rows.empty()){Microsoft::WRL::ComPtr<IRawElementProviderSimple> item;item.Attach(static_cast<IRawElementProviderSimple*>(new Provider(model_,static_cast<int>(selected))));UiaRaiseAutomationEvent(item.Get(),UIA_SelectionItem_ElementSelectedEventId);}}
        }catch(...){model_->shown=false;model_->rows.clear();}
    }
    void Show(std::atomic<long>* leases)noexcept{try{if(UiaClientsAreListening())UiaRaiseAutomationEvent(Root(leases),UIA_MenuOpenedEventId);}catch(...){}}
    void Hide()noexcept{model_->shown=false;model_->rows.clear();model_->rectangles.clear();++model_->generation;if(root_&&UiaClientsAreListening())UiaRaiseAutomationEvent(static_cast<IRawElementProviderSimple*>(root_.Get()),UIA_MenuClosedEventId);}
    void Disconnect()noexcept{const auto window=model_->window;model_->active=false;model_->shown=false;model_->action={};model_->rows.clear();model_->rectangles.clear();if(window)UiaReturnRawElementProvider(window,0,0,nullptr);model_->window=nullptr;root_.Reset();}
};
}
