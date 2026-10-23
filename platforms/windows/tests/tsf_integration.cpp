// SPDX-License-Identifier: AGPL-3.0-only
// Isolated SDK integration host: real TSF contexts, no system registration/input injection.
#define NOMINMAX
#include <windows.h>
#include <msctf.h>
#include <textstor.h>
#include <olectl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include "../tsf/identity.h"
#include "../tsf/input_scope.h"
#include "../tsf/candidate_interaction.h"
#include "../tsf/candidate_layout.h"
#include "../tsf/async_broker.h"
#include "../shared/protected_file.h"
#include "candidate_test_interface.h"
#include "test_support.h"
#include "../translator/client.h"

using Microsoft::WRL::ComPtr;
namespace {
using namespace transinput::testing;

class TextStore final : public ITextStoreACP,public ITfInputScope {
    std::atomic<ULONG> references_{1};
    ComPtr<ITextStoreACPSink> sink_;
    std::wstring text_;
    TS_SELECTION_ACP selection_{0, 0, {TS_AE_END, FALSE}};
    DWORD lock_{};
    DWORD pending_lock_{};
    HWND window_{};
    InputScope input_scope_;
    bool scope_requested_{},scope_failure_{};
    bool Range(LONG start, LONG end) const { return start >= 0 && end >= start && end <= static_cast<LONG>(text_.size()); }
public:
    unsigned writes{};
    unsigned locks{};
    bool read_only{};
    bool reject_chinese{};
    unsigned scope_reads{};
    unsigned extent_reads{};LONG extent_start{},extent_end{};DWORD extent_lock{};
    bool hold_reads{};
    bool hold_writes{};
    void NotifyLayout(){if(sink_)Check(sink_->OnLayoutChange(TS_LC_CHANGE,1),"notify own layout change");}
    void ReleaseReads(){
        hold_reads=false;if(pending_lock_&&!lock_){const auto flags=std::exchange(pending_lock_,0);HRESULT result{};Check(RequestLock(flags,&result),"grant deferred own read lock");Check(result,"deferred own read result");}
    }
    void ReleaseWrites(){hold_writes=false;ReleaseReads();}
    void SetReadOnly(bool value) {
        read_only = value;
        if (sink_) Check(sink_->OnStatusChange(value ? TS_SD_READONLY : 0u), "notify read-only status");
    }
    explicit TextStore(HWND window,InputScope scope=IS_DEFAULT,bool failure=false) : window_(window),input_scope_(scope),scope_failure_(failure) {}
    const std::wstring& text() const { return text_; }
    LONG cursor() const { return selection_.acpEnd; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if(iid==IID_IUnknown||iid==IID_ITextStoreACP)*result=static_cast<ITextStoreACP*>(this);
        else if(iid==__uuidof(ITfInputScope))*result=static_cast<ITfInputScope*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown* sink, DWORD) override {
        if (iid != IID_ITextStoreACPSink || !sink) return E_INVALIDARG;
        if (sink_) return CONNECT_E_ADVISELIMIT;
        return sink->QueryInterface(IID_PPV_ARGS(&sink_));
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown* sink) override {
        if (!sink_ || !sink) return CONNECT_E_NOCONNECTION;
        ComPtr<IUnknown> identity;
        sink_.As(&identity);
        ComPtr<IUnknown> expected;
        if (FAILED(sink->QueryInterface(IID_PPV_ARGS(&expected))) || identity.Get() != expected.Get()) return CONNECT_E_NOCONNECTION;
        sink_.Reset(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags, HRESULT* result) override {
        if (!result) return E_POINTER;
        *result = E_FAIL;
        if (!sink_) return E_UNEXPECTED;
        if((hold_reads&&(flags&TS_LF_READWRITE)==TS_LF_READ)||(hold_writes&&(flags&TS_LF_READWRITE)==TS_LF_READWRITE)){
            if(flags&TS_LF_SYNC){*result=TS_E_SYNCHRONOUS;return S_OK;}
            pending_lock_|=flags;*result=TS_S_ASYNC;return S_OK;
        }
        if (lock_) {
            if (flags & TS_LF_SYNC) *result = TS_E_SYNCHRONOUS;
            else { pending_lock_ = flags; *result = TS_S_ASYNC; }
            return S_OK;
        }
        lock_ = flags; ++locks;
        *result = sink_->OnLockGranted(flags);
        lock_ = 0;
        unsigned serviced{};
        while (pending_lock_ && serviced++ < 8) {
            flags = std::exchange(pending_lock_, 0);
            lock_ = flags; ++locks;
            sink_->OnLockGranted(flags);
            lock_ = 0;
        }
        return pending_lock_ ? E_UNEXPECTED : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS* status) override {
        if (!status) return E_POINTER;
        *status = {read_only ? TS_SD_READONLY : 0u, 0}; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInsert(LONG start, LONG end, ULONG, LONG* first, LONG* last) override {
        if (!first || !last) return E_POINTER;
        if (!Range(start, end)) return TS_E_INVALIDPOS;
        *first = start; *last = end; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* selection, ULONG* fetched) override {
        if (!fetched) return E_POINTER;
        *fetched = 0;
        if (!lock_) return TS_E_NOLOCK;
        if (!count || !selection) return E_INVALIDARG;
        if (index != TS_DEFAULT_SELECTION && index != 0) return E_INVALIDARG;
        *selection = selection_; *fetched = 1; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(ULONG count, const TS_SELECTION_ACP* selection) override {
        if ((lock_ & TS_LF_READWRITE) != TS_LF_READWRITE) return TS_E_NOLOCK;
        if (count != 1 || !selection) return E_INVALIDARG;
        if (!Range(selection->acpStart, selection->acpEnd)) return TS_E_INVALIDPOS;
        selection_ = *selection; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetText(LONG start, LONG end, WCHAR* plain, ULONG plain_capacity,
        ULONG* plain_count, TS_RUNINFO* runs, ULONG run_capacity, ULONG* run_count, LONG* next) override {
        if (!plain_count || !run_count || !next || (plain_capacity && !plain) || (run_capacity && !runs)) return E_POINTER;
        *plain_count = 0; *run_count = 0; *next = start;
        if (!lock_) return TS_E_NOLOCK;
        if (end == -1) end = static_cast<LONG>(text_.size());
        if (!Range(start, end)) return TS_E_INVALIDPOS;
        const ULONG available = static_cast<ULONG>(end - start);
        const ULONG length = plain_capacity ? std::min(available, plain_capacity) : available;
        if (plain_capacity) {
            std::copy_n(text_.data() + start, length, plain);
            *plain_count = length;
        }
        if (run_capacity) { runs[0] = {length, TS_RT_PLAIN}; *run_count = 1; }
        *next = start + static_cast<LONG>(length); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG start, LONG end, const WCHAR* value, ULONG length, TS_TEXTCHANGE* change) override {
        if ((lock_ & TS_LF_READWRITE) != TS_LF_READWRITE) return TS_E_NOLOCK;
        if (!change || (length && !value)) return E_POINTER;
        if (!Range(start, end) || length > 65536) return TS_E_INVALIDPOS;
        if(reject_chinese&&length&&std::any_of(value,value+length,[](wchar_t c){return c>=0x3400;}))return E_ACCESSDENIED;
        try {
            text_.replace(static_cast<size_t>(start), static_cast<size_t>(end - start), value ? value : L"", length);
            *change = {start, end, start + static_cast<LONG>(length)};
            selection_ = {change->acpNewEnd, change->acpNewEnd, {TS_AE_END, FALSE}};
            ++writes; return S_OK;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject** value) override { if (value) *value = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown** value) override { if (value) *value = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* insertable) override { if (!insertable) return E_POINTER; *insertable = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags, const WCHAR* value, ULONG length, LONG* start, LONG* end, TS_TEXTCHANGE* change) override {
        if (!lock_) return TS_E_NOLOCK;
        const LONG first = selection_.acpStart, last = selection_.acpEnd;
        if (start) *start = first;
        if (end) *end = last;
        if (flags & TS_IAS_QUERYONLY) return S_OK;
        const auto hr = SetText(0, first, last, value, length, change);
        if (SUCCEEDED(hr) && end) *end = first + static_cast<LONG>(length);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG count, const TS_ATTRID* attributes) override {
        if(count&&!attributes)return E_POINTER;scope_requested_=false;
        for(ULONG i=0;i<count;++i)if(attributes[i]==GUID_PROP_INPUTSCOPE)scope_requested_=true;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG count, const TS_ATTRID* attributes, DWORD flags) override {return RequestSupportedAttrs(flags,count,attributes);}
    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG halt, ULONG, const TS_ATTRID*, DWORD, LONG* next, BOOL* found, LONG* offset) override {
        if (!next || !found || !offset) return E_POINTER;
        *next = halt; *found = FALSE; *offset = 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG count, TS_ATTRVAL* values, ULONG* fetched) override {
        if(!fetched)return E_POINTER;*fetched=0;if(count&&!values)return E_POINTER;
        if(count&&scope_requested_){values[0]={};values[0].idAttr=GUID_PROP_INPUTSCOPE;values[0].varValue.vt=VT_UNKNOWN;
            values[0].varValue.punkVal=static_cast<ITfInputScope*>(this);AddRef();*fetched=1;scope_requested_=false;}return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetInputScopes(InputScope** scopes,UINT* count)override {
        if(!scopes||!count)return E_POINTER;*scopes=nullptr;*count=0;++scope_reads;if(scope_failure_)return E_FAIL;
        *scopes=static_cast<InputScope*>(CoTaskMemAlloc(sizeof(InputScope)));if(!*scopes)return E_OUTOFMEMORY;
        **scopes=input_scope_;*count=1;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPhrase(BSTR** phrases,UINT* count)override{if(!phrases||!count)return E_POINTER;*phrases=nullptr;*count=0;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetRegularExpression(BSTR* value)override{if(value)*value=nullptr;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetSRGS(BSTR* value)override{if(value)*value=nullptr;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetXML(BSTR* value)override{if(value)*value=nullptr;return E_NOTIMPL;}
    HRESULT STDMETHODCALLTYPE GetEndACP(LONG* end) override { if (!end) return E_POINTER; if (!lock_) return TS_E_NOLOCK; *end = static_cast<LONG>(text_.size()); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie* view) override { if (!view) return E_POINTER; *view = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie, const POINT*, DWORD, LONG* position) override { if (!position) return E_POINTER; *position = selection_.acpEnd; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie, LONG start, LONG end, RECT* rect, BOOL* clipped) override {
        if (!rect || !clipped) return E_POINTER;
        if(!lock_)return TS_E_NOLOCK;++extent_reads;extent_start=start;extent_end=end;extent_lock=lock_;
        *rect = {}; *clipped = FALSE;
        return TS_E_NOLAYOUT; // explicitly headless: no candidate window/foreign UI automation
    }
    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie, RECT* rect) override { if (!rect) return E_POINTER; *rect = {}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie, HWND* window) override { if (!window) return E_POINTER; *window = window_; return S_OK; }
};

struct UiSink final:ITfUIElementSink {
    std::atomic<ULONG> refs{1};ComPtr<ITfUIElementMgr> manager;ComPtr<ITfCandidateListUIElementBehavior> list;unsigned begins{},updates{},ends{};
    explicit UiSink(ITfThreadMgr* value){Check(value->QueryInterface(IID_PPV_ARGS(&manager)),"UILess manager");}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** value)override{if(!value)return E_POINTER;*value=nullptr;if(iid==IID_IUnknown||iid==IID_ITfUIElementSink)*value=static_cast<ITfUIElementSink*>(this);else return E_NOINTERFACE;AddRef();return S_OK;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}ULONG STDMETHODCALLTYPE Release()override{const auto value=--refs;if(!value)delete this;return value;}
    HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD,BOOL* show)override{if(!show)return E_POINTER;++begins;*show=FALSE;return S_OK;}
    HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD id)override{++updates;ComPtr<ITfUIElement> value;if(FAILED(manager->GetUIElement(id,&value)))return E_FAIL;return value.As(&list);}
    HRESULT STDMETHODCALLTYPE EndUIElement(DWORD)override{++ends;return S_OK;}
};
struct Host {
    ComPtr<ITfThreadMgrEx> manager;
    ComPtr<ITfDocumentMgr> document;
    ComPtr<ITfContext> context;
    ComPtr<TextStore> store;
    ComPtr<ITfTextInputProcessorEx> service;
    ComPtr<ITfKeyEventSink> keys;
    ComPtr<ITestCandidateView> test_keys;
    ComPtr<ITfInputProcessorProfileMgr> profiles;
    ComPtr<LocalFactory> local_factory;
    ComPtr<UiSink> ui_sink;ComPtr<ITfSource> ui_source;DWORD ui_cookie{TF_INVALID_COOKIE};
    TfClientId client{};
    DWORD registration{};
    LANGID language{};
    bool profile_registered{};
    HWND window{};
    explicit Host(IClassFactory* factory,InputScope scope=IS_DEFAULT,bool scope_failure=false,bool uiless=false) {
      try {
        window = CreateWindowExW(0, L"STATIC", L"TransInput integration text store", 0,
            0, 0, 1, 1, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(window != nullptr, "headless window unavailable");
        Check(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager)), "create thread manager");
        Check(manager->ActivateEx(&client, TF_TMAE_NOACTIVATETIP), "activate isolated TSF thread");
        Check(manager->CreateDocumentMgr(&document), "create document");
        store.Attach(new TextStore(window,scope,scope_failure));
        TfEditCookie cookie{};
        Check(document->CreateContext(client, 0, static_cast<ITextStoreACP*>(store.Get()), &context, &cookie), "create real TSF context");
        Check(document->Push(context.Get()), "push context");
        Check(manager->SetFocus(document.Get()), "set TSF focus");
        local_factory.Attach(new LocalFactory(factory));
        Check(CoRegisterClassObject(transinput::ClassId, local_factory.Get(), CLSCTX_INPROC_SERVER,
            REGCLS_MULTIPLEUSE, &registration), "register process-local class");
        Check(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&profiles)), "create profile manager");
        language = LOWORD(reinterpret_cast<ULONG_PTR>(GetKeyboardLayout(0)));
        Check(profiles->RegisterProfile(transinput::ClassId, language, transinput::ProfileId,
            transinput::PrototypeName, static_cast<ULONG>(std::size(transinput::PrototypeName) - 1),
            nullptr, 0, 0, nullptr, 0, TRUE, TF_RP_LOCALPROCESS | TF_RP_HIDDENINSETTINGUI), "register process-local profile");
        profile_registered = true;
        const auto activated = profiles->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, language,
            transinput::ClassId, transinput::ProfileId, nullptr, TF_IPPMF_FORPROCESS);
        Check(activated, "activate process-local profile");
        Require(activated == S_OK, "local profile is not enabled");
        Check(manager->Deactivate(), "restart isolated manager");
        // Local profiles cannot advertise the machine-level UIELEMENTENABLED
        // category. Exercise real sink visibility negotiation without changing
        // machine categories or pretending strict flag-4 activation was tested.
        Check(manager->ActivateEx(&client, 0), "activate selected local profile manager");
        Check(manager->SetFocus(document.Get()), "restore focus after manager restart");
        Pump();
        service = local_factory->created;
        if (!service) {
            // NOACTIVATETIP suppresses automatic loading of installed services.
            // After selecting the local profile, activate only our own instance.
            Check(factory->CreateInstance(nullptr, IID_PPV_ARGS(&service)), "create isolated TIP");
            ComPtr<ITfClientId> ids;
            TfClientId tip_client{};
            Check(manager.As(&ids), "query client IDs");
            Check(ids->GetClientId(transinput::ClassId, &tip_client), "obtain local TIP client ID");
            Check(service->ActivateEx(manager.Get(), tip_client, 0), "activate selected local TIP");
        }
        Check(service.As(&keys), "query key sink");
        Check(service.As(&test_keys),"query fixed SDK keys");
        if(uiless){ui_sink.Attach(new UiSink(manager.Get()));Check(manager.As(&ui_source),"UILess sink source");Check(ui_source->AdviseSink(IID_ITfUIElementSink,ui_sink.Get(),&ui_cookie),"advise own UILess sink");}
    } catch (...) {
        Cleanup();
        throw;
      }
    }
    ~Host() { Cleanup(); }
    void Cleanup() noexcept {
        if (profiles && profile_registered) {
            profiles->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, language,
                transinput::ClassId, transinput::ProfileId, nullptr, TF_IPPMF_FORPROCESS);
        }
        if (service) service->Deactivate();
        if(ui_source&&ui_cookie!=TF_INVALID_COOKIE)ui_source->UnadviseSink(std::exchange(ui_cookie,TF_INVALID_COOKIE));ui_source.Reset();ui_sink.Reset();
        Pump();
        test_keys.Reset();keys.Reset(); service.Reset();
        if (manager) manager->SetFocus(nullptr);
        if (document) document->Pop(TF_POPF_ALL);
        context.Reset(); document.Reset(); store.Reset();
        if (manager) manager->Deactivate();
        manager.Reset();
        if (profiles && profile_registered) profiles->UnregisterProfile(transinput::ClassId, language,
            transinput::ProfileId, TF_URP_LOCALPROCESS);
        profile_registered = false;
        profiles.Reset();
        if (registration) CoRevokeClassObject(std::exchange(registration, 0));
        if (local_factory) local_factory->created.Reset();
        local_factory.Reset();
        if (window) DestroyWindow(std::exchange(window, nullptr));
    }
    bool measure_keys{};std::vector<double> key_micros;
    void Key(WPARAM key,uint32_t modifiers=0) {
        BOOL eaten{};
        double elapsed{};Check(test_keys->Key(context.Get(),static_cast<uint32_t>(key),modifiers,&eaten,measure_keys?&elapsed:nullptr),"fixed SDK key callbacks");
        if(measure_keys)key_micros.push_back(elapsed);
        Require(eaten==TRUE,"fixed SDK key was not consumed");
    }
    void Type(const char* text) {
        for (; *text; ++text) {
            if(*text=='\'')Key(VK_OEM_7);
            else {Require(*text>='a'&&*text<='z',"unsupported fixed spelling character");Key(static_cast<unsigned char>(*text-'a'+'A'));}
        }
    }
};
void Pumps(unsigned count){while(count--)Pump();}
void CandidateClickCases(){
    transinput::CandidateLayout layout;layout.Set({32,160,32},100);
    Require(layout.Content()==224&&layout.Hit(0)==size_t{0}&&layout.Hit(33)==size_t{1},"wrapped row geometry");
    Require(layout.ScrollTo(10000)&&layout.Offset()==124&&layout.Hit(69)==size_t{2},"scrolled candidate hit mismatch");
    Require(!layout.Hit(-1)&&!layout.Hit(100),"header/footer selected a candidate");
    layout.Set({32},100);Require(layout.Offset()==0&&layout.MaxOffset()==0&&!layout.Hit(90),"shorter content kept stale offset");
    transinput::CandidateInteraction click;
    click.Press(1);Require(click.Release(1)==size_t{1},"stable candidate click failed");
    click.Press(1);click.Changed();Require(!click.Release(1),"changed translation accepted stale press");
    click.Press(1);Require(!click.Release(2),"drag between candidates selected another row");
    click.Press(0);click.Cancel();Require(!click.Release(0),"hidden or focus-lost view accepted press");
    click.Press({});Require(!click.Release(0),"outside press selected candidate on release");
    click.Press(0);Require(!click.Release({}),"release outside candidate committed text");
}
void WaitText(TextStore* store,const std::wstring& expected){
    const auto deadline=GetTickCount64()+2000;
    while(store->text()!=expected&&GetTickCount64()<deadline)Pump();
    if(store->text()!=expected)std::fwprintf(stderr,L"fixed test text mismatch: expected [%ls], actual [%ls]\n",expected.c_str(),store->text().c_str());
    Require(store->text()==expected,"bounded text completion wait failed");
}
ComPtr<ITestCandidateView> WaitRows(Host& host,uint32_t expected){
    ComPtr<ITestCandidateView> view;Check(host.service.As(&view),"test-only candidate interface");
    uint64_t generation{};uint32_t rows{};const auto deadline=GetTickCount64()+2000;
    do{Pump();Check(view->Read(&generation,&rows),"read own candidate actions");}while(rows!=expected&&GetTickCount64()<deadline);
    Require(rows==expected,"candidate actions not ready within deadline");return view;
}
ComPtr<ITestCandidateView> HoldCandidates(Host& host){
    ComPtr<ITestCandidateView> view;Check(host.service.As(&view),"delayed candidate control");
    Check(view->HoldCandidates(TRUE),"hold own candidate replies");return view;
}
ComPtr<ITestCandidateView> WaitEnglish(Host& host){
    auto view=host.test_keys;BOOL ready{};uint32_t selected{};const auto deadline=GetTickCount64()+2500;
    do{Pump();Check(view->TranslationState(&ready,&selected),"read inline translation state");}while(!ready&&GetTickCount64()<deadline);
    Require(ready,"inline English was not ready");return view;
}
void LiteralText(Host& host,const std::wstring& text){
    // Fixed virtual-key fixture for intended US ASCII; the assertions below
    // compare whole independent outputs, not the production key conversion.
    constexpr wchar_t plain[]=L"0123456789 -=[]\\;',./`";
    constexpr wchar_t shifted[]=L")!@#$%^&*( _+{}|:\"<>?~";
    constexpr WPARAM keys[]={'0','1','2','3','4','5','6','7','8','9',VK_SPACE,VK_OEM_MINUS,VK_OEM_PLUS,VK_OEM_4,VK_OEM_6,VK_OEM_5,VK_OEM_1,VK_OEM_7,VK_OEM_COMMA,VK_OEM_PERIOD,VK_OEM_2,VK_OEM_3};
    static_assert(std::size(plain)==std::size(shifted)&&std::size(keys)+1==std::size(plain));
    for(const auto character:text){
        if(character>=L'a'&&character<=L'z')host.Key(static_cast<WPARAM>(character-L'a'+L'A'),TiTestCaps|TiTestShift);
        else if(character>=L'A'&&character<=L'Z')host.Key(static_cast<WPARAM>(character),TiTestCaps);
        else{
            const auto normal=std::wstring_view(plain).find(character),shift=std::wstring_view(shifted).find(character);
            Require(normal!=std::wstring_view::npos||shift!=std::wstring_view::npos,"unsupported fixed literal fixture");
            host.Key(keys[normal!=std::wstring_view::npos?normal:shift],TiTestCaps|(normal==std::wstring_view::npos?TiTestShift:0u));
        }
    }
}
void MixedCases(IClassFactory* factory,TranslationFixture& translation,CandidateFixture& candidates){
    translation.Mode(0);
    const auto edge=[](Host& host,uint32_t key,uint32_t modifiers,BOOL up){BOOL eaten{};Check(host.test_keys->KeyEvent(host.context.Get(),key,modifiers,up,&eaten),"owned modifier edge");return eaten!=FALSE;};
    {Host host(factory);BOOL eaten{};
        Require(edge(host,VK_SHIFT,TiTestShift,FALSE)&&edge(host,VK_SHIFT,TiTestShift,FALSE),"Shift repeat not owned");
        Require(edge(host,VK_SHIFT,0,TRUE),"Shift release not owned");
        Check(host.test_keys->Key(host.context.Get(),'A',0,&eaten,nullptr),"idle Shift English passthrough");Require(!eaten,"Shift did not switch to English exactly once");
        host.Key(VK_SHIFT);host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");host.Key(VK_SHIFT);
        for(const auto key:{'G','P','T'})host.Key(key,TiTestShift);
        host.Key(VK_SHIFT);host.Type("ceshi");host.Key(VK_SPACE);Check(control->HoldCandidates(FALSE),"release Shift boundary");WaitText(host.store.Get(),L"你好GPT测试");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Key(VK_SHIFT);host.Key('G');host.Key(VK_SHIFT);host.Type("ceshi");host.Key(VK_SPACE);
        Check(control->HoldCandidates(FALSE),"release numeric then Shift boundary");WaitText(host.store.Get(),L"北京g测试");}
    {Host host(factory);auto control=HoldCandidates(host);host.store->hold_writes=true;host.Type("nihao");host.Key(VK_SHIFT);host.Key('G');host.Key(VK_SHIFT);host.Type("ceshi");host.Key(VK_SPACE);
        Require(host.store->text().empty(),"Shift boundary bypassed held write lock");Check(control->HoldCandidates(FALSE),"release Shift candidates behind writes");host.store->ReleaseWrites();WaitText(host.store.Get(),L"你好g测试");}
    {Host host(factory);Require(edge(host,VK_LSHIFT,TiTestShift,FALSE),"left Shift down");Require(!edge(host,VK_TAB,TiTestShift,FALSE),"Shift Tab captured");Require(edge(host,VK_LSHIFT,0,TRUE),"left Shift up");
        host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    {Host host(factory);for(const auto modifier:{TiTestControl,TiTestAlt,TiTestWin}){
        Require(!edge(host,VK_SHIFT,modifier|TiTestShift,FALSE),"modified Shift captured");Require(!edge(host,VK_SHIFT,modifier,TRUE),"modified Shift release captured");}
        Require(edge(host,VK_LSHIFT,TiTestShift,FALSE)&&edge(host,VK_RSHIFT,TiTestShift,FALSE),"two Shift downs");
        Require(edge(host,VK_RSHIFT,TiTestShift,TRUE)&&edge(host,VK_LSHIFT,0,TRUE),"two Shift ups");
        host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    {Host host(factory);Require(edge(host,VK_SHIFT,TiTestShift,FALSE),"Shift before blur");Check(host.keys->OnSetFocus(FALSE),"blur during Shift");
        Require(!edge(host,VK_SHIFT,0,TRUE),"stale Shift release owned after blur");host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    {Host host(factory);host.store->SetReadOnly(true);Require(!edge(host,VK_SHIFT,TiTestShift,FALSE)&&!edge(host,VK_SHIFT,0,TRUE),"readonly Shift captured");
        host.store->SetReadOnly(false);host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    translation.Mode(0);
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");host.Key(VK_SPACE);host.Key('0');
        Check(control->HoldCandidates(FALSE),"release mixed boundary");WaitText(host.store.Get(),L"你好0");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");host.Key(VK_SPACE);
        for(const auto key:{'2','0','2','6'})host.Key(key);
        Check(control->HoldCandidates(FALSE),"release pending year");WaitText(host.store.Get(),L"你好2026");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");LiteralText(host,L"GPT OpenAI 2026");
        host.Type("ceshi");host.Key(VK_SPACE);Check(control->HoldCandidates(FALSE),"release Caps boundary");WaitText(host.store.Get(),L"你好GPT OpenAI 2026测试");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');LiteralText(host,L"Go2026 ");
        host.Type("nihao");host.Key(VK_SPACE);Check(control->HoldCandidates(FALSE),"release conditional mixed suffix");WaitText(host.store.Get(),L"北京Go2026 你好");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("qqqqqqqq");host.Key('9');LiteralText(host,L"Ab0");
        Check(control->HoldCandidates(FALSE),"release invalid-choice mixed suffix");WaitText(host.store.Get(),L"qqqqqqqqAb0");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");host.Key('2',TiTestShift);host.Key('0');
        Check(control->HoldCandidates(FALSE),"release shifted literal boundary");WaitText(host.store.Get(),L"你好@0");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");host.Key(VK_SPACE);host.Key(VK_OEM_MINUS);host.Key(VK_OEM_PLUS);host.Key('1',TiTestShift);
        Check(control->HoldCandidates(FALSE),"release literal punctuation after commit");WaitText(host.store.Get(),L"你好-=!");}
    {Host host(factory);host.Type("nihao");WaitRows(host,9);host.Key(VK_OEM_7,TiTestShift);WaitText(host.store.Get(),L"你好\"");}
    {Host host(factory);host.Type("xi");host.Key(VK_OEM_7);host.Type("an");host.Key(VK_RETURN);WaitText(host.store.Get(),L"xi'an");}
    {Host host(factory);host.Type("nihao");WaitRows(host,9);host.Key('0');WaitText(host.store.Get(),L"你好0");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");host.Key(VK_NUMPAD0);host.Key(VK_DECIMAL);host.Key(VK_NUMPAD5);
        Check(control->HoldCandidates(FALSE),"release numeric keypad suffix");WaitText(host.store.Get(),L"你好0.5");}
    for(const auto& text:{L"name@example.com",L"https://example.test/a?x=1",L"C:\\Temp\\Demo_01.txt",L"()[]{}<>!?@#$%^&*+-=;:'\"`~|,./\\"}){
        Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");LiteralText(host,text);
        Check(control->HoldCandidates(FALSE),"release fixed email/path/symbol suffix");WaitText(host.store.Get(),L"你好"+std::wstring(text));
    }
    {Host host(factory);host.store->hold_writes=true;auto control=HoldCandidates(host);host.Type("nihao");host.Key(VK_SPACE);
        Check(control->HoldCandidates(FALSE),"resolve before held write lock");Pumps(2);
        Require(host.store->text().empty(),"held native writes unexpectedly ran");LiteralText(host,L"Go0");host.Key(VK_OEM_PLUS,TiTestShift);
        host.store->ReleaseWrites();WaitText(host.store.Get(),L"你好Go0+");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");LiteralText(host,L"Go0");host.Type("ceshi");host.Key(VK_ESCAPE);
        Check(control->HoldCandidates(FALSE),"release accepted literals before cancel");WaitText(host.store.Get(),L"你好Go0");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');LiteralText(host,L"Go0");host.Type("ceshi");
        Check(host.keys->OnSetFocus(FALSE),"lose focus with accepted literals");Check(control->HoldCandidates(FALSE),"release original-context mixed commits");WaitText(host.store.Get(),L"北京Go0");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');LiteralText(host,L"Go0");host.Type("ceshi");
        auto old_document=host.document;auto old_context=host.context;auto old_store=host.store;
        ComPtr<ITfDocumentMgr> next_document;Check(host.manager->CreateDocumentMgr(&next_document),"mixed second document");
        ComPtr<TextStore> next_store;next_store.Attach(new TextStore(host.window));ComPtr<ITfContext> next_context;TfEditCookie cookie{};
        Check(next_document->CreateContext(host.client,0,static_cast<ITextStoreACP*>(next_store.Get()),&next_context,&cookie),"mixed second context");
        Check(next_document->Push(next_context.Get()),"mixed push second context");Check(host.manager->SetFocus(next_document.Get()),"mixed context switch");
        host.document=next_document;host.context=next_context;host.store=next_store;host.Type("nihao");host.Key(VK_SPACE);LiteralText(host,L"New0");
        Check(control->HoldCandidates(FALSE),"release mixed original contexts");WaitText(old_store.Get(),L"北京Go0");WaitText(next_store.Get(),L"你好New0");
        Check(old_document->Pop(TF_POPF_ALL),"mixed pop original context");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');LiteralText(host,L"Go0");
        const auto started=GetTickCount64();Check(host.service->Deactivate(),"mixed shutdown");Require(GetTickCount64()-started<100,"mixed shutdown blocked UI");WaitText(host.store.Get(),L"bjGo0");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");LiteralText(host,L"Go0");
        Check(control->CandidateFailure(ERROR_TIMEOUT),"fail candidate mixed resolution");Check(control->HoldCandidates(FALSE),"release mixed candidate failure");WaitText(host.store.Get(),L"nihaoGo0");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("nihao");host.Key(VK_SPACE);BOOL eaten{};
        for(const auto modifier:{TiTestControl,TiTestAlt,TiTestWin}){Check(host.test_keys->Key(host.context.Get(),'N',modifier|TiTestCaps,&eaten,nullptr),"mixed shortcut passthrough");Require(!eaten,"mixed mode captured app shortcut");}
        host.store->SetReadOnly(true);Check(host.test_keys->Key(host.context.Get(),'A',TiTestCaps,&eaten,nullptr),"mixed readonly refusal");Require(!eaten,"readonly mixed key accepted");
        host.store->SetReadOnly(false);LiteralText(host,L"A");Check(control->HoldCandidates(FALSE),"release readonly restore");WaitText(host.store.Get(),L"你好A");
        Check(host.test_keys->Key(host.context.Get(),'A',TiTestCaps,&eaten,nullptr),"idle Caps host path");Require(!eaten,"idle English did not return to host");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');unsigned accepted{};BOOL eaten{};HRESULT result{};
        while(accepted<64){result=host.test_keys->Key(host.context.Get(),'A',TiTestCaps,&eaten,nullptr);if(FAILED(result))break;Require(eaten,"reserved mixed command escaped");++accepted;}
        Require(result==HRESULT_FROM_WIN32(ERROR_BUSY)&&accepted>0&&accepted<64&&!eaten,"mixed queue did not enforce bound");
        Check(control->HoldCandidates(FALSE),"release bounded mixed queue");WaitText(host.store.Get(),L"北京"+std::wstring(accepted,L'A'));}
    for(unsigned batch=0;batch<16;++batch){Host host(factory);auto control=HoldCandidates(host);std::wstring expected;
        for(unsigned item=0;item<4;++item){host.Type("nihao");host.Key(VK_SPACE);LiteralText(host,L"Go0 ");expected+=L"你好Go0 ";}
        Check(control->HoldCandidates(FALSE),"release rapid mixed batch");WaitText(host.store.Get(),expected);}
    {Host host(factory);auto control=HoldCandidates(host);Check(control->SetLearning(TRUE),"enable mixed learning");host.Type("bj");host.Key('2');LiteralText(host,L"Go0");
        Check(control->HoldCandidates(FALSE),"release Chinese-only mixed learning");WaitText(host.store.Get(),L"北京Go0");Pump();Require(candidates.First("bj")=="北京","successful Chinese choice did not learn");}
    Require(translation.Count()==0,"disabled mixed input reached translator");
    translation.Mode(1);
    {Host host(factory);BOOL eaten{};Check(host.test_keys->Key(host.context.Get(),'A',TiTestCaps,&eaten,nullptr),"pure English with translator enabled");Require(!eaten,"pure English was composed as Chinese");Pumps(2);}
    Require(translation.Count()==0,"pure English requested translation");
    {Host host(factory);host.Type("nihao");WaitEnglish(host);Require(translation.Source()=="你好","unexpected translation source before literal boundary");const auto requests=translation.Count();
        host.Key('2',TiTestCaps);WaitText(host.store.Get(),L"你好2");Pumps(2);
        Require(translation.Count()==requests,"literal suffix created another translation observation");}
    translation.Mode(0);
    std::puts("PASS: Shift tap toggles, repeats/chords/dual Shift/blur/readonly; ordered Chinese-English boundaries behind choices/write locks; ASCII/Caps/digits/keypad/email/path/symbols; cancel/failure/bounded queue; 64 rapid mixed commits; Chinese-only learning; no English translation");
}
ComPtr<ITestCandidateView> WaitPage(Host& host,uint32_t expected);
void UiLessCases(IClassFactory* factory,TranslationFixture& translation,CandidateFixture& candidates){
    translation.Mode(0);
    const auto wait=[](Host& host){const auto deadline=GetTickCount64()+2000;UINT count{};do{Pump();if(host.ui_sink->list&&SUCCEEDED(host.ui_sink->list->GetCount(&count))&&count)break;}while(GetTickCount64()<deadline);Require(count&&host.ui_sink->begins&&host.ui_sink->updates,"UILess candidates not published");};
    const auto first=[](ITfCandidateListUIElementBehavior* list){BSTR text{};Check(list->GetString(0,&text),"UILess first string");std::wstring value(text,SysStringLen(text));SysFreeString(text);return value;};
    {Host host(factory,IS_DEFAULT,false,true);host.Type("nihao");WaitPage(host,0);wait(host);auto list=host.ui_sink->list;Require(first(list.Get())==L"你好","UILess wrong candidate");
        BOOL shown=TRUE;Check(list->IsShown(&shown),"UILess visibility");Require(!shown,"host suppression ignored");ComPtr<ITfDocumentMgr> document;Check(list->GetDocumentMgr(&document),"UILess original document");Require(document.Get()==host.document.Get(),"UILess document crossed context");
        Check(list->Finalize(),"UILess finalize");WaitText(host.store.Get(),L"你好");UINT count{};Require(list->GetCount(&count)==CO_E_OBJNOTCONNECTED&&host.ui_sink->ends==1,"UILess ended element remained actionable");
        host.Type("ceshi");WaitPage(host,0);wait(host);Require(list.Get()!=host.ui_sink->list.Get(),"UILess reused stale element");Check(host.ui_sink->list->Abort(),"UILess cancel");WaitText(host.store.Get(),L"你好");}
    {Host host(factory,IS_DEFAULT,false,true);const auto rows=candidates.Candidates("n",1);host.Type("n");WaitPage(host,0);wait(host);host.Key(VK_NEXT);WaitPage(host,1);wait(host);
        Require(first(host.ui_sink->list.Get())==rows[0],"UILess page update used old rows");Check(host.ui_sink->list->SetSelection(1),"UILess select second");host.store->SetReadOnly(true);Check(host.ui_sink->list->Finalize(),"UILess read-only selection");Pumps(2);Require(host.store->text()==L"n","UILess wrote read-only document");host.store->SetReadOnly(false);Check(host.ui_sink->list->Finalize(),"UILess page finalize");WaitText(host.store.Get(),rows[1]);}
    translation.Mode(1);
    {Host host(factory,IS_DEFAULT,false,true);host.Type("nihao");WaitEnglish(host);UINT count{};Check(host.ui_sink->list->GetCount(&count),"inline UILess count");
        Require(count==9&&first(host.ui_sink->list.Get())==L"你好 — Hello","inline UILess lost Chinese choices or split English into another candidate");
        const auto updates=host.ui_sink->updates,writes=host.store->writes;Pumps(6);
        Require(host.ui_sink->updates==updates&&host.store->writes==writes,"unchanged translation polls republished UI or host text");
        host.Key(VK_RETURN,TiTestControl);WaitText(host.store.Get(),L"Hello");}
    translation.Mode(0);
    Require(GetFileAttributesW(candidates.path.c_str())==INVALID_FILE_ATTRIBUTES,"UILess preview recorded preferences");
    std::puts("PASS: real TSF UILess sink suppresses native UI; owned strings and original document; selection/finalize/abort; paging/read-only; End disconnects retained host references");
}
void SpellingCases(IClassFactory* factory,TranslationFixture& translation,CandidateFixture& candidates){
    translation.Mode(0);
    const auto select=[&](Host& host,const char* input,const wchar_t* expected){
        const auto rows=candidates.Candidates(input);const auto found=std::find(rows.begin(),rows.end(),expected);
        Require(found!=rows.end(),"mixed spelling missing expected Chinese");host.Type(input);WaitPage(host,0);
        host.Key(static_cast<WPARAM>('1'+std::distance(rows.begin(),found)));WaitText(host.store.Get(),expected);
    };
    for(const auto& item:{std::pair{"zhguo",L"中国"},std::pair{"srufa",L"输入法"},std::pair{"rgongzhineng",L"人工智能"},std::pair{"ni'h",L"你好"}}){Host host(factory);select(host,item.first,item.second);}
    const auto rows=candidates.Candidates("shuruf");const auto found=std::find(rows.begin(),rows.end(),L"输入法");Require(found!=rows.end(),"mixed spelling fixture word");
    const auto choice=static_cast<WPARAM>('1'+std::distance(rows.begin(),found));
    {Host host(factory);host.Type("shurqf");host.Key(VK_LEFT);host.Key(VK_BACK);host.Type("u");host.Key(VK_RIGHT);WaitPage(host,0);host.Key(choice);WaitText(host.store.Get(),L"输入法");}
    {Host host(factory);host.Type("shuruf");host.Key(VK_ESCAPE);host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    {Host host(factory);host.Type("shuruf");host.Key(VK_RETURN);WaitText(host.store.Get(),L"shuruf");}
    {Host host(factory);auto held=HoldCandidates(host);host.Type("shuruf");host.Key(choice);host.Type("nihao");host.Key(VK_SPACE);
        Check(held->HoldCandidates(FALSE),"release mixed spelling results");WaitText(host.store.Get(),L"输入法你好");}
    {Host host(factory);std::wstring expected;for(unsigned batch=0;batch<8;++batch){for(unsigned i=0;i<4;++i){host.Type("shuruf");host.Key(choice);expected+=L"输入法";}WaitText(host.store.Get(),expected);}}
    Require(GetFileAttributesW(candidates.path.c_str())==INVALID_FILE_ATTRIBUTES,"mixed spelling previews created learning file");
    translation.Mode(1);
    {Host host(factory,IS_PRIVATE);host.Type("shuruf");WaitPage(host,0);Pumps(2);Require(translation.Count()==0,"private mixed spelling sent translation");host.Key(VK_SPACE);WaitText(host.store.Get(),rows[0]);}
    {Host host(factory);host.Type("shuruf");WaitEnglish(host);Require(translation.Source()=="输入法","mixed spelling translated pinyin or stale source");host.Key(VK_SPACE);WaitText(host.store.Get(),L"输入法");}
    translation.Mode(0);
    const auto choices=candidates.Candidates("bjing");Require(choices.size()>1,"mixed spelling learn alternatives");
    {Host host(factory);ComPtr<ITestCandidateView> view;Check(host.service.As(&view),"mixed spelling learning view");Check(view->SetLearning(TRUE),"mixed spelling learning on");host.Type("bjing");WaitPage(host,0);host.Key('2');WaitText(host.store.Get(),choices[1]);}
    auto deadline=GetTickCount64()+2000;while(candidates.Candidates("bjing")[0]!=choices[1]&&GetTickCount64()<deadline)Pump();Require(candidates.Candidates("bjing")[0]==choices[1],"mixed spelling choice not learned");
    candidates.Restart();Require(candidates.Candidates("bjing")[0]==choices[1],"mixed spelling learned order lost after restart");
    std::puts("PASS: mixed syllable spelling/digraphs/boundaries; middle edit/cancel/raw; held results and 32 rapid commits; private/mock translation; successful selection learning and encrypted restart");
}
void PersonalWordCases(IClassFactory* factory,TranslationFixture& translation,CandidateFixture& candidates){
    using namespace transinput::words_control;translation.Mode(0);uint64_t sequence=1000;
    const auto add=[&](const Row& row){
        const auto ready_deadline=GetTickCount64()+2000;Result ready;
        do{ready=Query(candidates.endpoint,candidates.executable,nullptr,0,0,transinput::wire::WordsStatus,{},candidates.pid);if(!ready.status&&ready.view.health==Ready)break;Pump();}while(GetTickCount64()<ready_deadline);
        Require(!ready.status&&ready.view.health==Ready,"TSF word profile readiness");
        const auto saved=Change(candidates.endpoint,candidates.executable,nullptr,transinput::wire::WordsAdd,sequence++,row,0,candidates.pid);
        Require(!saved.status&&saved.view.state==Succeeded,"TSF fixed personal word add");
    };
    const auto baseline=candidates.Candidates("xingheyanjiusuo");const Row word{"xing he yan jiu suo","星河研究所"};add(word);
    {Host host(factory);host.Type("xingheyanjiusuo");host.Key(VK_SPACE);WaitText(host.store.Get(),L"星河研究所");}
    {Host host(factory);host.Type("nihaoxingheyanjiusuo");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好星河研究所");}
    {Host host(factory);host.Type("xingheyanjiusuonihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"星河研究所你好");}
    {Host host(factory);host.Type("xingheyanjiusuo");host.Key(VK_ESCAPE);WaitText(host.store.Get(),L"");host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    for(unsigned i=0;i<12;++i)add({"ce shi","测试词"+std::to_string(i)});
    const auto second=candidates.Candidates("ceshi",1);Require(second.size()>=2,"personal word second page");
    {Host host(factory);host.Type("ceshi");host.Key(VK_NEXT);WaitPage(host,1);host.Key('2');WaitText(host.store.Get(),second[1]);}
    translation.Mode(1);
    {Host host(factory,IS_PRIVATE);host.Type("xingheyanjiusuo");WaitPage(host,0);Pumps(2);Require(host.store->scope_reads>0&&translation.Count()==0,"private personal word translated");host.Key(VK_SPACE);WaitText(host.store.Get(),L"星河研究所");}
    {Host host(factory);host.Type("xingheyanjiusuo");WaitEnglish(host);Require(translation.Source()==word.text,"translator got vocabulary other than current candidate");host.Key(VK_SPACE);WaitText(host.store.Get(),L"星河研究所");}
    translation.Mode(0);candidates.Restart(false);
    {Host host(factory);host.Type("xingheyanjiusuo");host.Key(VK_SPACE);WaitText(host.store.Get(),L"星河研究所");}
    const auto deleted=Change(candidates.endpoint,candidates.executable,nullptr,transinput::wire::WordsRemove,sequence++,word,0,candidates.pid);Require(!deleted.status&&deleted.view.state==Succeeded,"TSF word removal");
    const auto after=candidates.Candidates("xingheyanjiusuo");Require(after==baseline,"deleted personal override did not restore base ranking");
    const auto initials_baseline=candidates.Candidates("xhyjs");add(word);add({word.pinyin,"星河研修所"});
    for(const auto* input:{"xhyjs","xinghe","xinghyjs"}){
        uint32_t page{};std::vector<std::wstring> rows;size_t index{};bool found_word{};
        for(;page<10;++page){rows=candidates.Candidates(input,page);const auto found=std::find(rows.begin(),rows.end(),L"星河研究所");
            if(found!=rows.end()){index=static_cast<size_t>(std::distance(rows.begin(),found));found_word=true;break;}}
        Require(found_word,"personal initials/completion absent from bounded candidate pool");
        const auto choice=static_cast<WPARAM>('1'+index);
        const auto prepare=[&](Host& host){host.Type(input);WaitPage(host,0);for(uint32_t i=0;i<page;++i){host.Key(VK_NEXT);WaitPage(host,i+1);}};
        {Host host(factory,IS_PRIVATE);ComPtr<ITestCandidateView> view;Check(host.service.As(&view),"personal private learning view");Check(view->SetLearning(TRUE),"personal private learning on");
            prepare(host);host.Key(choice);WaitText(host.store.Get(),L"星河研究所");}
        Require(candidates.Candidates(input,page)==rows,"private personal selection changed preference");
        {Host host(factory);ComPtr<ITestCandidateView> view;Check(host.service.As(&view),"personal learning view");Check(view->SetLearning(TRUE),"personal learning on");
            prepare(host);host.Key(VK_ESCAPE);}
        Require(candidates.Candidates(input,page)==rows,"personal preview/cancel learned a word");
        {Host host(factory);ComPtr<ITestCandidateView> view;Check(host.service.As(&view),"personal successful learning view");Check(view->SetLearning(TRUE),"personal successful learning on");
            prepare(host);host.Key(choice);WaitText(host.store.Get(),L"星河研究所");}
        const auto deadline=GetTickCount64()+2000;while(candidates.First(input)!="星河研究所"&&GetTickCount64()<deadline)Pump();
        Require(candidates.First(input)=="星河研究所","successful personal abbreviation/completion not learned");
    }
    candidates.Restart();
    for(const auto* input:{"xhyjs","xinghe","xinghyjs"})Require(candidates.First(input)=="星河研究所","personal selection lost at broker restart");
    for(const auto& row:{word,Row{word.pinyin,"星河研修所"}}){const auto removed=Change(candidates.endpoint,candidates.executable,nullptr,transinput::wire::WordsRemove,sequence++,row,0,candidates.pid);Require(!removed.status&&removed.view.state==Succeeded,"learned personal word delete");}
    Require(candidates.Candidates("xhyjs")==initials_baseline,"stale personal learning resurrected removed initials");
    std::puts("PASS: explicit personal word and base phrase composition; cancel; second-page selection; private-input suppression and current-candidate-only mock translation; encrypted broker restart and deletion");
}
ComPtr<ITestCandidateView> WaitPage(Host& host,uint32_t expected){
    ComPtr<ITestCandidateView> view;Check(host.service.As(&view),"page state control");
    uint32_t page{},total{};BOOL ready{};const auto deadline=GetTickCount64()+2000;
    do{Pump();Check(view->PageState(&page,&total,&ready),"read own page state");}while((!ready||page!=expected)&&GetTickCount64()<deadline);
    Require(ready&&page==expected&&total<=90,"page did not become ready within bounds");return view;
}
void PageCases(IClassFactory* factory,TranslationFixture& translation,CandidateFixture& candidates){
    translation.Mode(0);const auto first=candidates.Candidates("n"),second=candidates.Candidates("n",1),last=candidates.Candidates("n",9);
    Require(first.size()==9&&second.size()==9&&last.size()==9,"fixed page baseline missing");
    {Host host(factory);host.Type("n");WaitPage(host,0);host.Key(VK_NEXT);WaitPage(host,1);host.Key('1');WaitText(host.store.Get(),second[0]);}
    {Host host(factory);auto view=HoldCandidates(host);host.Type("n");host.Key(VK_NEXT);host.Key('2');Pump();
        Require(host.store->text()==L"n","unready page committed stale first page");Check(view->HoldCandidates(FALSE),"release pending page");WaitText(host.store.Get(),second[1]);}
    {Host host(factory);host.Type("n");for(unsigned i=0;i<14;++i)host.Key(VK_NEXT);WaitPage(host,9);host.Key(VK_NEXT);WaitPage(host,9);
        host.Key(VK_DOWN);host.Key(VK_SPACE);WaitText(host.store.Get(),last[1]);}
    {Host host(factory);host.Type("n");WaitPage(host,0);host.Key(VK_PRIOR);WaitPage(host,0);host.Key(VK_OEM_PLUS);WaitPage(host,1);
        host.Key(VK_OEM_MINUS);WaitPage(host,0);host.Key('2');WaitText(host.store.Get(),first[1]);}
    {Host host(factory);host.Type("n");host.Key(VK_NEXT);WaitPage(host,1);host.Type("i");WaitPage(host,0);host.Key('1');WaitText(host.store.Get(),L"你");}
    {Host host(factory);auto view=HoldCandidates(host);host.Type("qqqqqqqq");host.Key(VK_NEXT);host.Key('9');Check(view->HoldCandidates(FALSE),"release empty page");
        WaitPage(host,0);Require(host.store->text()==L"qqqqqqqq","invalid page choice committed source");host.Key(VK_RETURN);WaitText(host.store.Get(),L"qqqqqqqq");}
    {Host host(factory);host.Type("n");auto view=WaitPage(host,0);Check(view->Press(0),"press before page change");host.Key(VK_NEXT);WaitPage(host,1);
        Check(view->ReleasePress(0),"release stale page press");Pump();Require(host.store->text()==L"n","page change kept stale mouse action");
        Check(view->Press(0),"press current page");Check(view->ReleasePress(0),"release current page");WaitText(host.store.Get(),second[0]);}
    {Host host(factory);auto view=HoldCandidates(host);host.Type("n");host.Key(VK_NEXT);host.Key('2');host.Type("nihao");host.Key(VK_SPACE);host.Type("ceshi");
        Check(host.keys->OnSetFocus(FALSE),"focus loss during page choice");Check(view->HoldCandidates(FALSE),"release detached page choice");WaitText(host.store.Get(),second[1]+L"你好");
        host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),second[1]+L"你好你好");}
    {Host host(factory);std::wstring expected;
        for(unsigned batch=0;batch<16;++batch){for(unsigned i=0;i<4;++i){host.Type("n");host.Key(VK_NEXT);host.Key('2');expected+=second[1];}WaitText(host.store.Get(),expected);}}
    translation.Mode(2);
    {Host host(factory);host.Type("n");WaitPage(host,0);host.Key(VK_NEXT);WaitPage(host,1);WaitEnglish(host);
        const auto chinese=second[0];const auto bytes=translation.Source();
        const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,chinese.data(),static_cast<int>(chinese.size()),nullptr,0,nullptr,nullptr);
        std::string expected(size,'\0');Require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,chinese.data(),static_cast<int>(chinese.size()),expected.data(),size,nullptr,nullptr)==size,"page source encode");
        Require(bytes==expected,"translator observed previous page");host.Key(VK_RETURN,TiTestControl);WaitText(host.store.Get(),L"Fixture translation");}
    translation.Mode(1);
    {Host host(factory,IS_PRIVATE);host.Type("n");host.Key(VK_NEXT);WaitPage(host,1);Pumps(2);Require(translation.Count()==0,"private page sent to translator");host.Key(VK_SPACE);WaitText(host.store.Get(),second[0]);}
    translation.Mode(0);
    Require(GetFileAttributesW(candidates.path.c_str())==INVALID_FILE_ATTRIBUTES,"page previews or ordinary tests learned data");
    {Host host(factory);host.Type("n");host.Key(VK_NEXT);auto view=WaitPage(host,1);Check(view->SetLearning(TRUE),"page learning enable");
        // New composition fixes the enabled learning mode at its first letter.
        host.Key(VK_ESCAPE);host.Type("n");host.Key(VK_NEXT);WaitPage(host,1);host.Key('2');WaitText(host.store.Get(),second[1]);}
    auto deadline=GetTickCount64()+2000;while(candidates.Candidates("n")[0]!=second[1]&&GetTickCount64()<deadline)Pump();
    Require(candidates.Candidates("n")[0]==second[1],"off-page choice did not learn");candidates.Restart();
    deadline=GetTickCount64()+2000;while(candidates.Candidates("n")[0]!=second[1]&&GetTickCount64()<deadline)Pump();
    Require(candidates.Candidates("n")[0]==second[1],"off-page learning did not restore");
    std::puts("PASS: 10 pages, 9 rows; cold and ready selection; boundary/reset/edit; stale mouse/translation; private input; 64 rapid second-page commits; encrypted off-page learning restore");
}
void DigitCases(IClassFactory* factory,TranslationFixture& translation,CandidateFixture& candidates){
    translation.Mode(0);const auto choices=candidates.Candidates("bj");
    Require(choices.size()==9&&choices[1]==L"北京","unexpected fixed digit baseline");
    for(size_t i=0;i<choices.size();++i){
        Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('1'+i);Pump();
        Require(host.store->text()==L"bj","choice committed before held candidate was released");
        Check(control->HoldCandidates(FALSE),"release fixed digit reply");WaitText(host.store.Get(),choices[i]);
    }
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');
        host.Type("nihao");host.Key(VK_SPACE);host.Type("rgzn");host.Key(VK_OEM_PERIOD);Pump();
        Require(host.store->text()==L"bj","following draft escaped selection barrier");
        Check(control->HoldCandidates(FALSE),"release ordered digits");WaitText(host.store.Get(),L"北京你好人工智能。");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("qqqqqqqq");host.Key('9');Pump();
        Check(control->HoldCandidates(FALSE),"release invalid digit");WaitText(host.store.Get(),L"qqqqqqqq");
        host.Type("n");host.Key(VK_RETURN);WaitText(host.store.Get(),L"qqqqqqqqn");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("qqqqqqqq");host.Key('9');host.Key(VK_BACK);host.Key(VK_LEFT);host.Type("n");host.Key(VK_RETURN);
        Check(control->HoldCandidates(FALSE),"release invalid digit with deferred edit");WaitText(host.store.Get(),L"qqqqqqnq");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("qqqqqqqq");host.Key('9');host.Key(VK_ESCAPE);host.Type("nihao");host.Key(VK_SPACE);
        Check(control->HoldCandidates(FALSE),"release invalid digit with cancel");WaitText(host.store.Get(),L"你好");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Type("niho");host.Key(VK_LEFT);host.Type("a");host.Key(VK_RIGHT);host.Key(VK_BACK);host.Type("o");host.Key(VK_SPACE);
        Check(control->HoldCandidates(FALSE),"release deferred middle edit");WaitText(host.store.Get(),L"北京你好");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Key(VK_OEM_COMMA);host.Key(VK_SPACE);host.Type("nihao");host.Key(VK_SPACE);
        Check(control->HoldCandidates(FALSE),"release literal suffix");WaitText(host.store.Get(),L"北京， 你好");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Key(VK_RETURN);host.Key(VK_ESCAPE);host.Type("nihao");host.Key(VK_SPACE);
        Check(control->HoldCandidates(FALSE),"release IME-only control keys");WaitText(host.store.Get(),L"北京你好");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Type("nihao");host.Key(VK_SPACE);host.Type("ceshi");
        Check(host.keys->OnSetFocus(FALSE),"focus loss during numeric choice");
        Check(control->HoldCandidates(FALSE),"release detached context choices");WaitText(host.store.Get(),L"北京你好");
        host.Type("rgzn");host.Key(VK_SPACE);WaitText(host.store.Get(),L"北京你好人工智能");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Type("nihao");host.Key(VK_SPACE);host.Type("ceshi");
        auto old_document=host.document;auto old_context=host.context;auto old_store=host.store;
        ComPtr<ITfDocumentMgr> next_document;Check(host.manager->CreateDocumentMgr(&next_document),"numeric second document");
        ComPtr<TextStore> next_store;next_store.Attach(new TextStore(host.window));ComPtr<ITfContext> next_context;TfEditCookie cookie{};
        Check(next_document->CreateContext(host.client,0,static_cast<ITextStoreACP*>(next_store.Get()),&next_context,&cookie),"numeric second context");
        Check(next_document->Push(next_context.Get()),"numeric push second context");Check(host.manager->SetFocus(next_document.Get()),"numeric switch context");
        host.document=next_document;host.context=next_context;host.store=next_store;host.Type("bj");host.Key('3');
        Check(control->HoldCandidates(FALSE),"release two original contexts");WaitText(old_store.Get(),L"北京你好");WaitText(next_store.Get(),choices[2]);
        Check(old_document->Pop(TF_POPF_ALL),"numeric pop original context");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');Pump();host.store->reject_chinese=true;
        Check(control->HoldCandidates(FALSE),"release rejected numeric write");Pump();Require(host.store->text()==L"bj","failed numeric write was replayed or dropped source");
        host.store->reject_chinese=false;host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Type("bj");host.Key('2');Pump();host.store->reject_chinese=true;
        Check(control->HoldCandidates(FALSE),"release rejected write before next choice");Pumps(3);
        Require(host.store->text()==L"bj","rejected queued choices changed original source");
        host.store->reject_chinese=false;host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");}
    for(const auto error:{ERROR_TIMEOUT,ERROR_INVALID_DATA}){
        Host host(factory);ComPtr<ITestCandidateView> control;Check(host.service.As(&control),"numeric error control");
        Check(control->CandidateFailure(error),"fail own candidate result");host.Type("bj");host.Key('2');host.Type("nihao");host.Key(VK_SPACE);
        WaitText(host.store.Get(),L"bjnihao");Check(control->CandidateFailure(ERROR_SUCCESS),"restore candidate results");
        host.Type("nihao");host.Key(VK_SPACE);WaitText(host.store.Get(),L"bjnihao你好");
    }
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Type("nihao");host.Key(VK_SPACE);host.Type("ceshi");
        const auto started=GetTickCount64();Check(host.service->Deactivate(),"numeric choice shutdown");
        Require(GetTickCount64()-started<100,"numeric choice shutdown blocked UI");WaitText(host.store.Get(),L"bjnihao");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');host.Type("bj");host.Key('2');host.Type("nihao");
        const auto started=GetTickCount64();Check(host.service->Deactivate(),"consecutive numeric shutdown");
        Require(GetTickCount64()-started<100,"consecutive choice shutdown blocked UI");WaitText(host.store.Get(),L"bjbj");}
    {Host host(factory);auto control=HoldCandidates(host);host.Type("bj");host.Key('2');
        unsigned accepted{};BOOL eaten{};HRESULT congested=S_OK;
        while(accepted<64){congested=host.test_keys->Key(host.context.Get(),'N',0,&eaten,nullptr);if(FAILED(congested))break;++accepted;}
        Require(accepted>0&&accepted<64&&congested==HRESULT_FROM_WIN32(ERROR_BUSY)&&!eaten,"deferred keys were unbounded or rejected as consumed");
        Check(control->HoldCandidates(FALSE),"release bounded keys");WaitText(host.store.Get(),L"北京"+std::wstring(accepted,L'n'));
        host.Key(VK_RETURN);WaitText(host.store.Get(),L"北京"+std::wstring(accepted,L'n'));}
    {Host host(factory);host.measure_keys=true;host.key_micros.reserve(3072);std::wstring expected;
        for(unsigned batch=0;batch<128;++batch){
            for(unsigned i=0;i<8;++i){host.Type("bj");host.Key('2');expected+=L"北京";}
            WaitText(host.store.Get(),expected);
        }
        Require(expected.size()==2048&&host.key_micros.size()==3072,"rapid digit count");
        std::sort(host.key_micros.begin(),host.key_micros.end());
        const auto percentile=[&](size_t percent){return host.key_micros[(host.key_micros.size()-1)*percent/100];};
        std::printf("Headless fixed-modifier OnKeyDown only: n=%zu p50_us=%.3f p95_us=%.3f p99_us=%.3f max_us=%.3f; excludes modifier capture, OS dispatch, rendering and network\n",
            host.key_micros.size(),percentile(50),percentile(95),percentile(99),host.key_micros.back());}
    Require(GetFileAttributesW(candidates.path.c_str())==INVALID_FILE_ATTRIBUTES,"numeric tests leaked learning data");
    {Host host(factory);auto control=HoldCandidates(host);Check(control->SetLearning(TRUE),"numeric learning enable");host.Type("bj");host.Key('2');
        Check(control->HoldCandidates(FALSE),"release numeric learning");WaitText(host.store.Get(),L"北京");}
    const auto deadline=GetTickCount64()+2000;while(candidates.First("bj")!="北京"&&GetTickCount64()<deadline)Pump();
    Require(candidates.First("bj")=="北京","deferred numeric choice did not learn successful Chinese");
    std::puts("PASS: digits 1-9 with held replies; invalid choice preserves/edit/cancel; ordered suffixes and drafts; focus/shutdown; bounded 64-slot input; 1024 rapid second-candidate commits");
}
void CandidateActions(IClassFactory* factory,TranslationFixture& fixture){
    fixture.Mode(0);
    {Host host(factory);host.Type("nh");auto view=WaitRows(host,9);Check(view->Press(0),"press Chinese");Check(view->ReleasePress(0),"release Chinese");WaitText(host.store.Get(),L"你好");}
    {Host host(factory);host.Type("nihao");auto view=WaitRows(host,9);Check(view->Press(0),"press old candidate");host.Type("x");Check(view->ReleasePress(0),"release changed candidate");Pump();
        Require(host.store->text()==L"nihaox","changed composition mouse press committed text");host.Key(VK_ESCAPE);}
    {Host host(factory);host.Type("nh");auto view=WaitRows(host,9);Check(view->Press(0),"press before readonly");host.store->SetReadOnly(true);Check(view->ReleasePress(0),"release readonly");Pump();
        Require(host.store->text()==L"nh","readonly mouse action committed");host.store->SetReadOnly(false);Check(view->Press(0),"press before focus loss");host.keys->OnSetFocus(FALSE);Check(view->ReleasePress(0),"release after focus loss");Pump();Require(host.store->text().empty(),"focus lost mouse action committed");}
    fixture.Mode(1);
    {Host host(factory);host.Type("bj");auto view=WaitEnglish(host);WaitRows(host,9);const auto writes=host.store->writes;
        Check(view->Press(1),"select Chinese for inline English");Check(view->ReleasePress(1),"release Chinese selection");WaitEnglish(host);
        Require(fixture.Source()=="北京"&&host.store->writes==writes&&host.store->text()==L"bj","Chinese selection committed text or translated wrong row");
        host.store->SetReadOnly(true);Check(view->Press(9),"press readonly English");Check(view->ReleasePress(9),"release readonly English");Pump();Require(host.store->text()==L"bj","readonly inline English committed");
        host.store->SetReadOnly(false);Check(view->Press(9),"press inline English");Check(view->ReleasePress(9),"release inline English");WaitText(host.store.Get(),L"Beijing");}
    fixture.Mode(2);
    {Host host(factory);host.Type("nihao");auto view=WaitRows(host,9);Check(view->Press(0),"press before translation");WaitEnglish(host);Check(view->ReleasePress(0),"release changed inline translation");Pump();
        Require(host.store->text()==L"nihao","arriving translation changed pressed action");Check(view->Press(9),"press current English");Check(view->ReleasePress(9),"release current English");WaitText(host.store.Get(),L"Hello");}
    fixture.Mode(0);
}
void LearningCases(IClassFactory* factory,TranslationFixture& translation,CandidateFixture& candidates){
    translation.Mode(0);
    const auto enable=[](Host& host){ComPtr<ITestCandidateView> view;Check(host.service.As(&view),"learning test control");Check(view->SetLearning(TRUE),"enable test learning");};
    Require(candidates.First("bj")=="比较"&&GetFileAttributesW(candidates.path.c_str())==INVALID_FILE_ATTRIBUTES,"ordinary fixtures recorded selections");
    {Host host(factory);enable(host);host.Type("bj");WaitRows(host,9);host.Key(VK_ESCAPE);Pump();Require(candidates.First("bj")=="比较","preview or cancel learned selection");}
    for(const auto scope:{IS_PASSWORD,IS_PRIVATE,IS_NUMERIC_PASSWORD,IS_NUMERIC_PIN,IS_ALPHANUMERIC_PIN,IS_ALPHANUMERIC_PIN_SET,IS_LOGINNAME}){
        Host host(factory,scope);enable(host);host.Type("bj");WaitRows(host,9);host.Key('2');Pump();
        Require(host.store->text()==L"北京"&&host.store->scope_reads>0&&candidates.First("bj")=="比较","sensitive Chinese commit learned selection");
    }
    {Host host(factory,IS_DEFAULT,true);enable(host);host.Type("bj");WaitRows(host,9);host.Key('2');Pump();Require(candidates.First("bj")=="比较","unknown scope learned selection");}
    {Host host(factory);enable(host);host.Type("bj");auto view=WaitRows(host,9);host.store->SetReadOnly(true);Check(view->Press(1),"readonly learning press");Check(view->ReleasePress(1),"readonly learning release");Pump();host.store->SetReadOnly(false);Require(candidates.First("bj")=="比较","readonly click learned selection");host.Key(VK_ESCAPE);}
    {Host host(factory);enable(host);host.Type("bj");WaitRows(host,9);host.store->reject_chinese=true;host.Key('2');Pump();
        Require(host.store->text()==L"bj"&&candidates.First("bj")=="比较","failed TSF write learned selection");}
    translation.Mode(1);
    for(unsigned choice:{0u,2u,3u}){Host host(factory);enable(host);host.Type("bj");WaitEnglish(host);host.Key(VK_DOWN);auto view=WaitEnglish(host);
        if(!choice){Check(view->Press(9),"English learning exclusion press");Check(view->ReleasePress(9),"English learning exclusion release");}else host.Key('0'+choice);
        WaitText(host.store.Get(),choice==3?L"北京 Beijing":L"Beijing");Require(candidates.First("bj")=="比较","English/bilingual learned model output");}
    Require(GetFileAttributesW(candidates.path.c_str())==INVALID_FILE_ATTRIBUTES,"excluded input created learning file");
    translation.Mode(0);
    {Host host(factory);enable(host);host.Type("bj");WaitRows(host,9);host.Key('2');WaitText(host.store.Get(),L"北京");}
    auto deadline=GetTickCount64()+2000;while(candidates.First("bj")!="北京"&&GetTickCount64()<deadline)Pump();
    Require(candidates.First("bj")=="北京","successful explicit selection was not learned");
    candidates.Restart();
    deadline=GetTickCount64()+2000;while(candidates.First("bj")!="北京"&&GetTickCount64()<deadline)Pump();
    Require(candidates.First("bj")=="北京","learning failed to survive owned broker restart");
    {Host host(factory);enable(host);host.Type("bj");auto view=WaitRows(host,9);Check(view->Press(2),"learn mouse candidate");Check(view->ReleasePress(2),"learn mouse release");WaitText(host.store.Get(),L"不仅");}
    deadline=GetTickCount64()+2000;while(candidates.First("bj")!="不仅"&&GetTickCount64()<deadline)Pump();
    Require(candidates.First("bj")=="不仅","mouse selection did not update recent preference");
}
void TranslationCases(IClassFactory* factory,TranslationFixture& fixture){
    fixture.Mode(2);
    {Host host(factory);host.Type("nihao");WaitRows(host,9);const auto writes=host.store->writes;
        WaitEnglish(host);Pumps(2);Require(host.store->writes==writes&&host.store->text()==L"nihao","inline translation rewrote host text");
        host.Key(VK_RETURN,TiTestControl);WaitText(host.store.Get(),L"Hello");}
    fixture.Mode(1);
    {Host host(factory);host.Type("bj");WaitEnglish(host);WaitRows(host,9);const auto writes=host.store->writes;
        host.Key(VK_DOWN);auto view=WaitEnglish(host);BOOL ready{};uint32_t selected{};Check(view->TranslationState(&ready,&selected),"inline selected row");
        Require(ready&&selected==1&&fixture.Source()=="北京"&&host.store->writes==writes,"arrow selection rewrote text or translated another Chinese candidate");
        host.Key(VK_RETURN,TiTestControl);WaitText(host.store.Get(),L"Beijing");}
    fixture.Mode(1);
    {Host host(factory);host.Type("bj");WaitEnglish(host);host.Key(VK_DOWN);WaitEnglish(host);host.Key('1');WaitText(host.store.Get(),L"北京");}
    {Host host(factory);host.Type("nihao");WaitEnglish(host);host.Key('2');WaitText(host.store.Get(),L"Hello");}
    {Host host(factory);host.Type("nihao");WaitEnglish(host);host.Key('3');WaitText(host.store.Get(),L"你好 Hello");}
    {Host host(factory);host.Type("nihao");WaitEnglish(host);host.Key(VK_NUMPAD2);WaitText(host.store.Get(),L"Hello");}
    {Host host(factory);host.Type("nihao");WaitEnglish(host);host.Key(VK_RETURN);WaitText(host.store.Get(),L"nihao");Pumps(3);Require(host.store->text()==L"nihao","Enter committed a translated result");}
    {Host host(factory);host.Type("nihao");WaitEnglish(host);host.Key('4');WaitText(host.store.Get(),L"你好4");}
    {Host host(factory);host.Type("bj");WaitEnglish(host);host.Key(VK_DOWN);host.Key('2');WaitEnglish(host);host.Key('2');WaitText(host.store.Get(),L"Beijing");}
    {Host host(factory);host.Type("nihao");WaitEnglish(host);host.store->SetReadOnly(true);BOOL eaten{};
        for(const auto key:{'2','3'}){Check(host.test_keys->Key(host.context.Get(),key,0,&eaten,nullptr),"readonly numeric translation");Require(!eaten,"readonly translation key accepted");}
        Require(host.store->text()==L"nihao","readonly numeric translation wrote");host.store->SetReadOnly(false);host.Key(VK_ESCAPE);}
    fixture.Mode(2);
    {Host host(factory);host.Type("nihao");WaitRows(host,9);host.Key('2');host.Key('3');host.Key(VK_RETURN,TiTestControl);Pump();Require(host.store->text()==L"nihao","waiting submitted empty or stale English/bilingual");
        host.Key(VK_SPACE);Pumps(8);Require(host.store->text()==L"你好","waiting Chinese or late-result suppression failed");}
    fixture.Mode(2);
    {Host host(factory);host.Type("nihao");Pumps(2);host.Key(VK_OEM_COMMA);Pumps(8);Require(host.store->text()==L"你好，","punctuation during translation wait lost Chinese");}
    fixture.Mode(2);
    {Host host(factory);host.Type("nihao");Pumps(2);host.Key(VK_ESCAPE);host.Type("beijing");WaitEnglish(host);host.Key(VK_RETURN,TiTestControl);WaitText(host.store.Get(),L"Beijing");}
    fixture.Mode(3);
    {Host host(factory);host.Type("nihao");Pumps(4);fixture.Mode(1);host.Key(VK_F5);WaitEnglish(host);host.Key(VK_RETURN,TiTestControl);WaitText(host.store.Get(),L"Hello");}
    fixture.Mode(0);
    {Host host(factory);host.Type("nihao");Pumps(2);fixture.Mode(1);Pumps(12);host.Key('2');Pump();const auto chinese=host.store->text();
        Require(!chinese.empty()&&chinese!=L"Hello"&&chinese!=L"nihao","mode changed digit meaning mid-composition");
        host.Type("nihao");WaitEnglish(host);host.Key(VK_RETURN,TiTestControl);WaitText(host.store.Get(),chinese+L"Hello");}
    for(const auto scope:{IS_PASSWORD,IS_PRIVATE,IS_NUMERIC_PASSWORD,IS_NUMERIC_PIN,IS_ALPHANUMERIC_PIN,IS_ALPHANUMERIC_PIN_SET,IS_LOGINNAME}){
        fixture.Mode(1);Host host(factory,scope);host.Type("nihao");Pumps(4);Require(host.store->scope_reads>0&&fixture.Count()==0,"sensitive input sent to translator");
        host.Key(VK_SPACE);Pump();Require(host.store->text()==L"你好","sensitive input lost Chinese ability");
    }
    fixture.Mode(1);
    {Host host(factory,IS_DEFAULT,true);host.Type("nihao");Pumps(4);Require(host.store->scope_reads>0&&fixture.Count()==0,"failed scope query sent text");host.Key(VK_SPACE);Pump();}
    fixture.Mode(0);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && (argc!=3||(std::wstring(argv[2])!=L"--digits"&&std::wstring(argv[2])!=L"--pages"&&std::wstring(argv[2])!=L"--mixed"&&std::wstring(argv[2])!=L"--words"&&std::wstring(argv[2])!=L"--spelling"&&std::wstring(argv[2])!=L"--uiless"))) return 2;
    const bool digits=argc==3&&std::wstring(argv[2])==L"--digits";
    const bool pages=argc==3&&std::wstring(argv[2])==L"--pages";
    const bool mixed=argc==3&&std::wstring(argv[2])==L"--mixed";
    const bool words=argc==3&&std::wstring(argv[2])==L"--words";
    const bool spelling=argc==3&&std::wstring(argv[2])==L"--spelling";
    const bool uiless=argc==3&&std::wstring(argv[2])==L"--uiless";
    if(std::wstring(argv[1]).find(L"TransInputPrototypeTest.dll")==std::wstring::npos)return 2;
    try{CandidateClickCases();}catch(const std::exception& error){std::fprintf(stderr,"FAIL: %s\n",error.what());return 1;}
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 3;
    const auto module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) { CoUninitialize(); return 4; }
    try {
        using GetClass = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
        using CanUnload = HRESULT(WINAPI*)();
        const auto get_class = std::bit_cast<GetClass>(GetProcAddress(module, "DllGetClassObject"));
        const auto can_unload = std::bit_cast<CanUnload>(GetProcAddress(module, "DllCanUnloadNow"));
        Require(get_class && can_unload, "missing exports");
        ComPtr<IClassFactory> factory;
        Check(get_class(transinput::ClassId, IID_PPV_ARGS(&factory)), "factory");
        CandidateFixture candidate_fixture;
        TranslationFixture translation_fixture;
        if(uiless)UiLessCases(factory.Get(),translation_fixture,candidate_fixture);
        else if(spelling)SpellingCases(factory.Get(),translation_fixture,candidate_fixture);
        else if(words)PersonalWordCases(factory.Get(),translation_fixture,candidate_fixture);
        else if(mixed)MixedCases(factory.Get(),translation_fixture,candidate_fixture);
        else if(digits)DigitCases(factory.Get(),translation_fixture,candidate_fixture);
        else if(pages)PageCases(factory.Get(),translation_fixture,candidate_fixture);
        else{
        {
            Host host(factory.Get());auto control=HoldCandidates(host);host.Type("nihao");WaitText(host.store.Get(),L"nihao");
            const auto writes=host.store->writes;Check(control->HoldCandidates(FALSE),"release presentation-only candidates");
            WaitRows(host,9);Pumps(2);
            Require(host.store->writes==writes,"candidate presentation rewrote unchanged host text");
            host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");
        }
        {
            Host host(factory.Get());BOOL eaten{};
            for(const auto modifier:{TiTestControl,TiTestAlt,TiTestWin,TiTestCaps}){
                Check(host.test_keys->Key(host.context.Get(),'N',modifier,&eaten,nullptr),"fixed modifier passthrough");Require(!eaten,"fixed shortcut or idle Caps was consumed");
            }
            Require(host.test_keys->Key(host.context.Get(),'N',32,&eaten,nullptr)==E_INVALIDARG,"unknown fixed modifier accepted");
            Require(host.test_keys->Key(host.context.Get(),256,0,&eaten,nullptr)==E_INVALIDARG,"invalid fixed key accepted");
            host.Type("nihao");Check(host.test_keys->Key(host.context.Get(),'1',TiTestShift,&eaten,nullptr),"fixed shifted punctuation");
            Require(eaten,"fixed shifted punctuation escaped");WaitText(host.store.Get(),L"你好！");
        }
        {
            const auto started=GetTickCount64();
            Host host(factory.Get());host.Type("nh");host.Key(VK_SPACE);WaitText(host.store.Get(),L"你好");
            std::printf("Headless host activation + initials commit observation: %llu ms (test includes message-pump intervals)\n",GetTickCount64()-started);
            Require(host.store->text()==L"你好","initials did not cross real broker/TSF boundary");
        }
        {
            Host host(factory.Get());host.Type("srf");host.Key(VK_OEM_COMMA);
            host.Type("rgzn");host.Key(VK_OEM_PERIOD);Pump();
            Require(host.store->text()==L"输入法，人工智能。","pending punctuation lost or preceded its Chinese candidate");
        }
        {
            Host host(factory.Get());host.Type("nihao");Pump();host.Key(VK_OEM_COMMA);Pump();
            Require(host.store->text()==L"你好，","ready candidate punctuation failed");
            host.Type("qqqqqqqq");host.Key(VK_OEM_PERIOD);Pump();
            Require(host.store->text()==L"你好，qqqqqqqq。","unknown pinyin punctuation dropped literal input");
            BOOL eaten=TRUE;Check(host.keys->OnTestKeyDown(host.context.Get(),VK_OEM_PERIOD,0,&eaten),"idle punctuation");
            Require(!eaten,"idle punctuation intercepted numeric/path input");
        }
        {
            Host host(factory.Get());host.Type("nihao");host.Key(VK_OEM_COMMA);
            Check(host.service->Deactivate(),"deactivate with unresolved punctuation");Pump();
            Require(host.store->text()==L"nihao，","shutdown lost accepted literal punctuation fallback");
        }
        {
            Host host(factory.Get());
            host.Type("woshizhongguoren");host.Key(VK_SPACE);Pump();
            Require(host.store->text()==L"我是中国人","real dictionary sentence did not cross broker/TSF boundary");
        }
        {
            Host host(factory.Get());
            host.Type("nihao");host.Key(VK_SPACE);
            auto old_document=host.document;auto old_context=host.context;auto old_store=host.store;
            ComPtr<ITfDocumentMgr> next_document;
            Check(host.manager->CreateDocumentMgr(&next_document),"create second document");
            ComPtr<TextStore> next_store;next_store.Attach(new TextStore(host.window));
            ComPtr<ITfContext> next_context;TfEditCookie cookie{};
            Check(next_document->CreateContext(host.client,0,static_cast<ITextStoreACP*>(next_store.Get()),&next_context,&cookie),"create second context");
            Check(next_document->Push(next_context.Get()),"push second context");
            Check(host.manager->SetFocus(next_document.Get()),"switch original context before response");
            host.document=next_document;host.context=next_context;host.store=next_store;
            host.Type("beijing");host.Key(VK_SPACE);Pump();
            Require(old_store->text()==L"你好"&&next_store->text()==L"北京","accepted commit crossed input contexts");
            Check(old_document->Pop(TF_POPF_ALL),"pop original context");
        }
        {
            Host host(factory.Get());
            host.Type("nihao");host.Key(VK_SPACE);
            const auto started=GetTickCount64();
            Check(host.service->Deactivate(),"deactivate with unresolved commit");
            Require(GetTickCount64()-started<100,"deactivation waited on background worker");
            Pump();Require(host.store->text()==L"nihao","deactivation lost accepted source text");
        }
        {
            Host host(factory.Get());
            host.Type("nihao"); host.Key(VK_SPACE);
            host.Type("zhongwen"); host.Key(VK_SPACE);
            Pump();
            Require(host.store->text() == L"你好中文", "two queued commits lost, reordered or duplicated");
        }
        {
            Host host(factory.Get());
            host.Type("nihao"); host.Key(VK_LEFT); host.Key(VK_LEFT); host.Key(VK_BACK); host.Type("h");
            Pump();
            Require(host.store->text() == L"nihao", "middle edit text mismatch");
            Require(host.store->cursor() == 3, "composition cursor position lost");
            Require(host.store->extent_reads&&host.store->extent_start==0&&host.store->extent_end==0,"candidate anchor did not stay at composition start");
            // Layout notifications must refresh with a read-only session and
            // must not rewrite the user's composition or create a commit.
            const auto writes=host.store->writes;const auto reads=host.store->extent_reads;
            host.store->hold_reads=true;
            for(unsigned i=0;i<32;++i)host.store->NotifyLayout();Pump();
            Require(host.store->extent_reads==reads,"layout accessed text while native read lock was deferred");
            host.store->ReleaseReads();Pump();
            Require(host.store->extent_reads>reads,"layout change did not refresh candidate anchor");
            Require(host.store->extent_reads<=reads+2,"layout reads were not coalesced");
            Require((host.store->extent_lock&TS_LF_READWRITE)==TS_LF_READ,"layout callback requested a write lock");
            Require(host.store->writes==writes&&host.store->text()==L"nihao","layout callback rewrote composition");
            const auto positioned_reads=host.store->extent_reads;
            NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE,host.window,OBJID_WINDOW,CHILDID_SELF);Pump();
            Require(host.store->extent_reads>positioned_reads,"host-window location event did not refresh anchor");
            Require((host.store->extent_lock&TS_LF_READWRITE)==TS_LF_READ&&host.store->writes==writes,"host-window movement rewrote composition");
            const auto unrelated_reads=host.store->extent_reads;
            const auto unrelated=CreateWindowExW(0,L"STATIC",L"own unrelated layout test",0,0,0,1,1,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr);
            Require(unrelated!=nullptr,"own unrelated window");NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE,unrelated,OBJID_WINDOW,CHILDID_SELF);Pump();DestroyWindow(unrelated);
            Require(host.store->extent_reads==unrelated_reads,"unrelated window location refreshed another editor");
            host.Key(VK_SPACE); Pump();
            Require(host.store->text() == L"你好", "middle edit commit mismatch");
            const auto finished_reads=host.store->extent_reads;host.store->NotifyLayout();Pump();
            Require(host.store->extent_reads==finished_reads,"completed composition still read layout");
        }
        {
            Host host(factory.Get());
            host.Type("nihao"); host.Key(VK_ESCAPE); Pump();
            Require(host.store->text().empty(), "cancel left composition text");
            host.Type("n"); host.Key(VK_BACK); Pump();
            Require(host.store->text().empty(), "last backspace left composition");
            host.Type("zzzz"); host.Key(VK_RETURN); Pump();
            Require(host.store->text() == L"zzzz", "literal commit lost");
        }
        {
            Host host(factory.Get());
            host.Type("nihao");
            Check(host.keys->OnSetFocus(FALSE), "focus loss"); Pump();
            Require(host.store->text().empty(), "focus loss left pending composition");
            host.Type("ceshi"); host.Key(VK_SPACE); Pump();
            Require(host.store->text() == L"测试", "focus recovery failed");
        }
        {
            Host host(factory.Get());
            std::wstring expected;
            for (unsigned batch = 0; batch < 64; ++batch) {
                // No message pumping between these 16 independent commits.
                for (unsigned i = 0; i < 16; ++i) {
                    host.Type("nihao"); host.Key(i%2?VK_SPACE:VK_OEM_COMMA); expected += i%2?L"你好":L"你好，";
                }
                Pump();
                Require(host.store->text() == expected, "rapid batches lost or replayed accepted commits");
            }
            Require(expected.size() == 2560, "stress count mismatch");
        }
        {
            Host host(factory.Get());
            for (unsigned i = 0; i < 32; ++i) { host.Type("nihao"); host.Key(VK_SPACE); }
            BOOL eaten{};
            const auto congested = host.test_keys->Key(host.context.Get(), 'N', 0, &eaten,nullptr);
            Require(congested == HRESULT_FROM_WIN32(ERROR_BUSY) && !eaten, "queue limit did not reject unaccepted work");
            Pump();
            host.Type("nihao"); host.Key(VK_SPACE); Pump();
            std::wstring expected;
            for (unsigned i = 0; i < 33; ++i) expected += L"你好";
            Require(host.store->text() == expected, "backpressure recovery replayed rejected input");
        }
        {
            Host host(factory.Get());
            host.store->SetReadOnly(true);
            BOOL eaten = TRUE;
            Check(host.keys->OnTestKeyDown(host.context.Get(), 'N', 0, &eaten), "read-only test");
            Require(!eaten, "read-only host intercepted typing");
            Check(host.keys->OnKeyDown(host.context.Get(), 'N', 0, &eaten), "read-only key");
            Require(!eaten, "read-only key mutated composition");
            host.store->SetReadOnly(false);
            host.Type("ceshi"); host.Key(VK_SPACE); Pump();
            Require(host.store->text() == L"测试", "read-only recovery leaked rejected key");
        }
        CandidateActions(factory.Get(),translation_fixture);
        TranslationCases(factory.Get(),translation_fixture);
        LearningCases(factory.Get(),translation_fixture,candidate_fixture);
        }
        factory.Reset(); Pump();
        Require(can_unload() == S_OK, "active context/COM sink leaked");
        FreeLibrary(module); CoUninitialize();
        if(!digits&&!pages&&!mixed&&!words&&!spelling&&!uiless)std::puts("PASS: real dictionary/initials via broker/TSF; 1024 rapid Chinese/punctuation commits, original contexts, mouse actions, mock translation, private-input suppression; successful-only local learning, excluded preview/private/failed/English choices, encrypted broker restart; nonblocking close and unload");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        FreeLibrary(module); CoUninitialize(); return 1;
    }
}
