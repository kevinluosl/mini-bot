#ifndef _DEEPBOT_EYES_DISPLAY_H_
#define _DEEPBOT_EYES_DISPLAY_H_

#include "display/oled_display.h"
#include "device_state.h"

#include <lvgl.h>
#include <atomic>
#include <cstdint>

// 在原 OLED 界面上叠加眼睛；隐藏而不删除基类仍会访问的控件。
class DeepbotEyesDisplay : public OledDisplay {
public:
    // 眼睛支持圆角矩形和圆形两种光栅几何。
    enum class EyeStyle {
        kRoundedRect,  // 扁圆角矩形（0.96 板，默认，勿改动）
        kCircle,       // 正圆（cam 板机器人造型）
    };

private:
    // 眼睛可用区顶边；单色屏传 0 使用整屏。
    int zone_top_ = 16;
    static constexpr int kBlueZoneBottom = 64;
    // 上下边缘的总留白。
    static constexpr int kEyeZoneInset = 8;
    // 在居中布局基础上向上微调。
    static constexpr int kEyeVerticalLift = 2;
    // 眼睛中心 y（蓝区居中，圆直径变化时上下对称伸缩）。
    int eye_center_y_ = 40;
    int eye_h_max_ = 40;
    EyeStyle style_ = EyeStyle::kRoundedRect;

    // 圆角矩形眼睛的固定宽度，也是通用宽度上限。
    static constexpr int EYE_W = 40;

    // 扁圆角矩形的待机高度。
    static constexpr int EYE_H_STANDBY = 22;
    // 聆听抬到 36：保持"听到唤醒词就睁大"的高度层次（36 > 22）。
    // 为何不用蓝区上限 44：抬到 44 会让聆听那一下非常突兀（22→44 翻倍跳变），
    // 36 的跳变幅度（+14px）更接近原本认可的 24→36（+12px）手感。
    static constexpr int EYE_H_LISTEN = 36;
    // 说话时缩到 18（比待机 22 小一档）：保留"说话时略缩小"的律动层次；
    // 叠上 ±2px 音节律动后峰值 20px，仍严格小于待机 22px。
    static constexpr int EYE_H_TALK = 18;

    static constexpr int EYE_D_STANDBY = 36;
    // 聆听：睁到最大 40px，表现"听到唤醒词"的机警感。
    static constexpr int EYE_D_LISTEN = 40;
    // 说话：缩到 32px，保留"说话时略缩小"的律动层次。
    // 叠 ±2px 音节律动后峰值 34px，仍严格小于待机 36px。
    static constexpr int EYE_D_TALK = 32;

    static constexpr int EYE_SMILE_W = 34;
    // 完整轮廓由共享扫描行几何函数（RoundedRectHalfWidth）生成：
    // 上下有明确直边、四角按半径（高度×0.35）圆弧过渡；
    // 眨眼变矮时半径随高度缩小，形状连续变化不跳变。
    static constexpr int EYE_LEFT_CX = 38;
    static constexpr int EYE_RIGHT_CX = 90;
    static constexpr int EYE_MIN_H = 3;

    // 笑眼画圆头弧带；哭泣在实心眼睛上叠加泪滴。
    enum class EyeShape {
        kOval,     // 实心圆角矩形（默认；哭眼也用它，靠泪滴区分）
        kSmileArc, // 圆头粗 ∪ 弧（笑眼：happy）
    };


    // 聆听和说话使用不同位置的常亮点。
    static constexpr int kIndicatorSize = 4;               // 圆点直径（缩小版）
    static constexpr int kIndicatorXListening = 4;         // 聆听时 x 位置（banner 左上角）
    static constexpr int kIndicatorYListening = 4;         // 聆听时 y 位置（banner 黄区）
    static constexpr int kIndicatorXSpeaking = 4;          // 说话时 x 位置（眼睛左侧）
    static constexpr int kIndicatorYSpeaking = 32;         // 说话时 y 位置（蓝区，眼睛左侧）

    // 说话时点的闪烁周期（tick 数，1 tick = 60ms）。8 tick ≈ 480ms，接近说话节奇。
    // 注意：现在两种状态都不闪烁，只靠颜色（位置）区分，保留此常量以免后续改动。
    static constexpr uint32_t kIndicatorBlinkTicks = 8;
    uint32_t indicator_tick_ = 0;  // 闪烁计时（暂未使用）
    bool indicator_on_ = true;     // 当前点是否点亮（暂未使用）
    int last_indicator_x_ = -1;    // 上一次设定的 x
    int last_indicator_y_ = -1;    // 上一次设定的 y

    lv_obj_t* indicator_ = nullptr;  // 状态指示点（待机时隐藏）

    enum class EyeState {
        kStandby,    // 待机
        kListening,  // 聆听
        kSpeaking,   // 说话
    };

    // 短时情绪叠加在设备状态上，结束后自动恢复状态表情。
    enum class EyeEmotion {
        kNone,      // 无情绪（用状态表情）
        kHappy,     // 开心：眯成弯眼，略上提
        kSad,       // 悲伤：眼低下垂，视线下沉
        kAngry,     // 生气：眯成一条窄缝，向下压
        kCrying,    // 哭泣：闭得很小 + 抽泣颤动 + 下垂
        kFocus,     // 专注：用于看、张望和倾听
        kSurprised, // 惊讶：瞪圆双眼
    };

    // 情绪有效期：2 秒。
    //
    // tick 为 60ms，2000/60 = 33.3，向上取整为 34 tick (= 2040ms)，
    // 向上取整是为了保证不短于 2 秒。
    //
    // 为何从 1 秒改为 2 秒：实测用户日常句子的语音时长平均约 2.9 秒，
    // 而原 1 秒的情绪在语音播报到 1/3 时就消失了（如“我先笑一下”这句
    //   播了 2.87 秒，happy 在 1 秒后过期，剩下的时间用户听到“笑啦”
    // 但眼睛已恢复微瞑），观感上就是“闪一下就没了”。
    // 取 2 秒是权衡：足够看清，又不至于跨两句一直卡在同一个表情。
    static constexpr uint32_t kEmotionDurationTicks = 34;

    EyeEmotion emotion_ = EyeEmotion::kNone;
    EyeEmotion head_emotion_ = EyeEmotion::kNone;
    bool head_emotion_running_ = false;
    uint32_t head_emotion_until_tick_ = 0;
    bool touch_smile_ = false;  // 持显示锁访问，不进入工具表情队列。
    EyeEmotion VisibleEmotion() const {
        if (head_emotion_ != EyeEmotion::kNone) return head_emotion_;
        return emotion_ != EyeEmotion::kNone ? emotion_ : (touch_smile_ ? EyeEmotion::kHappy : EyeEmotion::kNone);
    }
    static EyeEmotion ParseEmotion(const char* emotion);
    uint32_t emotion_until_tick_ = 0;

    // 固定容量，避免堆分配和表情积压。
    static constexpr int kEmotionQueueSize = 8;
    EyeEmotion emotion_queue_[kEmotionQueueSize] = {};
    int emotion_queue_head_ = 0;   // 下一个要执行的位置
    int emotion_queue_count_ = 0;  // 当前待执行数量

    // 把队首情绪取出并激活（重置 2 秒计时）。队列为空时返回 false。
    // 仅由 OnAnimTick（LVGL 任务、已持锁）调用。
    bool PopNextEmotion();

    // 把情绪枚举转成字符串，仅用于日志
    static const char* EmotionName(EyeEmotion emotion);

    lv_obj_t* eye_left_ = nullptr;
    lv_obj_t* eye_right_ = nullptr;
    // 全屏黑色遮罩：单独一层盖住基类的 container_ 全屏白底。
    // LVGL 默认主题是 light 模式（CONFIG_LV_THEME_DEFAULT_DARK 未开），背景为白色；
    // 而单色转换的判据是 blue > 16（见 esp_lvgl_port 的 _lvgl_port_transform_monochrome），
    // 白色 blue=31 会点亮像素，导致整个屏幕发光（双色屏呈现上黄下蓝）。
    // 这层遮罩创建于 container_ 之后、眼睛之前，z 序夹在两者中间，能稳定压住白底。
    lv_obj_t* bg_mask_ = nullptr;
    lv_timer_t* anim_timer_ = nullptr;

    EyeState state_ = EyeState::kStandby;
    uint32_t tick_ = 0;  // 计时 tick（每 tick 60ms）

    // 眨眼状态机（所有状态都会眨眼，这是"像人"的关键）
    int blink_pct_ = 0;        // 0=完全睁开，100=完全闭合
    bool blinking_ = false;
    int blink_dir_ = 1;        // +1 闭合中，-1 睁眼中
    uint32_t next_blink_tick_ = 0;
    int pending_extra_blinks_ = 0;  // 待执行的连眨次数（真人偶尔会连眨两下）

    // 视线漂移（待机时大步幅左右扫视；聆听时小幅度左右扫动寻找声音；说话时保持居中）
    int look_offset_ = 0;
    int look_target_ = 0;
    uint32_t next_look_tick_ = 0;

    // 传感器任务仅发布方向，LVGL 任务消费；独立方向值不承担其他状态同步。
    std::atomic<int> external_gaze_{0};

    // 外部视线幅度（正数=向右，负数=向左）。
    //
    // 取 10px：直径 36~40px 时约是 1/4 瞳孔宽度，观感上明显偏头看，
    // 又不至于把眼睛推得贴到另一个眼睛边上。
    static constexpr int kExternalGazePx = 10;

    // 当前状态的进入 tick（用于计算聆听开头“先睁大再回落正常”的过渡，以及说话时的微幅律动）
    uint32_t state_enter_tick_ = 0;

    // 几何缓存：只有高度/偏移真正变化时才去改 oled 对象。
    // 这块 I2C OLED 一次整屏刷新（128x64 → 1KB @400kHz）开销不小，
    // 若每个 tick 都无条件 set_size/set_pos，会持续占着 LVGL 任务与 I2C 总线，
    // 拖慢音频链路，表现为“呼暖/说话反应变铝、不灵敏”。
    int last_eye_h_ = -1;
    // 宽度也参与重绘判据：眨眼时高度变、宽度不变，
    // 但状态切换（待机36→聆听40）时宽度会变，必须重绘。
    int last_eye_w_ = -1;
    int last_eye_offset_ = -999;
    // 情绪会改变眼睛的垂直位置，所以 y 也必须纳入缓存判据，
    // 否则“高度不变但位置变”的情绪（如 happy 上提）不会触发重绘。
    int last_eye_y_ = -999;
    // 形状与内收量同样要进缓存：形状切换/内收变化时尺寸可能不变，也必须重绘。
    EyeShape last_eye_shape_ = EyeShape::kOval;
    int last_eye_converge_ = -999;
    int last_eye_shear_ = -999;

    // 按当前状态刷新下一次眨眼/视线漂移的时间（状态切换时调用，避免沿用旧节奏）
    void ScheduleNextBlink();
    void ScheduleNextLook();
    // 刷新状态指示点（聆听：banner 黄点常亮；说话：眼侧蓝点常亮；待机隐藏）
    void UpdateIndicator();
    // 推进指示点闪烁（仅在说话状态下由 OnAnimTick 调用）
    void TickIndicatorBlink();

    // true = 当前应该在"眼睛模式"；false = 让基类显示文字（配网/激活等必要提示）
    // 初值故意设为 true（表示“尚未初始化”）：SetupUI 末尾会用真实状态调一次
    // ApplyDisplayMode()，从而保证首次一定会真正执行一遍（眼睛创建后默认是可见的，
    // 而启动阶段需要把它隐藏掉）。之后模式未变化时 ApplyDisplayMode 会提前返回。
    bool eyes_mode_ = true;

    // 创建/销毁眼睛对象与动画定时器
    void CreateEyes();
    void DestroyEyes();

    // 动画 tick：推进眨眼、张望、说话节奏，并按当前状态刷新几何
    void OnAnimTick();
    static void AnimTimerCallback(lv_timer_t* timer);

    // 眨眼时只缩高度；x_offset 同向平移，converge 让两眼相向或相背移动。
    void ApplyEyeGeometry(int base_width, int height, int x_offset, int y_offset = 0,
                          EyeShape shape = EyeShape::kOval, int converge = 0);

    // 实际的尺寸/位置/重绘落地（两种眼型共用）。
    //
    // 拆出来的目的：宽度计算必须按板型风格分岔（圆形取直径、扁矩形恒为 EYE_W），
    // 而“判重 + 摆位 + 失效重绘”这段两者完全一样。
    // 分开后主函数只做宽度决策，这段保持单一实现，避免两份逻辑走偏。
    void ApplyEyeGeometryWithWidth(int w, int h, int x_offset, int y_offset,
                                   EyeShape shape, int converge);

    // 眼睛内容的绘制回调（LV_EVENT_DRAW_MAIN）。
    //
    // 为何要自绘而不是用控件样式：
    //   1) 圆角矩形：LVGL 的 radius 会被裁到短边一半，且四角只能等半径，
    //      无法精确控制"直边 + 0.35h 圆角"的比例；因此用 DRAW_MAIN 逐行光栅化。
    //   2) 真弧线：单边 border 画出的是「两侧小圆角 + 中间长直边」，
    //      当高度扁到 18px 时几乎就是一条横线；而 LVGL 的 lv_draw_arc
    //      宽高比恒为 2:1、线宽也难随心调，画不出圆头粗弧。笑眼因此
    //      改为逐列光栅化圆头粗 ∪ 弧（见 eye_geometry::ArcCenterline）。
    // 用 DRAW_MAIN 而不是 canvas：对象仍是普通 lv_obj，尺寸、位置、无效区域
    // 和缓存判据继续沿用现有机制，只把画的内容换成眼睛像素。
    static void EyeDrawCb(lv_event_t* e);
    // mirror=true 表示右眼：泪滴要画在右侧，与左眼镜像对称。
    void DrawEyeShape(lv_layer_t* layer, const lv_area_t& coords, EyeShape shape, bool mirror);

    // 当前状态下的基础尺寸（未叠加眨眼）。
    //
    // 宽高分开返回，是为了同时支持两种眼型：
    //   * kRoundedRect：宽恒定 EYE_W，高按状态变（22/36/18）
    //   * kCircle    ：宽 == 高 == 直径（36/40/32）
    // 眨眼时两者都只压高度、宽度保持不变，
    // 所以对两种眼型都能得到“上下眼皮合拢”的观感。
    int BaseHeightForState() const;
    // 当前状态下的基准宽度（不受眨眼影响）。
    int BaseWidthForState() const;

    // 情绪对应的眼睛高度（覆盖状态）。返回 -1 表示无情绪、应交回状态处理。
    int EmotionHeight() const;
    // 情绪对应的垂直偏移（正数向下）。有情绪时同时抑制视线漂移。
    int EmotionYOffset() const;
    // 情绪对应的眼睛形状（默认 kOval）。
    EyeShape EmotionShape() const;
    // 情绪对应的双眼内收量（正数=向内靠）。angry 用它表达「盯住你」的聚焦感。
    // 双眼靠拢量：正数向内（生气聚焦），负数向外（悲伤涣散）。
    int EmotionConverge() const;
    // 外眼角下垂量（像素），只有悲伤非 0。
    int EmotionShear() const;
    // 由设备状态机推导眼睛表情（唯一可信来源）
    void SyncStateFromDevice();
    // 设备状态是否应该用眼睛界面
    static bool ShouldShowEyes(DeviceState state);
    // 应用"眼睛/文字"两种显示模式
    void ApplyDisplayMode(bool eyes_mode);
    // 屏幕底色设为黑（双色屏上不点亮的像素即黑色）
    void ApplyBlackBackground();
    void ShowBrandText();

public:
    // zone_top 为眼睛绘制区顶部。
    DeepbotEyesDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                       int width, int height, bool mirror_x, bool mirror_y, int zone_top = 16,
                       EyeStyle style = EyeStyle::kRoundedRect);
    ~DeepbotEyesDisplay() override;

    // 先构建基类 UI，再隐藏其中的文字控件并叠加眼睛
    void SetupUI() override;

    // 状态入口。注意：不能用字符串匹配判断待机——待机时 LvglDisplay::UpdateStatusBar
    // 会用当前时间（如 "16:05"）调用本函数，字符串对不上 STANDBY，会被误判成
    // "非待机"从而退回默认 UI（小人与顶部 banner）。因此改为读取设备状态机。
    void SetStatus(const char* status) override;

    // 眼睛界面下聊天文字不显示；情绪则在本类内部用几何图形表达
    void SetChatMessage(const char* role, const char* content) override;
    void SetEmotion(const char* emotion) override;
    void SetConversationNeutral() override;
    void SetTouchSmile(bool enabled);
    void StartHeadActionEmotion(const char* emotion);
    void FinishHeadActionEmotion();
    bool HasPendingToolEmotion();
    // 默认清空待播情绪（不动当前）；cam 的已接受动作跨轮也继续执行。
    void ClearPendingActions() override;

    // 主题切换后重新把屏幕底色设为黑
    void SetTheme(Theme* theme) override;

    void SetGazeDirection(int dir);
};

#endif  // _DEEPBOT_EYES_DISPLAY_H_
