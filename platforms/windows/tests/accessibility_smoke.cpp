// SPDX-License-Identifier: AGPL-3.0-only
// Tests only our own provider objects. No desktop enumeration or input injection.
#define NOMINMAX
#include "../tsf/candidate_accessibility.h"
#include <cstdio>
#include <stdexcept>
#include <thread>
using Microsoft::WRL::ComPtr;
using namespace transinput::accessibility;
namespace {
void Need(bool value,const char* text){if(!value)throw std::runtime_error(text);}
std::wstring Property(IRawElementProviderSimple* value,PROPERTYID id){VARIANT result{};Need(value->GetPropertyValue(id,&result)==S_OK&&result.vt==VT_BSTR,"UIA string property");std::wstring text(result.bstrVal,SysStringLen(result.bstrVal));VariantClear(&result);return text;}
}
int wmain(){const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(initialized))return 2;int result{};
try{std::atomic<long> leases{};auto model=std::make_shared<Model>();model->leases=&leases;model->shown=true;model->generation=20;model->rows={L"你好",L"测试"};model->rectangles={{10,20,100,50},{10,50,100,80}};model->bounds={0,0,120,100};unsigned actions{};size_t selected{};
    model->action=[&](transinput::UiAction,size_t row,uint64_t generation){Need(generation==20,"UIA stale generation dispatched");++actions;selected=row;return S_OK;};
    {
        ComPtr<Provider> root;root.Attach(new Provider(model,-1));Need(Property(root.Get(),UIA_AutomationIdPropertyId)==L"IME_Candidate_Window","UIA IME id");
        ComPtr<IRawElementProviderFragment> first,next;Need(root->Navigate(NavigateDirection_FirstChild,&first)==S_OK&&first,"UIA first child");Need(first->Navigate(NavigateDirection_NextSibling,&next)==S_OK&&next,"UIA next child");
        ComPtr<IRawElementProviderSimple> item;Need(SUCCEEDED(next.As(&item))&&Property(item.Get(),UIA_NamePropertyId)==L"测试","UIA candidate name");
        ComPtr<ISelectionItemProvider> selection;ComPtr<IInvokeProvider> invoke;Need(SUCCEEDED(item.As(&selection))&&SUCCEEDED(item.As(&invoke)),"UIA candidate patterns");Need(selection->Select()==S_OK&&actions==1&&selected==1,"UIA selection routing");BOOL chosen{};Need(selection->get_IsSelected(&chosen)==S_OK&&chosen,"UIA selected property");Need(invoke->Invoke()==S_OK&&actions==2,"UIA invoke routing");
        SAFEARRAY* chosen_items{};Need(root->GetSelection(&chosen_items)==S_OK&&chosen_items,"UIA selection array");LONG count{};Need(SUCCEEDED(SafeArrayGetUBound(chosen_items,1,&count))&&count==0,"UIA single selection");SafeArrayDestroy(chosen_items);
        UiaRect bounds{};Need(next->get_BoundingRectangle(&bounds)==S_OK&&bounds.left==10&&bounds.top==50&&bounds.height==30,"UIA screen bounds");Need(next->SetFocus()==UIA_E_NOTSUPPORTED,"UIA focus must stay in editor");
        HRESULT cross{};std::thread thread([&]{VARIANT value{};cross=root->GetPropertyValue(UIA_NamePropertyId,&value);VariantClear(&value);});thread.join();Need(cross==RPC_E_WRONG_THREAD,"UIA threading guard");
        ++model->generation;Need(invoke->Invoke()==UIA_E_ELEMENTNOTAVAILABLE&&actions==2,"UIA stale row invoked new text");model->active=false;VARIANT value{};Need(root->GetPropertyValue(UIA_NamePropertyId,&value)==UIA_E_ELEMENTNOTAVAILABLE,"UIA disposed root readable");VariantClear(&value);
    }
    Need(leases==0,"UIA retained provider lease leak");std::puts("PASS: UIA IME id/candidate names; fragment navigation/bounds; selection/invoke; no editor focus steal; STA confinement; stale/disposed rejection; provider lifetime");
    {Bridge bridge;const RECT bounds{0,0,100,100};bridge.Update(nullptr,bounds,{L"你好"},{{0,0,100,50}},0,1,{},&leases);ComPtr<IRawElementProviderSimple> old=bridge.Root(&leases);bridge.Disconnect();bridge.Update(nullptr,bounds,{L"测试"},{{0,0,100,50}},0,2,{},&leases);ComPtr<IRawElementProviderSimple> fresh=bridge.Root(&leases);VARIANT value{};Need(old->GetPropertyValue(UIA_NamePropertyId,&value)==UIA_E_ELEMENTNOTAVAILABLE,"old UIA window root resurrected after recreation");Need(old.Get()!=fresh.Get()&&Property(fresh.Get(),UIA_NamePropertyId)==L"TransInput 候选","new UIA window root missing");}
    Need(leases==0,"UIA bridge recreate lease leak");
}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());result=1;}CoUninitialize();return result;}
