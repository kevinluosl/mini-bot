#pragma once

// 输入为已去抖电平。各路上升沿都在当拍消费，被挡住的长按不会迟到接管。
class DeepbotSensorFocus {
public:
    enum class Input : unsigned { kNone = 0, kRight = 1, kLeft = 2, kHead = 4 };
    struct Update {
        Input active;
        bool started;
    };

    void Reset(bool head, bool left, bool right) {
        held_ = Mask(head, left, right);  // 上电时已激活的输入不算新触发。
        active_ = Input::kNone;
    }

    Update Tick(bool head, bool left, bool right) {
        const auto current = Mask(head, left, right);
        const auto pressed = current & ~held_;
        held_ = current;
        if ((current & Bit(active_)) != 0) return {active_, false};

        active_ = Input::kNone;
        // 同一采样拍无法区分先后：红外优先；两侧同拍沿用右侧优先。
        for (const auto candidate : kTieOrder) {
            if ((pressed & Bit(candidate)) != 0) {
                active_ = candidate;
                break;
            }
        }
        return {active_, active_ != Input::kNone};
    }

private:
    static constexpr Input kTieOrder[] = {Input::kRight, Input::kLeft, Input::kHead};
    static constexpr unsigned Bit(Input input) { return static_cast<unsigned>(input); }
    static constexpr unsigned Mask(bool head, bool left, bool right) {
        return (head ? Bit(Input::kHead) : 0) | (left ? Bit(Input::kLeft) : 0) |
               (right ? Bit(Input::kRight) : 0);
    }

    unsigned held_ = 0;
    Input active_ = Input::kNone;
};
