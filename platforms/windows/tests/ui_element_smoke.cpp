// SPDX-License-Identifier: AGPL-3.0-only
#define NOMINMAX
#include "../tsf/candidate_ui.h"
#include <cstdio>
#include <thread>
#include <stdexcept>
using Microsoft::WRL::ComPtr;
namespace {
void Need(bool value,const char* text){if(!value)throw std::runtime_error(text);}
class Manager final:public ITfUIElementMgr {
    std::atomic<ULONG> refs_{1};
public:
    ComPtr<ITfUIElement> retained;unsigned begins{},updates{},ends{};bool show{},fail{},reenter{};std::function<void()> on_begin;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** value)override{if(!value)return E_POINTER;*value=nullptr;if(iid==IID_IUnknown||iid==IID_ITfUIElementMgr)*value=static_cast<ITfUIElementMgr*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs_;}ULONG STDMETHODCALLTYPE Release()override{const auto count=--refs_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE BeginUIElement(ITfUIElement* value,BOOL* shown,DWORD* id)override{if(!value||!shown||!id)return E_POINTER;++begins;retained=value;*shown=show?TRUE:FALSE;*id=17;if(reenter&&on_begin)on_begin();return fail?E_FAIL:S_OK;}
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id)override{if(id!=17)return E_INVALIDARG;++updates;return S_OK;}
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD id)override{if(id!=17)return E_INVALIDARG;++ends;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetUIElement(DWORD,ITfUIElement** value)override{return retained.CopyTo(value);}
    HRESULT STDMETHODCALLTYPE EnumUIElements(IEnumTfUIElements**)override{return E_NOTIMPL;}
};
}
int wmain(){try{
    std::atomic<long> leases{};ComPtr<Manager> manager;manager.Attach(new Manager);
    // The test manager implements just the queried UIElementMgr interface. No
    // desktop, installed TIP, registry, keyboard injection or network is used.
    transinput::CandidateUiSession session;session.Manager(manager.Get());
    unsigned actions{};uint64_t observed{};size_t chosen{};
    const auto action=[&](transinput::UiAction,size_t row,uint64_t generation){++actions;chosen=row;observed=generation;return S_OK;};
    Need(!session.Publish({L"你好",L"测试"},0,12,nullptr,&leases,action,{}),"host FALSE did not suppress native UI");
    Need(manager->begins==1&&manager->updates==1&&leases==1,"UI begin/update/lifetime");
    ComPtr<ITfCandidateListUIElementBehavior> list;Need(SUCCEEDED(manager->retained.As(&list)),"candidate behavior missing");
    UINT count{},selection{};Need(list->GetCount(&count)==S_OK&&count==2&&list->GetCount(nullptr)==E_POINTER,"UI count bounds");
    BSTR text{};Need(list->GetString(1,&text)==S_OK&&std::wstring(text)==L"测试","owned UI string");SysFreeString(text);text=nullptr;Need(list->GetString(2,&text)==E_INVALIDARG&&!text,"UI invalid index");
    Need(list->SetSelection(1)==S_OK&&actions==1&&chosen==1&&observed==12&&list->GetSelection(&selection)==S_OK&&selection==1,"UI selection callback");
    Need(list->Finalize()==S_OK&&actions==2,"UI finalize");UINT pages[]{0,1},bad[]{1,0};Need(list->SetPageIndex(pages,2)==S_OK&&list->SetPageIndex(bad,2)==E_INVALIDARG,"UI page validation");
    Need(list->GetPageIndex(nullptr,0,&count)==S_FALSE&&count==2&&list->GetCurrentPage(&selection)==S_OK&&selection==1,"UI page count/current");
    HRESULT cross{};std::thread thread([&]{UINT value{};cross=list->GetCount(&value);});thread.join();Need(cross==RPC_E_WRONG_THREAD,"UI thread confinement");
    Need(!session.Publish({},0,13,nullptr,&leases,action,{}),"pending candidates lost host suppression");
    Need(manager->begins==1&&manager->ends==0&&list->GetCount(&count)==S_OK&&count==0,"pending candidates ended/replaced UI session");
    Need(list->Finalize()==E_UNEXPECTED&&list->SetSelection(0)==E_INVALIDARG,"pending candidates retained stale action");
    Need(!session.Publish({L"测试"},0,14,nullptr,&leases,action,{}),"new candidates lost host suppression");
    Need(manager->begins==1&&manager->ends==0&&list->GetCount(&count)==S_OK&&count==1,"candidate reply recreated UI session");
    Need(list->Show(TRUE)==S_OK&&session.Allowed(),"host Show TRUE ignored");session.End();Need(manager->ends==1&&list->Finalize()==CO_E_OBJNOTCONNECTED&&leases==1,"retained root acted after end");
    manager->retained.Reset();list.Reset();Need(leases==0,"retained UI lease leak");manager->fail=true;
    Need(!session.Publish({L"你好"},0,13,nullptr,&leases,action,{}),"failed Begin showed UI");manager->retained.Reset();Need(leases==0,"failed Begin root leak");manager->fail=false;manager->reenter=true;manager->on_begin=[&]{session.End();};
    Need(!session.Publish({L"你好"},0,14,nullptr,&leases,action,{}),"reentrant Begin survived ended session");manager->retained.Reset();Need(leases==0&&manager->ends==2,"reentrant Begin cleanup");
    std::puts("PASS: UILess host visibility negotiation; bounded owned strings/pages; selection/finalize; STA confinement; stale-reference disconnect; failed/reentrant Begin; module leases");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}}
