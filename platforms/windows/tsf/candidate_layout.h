// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <algorithm>
#include <optional>
#include <vector>
#include <utility>

namespace transinput {
// Pixel geometry shared by drawing, wheel/scrollbar handling and hit testing.
// Rows are bounded by the core/translation text limits; no document data lives here.
class CandidateLayout {
    std::vector<int> heights_;
    int viewport_{},content_{},offset_{};
public:
    void Reset(){offset_=0;}
    void Set(std::vector<int> heights,int viewport){
        heights_=std::move(heights);viewport_=std::max(0,viewport);content_=0;
        for(auto& height:heights_){height=std::clamp(height,1,200000);content_+=height;}
        offset_=std::clamp(offset_,0,MaxOffset());
    }
    int Content()const noexcept{return content_;}
    int Viewport()const noexcept{return viewport_;}
    int Offset()const noexcept{return offset_;}
    int MaxOffset()const noexcept{return std::max(0,content_-viewport_);}
    bool ScrollTo(int offset)noexcept{const auto next=std::clamp(offset,0,MaxOffset());const bool changed=offset_!=next;offset_=next;return changed;}
    int Top(size_t row)const noexcept{int top{};for(size_t i=0;i<std::min(row,heights_.size());++i)top+=heights_[i];return top;}
    int Height(size_t row)const noexcept{return row<heights_.size()?heights_[row]:0;}
    std::optional<size_t> Hit(int y)const noexcept{
        if(y<0||y>=viewport_)return {};y+=offset_;
        int top{};for(size_t row=0;row<heights_.size();++row){if(y<top+heights_[row])return row;top+=heights_[row];}return {};
    }
};
}
