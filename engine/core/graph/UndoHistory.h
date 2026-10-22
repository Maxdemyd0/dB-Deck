#pragma once
// Undo/redo of profile snapshots, with coalescing of continuous edits: a
// whole fader drag (many small changes to the same control) is one step.
// Control thread only.

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace dbdeck {

class UndoHistory {
public:
    using Clock = std::chrono::steady_clock;
    using json = nlohmann::json;

    explicit UndoHistory(size_t maxDepth = 50, std::chrono::milliseconds window = std::chrono::milliseconds(1500))
        : max_(maxDepth), window_(window) {}

    // Records the state before a change. A change with the same non-empty key
    // within `window` of the previous one merges into that step, so `before`
    // is only evaluated when a new step starts. Any change clears redo.
    void record(const std::function<json()>& before, const std::string& key, Clock::time_point now) {
        redo_.clear();
        const bool merge = !key.empty() && key == lastKey_ && !undo_.empty() && now - lastAt_ <= window_;
        lastKey_ = key;
        lastAt_ = now;
        if (merge) return;
        undo_.push_back(before());
        if (undo_.size() > max_) undo_.pop_front();
    }

    // Returns the snapshot to restore; `current` becomes the redo step.
    std::optional<json> undo(const json& current) { return step(undo_, redo_, current); }
    // Returns the snapshot to restore; `current` becomes the undo step.
    std::optional<json> redo(const json& current) { return step(redo_, undo_, current); }

    void clear() {
        undo_.clear();
        redo_.clear();
        lastKey_.clear();
    }
    size_t undoDepth() const { return undo_.size(); }
    size_t redoDepth() const { return redo_.size(); }

private:
    std::optional<json> step(std::deque<json>& from, std::deque<json>& to, const json& current) {
        if (from.empty()) return std::nullopt;
        json snap = std::move(from.back());
        from.pop_back();
        to.push_back(current);
        if (to.size() > max_) to.pop_front();
        lastKey_.clear();  // the next edit starts a new step
        return snap;
    }

    std::deque<json> undo_, redo_;
    std::string lastKey_;
    Clock::time_point lastAt_{};
    size_t max_;
    std::chrono::milliseconds window_;
};

}  // namespace dbdeck
