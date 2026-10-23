// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <cwchar>
#include <exception>
#include <memory>
#include <utility>

namespace transinput {
// One message-frame buffer for this STA only. The timer never writes text.
// The service owns the window; shutdown restores targets before teardown.
class CommitPresentation final:public std::enable_shared_from_this<CommitPresentation> {
    HMODULE module_;std::atomic<long>* leases_;DWORD thread_{GetCurrentThreadId()};
    HWND window_{};wchar_t name_[96]{};ATOM atom_{};std::array<HWND,8> targets_{};
    unsigned depth_{};
    static bool Local(HWND window)noexcept{DWORD process{};return window&&GetWindowThreadProcessId(window,&process)==GetCurrentThreadId()&&process==GetCurrentProcessId();}
    static LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM key,LPARAM data){
        auto self=reinterpret_cast<CommitPresentation*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<CommitPresentation*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(message==WM_TIMER&&key==1&&self){auto lifetime=self->weak_from_this().lock();if(lifetime&&!lifetime->depth_)lifetime->Restore();return 0;}
        if(message==WM_NCDESTROY)SetWindowLongPtrW(window,GWLP_USERDATA,0);
        return DefWindowProcW(window,message,key,data);
    }
public:
    class Call final {
        CommitPresentation* owner_;int exceptions_{std::uncaught_exceptions()};
    public:
        explicit Call(CommitPresentation* owner):owner_(owner){if(owner_)++owner_->depth_;}
        ~Call(){if(owner_){--owner_->depth_;if(std::uncaught_exceptions()>exceptions_)owner_->Restore();}}
        Call(const Call&)=delete;Call& operator=(const Call&)=delete;
    };
    CommitPresentation(HMODULE module,std::atomic<long>* leases=nullptr):module_(module),leases_(leases){
        if(leases_)++*leases_;
        swprintf_s(name_,L"TransInput.CommitFrame.%p",static_cast<void*>(this));
        WNDCLASSW cls{};cls.hInstance=module_;cls.lpszClassName=name_;cls.lpfnWndProc=Procedure;atom_=RegisterClassW(&cls);
        if(atom_)window_=CreateWindowExW(0,name_,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,module_,this);
    }
    ~CommitPresentation(){Restore();if(window_)DestroyWindow(window_);if(atom_)UnregisterClassW(name_,module_);if(leases_)--*leases_;}
    static bool Eligible(HWND target)noexcept{
        wchar_t name[64]{};return Local(target)&&GetClassNameW(target,name,64)&&!wcscmp(name,L"Scintilla");
    }
    bool Active(HWND target)const noexcept{for(const auto item:targets_)if(item&&item==target)return true;return false;}
    bool Empty()const noexcept{for(const auto item:targets_)if(item)return false;return true;}
    bool Begin(HWND target)noexcept{
        if(thread_!=GetCurrentThreadId()||!window_||Active(target)||!Eligible(target)||!IsWindowVisible(target)||GetPropW(target,L"SysSetRedraw"))return false;
        for(auto& item:targets_)if(!item){
            const bool first=Empty();item=target;SendMessageW(target,WM_SETREDRAW,FALSE,0);
            if(first&&!SetTimer(window_,1,16,nullptr)){Restore();return false;}
            return true;
        }
        return false;
    }
    void Restore(HWND only=nullptr)noexcept{
        if(thread_!=GetCurrentThreadId())return;
        // Clear ownership before host callbacks can reenter the service.
        for(auto& item:targets_)if(item&&(!only||item==only)){
            const auto target=std::exchange(item,nullptr);
            if(Local(target)&&GetPropW(target,L"SysSetRedraw")){
                SendMessageW(target,WM_SETREDRAW,TRUE,0);
                RedrawWindow(target,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN);
            }
        }
        if(window_&&Empty())KillTimer(window_,1);
    }
    CommitPresentation(const CommitPresentation&)=delete;
    CommitPresentation& operator=(const CommitPresentation&)=delete;
};
}
