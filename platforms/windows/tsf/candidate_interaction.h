// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace transinput {
// UI-thread-owned click state. Actionable content changes invalidate a press;
// progress-only repaints do not. No platform focus or keyboard events are sent.
class CandidateInteraction {
    uint64_t generation_{1};
    std::optional<size_t> pressed_;
public:
    uint64_t Generation() const noexcept { return generation_; }
    void Changed() noexcept { ++generation_; pressed_.reset(); }
    void Cancel() noexcept { pressed_.reset(); }
    void Press(std::optional<size_t> row) noexcept { pressed_=row; }
    std::optional<size_t> Release(std::optional<size_t> row) noexcept {
        const auto pressed=pressed_;pressed_.reset();
        return pressed&&pressed==row?row:std::nullopt;
    }
};
}
