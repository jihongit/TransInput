// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <unknwn.h>
#include <cstdint>
struct ITfContext;
enum TiTestModifiers : uint32_t {TiTestShift=1,TiTestControl=2,TiTestAlt=4,TiTestCaps=8,TiTestWin=16};
// Implemented only by TransInputPrototypeTest.dll. A local SDK test can exercise
// its own candidate actions without opening a desktop popup or injecting input.
struct __declspec(uuid("e3cbf18e-0980-4d71-838b-8afdd5188817")) ITestCandidateView : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Read(uint64_t* generation,uint32_t* rows)=0;
    virtual HRESULT STDMETHODCALLTYPE Press(uint32_t row)=0;
    virtual HRESULT STDMETHODCALLTYPE ReleasePress(uint32_t row)=0;
    virtual HRESULT STDMETHODCALLTYPE SetLearning(BOOL enabled)=0;
    virtual HRESULT STDMETHODCALLTYPE FreshSession(uint64_t* value)=0;
    virtual HRESULT STDMETHODCALLTYPE HoldCandidates(BOOL hold)=0;
    virtual HRESULT STDMETHODCALLTYPE CandidateFailure(DWORD status)=0;
    virtual HRESULT STDMETHODCALLTYPE PageState(uint32_t* page,uint32_t* total,BOOL* ready)=0;
    virtual HRESULT STDMETHODCALLTYPE RenderPreview(const wchar_t* path,uint32_t dpi,BOOL bottom,uint32_t* content,uint32_t* maximum)=0;
    // Fixed SDK callbacks only. Does not set/inject OS keyboard state.
    virtual HRESULT STDMETHODCALLTYPE Key(ITfContext* context,uint32_t key,uint32_t modifiers,BOOL* eaten,double* down_microseconds)=0;
    virtual HRESULT STDMETHODCALLTYPE SuppressWindow(BOOL suppress)=0;
};
