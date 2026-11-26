// cam 板头部动作引擎：把「点头」「摇头」「看左边」这类语义动作翻译成两轴角度轨迹。
//
// 为什么单独成文件、且不依赖任何 ESP-IDF 头：
//   1. 旧接口只能“设一个目标角度”，后台以固定速度平滑逼近。大模型想点头只能
//      连发 下/上/下/上，后一个目标在头还没到位时就覆盖前一个，几乎看不出动作，
//      每次还要多一轮模型往返。动作必须由固件按关键帧自己播放。
//   2. 纯逻辑（关键帧、排队、限位、按速度逐帧推进）可以在宿主机直接编译单测，
//      不需要烧录就能验证轨迹、打断和限位。
//   3. 板级文件已超 1000 行，动作逻辑放这里，板级只负责把角度写进 PWM。
//
// 坐标约定（与板级舵机代码一致）：
//   pan  = 1 号舵机逻辑角度，90 为中位，减小 = 左转，增大 = 右转
//   tilt = 2 号舵机逻辑角度，90 为中位，减小 = 抬头，增大 = 低头
//   2 号的物理反装由板级在写 PWM 时镜像，这里一律用逻辑角度。
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace deepbot_head {

// 关键帧：先以 speed（度/秒）同时向 (pan, tilt) 移动，两轴同时到位，
// 到位后停留 hold_ms。pan/tilt 是相对“动作中心”的偏移，由引擎换算成绝对角。
struct Keyframe {
    int pan;
    int tilt;
    int speed;
    int hold_ms;
};

// 一次动作展开后的完整轨迹。
// return_home=true 的是表情动作：做完回到动作开始前的朝向；
// false 的是姿态动作：停在最后一帧，成为新的静止朝向。
struct Motion {
    std::string name;
    int level = 2;
    bool return_home = false;
    const char* emotion = nullptr;  // 指向动作表中的常量。
    // 绝对坐标的关键帧（已加上中心并按限位收敛）。
    std::vector<Keyframe> frames;
};

// 两轴的机械限位（逻辑角度，闭区间）。
struct Limits {
    int pan_min;
    int pan_max;
    int tilt_min;  // 抬头极限
    int tilt_max;  // 低头极限
};

// 动作名清单（对外工具描述和参数校验共用，避免两处写漂）。
const std::vector<std::string_view>& ActionNames();
bool IsValidAction(std::string_view action);

// 把语义动作展开成绝对坐标轨迹。
//   from_pan / from_tilt：动作开始时头的朝向（姿态动作不用，表情动作以它为中心）
//   level：1 普通、2 使劲，越界会被收敛到 1..2
// 返回 false 表示动作名未知。
bool BuildMotion(std::string_view action, int level, int from_pan, int from_tilt,
                 const Limits& limits, Motion& out);

// 动作播放器：排队 + 逐帧推进。线程模型由调用方保证（板级用互斥锁包住）。
class HeadMotionPlayer {
public:
    static constexpr size_t kMaxQueued = 4;  // 一轮最多排 4 个，满了拒绝而不是丢最旧

    explicit HeadMotionPlayer(const Limits& limits) : limits_(limits) {}

    // 入队一个动作。返回空串表示成功，否则是给模型看的中文错误原因。
    // stop 立即清空旧动作，然后由播放器平滑回中，直到最后一拍才完成。
    std::string Enqueue(std::string_view action, int level, bool show_emotion = true);

    // 清空队列、中止当前动作，并把静止朝向设为中位。
    void Stop();

    // 每个控制周期调用一次：从当前实际角度 (pan, tilt) 出发，
    // 按当前关键帧的速度推进 dt_ms 毫秒，写回新的目标角度。
    // 返回 true 表示本拍由动作接管（调用方用返回的角度驱动舵机）；
    // false 表示没有动作在播放，调用方按自己的规则（静止朝向 / 传感器）决定目标。
    // started_emotion 仅在本拍启动新动作时输出表情，普通推进和空闲为 nullptr。
    bool Tick(int dt_ms, int& pan, int& tilt, const char** started_emotion = nullptr);

    bool Busy() const { return active_ || !queue_.empty(); }
    // 没有动作时头应停留的朝向（姿态动作和 stop 会改写它）。
    int RestPan() const { return rest_pan_; }
    int RestTilt() const { return rest_tilt_; }
    // 当前正在播放的动作名（日志与眼神跟随用），空闲时为空串。
    const std::string& ActiveName() const { return current_.name; }

private:
    struct Pending {
        std::string action;
        int level;
        bool show_emotion = true;
    };

    bool StartNext(int pan, int tilt);

    Limits limits_;
    std::deque<Pending> queue_;
    Motion current_;
    bool active_ = false;
    size_t frame_index_ = 0;
    int hold_left_ms_ = 0;
    bool holding_ = false;
    // 浮点位置：低速时每拍不足 1°，用整数会被截断成 0 导致永远走不到。
    float pos_pan_ = 90.0f;
    float pos_tilt_ = 90.0f;
    int rest_pan_ = 90;
    int rest_tilt_ = 90;
};

}  // namespace deepbot_head
