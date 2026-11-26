#include "deepbot_head_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace deepbot_head {
namespace {

// 三档速度（度/秒）。原平滑逼近是每 20ms 2°，即 100°/s，作为中档。
// 点头、摇头要用快档才像真的在点/摇；害羞、低落、思考用慢档才有情绪。
// 具体数值待实机调，集中在这里改。
constexpr int kSlow = 50;
constexpr int kMid = 100;
constexpr int kFast = 200;
// 俯仰轴强度 2（使劲）的“有力”档：点头、开心、惊讶更干脆。
// 250°/s 即 20ms 一拍 5°，仍远低于舵机空载极限（约 600°/s），带头部负载也跟得上。
// 舵机输出力矩由供电和型号决定，PWM 无法调力矩；“更用力”只能靠更大幅度 + 更快速度。
constexpr int kFaster = 250;

// 强度只有两档：1 普通、2 使劲（11/23 由三档收为两档，
// 新 1 档 = 旧 2 档，新 2 档 = 旧 3 档；三档时模型分不清“中”和“强”）。
constexpr int Pick(int level, int normal, int strong) {
    return level >= 2 ? strong : normal;
}

struct ActionInfo {
    std::string_view name;
    bool posture;  // true = 姿态（转过去保持），false = 表情（做完回原朝向）
    const char* emotion;  // 随动作真正开始显示，不另建一条表情队列。
};

// 清单顺序即工具描述顺序。新增动作要同时在 BuildRelative 里给出关键帧，
// 单测会逐个展开清单里的动作，漏写关键帧会直接失败。
constexpr ActionInfo kActions[] = {
    {"look_left", true, "focus"},       {"look_right", true, "focus"},
    {"look_up", true, "focus"},         {"look_down", true, "focus"},
    {"look_up_left", true, "focus"},    {"look_up_right", true, "focus"},
    {"look_down_left", true, "focus"},  {"look_down_right", true, "focus"},
    {"look_center", true, "neutral"},
    {"nod", false, "happy"},            {"shake", false, "focus"},
    {"look_around", false, "focus"},    {"curious", false, "surprised"},
    {"think", false, "focus"},          {"shy", false, "happy"},
    {"sad", false, "sad"},              {"happy", false, "happy"},
    {"surprised", false, "surprised"},  {"bow", false, "happy"},
    {"sleepy", false, "sad"},           {"dance", false, "happy"},
    {"listen", false, "focus"},         {"angry", false, "angry"},
    {"crying", false, "crying"},        {"stop", true, "neutral"},
};

const ActionInfo* Find(std::string_view action) {
    for (const auto& info : kActions) {
        if (info.name == action) return &info;
    }
    return nullptr;
}

// 姿态各强度的角度（普通 / 使劲）：
//   水平 40/55（横向限位保持 ±60°；强档离限位还有 5°，待实机确认）。
//   俯仰 11/23 按实机反馈加大（旧版上下动作太弱）：
//   抬头 35/50（限位 60），低头 30/38（限位 40，留 2° 余量不顶限位）。
int PanDeg(int level) { return Pick(level, 40, 55); }
int PanLookSpeed(int level) { return Pick(level, 125, 150); }
int UpDeg(int level) { return Pick(level, 35, 50); }
int DownDeg(int level) { return Pick(level, 30, 38); }

// 表情动作的相对关键帧（相对动作中心），不含最后回原朝向那一帧；
// return_speed 是回原朝向的速度。
struct Relative {
    std::vector<Keyframe> frames;
    int return_speed = kMid;
};

bool BuildRelative(std::string_view a, int lv, Relative& r) {
    auto& f = r.frames;
    if (a == "nod") {
        // 点头：向下点再回来。普通 22°×3；使劲 30°×3 且更快。
        const int amp = Pick(lv, 22, 30);
        const int speed = lv >= 2 ? kFaster : kFast;
        for (int i = 0, n = Pick(lv, 3, 3); i < n; ++i) {
            f.push_back({0, amp, speed, 60});
            f.push_back({0, 0, speed, 40});
        }
        r.return_speed = speed;
    } else if (a == "shake") {
        // 摇头：左右来回，最后回中。
        const int amp = Pick(lv, 24, 34);
        for (int i = 0, n = Pick(lv, 2, 3); i < n; ++i) {
            f.push_back({-amp, 0, kFast, 40});
            f.push_back({amp, 0, kFast, 40});
        }
        r.return_speed = kFast;
    } else if (a == "look_around") {
        // 张望：先抬头看向一侧，再略放低视线扫到另一侧，最后回原朝向。
        // 保留横向幅度和停顿，避免变成单一高度的左右摇头。
        const int amp = Pick(lv, 35, 48);
        const int lift = Pick(lv, 12, 18);
        // 从机械边界起步也要给俯仰和回程留足时间。
        const int hold = Pick(lv, 400, 420);
        f.push_back({-amp, -lift, kMid, hold});
        f.push_back({amp, -lift / 2, kMid, hold});
    } else if (a == "curious") {
        // 好奇：没有侧倾轴，用“微抬头 + 偏向一侧”近似歪头。
        f.push_back({Pick(lv, 18, 24), -Pick(lv, 16, 22), kMid, 800});
    } else if (a == "think") {
        // 思考：慢慢看向左上方，停住像在回忆。
        // 强档横向 30°，来回约 1.2 秒，停留 1.7 秒仍收在 3 秒内。
        f.push_back({-Pick(lv, 23, 30), -Pick(lv, 20, 28), kSlow,
                     Pick(lv, 1400, 1700)});
        r.return_speed = kSlow;
    } else if (a == "shy") {
        // 害羞：慢慢低头并偏开。
        f.push_back({Pick(lv, 18, 24), Pick(lv, 25, 32), kSlow,
                     Pick(lv, 900, 1200)});
        r.return_speed = kSlow;
    } else if (a == "sad") {
        // 低落：慢慢垂头，停一会儿再慢慢抬起。
        // 使劲时垂 38° 来回各 0.76 秒，停留缩到 1.4 秒以收在 3 秒内。
        f.push_back({0, Pick(lv, 30, 38), kSlow, Pick(lv, 1500, 1400)});
        r.return_speed = kSlow;
    } else if (a == "happy") {
        // 开心：快速小幅上下颠几下。使劲时幅度更大且更快。
        const int amp = Pick(lv, 10, 14);
        const int speed = lv >= 2 ? kFaster : kFast;
        for (int i = 0, n = Pick(lv, 5, 5); i < n; ++i) {
            f.push_back({0, -amp, speed, 30});
            f.push_back({0, 0, speed, 30});
        }
        r.return_speed = speed;
    } else if (a == "surprised") {
        // 惊讶：快速抬头，愣住半秒。使劲时抬 40° 且更快（抬头限位 60）。
        f.push_back({0, -Pick(lv, 30, 40), lv >= 2 ? kFaster : kFast, 500});
    } else if (a == "bow") {
        // 致谢：慢慢低头，停一下再抬起。
        f.push_back({0, Pick(lv, 30, 38), kSlow, Pick(lv, 900, 1100)});
        r.return_speed = kSlow;
    } else if (a == "sleepy") {
        // 犯困：慢慢垂头，抬一下，又垂下去。
        // 幅度加大后慢档来回更久，缩短停留把使劲档收在 3 秒内。
        const int amp = Pick(lv, 30, 38);
        f.push_back({0, amp, kSlow, 200});
        f.push_back({0, amp / 3, kMid, 100});
        f.push_back({0, amp, kSlow, Pick(lv, 500, 300)});
        r.return_speed = kSlow;
    } else if (a == "dance") {
        // 摇摆：左上、右上交替。
        const int amp = Pick(lv, 23, 27);
        for (int i = 0, n = Pick(lv, 3, 4); i < n; ++i) {
            f.push_back({-amp, -8, kFast, 80});
            f.push_back({amp, -8, kFast, 80});
        }
        r.return_speed = kFast;
    } else if (a == "listen") {
        // 倾听：偏向一侧微低头，停住认真听。
        f.push_back({Pick(lv, 18, 24), Pick(lv, 10, 14), kMid, 1500});
    } else if (a == "angry") {
        // 生气：微低头后短促地甩头，不用大幅撞限位。
        const int amp = Pick(lv, 12, 18);
        const int tilt = Pick(lv, 8, 12);
        f.push_back({-amp, tilt, kFast, 160});
        f.push_back({amp, tilt, kFast, 180});
        f.push_back({0, tilt, kMid, 250});
    } else if (a == "crying") {
        // 哭泣：垂头后小幅抽泣，再缓慢回到原朝向。
        const int tilt = Pick(lv, 22, 30);
        f.push_back({0, tilt, kSlow, 180});
        for (int i = 0; i < 3; ++i) {
            f.push_back({0, tilt - Pick(lv, 4, 6), kMid, 80});
            f.push_back({0, tilt, kMid, 120});
        }
        r.return_speed = kSlow;
    } else {
        return false;
    }
    return true;
}

int Clamp(int v, int lo, int hi) { return std::min(std::max(v, lo), hi); }

}  // namespace

const std::vector<std::string_view>& ActionNames() {
    static const std::vector<std::string_view> names = [] {
        std::vector<std::string_view> v;
        for (const auto& info : kActions) v.push_back(info.name);
        return v;
    }();
    return names;
}

bool IsValidAction(std::string_view action) { return Find(action) != nullptr; }

bool BuildMotion(std::string_view action, int level, int from_pan, int from_tilt,
                 const Limits& limits, Motion& out) {
    const ActionInfo* info = Find(action);
    if (info == nullptr || action == "stop") return false;
    level = Clamp(level, 1, 2);
    from_pan = Clamp(from_pan, limits.pan_min, limits.pan_max);
    from_tilt = Clamp(from_tilt, limits.tilt_min, limits.tilt_max);

    out = Motion{};
    out.name = std::string(action);
    out.level = level;
    out.return_home = !info->posture;
    out.emotion = info->emotion;

    if (info->posture) {
        // 姿态是相对中位的绝对朝向；单轴动作只改自己那一轴，另一轴保持原样，
        // 这样“先抬头再看左边”得到的是左上，而不是被拉回水平。
        int pan = from_pan;
        int tilt = from_tilt;
        const bool left = action.find("left") != std::string_view::npos;
        const bool right = action.find("right") != std::string_view::npos;
        const bool up = action.find("up") != std::string_view::npos;
        const bool down = action.find("down") != std::string_view::npos;
        if (left) pan = 90 - PanDeg(level);
        if (right) pan = 90 + PanDeg(level);
        if (up) tilt = 90 - UpDeg(level);
        if (down) tilt = 90 + DownDeg(level);
        if (action == "look_center") {
            pan = 90;
            tilt = 90;
        }
        // 只有纯横向姿态提速；斜向与俯仰姿态保持原速度。
        const int speed = (left || right) && !(up || down) ? PanLookSpeed(level) : kMid;
        out.frames.push_back({Clamp(pan, limits.pan_min, limits.pan_max),
                              Clamp(tilt, limits.tilt_min, limits.tilt_max), speed, 0});
        return true;
    }

    Relative rel;
    if (!BuildRelative(action, level, rel)) return false;

    // 以当前朝向为中心；靠近限位时把中心整体往回挪，保证幅度做完整，
    // 而不是在限位处被截平（截平后点头/摇头会变形，还可能顶住限位发响）。
    int min_pan = 0, max_pan = 0, min_tilt = 0, max_tilt = 0;
    for (const auto& k : rel.frames) {
        min_pan = std::min(min_pan, k.pan);
        max_pan = std::max(max_pan, k.pan);
        min_tilt = std::min(min_tilt, k.tilt);
        max_tilt = std::max(max_tilt, k.tilt);
    }
    const int center_pan = Clamp(from_pan, limits.pan_min - min_pan, limits.pan_max - max_pan);
    const int center_tilt =
        Clamp(from_tilt, limits.tilt_min - min_tilt, limits.tilt_max - max_tilt);
    for (const auto& k : rel.frames) {
        // 再收敛一次兜底：幅度超过整段行程时也绝不越过限位。
        out.frames.push_back({Clamp(center_pan + k.pan, limits.pan_min, limits.pan_max),
                              Clamp(center_tilt + k.tilt, limits.tilt_min, limits.tilt_max),
                              k.speed, k.hold_ms});
    }
    // 做完回到动作开始前的朝向（不是挪过的中心）。
    out.frames.push_back({from_pan, from_tilt, rel.return_speed, 0});
    return true;
}

std::string HeadMotionPlayer::Enqueue(std::string_view action, int level, bool show_emotion) {
    if (!IsValidAction(action)) {
        return "未知动作：" + std::string(action);
    }
    if (action == "stop") {
        Stop();
        // 中止旧动作后，回中也是本次工具的尾段，由同一播放器跟踪完成。
        queue_.push_back({"look_center", 1});
        return "";
    }
    if (queue_.size() >= kMaxQueued) {
        // 拒绝新动作而不是丢旧动作：模型连发太多时，先做完已排的更符合预期。
        return "动作队列已满（最多排 4 个），等当前动作做完再发";
    }
    queue_.push_back({std::string(action), Clamp(level, 1, 2), show_emotion});
    return "";
}

void HeadMotionPlayer::Stop() {
    queue_.clear();
    active_ = false;
    holding_ = false;
    current_ = Motion{};
    rest_pan_ = 90;
    rest_tilt_ = 90;
}

bool HeadMotionPlayer::StartNext(int pan, int tilt) {
    while (!queue_.empty()) {
        const Pending next = queue_.front();
        queue_.pop_front();
        const ActionInfo* info = Find(next.action);
        // 表情以“此刻实际朝向”为中心（被红外临时转过去时就在那边做）；
        // 姿态的另一轴以静止朝向为准，不继承传感器的临时偏转。
        const bool posture = info != nullptr && info->posture;
        Motion motion;
        if (!BuildMotion(next.action, next.level, posture ? rest_pan_ : pan,
                         posture ? rest_tilt_ : tilt, limits_, motion)) {
            continue;
        }
        // 传感器可能暂时改变实际俯仰。若本次转头还需恢复静止俯仰，
        // 两轴共用关键帧速度，沿用原速度以免同时加快俯仰回位。
        if (posture && tilt != motion.frames.front().tilt) {
            motion.frames.front().speed = std::min(motion.frames.front().speed, kMid);
        }
        if (!next.show_emotion) motion.emotion = nullptr;
        current_ = std::move(motion);
        active_ = true;
        frame_index_ = 0;
        holding_ = false;
        hold_left_ms_ = 0;
        pos_pan_ = static_cast<float>(pan);
        pos_tilt_ = static_cast<float>(tilt);
        return true;
    }
    return false;
}

bool HeadMotionPlayer::Tick(int dt_ms, int& pan, int& tilt, const char** started_emotion) {
    if (started_emotion != nullptr) *started_emotion = nullptr;
    if (!active_) {
        if (!StartNext(pan, tilt)) return false;
        if (started_emotion != nullptr) *started_emotion = current_.emotion;
    }

    float budget_ms = static_cast<float>(dt_ms);
    // 一拍里可能走完一帧并进入下一帧（快档小幅度时常见），用剩余时间继续推进，
    // 否则每个关键帧至少耗一拍，快动作会被拖慢。
    while (active_ && budget_ms > 0.0f) {
        const Keyframe& k = current_.frames[frame_index_];
        if (holding_) {
            const float used = std::min(budget_ms, static_cast<float>(hold_left_ms_));
            hold_left_ms_ -= static_cast<int>(std::ceil(used));
            budget_ms -= used;
            if (hold_left_ms_ > 0) break;
            holding_ = false;
            if (++frame_index_ >= current_.frames.size()) {
                // 姿态动作的终点成为新的静止朝向；表情动作已回到原处，不改静止朝向。
                if (!current_.return_home) {
                    rest_pan_ = current_.frames.back().pan;
                    rest_tilt_ = current_.frames.back().tilt;
                }
                active_ = false;
                current_ = Motion{};
                // 下一动作下一拍启动，确保一次 Tick 只发布一个表情切换和它对应的 PWM。
                break;
            }
            continue;
        }
        // 两轴沿直线同时到位：按距离较大的轴算耗时，另一轴等比例缩放。
        const float dp = static_cast<float>(k.pan) - pos_pan_;
        const float dtl = static_cast<float>(k.tilt) - pos_tilt_;
        const float dist = std::max(std::fabs(dp), std::fabs(dtl));
        const float speed = static_cast<float>(std::max(k.speed, 1));
        const float need_ms = dist * 1000.0f / speed;
        if (need_ms <= budget_ms) {
            pos_pan_ = static_cast<float>(k.pan);
            pos_tilt_ = static_cast<float>(k.tilt);
            budget_ms -= need_ms;
            holding_ = true;
            hold_left_ms_ = k.hold_ms;
            continue;
        }
        const float ratio = budget_ms / need_ms;
        pos_pan_ += dp * ratio;
        pos_tilt_ += dtl * ratio;
        budget_ms = 0.0f;
    }
    pan = static_cast<int>(std::lround(pos_pan_));
    tilt = static_cast<int>(std::lround(pos_tilt_));
    return true;
}

}  // namespace deepbot_head
