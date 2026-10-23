// SPDX-License-Identifier: AGPL-3.0-only
// Independent TSF adapter. Candidate calculation is in an isolated Rust host.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <msctf.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>
#include "identity.h"
#include "async_broker.h"
#include "async_translation.h"
#include "input_scope.h"
#include "input_policy.h"
#include "candidate_interaction.h"
#include "candidate_layout.h"
#include "candidate_ui.h"
#include "candidate_accessibility.h"
#include "session_id.h"
#ifdef TI_TRANSLATION_FIXTURE
#include "../tests/candidate_test_interface.h"
#include "../tests/preview_png.h"
#endif

using Microsoft::WRL::ComPtr;
namespace {
HMODULE module_handle{};
std::atomic<long> live_objects{};
std::atomic<long> server_locks{};
std::atomic<long> broker_channels{};
std::atomic<long> translation_channels{};
constexpr wchar_t candidate_class[] = L"TransInput.Prototype.Candidate.40e2b4b8";
constexpr size_t max_input = 128;
constexpr size_t max_pending = 64;
constexpr size_t english_target = 9;

void TraceFailure(const char* stage, HRESULT result) noexcept {
#ifdef TI_TEST_DIAGNOSTICS
    if (FAILED(result)) std::fprintf(stderr, "TIP %s: 0x%08lx\n", stage, static_cast<unsigned long>(result));
#else
    (void)stage; (void)result;
#endif
}

struct ModuleLease {
    ModuleLease() { ++live_objects; }
    ~ModuleLease() { --live_objects; }
    ModuleLease(const ModuleLease&) = delete;
    ModuleLease& operator=(const ModuleLease&) = delete;
};

// Observe only window locations in the current host process. Root-window moves
// need not generate a TSF text-layout notification (RichEdit is one example).
// No foreign-process subscription, window subclassing, polling or key hook.
class WindowPositionEvents {
    struct Entry {
        ModuleLease lease;
        DWORD thread{GetCurrentThreadId()};HWND owner{},root{};
        std::function<void()> callback;
    };
    inline static std::mutex mutex_;
    inline static std::unordered_map<HWINEVENTHOOK,std::weak_ptr<Entry>> entries_;
    std::shared_ptr<Entry> entry_;
    HWINEVENTHOOK hook_{};
    static void CALLBACK Event(HWINEVENTHOOK hook,DWORD event,HWND window,LONG object,LONG child,DWORD,DWORD)noexcept{
        try{
            if(event!=EVENT_OBJECT_LOCATIONCHANGE||object!=OBJID_WINDOW||child!=CHILDID_SELF)return;
            std::shared_ptr<Entry> entry;
            {std::lock_guard lock(mutex_);const auto found=entries_.find(hook);if(found!=entries_.end())entry=found->second.lock();}
            if(!entry||entry->thread!=GetCurrentThreadId()||!window||(window!=entry->owner&&window!=entry->root))return;
            auto callback=entry->callback;if(callback)callback();
        }catch(...){/* No exception may cross the Windows notification boundary. */}
    }
public:
    explicit WindowPositionEvents(std::function<void()> callback):entry_(std::make_shared<Entry>()){
        entry_->callback=std::move(callback);
        hook_=SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE,nullptr,Event,GetCurrentProcessId(),0,WINEVENT_OUTOFCONTEXT);
        if(!hook_)throw std::runtime_error("current-host window location subscription");
        try{std::lock_guard lock(mutex_);entries_.emplace(hook_,entry_);}
        catch(...){UnhookWinEvent(std::exchange(hook_,nullptr));throw;}
    }
    WindowPositionEvents(const WindowPositionEvents&)=delete;
    WindowPositionEvents& operator=(const WindowPositionEvents&)=delete;
    void Owner(HWND owner)noexcept{entry_->owner=owner;entry_->root=owner?GetAncestor(owner,GA_ROOT):nullptr;}
    ~WindowPositionEvents(){
        if(hook_){
            {std::lock_guard lock(mutex_);entries_.erase(hook_);}
            UnhookWinEvent(hook_);
        }
    }
};

struct CandidateView:std::enable_shared_from_this<CandidateView> {
    struct SelectedFont {HDC dc;HGDIOBJ old;SelectedFont(HDC target,HFONT value):dc(target),old(value?SelectObject(target,value):nullptr){}~SelectedFont(){if(old)SelectObject(dc,old);}};
    struct WindowDC {HWND window;HDC dc;explicit WindowDC(HWND value):window(value),dc(GetDC(value)){}~WindowDC(){if(dc)ReleaseDC(window,dc);}};
    HWND window{};
    HFONT font{};
    HFONT footer_font{};
    static constexpr int popup_width=460,row_height=28,footer_height=58,popup_height=5*row_height+footer_height;
    std::wstring preedit;
    std::vector<std::wstring> candidates;
    std::wstring english;
    bool translation_mode{};
    size_t selected{};
    uint64_t session{};
    uint64_t revision{};
    transinput::CandidateInteraction interaction;
    std::function<void(uint64_t,size_t)> choose;
    std::wstring status{L"空格选择 · Enter 提交拼音"};
    DWORD owner_thread{GetCurrentThreadId()};
    UINT font_dpi{};
    bool visible{};
    bool suppress_window{};
    bool layout_dirty{true};
    uint64_t painted_generation{};
    transinput::CandidateLayout layout;
    transinput::CandidateLayout english_layout;
    int selected_width{};
    transinput::CandidateUiSession ui;
    ComPtr<ITfDocumentMgr> document;
    std::function<HRESULT(transinput::UiAction,size_t,uint64_t)> ui_action;
    RECT last_caret{};HWND last_owner{};bool native_visible{};
    bool pending_frame{};
    static constexpr UINT_PTR frame_timer=1;
    transinput::accessibility::Bridge accessibility;
#ifdef TI_TRANSLATION_FIXTURE
    bool offscreen_fixture{};uint32_t placements{},paints{},shows{},hides{};
#endif

    void NativeHide()noexcept{if(window&&pending_frame)KillTimer(window,frame_timer);pending_frame=false;if(!native_visible)return;native_visible=false;accessibility.Hide();if(window){
#ifdef TI_TRANSLATION_FIXTURE
        ++hides;
#endif
        NotifyWinEvent(EVENT_OBJECT_IME_HIDE,window,OBJID_CLIENT,CHILDID_SELF);ShowWindow(window,SW_HIDE);}}
    void AccessibilitySnapshot()noexcept{
        if(!window||!native_visible||layout_dirty)return;
        try{RECT bounds{};if(!GetWindowRect(window,&bounds))return;std::vector<RECT> rows;rows.reserve(candidates.size());
            for(size_t i=0;i<candidates.size();++i){const auto top=bounds.top+Px(8)+layout.Top(i)-layout.Offset();RECT row{bounds.left+Px(16),std::max(top,bounds.top+Px(8)),i==selected&&!english.empty()?bounds.left+ChineseRight(bounds.right-bounds.left):bounds.right-Px(16),std::min(top+layout.Height(i),bounds.top+Px(8)+layout.Viewport())};if(IsRectEmpty(&row))row={};rows.push_back(row);}
            auto names=candidates;const auto chinese_count=candidates.size();
            if(!english.empty()&&selected<candidates.size()){names.push_back(english);auto translated=EnglishRect(bounds.right-bounds.left);OffsetRect(&translated,bounds.left,bounds.top);rows.push_back(translated);}
            const auto callback=[action=ui_action,chinese_count](transinput::UiAction kind,size_t row,uint64_t generation){return action?action(kind,row==chinese_count?english_target:row,generation):CO_E_OBJNOTCONNECTED;};
            accessibility.Update(window,bounds,std::move(names),std::move(rows),selected,interaction.Generation(),callback,&live_objects);
        }catch(...){/* Accessibility cannot fail a Chinese edit or painting. */}
    }
    void SyncUi(){
        const std::weak_ptr<CandidateView> weak=weak_from_this();
        const auto show=[weak](BOOL value){if(auto self=weak.lock()){if(!value)self->NativeHide();else if(self->visible)self->Position(self->last_caret,self->last_owner);}};
        auto labels=candidates;if(!english.empty()&&selected<labels.size())labels[selected]+=L" — "+english;
        try{if(!ui.Publish(std::move(labels),static_cast<UINT>(selected),interaction.Generation(),document.Get(),&live_objects,ui_action,show))NativeHide();}catch(...){ui.End();NativeHide();}
    }

    int Px(int value)const noexcept{return MulDiv(value,static_cast<int>(font_dpi?font_dpi:96),96);}
    void ContentChanged()noexcept{interaction.Changed();layout_dirty=true;english_layout.Reset();}
    int ChineseRight(int width)const noexcept{return std::max(Px(64),std::min(selected_width?selected_width:Px(230),width/3));}
    RECT EnglishRect(int width)const noexcept{
        if(english.empty()||selected>=candidates.size())return {};
        const auto top=Px(8)+layout.Top(selected)-layout.Offset();
        return {ChineseRight(width)+Px(24),std::max(Px(8),top+Px(2)),width-Px(16),std::min(Px(8)+layout.Viewport(),top+Px(row_height-2))};
    }
    void MeasureLayout(HDC dc,RECT bounds){
        layout.Set(std::vector<int>(candidates.size(),Px(row_height)),std::max(0,static_cast<int>(bounds.bottom)-Px(footer_height)));
        layout.EnsureVisible(selected);
        if(selected<candidates.size()){const auto label=std::to_wstring(selected+1)+L". "+candidates[selected];SIZE extent{};GetTextExtentPoint32W(dc,label.data(),static_cast<int>(label.size()),&extent);selected_width=Px(16)+extent.cx;}
        RECT measure{0,0,std::max(1,static_cast<int>(bounds.right)-ChineseRight(bounds.right)-Px(40)),0};
        if(!english.empty())DrawTextW(dc,english.data(),static_cast<int>(english.size()),&measure,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL);
        english_layout.Set({std::max(1,static_cast<int>(measure.bottom))},Px(row_height-4));layout_dirty=false;
    }
    void ScrollTo(int offset)noexcept{
        if(layout.ScrollTo(offset)){interaction.Changed();Refresh();}
    }
    void Draw(HDC dc,RECT bounds){
        HBRUSH background=CreateSolidBrush(RGB(12,24,60));FillRect(dc,&bounds,background);DeleteObject(background);
        SelectedFont selected_font(dc,font);SetBkMode(dc,TRANSPARENT);
        if(layout_dirty)MeasureLayout(dc,bounds);
        RECT text{};
        const int saved=SaveDC(dc);IntersectClipRect(dc,0,Px(8),bounds.right,Px(8)+layout.Viewport());
        for(size_t row=0;row<candidates.size();++row){
            const auto top=Px(8)+layout.Top(row)-layout.Offset();const auto height=layout.Height(row);
            if(top+height<=Px(8)||top>=Px(8)+layout.Viewport())continue;
            if(row==selected){RECT highlight{Px(8),top,bounds.right-Px(8),top+height};const auto brush=CreateSolidBrush(RGB(22,46,87));FillRect(dc,&highlight,brush);DeleteObject(brush);}
            text={Px(16),top+Px(2),row==selected&&!english.empty()?ChineseRight(bounds.right):bounds.right-Px(16),top+height-Px(2)};
            SetTextColor(dc,row==selected?RGB(61,236,212):RGB(245,247,255));
            const auto label=std::to_wstring(row+1)+L". "+candidates[row];
            DrawTextW(dc,label.data(),static_cast<int>(label.size()),&text,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
            if(row==selected&&!english.empty()){
                auto dash=text;dash.left=ChineseRight(bounds.right);dash.right=dash.left+Px(20);DrawTextW(dc,L"—",1,&dash,DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
                const auto clip=EnglishRect(bounds.right);const auto english_saved=SaveDC(dc);IntersectClipRect(dc,clip.left,clip.top,clip.right,clip.bottom);
                const auto text_top=top+Px(2)+std::max(0,(Px(row_height-4)-english_layout.Content())/2)-english_layout.Offset();
                RECT translated{clip.left,text_top,clip.right,text_top+english_layout.Content()};
                SetTextColor(dc,RGB(219,231,247));DrawTextW(dc,english.data(),static_cast<int>(english.size()),&translated,DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL);RestoreDC(dc,english_saved);
            }
        }
        RestoreDC(dc,saved);SetTextColor(dc,RGB(160,177,205));SelectedFont footer(dc,footer_font);
        text={Px(16),bounds.bottom-Px(44),bounds.right-Px(16),bounds.bottom-Px(25)};
        DrawTextW(dc,preedit.c_str(),-1,&text,DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
        text={Px(16),bounds.bottom-Px(23),bounds.right-Px(16),bounds.bottom-Px(5)};
        const auto hint=english_layout.MaxOffset()&&!english.empty()?L"滚轮查看英文 · "+status:status;
        DrawTextW(dc,hint.c_str(),-1,&text,DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
        AccessibilitySnapshot();
    }

    ~CandidateView() { Close(); }
    void Close() noexcept {
        ui.End();document.Reset();ui_action={};NativeHide();accessibility.Disconnect();
        choose={};interaction.Cancel();
        visible = false;
        if (window) { auto old = std::exchange(window, nullptr); DestroyWindow(old); }
        if (font) { DeleteObject(font); font = nullptr; }
        if (footer_font) { DeleteObject(footer_font); footer_font = nullptr; }
    }
    void Hide() noexcept { visible = false;interaction.Cancel();ui.End();document.Reset();NativeHide(); }
    void Suspend() noexcept {interaction.Cancel();painted_generation=0;NativeHide();}
    void Refresh() noexcept {
        if(!window)return;
        // Do not flash a blank intermediate frame between a key and its local
        // asynchronous result. This never delays input, query submission, or
        // invalidation of old hit targets; only painting waits, at most 40 ms.
        if(native_visible&&candidates.empty()){
            if(!pending_frame){pending_frame=SetTimer(window,frame_timer,40,nullptr)!=0;if(!pending_frame)InvalidateRect(window,nullptr,FALSE);}
            return;
        }
        if(pending_frame){KillTimer(window,frame_timer);pending_frame=false;}
        InvalidateRect(window,nullptr,FALSE);
    }
    std::optional<size_t> HitTest(LPARAM point) const noexcept {
        if(!visible||!window||!font_dpi||layout_dirty||painted_generation!=interaction.Generation())return {};
        RECT bounds{};if(!GetClientRect(window,&bounds))return {};
        const auto px=[this](int value){return MulDiv(value,static_cast<int>(font_dpi),96);};
        const int x=static_cast<short>(LOWORD(point)),y=static_cast<short>(HIWORD(point));
        if(x<px(16)||x>=bounds.right-px(16))return {};
        if(y<px(8)||y>=px(8)+layout.Viewport())return {};
        const POINT point_client{x,y};const auto english_bounds=EnglishRect(bounds.right);
        if(!english.empty()&&PtInRect(&english_bounds,point_client))return english_target;
        return layout.Hit(y-px(8));
    }
    void Position(RECT caret, HWND owner) {
        last_caret=caret;last_owner=owner;
        if (!visible || owner_thread != GetCurrentThreadId()) return;
        if(suppress_window||!ui.Allowed()){Suspend();return;}
        if(owner&&(!IsWindowVisible(owner)||IsIconic(GetAncestor(owner,GA_ROOT)))){
            Suspend();return;
        }
        if (!window) {
            WNDCLASSEXW wc{sizeof wc};
            wc.hInstance = module_handle;
            wc.lpfnWndProc = Procedure;
            wc.lpszClassName = candidate_class;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
            window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                candidate_class, L"TransInput", WS_POPUP, 0, 0, popup_width, popup_height,
                owner, nullptr, module_handle, this);
            if (!window) return;
        }
        if(GetWindow(window,GW_OWNER)!=owner)SetWindowLongPtrW(window,GWLP_HWNDPARENT,reinterpret_cast<LONG_PTR>(owner));
        const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForWindow(window);
        if (!font || font_dpi != dpi) {
            if (font) DeleteObject(font);
            if (footer_font) DeleteObject(footer_font);
            font = CreateFontW(-MulDiv(16, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            footer_font=CreateFontW(-MulDiv(13,static_cast<int>(dpi),96),0,0,0,FW_NORMAL,
                FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
            font_dpi = dpi;
            layout_dirty=true;
        }
        MONITORINFO monitor{sizeof monitor};
        GetMonitorInfoW(MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width=std::min(Px(popup_width),static_cast<int>(monitor.rcWork.right-monitor.rcWork.left));
        WindowDC target(window);if(!target.dc)return;SelectedFont selected_font(target.dc,font);
        const int maximum=std::max(Px(100),static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top)-Px(12));
        RECT previous{};GetWindowRect(window,&previous);const bool was_visible=native_visible;
        // Five stable rows plus preedit/status. Neither candidate count nor a
        // long translation may resize the popup or change its native frame.
        const int height=std::min(Px(popup_height),maximum);RECT content_bounds{0,0,width,height};
        if(layout_dirty)MeasureLayout(target.dc,content_bounds);
        int x = std::clamp(caret.left, monitor.rcWork.left, std::max(monitor.rcWork.left, monitor.rcWork.right - width));
        const int gap = MulDiv(6, static_cast<int>(dpi), 96);
        int y = caret.bottom + gap;
        if (y + height > monitor.rcWork.bottom) y = caret.top - height - gap;
        y = std::max(y, static_cast<int>(monitor.rcWork.top));
#ifdef TI_TRANSLATION_FIXTURE
        // Native rendering regression stays outside the user's desktop. This
        // switch and the probe interface do not exist in the production DLL.
        if(offscreen_fixture){x=GetSystemMetrics(SM_XVIRTUALSCREEN)-4096;y=GetSystemMetrics(SM_YVIRTUALSCREEN)-4096;}
#endif
        const bool moved=previous.left!=x||previous.top!=y;
        const bool resized=previous.right-previous.left!=width||previous.bottom-previous.top!=height;
        if(!was_visible||moved||resized){
            UINT flags=SWP_NOACTIVATE|SWP_NOOWNERZORDER;
            if(was_visible)flags|=SWP_NOZORDER;else flags|=SWP_SHOWWINDOW;
            if(!moved)flags|=SWP_NOMOVE;if(!resized)flags|=SWP_NOSIZE;
            if(!SetWindowPos(window,HWND_TOPMOST,x,y,width,height,flags))return;
#ifdef TI_TRANSLATION_FIXTURE
            ++placements;
#endif
        }
        native_visible=true;
        AccessibilitySnapshot();
        if(!was_visible){
#ifdef TI_TRANSLATION_FIXTURE
            ++shows;
#endif
            NotifyWinEvent(EVENT_OBJECT_IME_SHOW,window,OBJID_CLIENT,CHILDID_SELF);accessibility.Show(&live_objects);}
        else if(moved||resized)NotifyWinEvent(EVENT_OBJECT_IME_CHANGE,window,OBJID_CLIENT,CHILDID_SELF);
        if(!was_visible||resized)Refresh();
    }
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto self = reinterpret_cast<CandidateView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if(self&&self->window==hwnd){self->window=nullptr;self->native_visible=false;self->accessibility.Disconnect();}
        }
        if(self){
            try{
                if(message==WM_TIMER&&wparam==frame_timer){KillTimer(hwnd,frame_timer);self->pending_frame=false;InvalidateRect(hwnd,nullptr,FALSE);return 0;}
                if(message==WM_GETOBJECT&&self->window==hwnd&&self->native_visible&&static_cast<LONG>(lparam)==UiaRootObjectId)return UiaReturnRawElementProvider(hwnd,wparam,lparam,self->accessibility.Root(&live_objects));
                if(message==WM_DESTROY)self->accessibility.Disconnect();
                if(message==WM_MOUSEWHEEL){
                    POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};ScreenToClient(hwnd,&point);RECT bounds{};GetClientRect(hwnd,&bounds);const auto english_bounds=self->EnglishRect(bounds.right);
                    const auto wheel=GET_WHEEL_DELTA_WPARAM(wparam);
                    if(!self->english.empty()&&PtInRect(&english_bounds,point)){
                        // Keep successive views overlapping: the compact English
                        // viewport is shorter than the old 32-pixel wheel step.
                        const auto step=std::min(self->Px(row_height-8),self->english_layout.Viewport());
                        if(self->english_layout.ScrollTo(self->english_layout.Offset()-wheel*step/WHEEL_DELTA)){self->interaction.Changed();self->Refresh();}
                    }else self->ScrollTo(self->layout.Offset()-wheel*self->Px(row_height)/WHEEL_DELTA);
                    return 0;
                }
                if(message==WM_VSCROLL){
                    int offset=self->layout.Offset();SCROLLINFO info{sizeof info,SIF_TRACKPOS};
                    switch(LOWORD(wparam)){
                    case SB_LINEUP:offset-=self->Px(32);break;case SB_LINEDOWN:offset+=self->Px(32);break;
                    case SB_PAGEUP:offset-=self->layout.Viewport();break;case SB_PAGEDOWN:offset+=self->layout.Viewport();break;
                    case SB_TOP:offset=0;break;case SB_BOTTOM:offset=self->layout.MaxOffset();break;
                    case SB_THUMBTRACK:case SB_THUMBPOSITION:if(GetScrollInfo(hwnd,SB_VERT,&info))offset=info.nTrackPos;break;
                    default:break;
                    }
                    self->ScrollTo(offset);return 0;
                }
                if(message==WM_CAPTURECHANGED||message==WM_CANCELMODE)self->interaction.Cancel();
                if(message==WM_LBUTTONDOWN){
                    const auto row=self->HitTest(lparam);self->interaction.Press(row);
                    if(row)SetCapture(hwnd);return 0;
                }
                if(message==WM_LBUTTONUP){
                    const auto row=self->interaction.Release(self->HitTest(lparam));
                    const auto generation=self->interaction.Generation();
                    auto callback=self->choose;
                    if(GetCapture()==hwnd)ReleaseCapture();
                    // Callback can synchronously close/destroy this view.
                    if(row&&callback)callback(generation,*row);
                    return 0;
                }
            }catch(...){return 0;}
        }
        if (message == WM_PAINT && self) {
            const auto lifetime=self->shared_from_this();
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(hwnd, &paint);
            if(self->pending_frame){EndPaint(hwnd,&paint);return 0;}
            RECT bounds{}; GetClientRect(hwnd, &bounds);
            // Draw a complete frame offscreen; direct FillRect/text operations
            // briefly exposed a blank popup for every candidate update.
            const auto memory=CreateCompatibleDC(dc);
            const auto bitmap=memory?CreateCompatibleBitmap(dc,std::max(1L,bounds.right),std::max(1L,bounds.bottom)):nullptr;
            const auto old=bitmap?SelectObject(memory,bitmap):nullptr;
            try{self->Draw(bitmap?memory:dc,bounds);if(bitmap)BitBlt(dc,0,0,bounds.right,bounds.bottom,memory,0,0,SRCCOPY);self->painted_generation=self->interaction.Generation();
#ifdef TI_TRANSLATION_FIXTURE
                ++self->paints;
#endif
            }catch(...){self->painted_generation=0;}
            if(old)SelectObject(memory,old);if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);
            EndPaint(hwnd, &paint);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

enum class OperationKind { Update, Commit, Cancel, Resolve };
struct Operation {
    OperationKind kind;std::wstring text;size_t cursor{};uint64_t request{};size_t candidate{};
    std::wstring suffix,learning_input,learning_text;
    bool conditional{};
};
// Captured once at the physical key boundary; draining never rereads modifiers
// or injects keys into the host. These are composition commands, not OS events.
struct LogicalKey {WPARAM key{};wchar_t punctuation{};bool translation_enabled{};int page_direction{};wchar_t literal{};bool direct{},english{};};
struct ContextState {
    ComPtr<ITfContext> context;
    ComPtr<ITfComposition> composition;
    ComPtr<ITfCompositionSink> sink;
    ComPtr<ITfSource> layout_source;
    DWORD layout_cookie{TF_INVALID_COOKIE};
    bool anchor_pending{};
    std::unique_ptr<WindowPositionEvents> window_events;
    std::shared_ptr<CandidateView> view;
    std::deque<Operation> pending;
    std::deque<LogicalKey> deferred;
    uint64_t choice_request{};
    bool cancel_after_drain{};
    bool draining{};
    std::wstring input;
    std::wstring candidate_source;
    std::vector<std::wstring> candidates;
    uint64_t session{transinput::NewSessionId()};
    uint64_t revision{};
    uint64_t translation_session{},translation_revision{};
    std::wstring translation_source,translation_output;
    uint32_t translation_status{1};
    bool translation_mode{};
    bool scope_pending{};
    transinput::Scope scope{transinput::Scope::Unknown};
    std::function<bool(transinput::TranslationUpdate)> translate;
    std::function<bool(transinput::SelectionLearn)> learn;
    size_t cursor{};
    size_t selected{};
    uint32_t page{},candidate_total{};
    TfClientId client{TF_CLIENTID_NULL};
    DWORD owner_thread{GetCurrentThreadId()};
    bool scheduled{};
    bool ending{};
    bool presentation_enabled{true};
    bool unavailable{};
    HRESULT last_error{S_OK};
    ~ContextState(){if(layout_source&&layout_cookie!=TF_INVALID_COOKIE)layout_source->UnadviseSink(layout_cookie);}
};

std::string Utf8(const std::wstring& text){
    if(text.empty())return {};
    const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(!size)throw std::runtime_error("translation source UTF-16");std::string bytes(size,'\0');
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),bytes.data(),size,nullptr,nullptr))throw std::runtime_error("translation source UTF-16");return bytes;
}
std::wstring Wide(const std::string& text){
    if(text.empty())return {};
    const auto size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!size)throw std::runtime_error("translation output UTF-8");std::wstring wide(size,L'\0');
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),wide.data(),size))throw std::runtime_error("translation output UTF-8");return wide;
}
bool TranslationMode(const ContextState& state){return state.translation_mode&&state.scope!=transinput::Scope::Blocked;}
void InvalidateTranslation(ContextState& state){
    if(state.translate&&state.translation_session&&state.translation_revision)state.translate({transinput::translation::Clear,state.translation_session,state.translation_revision,{}});
    ++state.translation_revision;state.translation_source.clear();state.translation_output.clear();state.translation_status=1;
}
void OfferTranslation(ContextState& state){
    if(!state.presentation_enabled||!TranslationMode(state)||state.scope!=transinput::Scope::Allowed||state.input.empty()||
        state.candidate_source!=state.input||state.selected>=state.candidates.size()||!state.translate)return;
    const auto& text=state.candidates[state.selected];if(text==state.translation_source)return;
    InvalidateTranslation(state);state.translation_source=text;state.translation_status=2;
    if(!state.translate({transinput::translation::Observe,state.translation_session,state.translation_revision,Utf8(text)}))state.translation_status=5;
}
void DiscardCommands(ContextState& state) noexcept {
    state.pending.clear();state.deferred.clear();state.choice_request=0;
    state.input.clear();state.candidate_source.clear();state.candidates.clear();state.cursor=0;state.selected=0;state.page=0;state.candidate_total=0;++state.revision;
    try{InvalidateTranslation(state);if(state.view&&state.view->session==state.session)state.view->Hide();}catch(...){}
}

class CompositionSink final : public ITfCompositionSink {
    ModuleLease lease_;
    std::atomic<ULONG> references_{1};
    std::weak_ptr<ContextState> state_;
public:
    explicit CompositionSink(std::weak_ptr<ContextState> state) : state_(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfCompositionSink) *result = static_cast<ITfCompositionSink*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie, ITfComposition*) override {
        if (auto state = state_.lock()) {
            if (state->owner_thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
            state->composition.Reset();
            if (!state->ending) {
                DiscardCommands(*state);
            }
        }
        return S_OK;
    }
};

bool CanPosition(const ContextState& state) noexcept {
    return state.presentation_enabled&&state.view&&state.view->session==state.session&&state.view->visible&&state.composition&&!state.input.empty();
}
void PositionCandidates(ContextState& state,TfEditCookie cookie){
    if(!CanPosition(state))return;
    ComPtr<ITfContextView> context_view;HWND owner{};
    if(FAILED(state.context->GetActiveView(&context_view))||!context_view||FAILED(context_view->GetWnd(&owner))||!owner){state.view->Suspend();return;}
    if(state.window_events)state.window_events->Owner(owner);
    ComPtr<ITfRange> anchor;
    const auto selection_result=state.composition->GetRange(&anchor);
    // Keep the popup anchored to the beginning of this composition. Following
    // every letter's moving caret made the whole candidate window jump/repaint.
    RECT caret{};BOOL clipped{};
    if(FAILED(selection_result)||!anchor||FAILED(anchor->Collapse(cookie,TF_ANCHOR_START))){state.view->Suspend();return;}
    auto extent=context_view->GetTextExt(cookie,anchor.Get(),&caret,&clipped);
    if(extent!=TS_E_NOLAYOUT&&(FAILED(extent)||clipped||caret.bottom<=caret.top)){
        // If the composition start has scrolled out of view, use the visible
        // active caret rather than leave the user without a candidate window.
        TF_SELECTION selection{};ULONG fetched{};const auto result=state.context->GetSelection(cookie,TF_DEFAULT_SELECTION,1,&selection,&fetched);
        ComPtr<ITfRange> fallback;fallback.Attach(selection.range);
        if(SUCCEEDED(result)&&fetched==1&&fallback&&SUCCEEDED(fallback->Collapse(cookie,selection.style.ase==TF_AE_START?TF_ANCHOR_START:TF_ANCHOR_END)))extent=context_view->GetTextExt(cookie,fallback.Get(),&caret,&clipped);
    }
    // A pending layout is normal immediately after editing in Chromium hosts.
    // Leave an already shown frame intact until OnLayoutChange supplies a new
    // caret; never show a new popup using guessed/previous coordinates.
    if(extent==TS_E_NOLAYOUT&&state.view->last_owner==owner)return;
    if(FAILED(extent)||clipped||caret.bottom<=caret.top){state.view->Suspend();return;}
    state.view->Position(caret,owner);
}
class AnchorSession final : public ITfEditSession {
    ModuleLease lease_;std::atomic<ULONG> references_{1};std::shared_ptr<ContextState> state_;
public:
    explicit AnchorSession(std::shared_ptr<ContextState> state):state_(std::move(state)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(iid!=IID_IUnknown&&iid!=IID_ITfEditSession)return E_NOINTERFACE;
        *result=static_cast<ITfEditSession*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        if(state_->owner_thread!=GetCurrentThreadId())return RPC_E_WRONG_THREAD;
        struct Complete{ContextState& state;~Complete(){state.anchor_pending=false;}}complete{*state_};
        try{
            if(state_->scope_pending&&state_->composition){
                ComPtr<ITfRange> range;if(SUCCEEDED(state_->composition->GetRange(&range))&&range){
                    const auto scope=transinput::ReadScope(state_->context.Get(),range.Get(),cookie);
                    if(state_->scope!=transinput::Scope::Blocked)state_->scope=scope;
                    state_->scope_pending=false;OfferTranslation(*state_);
                }
            }
            PositionCandidates(*state_,cookie);return S_OK;
        }catch(...){if(state_->view)state_->view->Suspend();return E_FAIL;}
    }
};
HRESULT ScheduleAnchor(const std::shared_ptr<ContextState>& state){
    if(state->anchor_pending||!CanPosition(*state))return S_OK;
    ComPtr<AnchorSession> edit;edit.Attach(new AnchorSession(state));state->anchor_pending=true;
    HRESULT result{};const auto hr=state->context->RequestEditSession(state->client,edit.Get(),TF_ES_ASYNC|TF_ES_READ,&result);
    if(FAILED(hr)||FAILED(result)){state->anchor_pending=false;state->view->Suspend();return FAILED(hr)?hr:result;}
    return S_OK;
}
class LayoutSink final : public ITfTextLayoutSink {
    ModuleLease lease_;std::atomic<ULONG> references_{1};std::weak_ptr<ContextState> state_;
public:
    explicit LayoutSink(std::weak_ptr<ContextState> state):state_(std::move(state)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result)return E_POINTER;*result=nullptr;
        if(iid!=IID_IUnknown&&iid!=IID_ITfTextLayoutSink)return E_NOINTERFACE;
        *result=static_cast<ITfTextLayoutSink*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto count=--references_;if(!count)delete this;return count;}
    HRESULT STDMETHODCALLTYPE OnLayoutChange(ITfContext* context,TfLayoutCode code,ITfContextView*) override {
        try{
            const auto state=state_.lock();if(!state||state->context.Get()!=context)return S_OK;
            if(state->owner_thread!=GetCurrentThreadId())return RPC_E_WRONG_THREAD;
            if(code==TF_LC_DESTROY){if(CanPosition(*state))state->view->Suspend();return S_OK;}
            // One outstanding read per context. No synchronous write request
            // from this notification, and no text or candidate query replay.
            TraceFailure("candidate layout read",ScheduleAnchor(state));return S_OK;
        }catch(...){return E_FAIL;}
    }
};
HRESULT AttachLayout(const std::shared_ptr<ContextState>& state){
    const std::weak_ptr<ContextState> weak=state;
    try{state->window_events=std::make_unique<WindowPositionEvents>([weak]{if(const auto current=weak.lock())TraceFailure("candidate window-move read",ScheduleAnchor(current));});}
    catch(...){TraceFailure("candidate window-move subscription",E_FAIL);}
    auto hr=state->context.As(&state->layout_source);if(FAILED(hr))return hr;
    ComPtr<LayoutSink> sink;sink.Attach(new LayoutSink(state));
    hr=state->layout_source->AdviseSink(IID_ITfTextLayoutSink,sink.Get(),&state->layout_cookie);
    if(FAILED(hr)){state->layout_source.Reset();state->layout_cookie=TF_INVALID_COOKIE;}return hr;
}

HRESULT EditOperation(ContextState& state, TfEditCookie cookie, const Operation& operation) {
    auto learning_scope=transinput::Scope::Unknown;
    ComPtr<ITfRange> range;
    if (state.composition) {
        HRESULT hr = state.composition->GetRange(&range);
        if (FAILED(hr)) return hr;
    } else if (operation.kind != OperationKind::Cancel) {
        ComPtr<ITfInsertAtSelection> insertion;
        HRESULT hr = state.context.As(&insertion);
        if (FAILED(hr)) return hr;
        hr = insertion->InsertTextAtSelection(cookie, TF_IAS_QUERYONLY, nullptr, 0, &range);
        if (FAILED(hr)) return hr;
        if (operation.kind == OperationKind::Update) {
            ComPtr<ITfContextComposition> compositions;
            hr = state.context.As(&compositions);
            if (FAILED(hr)) return hr;
            hr = compositions->StartComposition(cookie, range.Get(), state.sink.Get(), &state.composition);
            if (FAILED(hr)) return hr;
        }
    }
    if (range) {
        if(operation.kind==OperationKind::Commit&&!operation.learning_input.empty()&&!operation.learning_text.empty()&&state.learn)
            learning_scope=transinput::ReadScope(state.context.Get(),range.Get(),cookie);
        if(operation.kind==OperationKind::Update&&state.translation_mode){
            const auto scope=transinput::ReadScope(state.context.Get(),range.Get(),cookie);
            if(state.scope!=transinput::Scope::Blocked)state.scope=scope;
            if(state.scope!=transinput::Scope::Allowed&&!state.translation_source.empty())InvalidateTranslation(state);
            state.scope_pending=false;
        }
        const auto& text = operation.text;
        HRESULT hr = range->SetText(cookie, 0, text.data(), static_cast<LONG>(text.size()));
        if (FAILED(hr)) return hr;
        ComPtr<ITfRange> selection;
        hr = range->Clone(&selection);
        if (FAILED(hr)) return hr;
        if (operation.kind == OperationKind::Update) {
            LONG shifted{};
            hr = selection->ShiftStart(cookie, static_cast<LONG>(operation.cursor), &shifted, nullptr);
            if (FAILED(hr)) return hr;
            hr = selection->Collapse(cookie, TF_ANCHOR_START);
        } else hr = selection->Collapse(cookie, TF_ANCHOR_END);
        if (FAILED(hr)) return hr;
        TF_SELECTION current{selection.Get(), {TF_AE_NONE, FALSE}};
        hr = state.context->SetSelection(cookie, 1, &current);
        if (FAILED(hr)) return hr;
    }
    if (operation.kind != OperationKind::Update && state.composition) {
        state.ending = true;
        const HRESULT hr = state.composition->EndComposition(cookie);
        state.ending = false;
        state.composition.Reset();
        if (FAILED(hr)) return hr;
    }
    if(operation.kind==OperationKind::Update)OfferTranslation(state);
    if(learning_scope==transinput::Scope::Allowed&&state.learn){
        // Learning is best effort after the complete TSF operation succeeded.
        // Allocation/queue/disk failure must never fail or replay committed text.
        try{state.learn({state.session,0,0,Utf8(operation.learning_input),Utf8(operation.learning_text)});}catch(...){}
    }
    return S_OK;
}

class EditSession final : public ITfEditSession {
    ModuleLease lease_;
    std::atomic<ULONG> references_{1};
    std::shared_ptr<ContextState> state_;
public:
    explicit EditSession(std::shared_ptr<ContextState> state) : state_(std::move(state)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfEditSession) *result = static_cast<ITfEditSession*>(this);
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
        if (state_->owner_thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
        try {
            size_t processed{};
            while (!state_->pending.empty() && processed++ < max_pending) {
                if(state_->pending.front().kind==OperationKind::Resolve)break;
                auto operation = std::move(state_->pending.front()); state_->pending.pop_front();
                state_->last_error = EditOperation(*state_, cookie, operation);
                if (FAILED(state_->last_error)) {TraceFailure("text edit operation",state_->last_error);state_->unavailable=true;DiscardCommands(*state_);break; }
            }
            state_->scheduled = false;
            if(SUCCEEDED(state_->last_error))ScheduleAnchor(state_);
            return state_->last_error;
        } catch (...) { state_->scheduled=false;state_->last_error=E_FAIL;state_->unavailable=true;DiscardCommands(*state_);return E_FAIL; }
    }
};

HRESULT Schedule(const std::shared_ptr<ContextState>& state){
    if(state->scheduled||state->pending.empty()||state->pending.front().kind==OperationKind::Resolve)return S_OK;
    ComPtr<EditSession> edit;edit.Attach(new EditSession(state));state->scheduled=true;
    HRESULT result{};const auto hr=state->context->RequestEditSession(state->client,edit.Get(),TF_ES_ASYNC|TF_ES_READWRITE,&result);
    if(FAILED(hr)||FAILED(result)){state->scheduled=false;state->last_error=FAILED(hr)?hr:result;return state->last_error;}
    return S_OK;
}
HRESULT Enqueue(const std::shared_ptr<ContextState>& state, Operation operation) {
    if (state->owner_thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    // An unsuccessful scheduling attempt must not become a delayed commit when
    // the host later accepts another key. Preallocate before mutating the queue.
    std::deque<Operation> previous;
    const bool can_schedule=!state->scheduled&&(state->pending.empty()||state->pending.front().kind!=OperationKind::Resolve);
    if(can_schedule)previous=state->pending;
    if (operation.kind == OperationKind::Update && !state->pending.empty()
        && state->pending.back().kind == OperationKind::Update) state->pending.back() = std::move(operation);
    else {
        if (state->pending.size() == max_pending) return HRESULT_FROM_WIN32(ERROR_BUSY);
        state->pending.push_back(std::move(operation));
    }
    try{const auto hr=Schedule(state);if(FAILED(hr))state->pending.swap(previous);return hr;}
    catch(...){if(can_schedule)state->pending.swap(previous);throw;}
}

std::wstring WideText(const TiText& text){
    if(text.length>TI_TEXT_CAPACITY)throw std::runtime_error("candidate size");
    if(!text.length)return {};
    const auto bytes=reinterpret_cast<const char*>(text.bytes);
    const auto count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,static_cast<int>(text.length),nullptr,0);
    if(!count)throw std::runtime_error("candidate UTF-8");
    std::wstring value(count,L'\0');
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,bytes,static_cast<int>(text.length),value.data(),count))throw std::runtime_error("candidate UTF-8");
    return value;
}

class TextService final : public ITfTextInputProcessorEx, public ITfKeyEventSink, public ITfThreadMgrEventSink
#ifdef TI_TRANSLATION_FIXTURE
    ,public ITestCandidateView
#endif
{
    struct ChooseTarget {TextService* owner{};};
    std::shared_ptr<ChooseTarget> choose_target_;
    ModuleLease lease_;
    std::atomic<ULONG> references_{1};
    ComPtr<ITfThreadMgr> manager_;
    ComPtr<ITfKeystrokeMgr> keys_;
    ComPtr<ITfSource> source_;
    std::shared_ptr<CandidateView> view_;
    std::shared_ptr<ContextState> state_;
    std::unique_ptr<transinput::AsyncBroker> broker_;
    std::unique_ptr<transinput::AsyncTranslation> translator_;
    std::unordered_map<uint64_t,std::shared_ptr<ContextState>> pending_commits_;
    uint64_t next_request_{1};
    std::array<bool, 256> key_down_{};
    DWORD cookie_{TF_INVALID_COOKIE};
    DWORD owner_thread_{};
    TfClientId client_{TF_CLIENTID_NULL};
    bool allow_learning_{},learning_enabled_{};
    bool stopping_{};
#ifdef TI_TRANSLATION_FIXTURE
    bool hold_candidates_{};
    DWORD candidate_failure_{};
    std::deque<transinput::BrokerReply> held_candidates_;
    std::optional<uint32_t> test_modifiers_;
#endif

    SHORT KeyState(int key)const noexcept{
#ifdef TI_TRANSLATION_FIXTURE
        if(test_modifiers_){
            const uint32_t flag=key==VK_SHIFT?TiTestShift:key==VK_CONTROL?TiTestControl:key==VK_MENU?TiTestAlt:
                key==VK_CAPITAL?TiTestCaps:(key==VK_LWIN||key==VK_RWIN)?TiTestWin:0;
            if(!(*test_modifiers_&flag))return 0;
            return key==VK_CAPITAL?SHORT{1}:static_cast<SHORT>(0x8000);
        }
#endif
        return GetKeyState(key);
    }
    wchar_t LiteralKey(WPARAM key,LPARAM flags)const noexcept{
        if(key>255)return 0;
#ifdef TI_TRANSLATION_FIXTURE
        if(test_modifiers_){
            // Deterministic US ASCII fixture only; never changes or reads the
            // user's keyboard layout/state. Production uses its active layout.
            const bool shift=(*test_modifiers_&TiTestShift)!=0,caps=(*test_modifiers_&TiTestCaps)!=0;
            if(key>='A'&&key<='Z')return static_cast<wchar_t>(key+((shift!=caps)?0:'a'-'A'));
            if(key>='0'&&key<='9')return shift?L")!@#$%^&*("[key-'0']:static_cast<wchar_t>(key);
            if(key>=VK_NUMPAD0&&key<=VK_NUMPAD9)return static_cast<wchar_t>(L'0'+key-VK_NUMPAD0);
            switch(key){
            case VK_SPACE:return L' ';case VK_OEM_MINUS:return shift?L'_':L'-';case VK_OEM_PLUS:return shift?L'+':L'=';
            case VK_OEM_1:return shift?L':':L';';case VK_OEM_2:return shift?L'?':L'/';case VK_OEM_3:return shift?L'~':L'`';
            case VK_OEM_4:return shift?L'{':L'[';case VK_OEM_5:return shift?L'|':L'\\';case VK_OEM_6:return shift?L'}':L']';
            case VK_OEM_7:return shift?L'"':L'\'';case VK_OEM_COMMA:return shift?L'<':L',';case VK_OEM_PERIOD:return shift?L'>':L'.';
            case VK_DECIMAL:return L'.';case VK_ADD:return L'+';case VK_SUBTRACT:return L'-';case VK_MULTIPLY:return L'*';case VK_DIVIDE:return L'/';
            default:return 0;
            }
        }
#endif
        std::array<BYTE,256> keyboard{};if(!GetKeyboardState(keyboard.data()))return 0;
        const auto layout=GetKeyboardLayout(0);
        auto scan=static_cast<UINT>((static_cast<ULONG_PTR>(flags)>>16)&0xff);
        if(!scan)scan=MapVirtualKeyExW(static_cast<UINT>(key),MAPVK_VK_TO_VSC,layout);
        wchar_t text[8]{};
        // Windows 10 1607+: bit 2 prevents modifying dead-key/kernel state.
        // Multi-character/dead/non-ASCII keys remain with the host, not guessed.
        const auto count=ToUnicodeEx(static_cast<UINT>(key),scan,keyboard.data(),text,8,4,layout);
        return count==1&&text[0]>=L' '&&text[0]<=L'~'?text[0]:0;
    }

    transinput::BrokerJob Job(const std::shared_ptr<ContextState>& state,bool commit,uint64_t request){
        std::string source;source.reserve(state->input.size());
        for(const auto c:state->input){if(c>127)throw std::runtime_error("non-ASCII composition");source.push_back(static_cast<char>(c));}
        return {state->session,state->revision,request,std::move(source),static_cast<uint32_t>(state->cursor),commit,state->page};
    }
    void Publish(const std::shared_ptr<ContextState>& state){
        if(state_!=state||!state->presentation_enabled)return;
        const auto lifetime=view_;
        const auto old_session=view_->session,old_revision=view_->revision;
        const auto old_selected=view_->selected;
        const auto old_candidates=view_->candidates;
        const auto old_english=view_->english;
        const auto old_status=view_->status,old_preedit=view_->preedit;
        view_->session=state->session;view_->preedit=state->input;view_->selected=state->selected;
        view_->revision=state->revision;
        view_->candidates=state->candidate_source==state->input?state->candidates:std::vector<std::wstring>{};
        view_->status=state->unavailable?L"中文服务暂不可用，Enter 可保留拼音":
            (state->candidate_source==state->input?L"第 "+std::to_wstring(state->page+1)+L"/"+
                std::to_wstring(std::max(1u,(state->candidate_total+8)/9))+L" 页 · PgUp/PgDn 翻页 · Enter 拼音":L"正在准备候选…");
        view_->translation_mode=TranslationMode(*state);view_->english.clear();
        if(view_->translation_mode&&!state->unavailable){
            if(state->translation_status==4&&state->candidate_source==state->input&&state->selected<state->candidates.size()&&
                state->translation_source==state->candidates[state->selected])view_->english=state->translation_output;
            view_->status=state->translation_status==4?L"↑↓ 选词 · 空格 / 数字 中文 · 点击英文 / Ctrl+Enter 英文":
                state->translation_status==5?L"翻译暂不可用 · 空格选中文 · F5 重试":
                state->scope==transinput::Scope::Unknown?L"正在检查输入框 · 空格选中文":
                state->translation_status==0?L"翻译已关闭 · 空格选中文":L"正在翻译 · 空格可先选中文";
        }
        if(state->choice_request){view_->candidates.clear();view_->status=L"正在选词…";}
        const bool content_changed=old_session!=view_->session||old_revision!=view_->revision||old_selected!=view_->selected||old_candidates!=view_->candidates||old_english!=view_->english;
        if(content_changed)view_->ContentChanged();
        view_->visible=!state->input.empty();if(!view_->visible)view_->Hide();else{
            state->context->GetDocumentMgr(&view_->document);
            if(content_changed||old_status!=view_->status||old_preedit!=view_->preedit)view_->Refresh();
            if(content_changed)view_->SyncUi();
        }
    }
    HRESULT FromHostUi(transinput::UiAction action,size_t row,uint64_t generation){
        ComPtr<TextService> lifetime(this);
        if(GetCurrentThreadId()!=owner_thread_)return RPC_E_WRONG_THREAD;
        if(!view_||!state_||!view_->visible||generation!=view_->interaction.Generation())return CO_E_OBJNOTCONNECTED;
        if(action==transinput::UiAction::Abort){Cancel();return S_OK;}
        if(row==english_target){if(action==transinput::UiAction::Finalize)Choose(generation,row);return S_OK;}
        if(row>=view_->candidates.size())return E_INVALIDARG;
        if(action==transinput::UiAction::Finalize){Choose(generation,row);return S_OK;}
        auto current=state_;TF_STATUS status{};if(FAILED(current->context->GetStatus(&status))||(status.dwDynamicFlags&TF_SD_READONLY))return S_OK;
        if(current->selected==row)return S_OK;current->selected=row;
        if(TranslationMode(*current)){InvalidateTranslation(*current);current->scope_pending=true;if(current->scope!=transinput::Scope::Blocked)current->scope=transinput::Scope::Unknown;}
        Publish(current);ScheduleAnchor(current);return S_OK;
    }
    void Click(uint64_t generation,size_t row){
        if(state_&&TranslationMode(*state_)&&row!=english_target){FromHostUi(transinput::UiAction::Select,row,generation);return;}
        Choose(generation,row);
    }
    void Choose(uint64_t generation,size_t row){
        ComPtr<TextService> lifetime(this);
        if(!manager_||GetCurrentThreadId()!=owner_thread_||!state_||!view_||!view_->visible||
            generation!=view_->interaction.Generation()||(row!=english_target&&row>=view_->candidates.size()))return;
        auto current=state_;
        TF_STATUS status{};ComPtr<ITfDocumentMgr> focused,original;
        if(FAILED(current->context->GetStatus(&status))||(status.dwDynamicFlags&TF_SD_READONLY))return;
        const auto focus_result=manager_->GetFocus(&focused),original_result=current->context->GetDocumentMgr(&original);
        if(FAILED(focus_result)||FAILED(original_result)||focused.Get()!=original.Get()){
            TraceFailure("candidate focus",FAILED(focus_result)?focus_result:FAILED(original_result)?original_result:E_UNEXPECTED);return;
        }
        if(state_!=current||!view_||!current->presentation_enabled||current->choice_request||current->input.empty()||current->candidate_source!=current->input||
            current->session!=view_->session||current->revision!=view_->revision||generation!=view_->interaction.Generation())return;
        const bool english=row==english_target;
        if(english&&(!TranslationMode(*current)||current->scope!=transinput::Scope::Allowed||current->translation_status!=4||view_->english.empty()||
            current->selected>=current->candidates.size()||current->translation_source!=current->candidates[current->selected]||view_->english!=current->translation_output))return;
        if(!english&&row>=view_->candidates.size())return;
        const auto text=english?view_->english:view_->candidates[row];
        Operation operation{OperationKind::Commit,text};
        if(current->learn&&!english){operation.learning_input=current->input;operation.learning_text=text;}
        if(FAILED(Enqueue(current,std::move(operation))))return;
        InvalidateTranslation(*current);current->input.clear();current->cursor=0;current->selected=0;++current->revision;
        current->candidate_source.clear();current->candidates.clear();Publish(current);
    }
    void FailPending(){
        auto pending=std::move(pending_commits_);pending_commits_.clear();
        for(auto& [id,state]:pending){
            for(auto& operation:state->pending)if(operation.kind==OperationKind::Resolve&&operation.request==id){
                operation.kind=OperationKind::Commit;
                if(operation.conditional){state->choice_request=0;ClearInput(*state);}
            }
            state->unavailable=true;Schedule(state);Drain(state);
        }
    }
    void Reply(transinput::BrokerReply reply){
        ComPtr<TextService> lifetime(this);
        if(!manager_||GetCurrentThreadId()!=owner_thread_)return;
#ifdef TI_TRANSLATION_FIXTURE
        if(candidate_failure_)reply.result.status=candidate_failure_;
        if(hold_candidates_&&reply.job.request){if(held_candidates_.size()<64)held_candidates_.push_back(std::move(reply));return;}
#endif
        if(reply.result.status!=ERROR_SUCCESS)TraceFailure("candidate reply",HRESULT_FROM_WIN32(reply.result.status));
        if(!reply.job.request){FailPending();if(state_){state_->unavailable=true;Publish(state_);}return;}
        if(reply.job.commit){
            const auto found=pending_commits_.find(reply.job.request);if(found==pending_commits_.end())return;
            auto state=found->second;pending_commits_.erase(found);
            const auto operation=std::find_if(state->pending.begin(),state->pending.end(),[&](const Operation& value){return value.kind==OperationKind::Resolve&&value.request==reply.job.request;});
            if(operation==state->pending.end()||state->session!=reply.job.session)return;
            bool resolved{};
            if(reply.result.status==ERROR_SUCCESS&&operation->candidate<reply.result.snapshot.candidate_count){
                try{
                    const auto chinese=WideText(reply.result.snapshot.candidates[operation->candidate]);
                    operation->text=chinese+operation->suffix;
                    if(!operation->learning_input.empty())operation->learning_text=chinese;
                    resolved=true;
                }
                catch(...){state->unavailable=true;}
            }else state->unavailable=reply.result.status!=ERROR_SUCCESS;
            const bool conditional=operation->conditional;
            operation->kind=conditional&&!resolved?OperationKind::Update:OperationKind::Commit;
            if(conditional){
                state->choice_request=0;
                if(resolved)ClearInput(*state);
                else if(reply.result.status==ERROR_SUCCESS){
                    state->page=reply.result.page;state->candidate_total=reply.result.total;
                    state->candidates.clear();
                    for(uint32_t i=0;i<reply.result.snapshot.candidate_count;++i)state->candidates.push_back(WideText(reply.result.snapshot.candidates[i]));
                    state->candidate_source=state->input;
                }
            }
            Schedule(state);if(conditional){Drain(state);Publish(state);}
            return;
        }
        if(!state_||state_->choice_request||state_->session!=reply.job.session||state_->revision!=reply.job.revision)return;
        auto state=state_;state->candidates.clear();state->candidate_source.clear();
        state->unavailable=reply.result.status!=ERROR_SUCCESS;
        if(!state->unavailable){
            state->page=reply.result.page;state->candidate_total=reply.result.total;
            try{for(uint32_t i=0;i<reply.result.snapshot.candidate_count;++i)state->candidates.push_back(WideText(reply.result.snapshot.candidates[i]));state->candidate_source=state->input;}
            catch(...){state->candidates.clear();state->unavailable=true;}
        }
        if(state->candidates.empty())state->selected=0;else state->selected=std::min(state->selected,state->candidates.size()-1);
        OfferTranslation(*state);Publish(state);ScheduleAnchor(state);
    }

    void TranslationReply(transinput::TranslationReply reply){
        ComPtr<TextService> lifetime(this);
        if(!manager_||GetCurrentThreadId()!=owner_thread_||!state_||state_->input.empty()||
            state_->translation_session!=reply.update.session||state_->translation_revision!=reply.update.revision||
            (reply.update.kind!=transinput::translation::Observe&&reply.update.kind!=transinput::translation::ReadState&&reply.update.kind!=transinput::translation::Retry))return;
        auto state=state_;
        if(!TranslationMode(*state)||state->scope!=transinput::Scope::Allowed)return;
        state->translation_status=reply.result.status||(reply.result.view.enabled&&reply.result.view.status==1)?5:reply.result.view.status;
        state->translation_output=reply.result.status?std::wstring{}:Wide(reply.result.view.output);
        Publish(state);ScheduleAnchor(state);
    }

    void Cancel() {
        if (state_) {
            InvalidateTranslation(*state_);
            state_->presentation_enabled=false;++state_->revision;
            state_->cancel_after_drain=true;
            if(state_->choice_request||state_->draining||!state_->deferred.empty()){
                // Accepted selections keep their original context. After they
                // drain, discard only the final, unsubmitted composition.
                state_.reset();if(view_)view_->Hide();return;
            }
            state_->input.clear(); state_->cursor = 0; state_->selected = 0;
            if (state_->composition || !state_->pending.empty()) {
                // Replace uncommitted updates, preserving already accepted commits.
                std::erase_if(state_->pending, [](const Operation& op) { return op.kind == OperationKind::Update; });
                Enqueue(state_, {OperationKind::Cancel, {}});
            }
            state_.reset();
        }
        if (view_) view_->Hide();
    }
    static void ClearInput(ContextState& state){
        InvalidateTranslation(state);state.input.clear();state.cursor=0;state.selected=0;++state.revision;
        state.page=0;state.candidate_total=0;
        state.candidate_source.clear();state.candidates.clear();
    }
    void Drain(const std::shared_ptr<ContextState>& state){
        // Each deferred command reserves one edit-queue slot at acceptance.
        // Stop at the next unresolved selection; never wait on this thread.
        if(state->draining)return;
        struct Draining {ContextState& state;~Draining(){state.draining=false;}} guard{*state};
        state->draining=true;
        while(!state->choice_request&&!state->deferred.empty()){
            const auto event=state->deferred.front();state->deferred.pop_front();
            bool accepted{};
            const auto hr=ApplyKey(state,event,accepted);
            if(!accepted){state->deferred.push_front(event);state->unavailable=true;TraceFailure("deferred composition",hr);break;}
        }
        if(state->cancel_after_drain&&!state->choice_request&&state->deferred.empty()){
            std::erase_if(state->pending,[](const Operation& op){return op.kind==OperationKind::Update;});
            Enqueue(state,{OperationKind::Cancel,{}});ClearInput(*state);
        }
    }
    bool Eats(WPARAM key,LPARAM flags) const {
        if(key==VK_RETURN&&state_&&!state_->input.empty()&&TranslationMode(*state_)&&(KeyState(VK_CONTROL)&0x8000)&&
            !(KeyState(VK_SHIFT)&0x8000)&&!(KeyState(VK_MENU)&0x8000)&&!(KeyState(VK_LWIN)&0x8000)&&!(KeyState(VK_RWIN)&0x8000))return true;
        if (!manager_ || GetCurrentThreadId() != owner_thread_) return false;
        if ((KeyState(VK_CONTROL) & 0x8000) || (KeyState(VK_MENU) & 0x8000)
            || (KeyState(VK_LWIN) & 0x8000) || (KeyState(VK_RWIN) & 0x8000)) return false;
        const bool composing = state_ && !state_->input.empty();
        const bool queued=state_&&(!state_->pending.empty()||!state_->deferred.empty()||state_->choice_request);
        if(KeyState(VK_CAPITAL)&1){
            if(!composing&&!queued)return false;
            if(LiteralKey(key,flags))return true;
        }
        if (key >= 'A' && key <= 'Z') return true;
        if((composing||queued)&&LiteralKey(key,flags))return true;
        return composing && ((key >= '1' && key <= '9') || key == VK_OEM_7 || key == VK_BACK || key == VK_DELETE
            || key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN
            || key == VK_SPACE || key == VK_RETURN || key == VK_ESCAPE || (key==VK_F5&&TranslationMode(*state_))
            || transinput::ComposingPunctuation(key,(KeyState(VK_SHIFT)&0x8000)!=0)
            || transinput::PageDirection(key,(KeyState(VK_SHIFT)&0x8000)!=0));
    }
public:
#ifdef TI_TRANSLATION_FIXTURE
    HRESULT STDMETHODCALLTYPE CheckPresentation(uint32_t* placements,uint32_t* paints) override {
        if(!placements||!paints)return E_POINTER;*placements=0;*paints=0;
        try{
            const auto owner=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"TransInput owned offscreen rendering fixture",WS_POPUP,
                GetSystemMetrics(SM_XVIRTUALSCREEN)-8192,GetSystemMetrics(SM_YVIRTUALSCREEN)-8192,100,100,nullptr,nullptr,module_handle,nullptr);
            if(!owner)return E_FAIL;
            struct Window{HWND value;~Window(){DestroyWindow(value);}} owned{owner};
            ShowWindow(owner,SW_SHOWNOACTIVATE);if(!IsWindowVisible(owner))ShowWindow(owner,SW_SHOWNOACTIVATE);
            auto view=std::make_shared<CandidateView>();view->visible=true;view->offscreen_fixture=true;
            const std::vector<std::wstring> rows(9,L"测试");view->candidates=rows;view->preedit=L"n";view->ContentChanged();
            RECT caret{100,100,101,124};view->Position(caret,owner);UpdateWindow(view->window);
            if(!view->native_visible)return E_FAIL;
            RECT bounds{};GetWindowRect(view->window,&bounds);
            if(bounds.right-bounds.left!=view->Px(CandidateView::popup_width)||bounds.bottom-bounds.top!=view->Px(CandidateView::popup_height))return E_FAIL;
            if(bounds.right>GetSystemMetrics(SM_XVIRTUALSCREEN)&&bounds.bottom>GetSystemMetrics(SM_YVIRTUALSCREEN))return E_FAIL;
            for(unsigned i=0;i<32;++i){
                const auto painted=view->paints;
                view->preedit+=L'i';view->candidates.clear();view->english.clear();view->ContentChanged();view->Position(caret,owner);view->Refresh();UpdateWindow(view->window);
                if(!view->pending_frame||view->paints!=painted||view->painted_generation==view->interaction.Generation())return E_FAIL;
                RECT pending{};GetWindowRect(view->window,&pending);if(!EqualRect(&bounds,&pending))return E_FAIL;
                view->candidates=i%3?rows:std::vector<std::wstring>{L"测试"};view->selected=std::min<size_t>(i%9,view->candidates.size()-1);view->english=i%2?L"You are":std::wstring(1000,L'A');view->ContentChanged();view->Position(caret,owner);view->Refresh();UpdateWindow(view->window);
                if(view->pending_frame||view->paints!=painted+1)return E_FAIL;
                RECT translated{};GetWindowRect(view->window,&translated);if(!EqualRect(&bounds,&translated))return E_FAIL;
                const auto chinese_point=MAKELPARAM(view->Px(24),view->Px(16)+view->layout.Top(view->selected)-view->layout.Offset());
                if(view->HitTest(chinese_point)!=view->selected)return E_FAIL;
                const auto english_rect=view->EnglishRect(bounds.right-bounds.left);
                if(view->HitTest(MAKELPARAM(english_rect.left+1,english_rect.top+1))!=english_target)return E_FAIL;
                // A click in the compact footer or below the fifth visible row
                // must not select an invisible Chinese candidate or English.
                if(view->HitTest(MAKELPARAM(view->Px(24),view->Px(8)+view->layout.Viewport()+1)))return E_FAIL;
                if(view->layout.Viewport()!=view->Px(5*CandidateView::row_height))return E_FAIL;
                if(!view->english.empty()&&view->english_layout.MaxOffset()){
                    const auto before=view->english_layout.Offset();POINT point{english_rect.left+1,english_rect.top+1};ClientToScreen(view->window,&point);
                    SendMessageW(view->window,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),MAKELPARAM(point.x,point.y));UpdateWindow(view->window);
                    if(view->english_layout.Offset()<=before||view->english_layout.Offset()-before>view->english_layout.Viewport())return E_FAIL;
                    GetWindowRect(view->window,&translated);if(!EqualRect(&bounds,&translated))return E_FAIL;
                }
            }
            *placements=view->placements;*paints=view->paints;
            if(view->placements!=1||view->shows!=1||view->hides||view->paints<33)return E_FAIL;
            view->candidates.clear();view->ContentChanged();view->Refresh();SendMessageW(view->window,WM_TIMER,CandidateView::frame_timer,0);UpdateWindow(view->window);
            if(view->pending_frame||view->paints!=*paints+1)return E_FAIL;
            // Hiding before a delayed frame must cancel its timer as well.
            view->Refresh();if(!view->pending_frame)return E_FAIL;
            view->Hide();view->Hide();if(view->hides!=1)return E_FAIL;
            if(view->pending_frame)return E_FAIL;
            return S_OK;
        }catch(...){return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE SuppressWindow(BOOL suppress) override {
        if(!manager_||GetCurrentThreadId()!=owner_thread_||!view_)return E_UNEXPECTED;
        view_->suppress_window=suppress!=FALSE;if(view_->suppress_window)view_->Suspend();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE TranslationState(BOOL* ready,uint32_t* selected) override {
        if(!ready||!selected)return E_POINTER;if(GetCurrentThreadId()!=owner_thread_||!view_)return E_UNEXPECTED;
        *ready=!view_->english.empty();*selected=static_cast<uint32_t>(view_->selected);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Key(ITfContext* context,uint32_t key,uint32_t modifiers,BOOL* eaten,double* elapsed) override {
        if(!eaten)return E_POINTER;*eaten=FALSE;if(elapsed)*elapsed=0;
        if(!context||key>255||(modifiers&~31u))return E_INVALIDARG;
        if(!manager_||GetCurrentThreadId()!=owner_thread_||test_modifiers_)return E_UNEXPECTED;
        struct FixedModifiers{TextService& owner;~FixedModifiers(){owner.test_modifiers_.reset();}}fixed{*this};
        test_modifiers_=modifiers;BOOL allowed{};
        auto hr=OnTestKeyDown(context,key,0,&allowed);if(FAILED(hr)||!allowed)return hr;
        const auto started=std::chrono::steady_clock::now();hr=OnKeyDown(context,key,0,eaten);
        if(elapsed)*elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-started).count();
        if(FAILED(hr)||!*eaten)return hr;
        BOOL up{};hr=OnTestKeyUp(context,key,0,&up);if(FAILED(hr)||!up)return FAILED(hr)?hr:E_FAIL;
        hr=OnKeyUp(context,key,0,&up);return FAILED(hr)?hr:up?S_OK:E_FAIL;
    }
    HRESULT STDMETHODCALLTYPE FreshSession(uint64_t* value) override {
        if(!value)return E_POINTER;
        try{*value=transinput::NewSessionId();return S_OK;}catch(...){return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE HoldCandidates(BOOL hold) override {
        if(GetCurrentThreadId()!=owner_thread_||!manager_)return E_UNEXPECTED;
        hold_candidates_=hold!=FALSE;
        if(!hold_candidates_){auto replies=std::move(held_candidates_);held_candidates_.clear();for(auto& reply:replies)Reply(std::move(reply));}
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CandidateFailure(DWORD status) override {
        if(GetCurrentThreadId()!=owner_thread_||!manager_)return E_UNEXPECTED;
        if(status!=ERROR_SUCCESS&&status!=ERROR_TIMEOUT&&status!=ERROR_INVALID_DATA)return E_INVALIDARG;
        candidate_failure_=status;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE PageState(uint32_t* page,uint32_t* total,BOOL* ready) override {
        if(!page||!total||!ready)return E_POINTER;
        if(GetCurrentThreadId()!=owner_thread_||!manager_)return E_UNEXPECTED;
        *page=state_?state_->page:0;*total=state_?state_->candidate_total:0;
        *ready=state_&&!state_->input.empty()&&!state_->choice_request&&state_->candidate_source==state_->input;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RenderPreview(const wchar_t* path,uint32_t dpi,BOOL bottom,uint32_t* content,uint32_t* maximum) override {
        if(!path||!content||!maximum||!(dpi==96||dpi==144||dpi==192))return E_INVALIDARG;
        try{
            CandidateView view;view.font_dpi=dpi;
            view.font=CreateFontW(-view.Px(16),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
            view.footer_font=CreateFontW(-view.Px(13),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
            if(!view.font)return E_FAIL;
            view.preedit=bottom?L"changyiwen":L"ns";view.status=L"↑↓ 选词 · 空格 / 数字 中文 · 点击英文 / Ctrl+Enter 英文";
            const std::wstring source=L"今天下午我们先核对这份虚构方案，确认后再安排明天的测试。";
            std::wstring english;
            for(unsigned i=0;i<8;++i)english+=L"Let's review this fictional proposal this afternoon and schedule tomorrow's test after everything is confirmed. ";
            english+=L"\r\nD:\\fictional\\"+std::wstring(180,L'x')+L"\\example.txt\r\nAll values are fictional. 😀";
            view.candidates={L"你是",L"鸟叔",L"那是",L"你说",source,L"女士",L"难受",L"年少",L"能说"};view.english=bottom?english:L"You are";view.selected=bottom?4:0;view.translation_mode=true;
            const int width=view.Px(CandidateView::popup_width),height=view.Px(CandidateView::popup_height);RECT bounds{0,0,width,height};
            transinput::testing::PreviewBuffer buffer(width,height);if(!buffer.dc||!buffer.bitmap)return E_FAIL;
            view.Draw(buffer.dc,bounds);if(bottom)view.english_layout.ScrollTo(view.english_layout.MaxOffset());view.Draw(buffer.dc,bounds);
            *content=static_cast<uint32_t>(view.english_layout.Content());*maximum=static_cast<uint32_t>(view.english_layout.MaxOffset());
            return buffer.Save(path)?S_OK:E_FAIL;
        }catch(...){return E_FAIL;}
    }
    HRESULT STDMETHODCALLTYPE SetLearning(BOOL enabled) override {
        if(GetCurrentThreadId()!=owner_thread_||!manager_||!allow_learning_)return E_UNEXPECTED;
        learning_enabled_=enabled!=FALSE;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Read(uint64_t* generation,uint32_t* rows) override {
        if(!generation||!rows)return E_POINTER;
        if(GetCurrentThreadId()!=owner_thread_||!view_)return E_UNEXPECTED;
        *generation=view_->interaction.Generation();*rows=static_cast<uint32_t>(view_->candidates.size());return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Press(uint32_t row) override {
        if(GetCurrentThreadId()!=owner_thread_||!view_)return E_UNEXPECTED;
        view_->interaction.Press(row<view_->candidates.size()||(row==english_target&&!view_->english.empty())?std::optional<size_t>{row}:std::nullopt);return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ReleasePress(uint32_t row) override {
        if(GetCurrentThreadId()!=owner_thread_||!view_)return E_UNEXPECTED;
        try{
            const auto selected=view_->interaction.Release(row<view_->candidates.size()||(row==english_target&&!view_->english.empty())?std::optional<size_t>{row}:std::nullopt);
            if(selected)Click(view_->interaction.Generation(),*selected);return S_OK;
        }catch(...){return E_FAIL;}
    }
#endif
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_ITfTextInputProcessor || iid == IID_ITfTextInputProcessorEx)
            *result = static_cast<ITfTextInputProcessorEx*>(this);
        else if (iid == IID_ITfKeyEventSink) *result = static_cast<ITfKeyEventSink*>(this);
        else if (iid == IID_ITfThreadMgrEventSink) *result = static_cast<ITfThreadMgrEventSink*>(this);
#ifdef TI_TRANSLATION_FIXTURE
        else if(iid==__uuidof(ITestCandidateView))*result=static_cast<ITestCandidateView*>(this);
#endif
        else return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE Activate(ITfThreadMgr* manager, TfClientId client) override { return ActivateEx(manager, client, 0); }
    HRESULT STDMETHODCALLTYPE ActivateEx(ITfThreadMgr* manager, TfClientId client, DWORD flags) override {
        if (!manager) return E_INVALIDARG;
        if (manager_) return E_UNEXPECTED;
        try {
            owner_thread_ = GetCurrentThreadId(); client_ = client; manager_ = manager;stopping_=false;
            view_ = std::make_shared<CandidateView>();
            choose_target_=std::make_shared<ChooseTarget>();choose_target_->owner=this;
            const std::weak_ptr<ChooseTarget> weak_choose=choose_target_;
            view_->choose=[weak_choose](uint64_t generation,size_t row){if(auto target=weak_choose.lock();target&&target->owner)target->owner->Click(generation,row);};
            view_->ui.Manager(manager);
            view_->ui_action=[weak_choose](transinput::UiAction action,size_t row,uint64_t generation)->HRESULT{if(auto target=weak_choose.lock();target&&target->owner)return target->owner->FromHostUi(action,row,generation);return CO_E_OBJNOTCONNECTED;};
            HRESULT hr = manager_.As(&keys_);
            TraceFailure("query key manager", hr);
            if (SUCCEEDED(hr)) { hr = keys_->AdviseKeyEventSink(client_, this, TRUE); TraceFailure("advise key events", hr); }
            if (SUCCEEDED(hr)) { hr = manager_.As(&source_); TraceFailure("query thread source", hr); }
            if (SUCCEEDED(hr)) { hr = source_->AdviseSink(IID_ITfThreadMgrEventSink, static_cast<ITfThreadMgrEventSink*>(this), &cookie_); TraceFailure("advise thread events", hr); }
            if (FAILED(hr)) { Deactivate(); return hr; }
            allow_learning_=(flags&TF_TMAE_SECUREMODE)==0;learning_enabled_=allow_learning_;
            transinput::AsyncBroker::Target broker_target;
#ifdef TI_TRANSLATION_FIXTURE
            // The integration DLL never connects to a real user's learning store.
            const auto candidate_id=std::to_wstring(GetCurrentProcessId())+L"c";
            broker_target.endpoint=transinput::pipe::Endpoint(candidate_id);
            broker_target.executable=transinput::AsyncBroker::Path(module_handle);
            broker_target.arguments=L"--parent="+std::to_wstring(GetCurrentProcessId())+L" --test-id="+candidate_id;
            learning_enabled_=false;
#endif
            broker_=std::make_unique<transinput::AsyncBroker>(module_handle,&live_objects,&broker_channels,[this](transinput::BrokerReply reply){Reply(std::move(reply));},std::move(broker_target));
            transinput::AsyncTranslation::Target translation_target;
#ifdef TI_TRANSLATION_FIXTURE
            // A separately named test DLL always targets a no-network fixture.
            // Normal builds have no environment/registry/exported override.
            translation_target.endpoint=transinput::translation::Endpoint(std::to_wstring(GetCurrentProcessId())+L"f");
            translation_target.executable=transinput::pipe::ParentPath(transinput::pipe::ModulePath(module_handle))+L"\\TransInputTranslationFixture.exe";
            translation_target.arguments=L"--parent="+std::to_wstring(GetCurrentProcessId());
#endif
            if(!(flags&TF_TMAE_SECUREMODE))translator_=std::make_unique<transinput::AsyncTranslation>(module_handle,&live_objects,&translation_channels,[this](transinput::TranslationReply reply){TranslationReply(std::move(reply));},std::move(translation_target));
            return S_OK;
        } catch (...) { Deactivate(); return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE Deactivate() override {
        if (manager_ && owner_thread_ != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
        stopping_=true;
        if(choose_target_)choose_target_->owner=nullptr;choose_target_.reset();
        broker_.reset();
#ifdef TI_TRANSLATION_FIXTURE
        held_candidates_.clear();hold_candidates_=false;candidate_failure_=0;
#endif
        try{FailPending();}catch(...){pending_commits_.clear();}
        try { Cancel(); } catch (...) { state_.reset(); }
        translator_.reset();
        if (auto view=std::move(view_)) view->Close();
        if (source_ && cookie_ != TF_INVALID_COOKIE) source_->UnadviseSink(std::exchange(cookie_, TF_INVALID_COOKIE));
        if (keys_ && client_ != TF_CLIENTID_NULL) keys_->UnadviseKeyEventSink(client_);
        source_.Reset(); keys_.Reset(); manager_.Reset(); client_ = TF_CLIENTID_NULL;
        key_down_.fill(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(BOOL foreground) override {
        try { if (!foreground) Cancel(); return S_OK; } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyDown(ITfContext* context, WPARAM key, LPARAM flags, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = FALSE;
        if(manager_&&GetCurrentThreadId()!=owner_thread_)return RPC_E_WRONG_THREAD;
        TF_STATUS status{};
        if (!context || FAILED(context->GetStatus(&status)) || (status.dwDynamicFlags & TF_SD_READONLY)) return S_OK;
        *eaten = Eats(key,flags); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTestKeyUp(ITfContext*, WPARAM key, LPARAM, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = key < key_down_.size() && key_down_[key]; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnKeyUp(ITfContext* context, WPARAM key, LPARAM flags, BOOL* eaten) override {
        const HRESULT hr = OnTestKeyUp(context, key, flags, eaten);
        if (key < key_down_.size()) key_down_[key] = false;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE OnPreservedKey(ITfContext*, REFGUID, BOOL* eaten) override {
        if (!eaten) return E_POINTER;
        *eaten = FALSE; return S_OK;
    }
    HRESULT ApplyKey(const std::shared_ptr<ContextState>& current,const LogicalKey& event,bool& accepted) {
        const auto key=event.key;
        try {
            auto& state = *current;
            if(event.english){accepted=true;if(current==state_&&view_)Choose(view_->interaction.Generation(),english_target);return S_OK;}
            if(state.input.empty()){
                if(event.direct||key<'A'||key>'Z'){
                    // A command accepted while the IME owned a composition
                    // stays an IME command. Never synthesize delayed Return,
                    // navigation or Backspace into an arbitrary host app.
                    std::wstring literal;
                    if(event.direct&&event.literal)literal.assign(1,event.literal);
                    else if(event.punctuation)literal.assign(1,event.punctuation);
                    else if(event.literal)literal.assign(1,event.literal);
                    else if(key==VK_SPACE)literal=L" ";
                    else if(key>='1'&&key<='9')literal.assign(1,static_cast<wchar_t>(key));
                    else if(key==VK_OEM_7)literal=L"'";
                    const auto hr=literal.empty()?S_OK:Enqueue(current,{OperationKind::Commit,std::move(literal)});
                    accepted=SUCCEEDED(hr);return hr;
                }
                state.learn=learning_enabled_&&broker_?broker_->LearningHook():std::function<bool(transinput::SelectionLearn)>{};
                InvalidateTranslation(state);state.translation_session=transinput::NewSessionId();state.translation_revision=0;
                state.translation_mode=event.translation_enabled;state.scope=transinput::Scope::Unknown;
            }
            struct Rollback {
                ContextState& state;std::wstring input;size_t cursor,selected;uint64_t revision;uint32_t page;bool keep{};
                ~Rollback(){if(!keep){state.input=std::move(input);state.cursor=cursor;state.selected=selected;state.revision=revision;state.page=page;}}
            } rollback{state,state.input,state.cursor,state.selected,state.revision,state.page};
            auto kind = OperationKind::Update;
            std::wstring output;
            std::wstring suffix;
            std::wstring chosen_chinese;
            bool chinese_choice{};
            size_t candidate{};
            const auto punctuation=event.punctuation;
            const bool literal_boundary=event.literal&&(event.direct||
                (!punctuation&&!event.page_direction&&!(key>='A'&&key<='Z')&&!(key>='1'&&key<='9'&&event.literal==static_cast<wchar_t>(key))&&
                    key!=VK_SPACE&&!(key==VK_OEM_7&&event.literal==L'\'')));
            if(literal_boundary){
                chinese_choice=true;suffix.assign(1,event.literal);
                if(state.candidate_source==state.input&&state.selected<state.candidates.size()){
                    kind=OperationKind::Commit;chosen_chinese=state.candidates[state.selected];output=chosen_chinese+suffix;
                }else{
                    kind=OperationKind::Resolve;output=state.input+suffix;candidate=state.selected;
                }
            }else if(event.page_direction){
                const auto last=state.candidate_source==state.input?state.candidate_total?((state.candidate_total-1)/9):0:9;
                state.page=event.page_direction<0?(state.page?state.page-1:0):std::min(state.page+1,last);state.selected=0;
            } else if (key >= 'A' && key <= 'Z') {
                if (state.input.size() == max_input) {
                    accepted=true;
                    return S_OK;
                }
                state.input.insert(state.cursor++, 1, static_cast<wchar_t>(key - 'A' + 'a')); state.selected = 0;
            } else if(key==VK_OEM_7){
                if(state.input.size()<max_input){state.input.insert(state.cursor++,1,L'\'');state.selected=0;}
            } else if (key == VK_BACK) {
                if (state.cursor) state.input.erase(--state.cursor, 1);
                state.selected = 0;
            } else if (key == VK_DELETE) {
                if (state.cursor < state.input.size()) state.input.erase(state.cursor, 1);
                state.selected = 0;
            } else if (key == VK_LEFT) { if (state.cursor) --state.cursor; }
            else if (key == VK_RIGHT) state.cursor = std::min(state.cursor + 1, state.input.size());
            else if (key == VK_UP) { if (state.selected) --state.selected; }
            else if (key == VK_DOWN) { if (state.candidate_source==state.input&&!state.candidates.empty()) state.selected = std::min(state.selected + 1, state.candidates.size() - 1); }
            else if (key == VK_ESCAPE) kind = OperationKind::Cancel;
            else if (key==VK_F5){if(state.translate&&state.translation_status==5&&!state.translation_source.empty()){
                // An explicit user retry also recovers after the agent lost its
                // context. A newer revision cannot replay an old observation.
                ++state.translation_revision;
                state.translate({transinput::translation::Observe,state.translation_session,state.translation_revision,Utf8(state.translation_source)});state.translation_status=2;
            }}
            else if(punctuation){
                chinese_choice=true;
                if(state.candidate_source==state.input&&state.selected<state.candidates.size()){
                    kind=OperationKind::Commit;chosen_chinese=state.candidates[state.selected];output=chosen_chinese+punctuation;
                }else{
                    kind=OperationKind::Resolve;suffix=punctuation;output=state.input+suffix;candidate=state.selected;
                }
            }
            else if (key == VK_RETURN) { kind = OperationKind::Commit; output = state.input; }
            else if (key == VK_SPACE || (key >= '1' && key <= '9')) {
                const auto index = key == VK_SPACE ? state.selected : static_cast<size_t>(key - '1');
                if(state.candidate_source==state.input&&index<state.candidates.size()){
                    kind=OperationKind::Commit;output=state.candidates[index];chosen_chinese=output;chinese_choice=true;
                }else if((key==VK_SPACE||key=='1')||state.candidate_source!=state.input){
                    kind=OperationKind::Resolve;output=state.input;candidate=index;chinese_choice=true;
                }
            }
            const bool text_changed=state.input!=rollback.input;
            if(text_changed){state.page=0;++state.revision;}
            const bool page_changed=state.page!=rollback.page;
            const bool selected_changed=state.selected!=rollback.selected;
            if(page_changed&&!text_changed)++state.revision;
            if (kind == OperationKind::Update && state.input.empty()) kind = OperationKind::Cancel;
            if (kind == OperationKind::Update) output = state.input;
            const auto request=kind==OperationKind::Resolve?next_request_++:0;
            std::optional<transinput::BrokerJob> work;
            if(request){work=Job(current,true,request);pending_commits_.emplace(request,current);}
            HRESULT hr{};
            Operation prepared_operation{kind,output,state.cursor,request,candidate,std::move(suffix)};
            bool conditional=kind==OperationKind::Resolve&&!literal_boundary&&!punctuation&&key>='2'&&key<='9';
            prepared_operation.conditional=conditional;
            if(chinese_choice&&state.learn){prepared_operation.learning_input=state.input;prepared_operation.learning_text=std::move(chosen_chinese);}
            const bool edit_needed=kind!=OperationKind::Update||text_changed||state.cursor!=rollback.cursor;
            try{hr=edit_needed?Enqueue(current,std::move(prepared_operation)):S_OK;}
            catch(...){if(request)pending_commits_.erase(request);throw;}
            if(FAILED(hr)){if(request)pending_commits_.erase(request);return hr;}
            accepted=true;rollback.keep=true;
            if(conditional)state.choice_request=request;
            if(request&&(!broker_||!broker_->Submit(std::move(*work)))){
                pending_commits_.erase(request);state.unavailable=true;
                // While closing, all accepted selections preserve their source,
                // including those behind the first unresolved choice.
                if(conditional&&stopping_)conditional=false;
                for(auto& operation:state.pending)if(operation.request==request)operation.kind=conditional?OperationKind::Update:OperationKind::Commit;
                state.choice_request=0;
                Schedule(current); // preserve original pinyin on service failure
            }
            if(!conditional&&(kind != OperationKind::Update || state.input.empty())) {
                ClearInput(state);
            } else {
                if(text_changed||page_changed){
                    if(text_changed)state.candidate_total=0;
                    InvalidateTranslation(state);
                    state.candidate_source.clear();state.candidates.clear();
                    if(broker_)broker_->Submit(Job(current,false,next_request_++));
                }
                if(selected_changed&&TranslationMode(state)){InvalidateTranslation(state);state.scope_pending=true;if(state.scope!=transinput::Scope::Blocked)state.scope=transinput::Scope::Unknown;}
                OfferTranslation(state);
            }
            Publish(current);
            if(!edit_needed)ScheduleAnchor(current);
            return S_OK;
        } catch (...) { return accepted?S_OK:E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE OnKeyDown(ITfContext* context, WPARAM key, LPARAM flags, BOOL* eaten) override {
        if(!eaten)return E_POINTER;*eaten=FALSE;
        BOOL allowed{};const auto test_result=OnTestKeyDown(context,key,flags,&allowed);
        if(FAILED(test_result)||!allowed)return test_result;
        bool accepted{};
        try{
            if(state_&&state_->context.Get()!=context)Cancel();
            if(!state_){
                state_=std::make_shared<ContextState>();state_->context=context;state_->client=client_;state_->view=view_;
                state_->sink.Attach(new CompositionSink(state_));if(translator_)state_->translate=translator_->SubmitHook();
                TraceFailure("advise candidate layout",AttachLayout(state_));
            }
            auto current=state_;
            const bool shift=(KeyState(VK_SHIFT)&0x8000)!=0;
            const bool direct=(KeyState(VK_CAPITAL)&1)!=0;
            const bool composing=!current->input.empty();
            LogicalKey event{key,composing&&!direct?transinput::ComposingPunctuation(key,shift):wchar_t{},translator_&&translator_->Enabled(),
                composing&&!direct?transinput::PageDirection(key,shift):0,
                direct||key<'A'||key>'Z'?LiteralKey(key,flags):wchar_t{},direct,key==VK_RETURN&&(KeyState(VK_CONTROL)&0x8000)!=0};
            if(!current->choice_request&&!current->deferred.empty())Drain(current);
            HRESULT hr{};
            if(current->choice_request||!current->deferred.empty()){
                if(current->pending.size()+current->deferred.size()>=max_pending-2)return HRESULT_FROM_WIN32(ERROR_BUSY);
                current->deferred.push_back(event);accepted=true;
            }else hr=ApplyKey(current,event,accepted);
            *eaten=accepted;if(accepted&&key<key_down_.size())key_down_[key]=true;return hr;
        }catch(...){*eaten=accepted;return accepted?S_OK:E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override {
        try { if (state_ && state_->context.Get() == context) Cancel(); return S_OK; } catch (...) { return E_FAIL; }
    }
    HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* focused, ITfDocumentMgr*) override {
        try {
            if (state_) {
                ComPtr<ITfDocumentMgr> previous;
                if (FAILED(state_->context->GetDocumentMgr(&previous)) || previous.Get() != focused) Cancel();
            }
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
};

class ClassFactory final : public IClassFactory {
    ModuleLease lease_;
    std::atomic<ULONG> references_{1};
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (iid != IID_IUnknown && iid != IID_IClassFactory) return E_NOINTERFACE;
        *result = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try {
            auto service = new TextService;
            const HRESULT hr = service->QueryInterface(iid, result);
            service->Release(); return hr;
        } catch (...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) ++server_locks;
        else {
            auto previous = server_locks.load();
            while (previous > 0 && !server_locks.compare_exchange_weak(previous, previous - 1)) {}
            if (previous == 0) return E_UNEXPECTED;
        }
        return S_OK;
    }
};
}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) module_handle = module;
    return TRUE;
}
STDAPI DllCanUnloadNow() {
    if (live_objects.load() != 0 || server_locks.load() != 0) return S_FALSE;
    if (!UnregisterClassW(candidate_class, module_handle) && GetLastError() != ERROR_CLASS_DOES_NOT_EXIST) return S_FALSE;
    if(!transinput::AsyncBroker::Unregister(module_handle))return S_FALSE;
    if(!transinput::AsyncTranslation::Unregister(module_handle))return S_FALSE;
    return S_OK;
}
STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (clsid != transinput::ClassId) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = new(std::nothrow) ClassFactory;
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(iid, result);
    factory->Release(); return hr;
}
