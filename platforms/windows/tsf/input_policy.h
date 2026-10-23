// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>

namespace transinput {
constexpr int PageDirection(WPARAM key,bool shift) noexcept {
    if(key==VK_PRIOR)return -1;
    if(key==VK_NEXT)return 1;
    if(!shift&&key==VK_OEM_MINUS)return -1;
    if(!shift&&key==VK_OEM_PLUS)return 1;
    return 0;
}
// Only used for Chinese composition. Idle punctuation normally passes through;
// accepted characters behind an unfinished commit use the same ordered queue.
constexpr wchar_t ComposingPunctuation(WPARAM key, bool shift) noexcept {
    switch (key) {
    case VK_OEM_COMMA: return shift ? L'《' : L'，';
    case VK_OEM_PERIOD: return shift ? L'》' : L'。';
    case VK_OEM_1: return shift ? L'：' : L'；';
    case VK_OEM_2: return shift ? L'？' : L'、';
    case '1': return shift ? L'！' : 0;
    case '9': return shift ? L'（' : 0;
    case '0': return shift ? L'）' : 0;
    default: return 0;
    }
}
}
