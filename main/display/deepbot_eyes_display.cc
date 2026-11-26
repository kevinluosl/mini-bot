#include "deepbot_eyes_display.h"
#include "eye_geometry.h"

#include "application.h"
#include "assets/lang_config.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_random.h>

#include <cstring>  // strcmp：SetEmotion 解析情绪名用

#define TAG "DeepbotEyes"

namespace {
// 动画节拍：60ms 一 tick（约 16fps）。OLED 是 I2C 单色屏，局部刷新开销小，
// 这个频率足以让眨眼和张望看起来连续，又不会挤占音频任务。
constexpr uint32_t kTickMs = 60;

// 眨眼：闭合快、睁开慢（真人特征）。60ms 一 tick：
//   闭合 ~3 tick（180ms），睁开 ~5 tick（300ms），总时长接近真人的 200~400ms。
// 之前闭合/睁开都用 25%/tick 对称变化，看起来机械。
constexpr int kBlinkCloseStep = 34;
constexpr int kBlinkOpenStep = 22;

// 各状态的眨眼间隔（tick 数，1 tick = 60ms）。真人不是匀速眨眼，而是随机间隔：
//   待机 1.2~3.6s（放松自然）
//   聆听 0.48~1.2s（刚听见唤醒先睁大，回落正常后眨眼加快、连眨增多，表现出急切等待主人说话的感觉）
//   说话 1.0~2.4s（微眯眼认真讲话，配合眨眼与轻柔律动）
constexpr uint32_t kBlinkStandbyMin = 20;
constexpr uint32_t kBlinkStandbyMax = 60;
constexpr uint32_t kBlinkListenMin = 8;
constexpr uint32_t kBlinkListenMax = 20;
constexpr uint32_t kBlinkTalkMin = 16;
constexpr uint32_t kBlinkTalkMax = 40;

// 连眨：一次眨眼结束后，按概率紧接着再眨一次
//   普通状态 15%，聆听等待状态提升到 35%（表现出催促/期待的小焦急感）
constexpr int kDoubleBlinkPercent = 15;
constexpr int kDoubleBlinkListenPercent = 35;
constexpr uint32_t kDoubleBlinkDelayMin = 3;  // 约 180ms
constexpr uint32_t kDoubleBlinkDelayMax = 5;  // 约 300ms

// 聆听状态“先睁大、再还原正常大小”的时序（tick 数，60ms/tick）：
//   0..7 tick（约 420ms）：保持聆听高度睁大，表现“听到唤醒词！”的机警感
//   8..13 tick（约 360ms）：平滑回落到待机高度，随后靠高频眨眼表现急切等待
//   （具体像素值按眼型风格取自 EYE_D_* / EYE_H_* 两套表，曲线逻辑共用）
//   注：聆听 36 > 待机 22，保留 +14px 的高度层次。
constexpr uint32_t kListenWideHoldTicks = 7;
constexpr uint32_t kListenShrinkTicks = 6;

// 视线漂移：
//   待机：大幅度（±16px）左右张望，让待机眼神活泼灵动；
//   聆听：小幅度（±8px，待机的一半）、较快节奏左右微移，配合快眨眼表现急切倾听；
//   说话：保持居中（不再左右移动）
constexpr int kLookMaxOffsetStandby = 16;
constexpr int kLookMaxOffsetListen = 8;
constexpr int kLookStepStandby = 2;           // 每 tick 移动 2px，转眼干脆有神
constexpr uint32_t kLookStandbyMin = 15;      // 待机：0.9s ~ 2.4s 换一次视线方向
constexpr uint32_t kLookStandbyMax = 40;
constexpr uint32_t kLookListenMin = 10;       // 聆听：0.6s ~ 1.5s 小幅换一次视线，表现急切等待
constexpr uint32_t kLookListenMax = 25;

// 说话时的音节微律动周期（在说话高度基础上做 -2..+2px 的轻柔起伏，像边笑边开口说话的生动表情）
constexpr int kTalkPulseTable[8] = {0, 1, 2, 1, 0, -1, -2, -1};

// 笑眼通过框高变化改变弧深，线宽保持 6 px。
constexpr int kEmoHappyMinH = 12;       // 开心：粗 ∪ 弧颤动区间下限
constexpr int kEmoHappyMaxH = 14;       // 开心：粗 ∪ 弧颤动区间上限
constexpr int kEmoHappyY = -3;          // 整体上提（笑得眉毛飞起来的感觉）

// 线宽相对于框高的千分比。
constexpr int kSmileThickPerMille = 350;

constexpr int kEmoSadH = 18;            // 悲伤：比生气高得多（生气 11），避免两者都是横椭圆
constexpr int kEmoSadY = 9;             // 下垂：比生气更靠下，表达「低落」
// 悲伤的外眼角朝下，与生气的内眼角朝下区分。
constexpr int kEmoSadShear = 4;
constexpr int kEmoAngryH = 11;          // 生气：窄缝（原 14 → 11，更细更凶）
constexpr int kEmoAngryY = 2;           // 微下压
constexpr int kEmoAngryConverge = 3;    // 生气：双眼向内收 3px（怒视聚焦）
// 悲伤：双眼向外散 2px（与生气的内收方向相反）。
// 生气向内收 = 盯着你；悲伤向外散 = 失落、注意力涣散。
constexpr int kEmoSadDiverge = 2;
constexpr int kEmoCryY = 5;             // 略下移：框高较大，保留下方泪水空间
// 颤动节奏（tick 数，60ms/tick）：
//   开心 1 tick 一档（约 60ms）-> 笑得轻快颤动
//   哭泣 2 tick 一档（约 120ms）-> 抽噎的顿挫感
//   泪水下落 3 tick 一档（约 180ms）-> 比抽噎更慢，像泪水慢慢滑下
constexpr uint32_t kEmoHappyStepTicks = 1;
constexpr uint32_t kEmoCryStepTicks = 2;
constexpr uint32_t kEmoCryTearStepTicks = 3;

// ===== 哭眼几何 =====
// 框高固定：框内上方是眼睛椭圆，下方留出泪水下落的空间。
// 为何不把「眼睛高 = 框高」：泪滴必须画在同一对象内（LVGL 会把绘制
// 裁剪到对象区域），所以框得比眼睛高。
constexpr int kEmoCryFrameH = 30;
// 框内椭圆的抽噎颤动区间。框高不变，只有眼睛在呼吸。
constexpr int kEmoCryEyeMinH = 12;
constexpr int kEmoCryEyeMaxH = 16;
// 泪滴尺寸：宽 4px（max_half=2）、高 7px。太小看不出是水珠，
// 太大会抢掉眼睛的存在感。
constexpr int kEmoCryTearW = 4;
constexpr int kEmoCryTearH = 7;
// 泪滴距框边的水平距离。左眼靠左、右眼靠右（镜像），两边对称才自然。
constexpr int kEmoCryTearInset = 6;

// 抽噎时框内椭圆的高度（在 MinH..MaxH 之间三角波往返）。
//
// 为何抽噎只动画「椭圆高」而不动「框高」：
//   框高是泪水的参考系，若框高也跟着变，泪滴会跟着上下抖，
//   看起来像在弹跳而不是在流。框固定、只有眼睛呼吸，才像真人在抽噎。
int CryEyeHeight(uint32_t tick) {
    const uint32_t step = tick / kEmoCryStepTicks;
    const uint32_t phase = step % 3;                        // 0,1,2
    const uint32_t tri = (phase < 2) ? phase : (4 - phase);  // 0,1,1
    const int span = kEmoCryEyeMaxH - kEmoCryEyeMinH;
    return kEmoCryEyeMinH + static_cast<int>(tri) * span / 2;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
// ================= 关键：本板颜色映射是反相的 =================
// 实测链路（esp_lvgl_port v2.x + esp_lcd_panel_invert_color(panel_, false)）：
//   1) display color_format 为 RGB565（OledDisplay 未指定 color_format）
//   2) _lvgl_port_transform_monochrome 把 blue>16 当作 chroma_color，然后
//         chroma_color=true  -> bit &= ~...  (置 0)
//         chroma_color=false -> bit |=  ...  (置 1)
//   3) SSD1306 在 invert_color(false) 下只有 GDDRAM bit=1 才点亮像素
//   ⇒ 白色(blue=31) → bit=0 → **不亮**
//   ⇒ 黑色(blue=0)  → bit=1 → **点亮**
// 也就是说不能按直觉用 lv_color_black() 表示背景，必须反过来。
// 用下面两个语义函数表达意图，避免后来人（包括自己）读代码时反复困惑。
lv_color_t PixelOn() { return lv_color_black(); }   // 点亮像素（显示为蓝/黄，取决于行）
lv_color_t PixelOff() { return lv_color_white(); }  // 熄灭像素（看起来是黑色）

uint32_t RandRange(uint32_t lo, uint32_t hi) {
    if (hi <= lo) return lo;
    return lo + (esp_random() % (hi - lo + 1));
}
}  // namespace

DeepbotEyesDisplay::DeepbotEyesDisplay(esp_lcd_panel_io_handle_t panel_io,
                                       esp_lcd_panel_handle_t panel, int width, int height,
                                       bool mirror_x, bool mirror_y, int zone_top,
                                       EyeStyle style)
    : OledDisplay(panel_io, panel, width, height, mirror_x, mirror_y), style_(style) {
    // 参数化“可用区间顶部”：双色屏跳过顶部黄色行，单色屏用满整屏。
    zone_top_ = zone_top;
    eye_center_y_ = (zone_top_ + kBlueZoneBottom) / 2;
    eye_h_max_ = (kBlueZoneBottom - zone_top_) - kEyeZoneInset;
    ESP_LOGI(TAG, "眼睛界面区间 y=%d..%d，中心 y=%d，最大高度 %d，上抬 %dpx，眼型=%s",
             zone_top_, kBlueZoneBottom, eye_center_y_, eye_h_max_, kEyeVerticalLift,
             (style_ == EyeStyle::kCircle) ? "圆形(cam)" : "扁圆角矩形(0.96)");
}

DeepbotEyesDisplay::~DeepbotEyesDisplay() {
    DestroyEyes();
    // 眼睛对象由 LVGL 随屏幕一起释放，这里只停掉定时器，
    // 避免 tick 回调访问已销毁的对象。
}

void DeepbotEyesDisplay::SetupUI() {
    // 先让基类把默认 UI 建出来（保留完整实现，等于自带一套可回退的备份）
    OledDisplay::SetupUI();

    {
        DisplayLockGuard lock(this);

        if (container_ != nullptr) lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_ != nullptr) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        if (low_battery_popup_ != nullptr) lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

        // 把基类各容器的底色一并改成"熄灭"（双保险）：
        // 即使因某种原因 container_ 被重新显示，也不会再点亮整屏。
        // 注意用 PixelOff()（白色）而不是黑色——本板映射是反相的，详见文件顶部说明。
        lv_obj_t* kContainers[] = {container_, content_, content_left_, content_right_, top_bar_};
        for (lv_obj_t* obj : kContainers) {
            if (obj != nullptr) {
                lv_obj_set_style_bg_color(obj, PixelOff(), 0);
                lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
            }
        }

        ApplyBlackBackground();
        CreateEyes();

        // 初始显示模式：按当前设备状态决定。
        // 启动阶段（starting/connecting/activating 等）状态不应该显示眼睛，
        // 让基类的文字提示（检查更新、连接中…）先显示出来，
        // 等真正进入待机/听聆/说话后再切到眼睛。
        ApplyDisplayMode(ShouldShowEyes(Application::GetInstance().GetDeviceState()));
    }
    // 锁外调用：ShowBrandText 内部自己加 DisplayLockGuard，
    // lvgl_port 的锁不可重入，嵌在锁内会死锁。
    ShowBrandText();
}

void DeepbotEyesDisplay::ShowBrandText() {
    auto* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    if (lvgl_theme == nullptr || lvgl_theme->text_font() == nullptr) return;

    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) return;

    lv_obj_set_width(content_left_, width_);
    if (content_right_ != nullptr) {
        lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_font(emotion_label_, lvgl_theme->text_font()->font(), 0);
    lv_label_set_text(emotion_label_, "mini-bot");
    lv_obj_center(emotion_label_);
}

void DeepbotEyesDisplay::ApplyBlackBackground() {
    // “黑背景”在本板等于把像素熄灭，而熄灭对应白色——见文件顶部 PixelOn/PixelOff 说明。
    // screen 也要压一次：主题切回 light 时会把它重置为浅色，
    // 并配合 CreateEyes() 里的全屏遮罩盖住基类 container_。
    auto screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, PixelOff(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
}

void DeepbotEyesDisplay::SetTheme(Theme* theme) {
    // 基类只设置字体；主题切换（assets 加载完成后会调用一次）可能把屏幕底色重置，
    // 所以这里在基类处理后重新压一次黑底。
    OledDisplay::SetTheme(theme);  // 内部自己加锁
    {
        DisplayLockGuard lock(this);
        ApplyBlackBackground();
    }
    ShowBrandText();
}

void DeepbotEyesDisplay::CreateEyes() {
    // 调用方必须已持有 LVGL 锁
    if (eye_left_ != nullptr) return;  // 防止重复创建

    auto screen = lv_screen_active();

    // 1) 先铺一层全屏黑底遮罩（z 序高于基类 container_，低于眼睛）。
    // 单色屏上“黑”= 不点亮（blue <= 16），所以这一层会真正盖住基类的白底。
    bg_mask_ = lv_obj_create(screen);
    lv_obj_set_size(bg_mask_, width_, height_);
    lv_obj_set_pos(bg_mask_, 0, 0);
    lv_obj_set_style_bg_color(bg_mask_, PixelOff(), 0);
    lv_obj_set_style_bg_opa(bg_mask_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bg_mask_, 0, 0);
    lv_obj_set_style_outline_width(bg_mask_, 0, 0);
    lv_obj_set_style_radius(bg_mask_, 0, 0);
    lv_obj_set_style_pad_all(bg_mask_, 0, 0);
    lv_obj_clear_flag(bg_mask_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bg_mask_, LV_OBJ_FLAG_CLICKABLE);

    auto make_eye = [this, screen]() {
        lv_obj_t* eye = lv_obj_create(screen);
        lv_obj_set_size(eye, BaseWidthForState(), BaseHeightForState());
        // 背景透明：眼睛内容全部由 EyeDrawCb 自绘（圆角矩形 / 粗 ∪ 弧）。
        // 若保留默认背景填充，自绘的椭圆会被矩形底色盖住；
        // 而单边 border 又画不出弧度，这就是改为自绘的原因。
        lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_outline_width(eye, 0, 0);
        lv_obj_set_style_pad_all(eye, 0, 0);
        lv_obj_set_style_radius(eye, 0, 0);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE);
        // 内容自绘：LVGL 不提供「椭圆控件」，radius 会被裁到短边一半，
        // 所以挂在 DRAW_MAIN 上逐行画椭圆 / 用 draw_arc 画弧。
        lv_obj_add_event_cb(eye, EyeDrawCb, LV_EVENT_DRAW_MAIN, this);
        return eye;
    };

    eye_left_ = make_eye();
    eye_right_ = make_eye();

    // 新创建的对象默认可见，而启动阶段应该显示文字，所以先隐藏双眼；
    // 随后由 SetupUI 末尾的 ApplyDisplayMode() 根据真实设备状态决定是否显示。
    lv_obj_add_flag(eye_left_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(eye_right_, LV_OBJ_FLAG_HIDDEN);

    // 2) 左上角状态指示点（z 序最上，不会被眼睛遮挡）。
    // 用途：眼睛界面没有文字，靠这个小点告诉用户当前是“正在听”还是“正在说”。
    indicator_ = lv_obj_create(screen);
    lv_obj_set_size(indicator_, kIndicatorSize, kIndicatorSize);
    lv_obj_set_style_radius(indicator_, kIndicatorSize / 2, 0);  // 圆点
    lv_obj_set_style_bg_color(indicator_, PixelOn(), 0);         // 点亮
    lv_obj_set_style_bg_opa(indicator_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(indicator_, 0, 0);
    lv_obj_set_style_outline_width(indicator_, 0, 0);
    lv_obj_set_style_pad_all(indicator_, 0, 0);
    lv_obj_clear_flag(indicator_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(indicator_, LV_OBJ_FLAG_CLICKABLE);
    // 初始隐藏（同上：交由 ApplyDisplayMode 统一决定）
    lv_obj_add_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
    // 初始按当前状态摆放
    UpdateIndicator();

    // 初始几何：按当前风格取待机尺寸（未眨眼，宽=基准宽、高=基准高）
    ApplyEyeGeometry(BaseWidthForState(), BaseHeightForState(), 0);

    // 首次眨眼/视线漂移时间（用统一调度函数，保证与其他地方节奏一致）
    tick_ = 0;
    state_enter_tick_ = 0;
    blinking_ = false;
    blink_pct_ = 0;
    blink_dir_ = 1;
    pending_extra_blinks_ = 0;
    // 重置几何缓存，确保首次一定会真正绘制到屏幕上
    last_eye_h_ = -1;
    last_eye_offset_ = -999;
    ScheduleNextBlink();
    ScheduleNextLook();
    // 重置指示点闪烁相位：切状态时从“常亮”开始，避免带着上一状态的明暗相位（尤其隐藏时）
    indicator_tick_ = 0;
    indicator_on_ = true;
    // 定时器回调运行在 LVGL 任务上下文，已持有显示锁，回调内部不要再加锁
    anim_timer_ = lv_timer_create(DeepbotEyesDisplay::AnimTimerCallback, kTickMs, this);
    ESP_LOGI(TAG, "眼睛界面已启动（60ms tick，蓝色区 y=%d..%d）", zone_top_, kBlueZoneBottom);
}

void DeepbotEyesDisplay::DestroyEyes() {
    if (anim_timer_ != nullptr) {
        lv_timer_delete(anim_timer_);
        anim_timer_ = nullptr;
    }
    // 眼睛对象不主动删除，交由 LVGL 在屏幕销毁时回收，以免与正在进行的渲染冲突。
    eye_left_ = nullptr;
    eye_right_ = nullptr;
    indicator_ = nullptr;
}

void DeepbotEyesDisplay::AnimTimerCallback(lv_timer_t* timer) {
    auto* self = static_cast<DeepbotEyesDisplay*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnAnimTick();
    }
}

bool DeepbotEyesDisplay::ShouldShowEyes(DeviceState state) {
    // 采用**白名单**：只有真正“日常对话”的状态才显示眼睛。
    // 启动流程中的状态（starting / connecting / activating / upgrading 等）
    // 必须交回基类显示文字，让用户看到“正在检查更新”“正在连接”等提示，
    // 否则一上电就变成眼睛，完全不知道设备在做什么。
    switch (state) {
        case kDeviceStateIdle:
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            return true;
        default:
            // 其余全部显示文字：Unknown / Starting / WifiConfiguring / Connecting /
            // Upgrading / Activating / AudioTesting / FatalError
            return false;
    }
}

void DeepbotEyesDisplay::SyncStateFromDevice() {
    // 设备状态机是唯一可信来源：不能依赖 SetStatus 的文案，
    // 因为待机时 UpdateStatusBar 会用 "HH:MM" 时间字符串调用 SetStatus。
    EyeState next = EyeState::kStandby;
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateListening:
            next = EyeState::kListening;
            break;
        case kDeviceStateSpeaking:
            next = EyeState::kSpeaking;
            break;
        case kDeviceStateIdle:
        default:
            next = EyeState::kStandby;
            break;
    }

    if (next == state_) return;  // 状态未变：不要重置眨眼，否则会被每秒一次的
                                 // UpdateStatusBar（SetStatus 带时间字符串）反复打断

    state_ = next;
    state_enter_tick_ = tick_;
    // 状态真正变化：立即睁眼并重排节奏（各状态的眨眼频率不同）
    blink_pct_ = 0;
    blinking_ = false;
    blink_dir_ = 1;
    pending_extra_blinks_ = 0;
    ScheduleNextBlink();
    // 刚进入聆听时先回中睁大（对应“听到呼唤看过来”），之后再按 ScheduleNextLook 节奏小幅左右移动
    if (state_ == EyeState::kListening) {
        look_target_ = 0;
    }
    ScheduleNextLook();
    // 诊断日志：只在状态真变时输出一次，便于现场核对状态映射
    // （例如“呼暖后却显示为说话”时，看这里收到的 DeviceState 到底是多少）
    ESP_LOGI(TAG, "眼睛状态 -> %s (DeviceState=%d)",
             state_ == EyeState::kListening   ? "呼暖"
             : state_ == EyeState::kSpeaking ? "说话"
                                              : "待机",
             static_cast<int>(Application::GetInstance().GetDeviceState()));
    // 注意：这里不能直接调 UpdateIndicator()。SyncStateFromDevice() 会被 SetStatus()
    // 在未持锁的阶段调用，而 LVGL 不是线程安全的，无锁操作 UI 对象会损坏其内部状态，
    // 轻则渲染错乱（屏幕出现异常颜色/图标），重则影响持有音频事件的任务。
    // 因此 UI 刷新统一由调用方在持锁后执行。
}

void DeepbotEyesDisplay::UpdateIndicator() {
    if (indicator_ == nullptr) return;

    // 待机不显示（眼睛本身就是"空闲"的语义，不需要额外提示）
    if (state_ == EyeState::kStandby || !eyes_mode_) {
        lv_obj_add_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(indicator_, PixelOn(), 0);  // 点亮（颜色由所在行决定）

    // 用户需求：
    //   聆听 -> banner 左上角 y=4，黄色
    //   说话 -> 眼睛左侧 y=32，蓝色（原位置不变）
    const int x = (state_ == EyeState::kListening) ? kIndicatorXListening : kIndicatorXSpeaking;
    const int y = (state_ == EyeState::kListening) ? kIndicatorYListening : kIndicatorYSpeaking;

    // 位置缓存：只在变化时才设定，避免每 tick 重复 set_pos
    if (last_indicator_x_ != x || last_indicator_y_ != y) {
        last_indicator_x_ = x;
        last_indicator_y_ = y;
        lv_obj_set_pos(indicator_, x, y);
    }

    // 两种状态都常亮，不闪烁，靠颜色（y 所在色区）区分
    indicator_on_ = true;
    indicator_tick_ = 0;
    lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
}

void DeepbotEyesDisplay::TickIndicatorBlink() {
    // 现在两种状态都不闪烁，仅靠颜色（位置）区分，此函数保留以免后续改动。
    // 如需重新启用闪烁，只需在此判断对应状态并切换 indicator_on_。
}
void DeepbotEyesDisplay::ApplyDisplayMode(bool eyes_mode) {
    // 性能关键：SetStatus 在待机时会被 UpdateStatusBar 每秒调用一次
    // （传入 "HH:MM" 时间字符串）。若每次都重设可见性与指示点，
    // 会让 LVGL 任务额外做无意义的工作，表现为“聆听/说话反应变钝”。
    // 模式真正变化时才处理一次。
    if (eyes_mode == eyes_mode_) return;
    eyes_mode_ = eyes_mode;

    if (eyes_mode) {
        // 眼睛模式：收起所有文字控件，只留黑底遮罩与眼睛
        if (container_ != nullptr) lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_ != nullptr) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        if (low_battery_popup_ != nullptr) lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        if (bg_mask_ != nullptr) lv_obj_remove_flag(bg_mask_, LV_OBJ_FLAG_HIDDEN);
        if (eye_left_ != nullptr) lv_obj_remove_flag(eye_left_, LV_OBJ_FLAG_HIDDEN);
        if (eye_right_ != nullptr) lv_obj_remove_flag(eye_right_, LV_OBJ_FLAG_HIDDEN);
        // 指示点按当前状态决定显示/隐藏（注意 eyes_mode_ 已在上方赋值）
        UpdateIndicator();
    } else {
        // 文字模式（配网/注册）：收起眼睛与遮罩，恢复基类控件以显示必要提示
        if (eye_left_ != nullptr) lv_obj_add_flag(eye_left_, LV_OBJ_FLAG_HIDDEN);
        if (eye_right_ != nullptr) lv_obj_add_flag(eye_right_, LV_OBJ_FLAG_HIDDEN);
        if (indicator_ != nullptr) lv_obj_add_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
        if (bg_mask_ != nullptr) lv_obj_add_flag(bg_mask_, LV_OBJ_FLAG_HIDDEN);
        if (container_ != nullptr) lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_ != nullptr) lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

void DeepbotEyesDisplay::OnAnimTick() {
    if (eye_left_ == nullptr || eye_right_ == nullptr) return;

    ++tick_;

    // ================= 情绪过期检查 =================
    // 情绪是"短暂覆盖"：2 秒后自动失效，回到状态表情。
    // 在这里集中处理过期（而不是在渲染时判断），能保证 last_* 缓存
    // 与后续重绘一定同步，避免"看着像是卡在情绪里"。
    //
    // 过期后先看队列里还有没有待执行的（比如"先笑再难过"连续两个），
    // 有就接着显示下一个，每个都完整跑满 2 秒；没才回到状态表情。
    // 注意不能用 if/else：PopNextEmotion 会把 emotion_ 重新设为非空，
    // 所以先清空当前再尝试出队。
    if (emotion_ != EyeEmotion::kNone && tick_ >= emotion_until_tick_) {
        ESP_LOGI(TAG, "情绪到期: %s", EmotionName(emotion_));
        emotion_ = EyeEmotion::kNone;
        // 情绪与状态的高度可能恰好相同，强制失效缓存保证一定重绘
        last_eye_h_ = -1;
        last_eye_y_ = -999;
        // 下一项也播满原定时长；仅在工具队列真正耗尽时唤醒主循环。
        PopNextEmotion();
        if (emotion_ == EyeEmotion::kNone && style_ == EyeStyle::kCircle) {
            Application::GetInstance().NotifyToolActivityDone();
        }
    }

    // 后续动作直接接管表情；最后一个短动作结束后仍留足观看时间。
    if (head_emotion_ != EyeEmotion::kNone && !head_emotion_running_ && tick_ >= head_emotion_until_tick_) {
        head_emotion_ = EyeEmotion::kNone;
        last_eye_h_ = -1;
        last_eye_y_ = -999;
        Application::GetInstance().NotifyToolActivityDone();
    }

    // 每 tick 同步一次设备状态：这样即使某些状态切换没有走 SetStatus，
    // 眼睛表情也能跟上（例如唤醒后直接进入 listening）。
    // 定时器回调运行在 LVGL 任务中且已持锁，这里可直接操作 UI。
    SyncStateFromDevice();
    const bool want_eyes = ShouldShowEyes(Application::GetInstance().GetDeviceState());
    if (want_eyes != eyes_mode_) {
        ESP_LOGI(TAG, "显示模式切换: %s -> %s (state=%d)", eyes_mode_ ? "眼睛" : "文字",
                 want_eyes ? "眼睛" : "文字",
                 static_cast<int>(Application::GetInstance().GetDeviceState()));
        ApplyDisplayMode(want_eyes);
    }
    if (!want_eyes) {
        return;  // 文字模式下不推进眼睛动画
    }

    // 刷新状态指示点（状态可能在上一 tick 改变）
    UpdateIndicator();
    // 推进指示点闪烁：仅说话状态会实际闪烁，呼暖常亮
    TickIndicatorBlink();

    // ================= 眨眼：所有状态都会眨（这是"像人"的关键）=================
    // 之前聆听/说话时每 tick 强制 blink_pct_=0，导致完全不眨眼，看起来像死物。
    //
    // 情绪期间跳过眨眼：情绪本身就是通过改变眼睛高度来表达的
    // （如 angry 只有 14px、crying 只有 8~16px），再叠加眨眼会出现
    // "悲伤时突然把眼闭上"的怪异观感。情绪只持续 2 秒，不眨也不突兵。
    if (VisibleEmotion() == EyeEmotion::kNone) {
        if (!blinking_ && tick_ >= next_blink_tick_) {
            blinking_ = true;
            blink_dir_ = 1;
        }
    }
    if (blinking_) {
        if (blink_dir_ > 0) {
            blink_pct_ += kBlinkCloseStep;  // 闭合快
            if (blink_pct_ >= 100) {
                blink_pct_ = 100;
                blink_dir_ = -1;  // 到底后转为睁开
            }
        } else {
            blink_pct_ -= kBlinkOpenStep;  // 睁开慢
            if (blink_pct_ <= 0) {
                blink_pct_ = 0;
                blinking_ = false;
                blink_dir_ = 1;
                // 真人偶尔连眨两下：聆听等待时连眨概率更高，表现出催促/期待感
                const int double_blink_prob =
                    (state_ == EyeState::kListening) ? kDoubleBlinkListenPercent : kDoubleBlinkPercent;
                if (pending_extra_blinks_ > 0) {
                    --pending_extra_blinks_;
                    next_blink_tick_ =
                        tick_ + RandRange(kDoubleBlinkDelayMin, kDoubleBlinkDelayMax);
                } else if (static_cast<int>(esp_random() % 100) < double_blink_prob) {
                    pending_extra_blinks_ = 1;
                    next_blink_tick_ =
                        tick_ + RandRange(kDoubleBlinkDelayMin, kDoubleBlinkDelayMax);
                } else {
                    ScheduleNextBlink();
                }
            }
        }
    }

    const int ext_gaze = external_gaze_.load(std::memory_order_relaxed);
    if (ext_gaze != 0) {
        const int goal = (ext_gaze > 0) ? kExternalGazePx : -kExternalGazePx;
        // 用与自动漂移相同的速度（kLookStepStandby）靠近，保持眼神运动手感一致
        if (look_offset_ < goal) {
            look_offset_ = Clamp(look_offset_ + kLookStepStandby, look_offset_, goal);
        } else if (look_offset_ > goal) {
            look_offset_ = Clamp(look_offset_ - kLookStepStandby, goal, look_offset_);
        }
        // 目标同步成当前位置：退出外部控制时不会突然弹回旧目标
        look_target_ = look_offset_;
    } else if (VisibleEmotion() != EyeEmotion::kNone) {
        look_target_ = 0;
        look_offset_ = 0;
    } else if (state_ == EyeState::kStandby || state_ == EyeState::kListening) {
        const int max_off =
            (state_ == EyeState::kStandby) ? kLookMaxOffsetStandby : kLookMaxOffsetListen;
        if (tick_ >= next_look_tick_) {
            // 70% 概率主动看向左/右（幅度在 max_off/2 ~ max_off 之间），30% 概率回看中间附近
            if (static_cast<int>(esp_random() % 100) < 70) {
                const int mag = static_cast<int>(RandRange(max_off / 2, max_off));
                // 倾向于往相反方向看，避免原地小碎步
                const int dir = (look_offset_ > 2)    ? -1
                                : (look_offset_ < -2) ? 1
                                                      : ((esp_random() & 1) ? 1 : -1);
                look_target_ = dir * mag;
            } else {
                look_target_ = static_cast<int>(RandRange(0, 4)) - 2;
            }
            ScheduleNextLook();
        }
        // 先把目标限制在当前状态允许的最大幅度内（例如从待机切到聆听时自动收窄）
        look_target_ = Clamp(look_target_, -max_off, max_off);
        if (look_offset_ < look_target_) {
            look_offset_ = Clamp(look_offset_ + kLookStepStandby, -max_off, look_target_);
        } else if (look_offset_ > look_target_) {
            look_offset_ = Clamp(look_offset_ - kLookStepStandby, look_target_, max_off);
        }
    } else {
        // 说话：视线直接快速回正，不左右乱动
        look_target_ = 0;
        if (look_offset_ > 0) {
            look_offset_ = Clamp(look_offset_ - kLookStepStandby, 0, kLookMaxOffsetStandby);
        } else if (look_offset_ < 0) {
            look_offset_ = Clamp(look_offset_ + kLookStepStandby, -kLookMaxOffsetStandby, 0);
        }
    }

    // ================= 计算最终高度 =================
    //
    // 两道优先级：情绪 > 状态。
    // 情绪高度返回 -1 表示无情绪，此时才用状态高度。
    // 情绪期间不叠加眨眼（上面已跳过眨眼的推进，blink_pct_ 不会变化）。
    int h;
    // base_h  = 未眨眼的完整高度，眨眼压缩就是拿它算；
    // h       = 眨眼压缩后的实际绘制高度；
    // base_w  = 不受眨眼影响的宽度（扁圆角矩形恒为 40；圆形等于同状态直径）。
    //
    // 宽高分开是「上下合拢式眨眼」的关键：
    //   眨眼时只把高度压扁，宽度纹丝不动 →
    //   圆先变横向椭圆、再变成一条圆角横线，即上下眼皮合拢。
    //   若让宽度跟着高度一起缩（早期圆形版的做法），
    //   眼睛会圆着向中心收缩，看起来像镜头缩图而不是闭眼。
    int base_h = 0;
    int base_w = 0;
    int y_off = 0;
    // 形状、双眼间距、倾斜也由情绪决定；无情绪时保持实心眼型、不内收不倾斜。
    // 放在循环外最终算一次，与高度/偏移保持同一优先级：情绪 > 状态。
    EyeShape shape = EyeShape::kOval;
    int converge = 0;
    const int emo_h = EmotionHeight();
    if (emo_h >= 0) {
        h = emo_h;
        base_h = emo_h;
        // 情绪时宽度仍取状态基准宽（不跟随情绪高度），
        // 这样两种风格的宽度语义一致，圆形模式下情绪表现为横向压扁的椭圆。
        base_w = BaseWidthForState();
        y_off = EmotionYOffset();
        shape = EmotionShape();
        converge = EmotionConverge();
    } else {
        base_h = BaseHeightForState();
        base_w = BaseWidthForState();
        h = base_h;

        // 叠加眨眼：只压高度
        if (blink_pct_ > 0) {
            h = EYE_MIN_H + (base_h - EYE_MIN_H) * (100 - blink_pct_) / 100;
        }
    }

    ApplyEyeGeometry(base_w, h, look_offset_, y_off, shape, converge);
}

void DeepbotEyesDisplay::ScheduleNextBlink() {
    // 按当前状态选择眨眼节奏：聆听时高频眨眼表现着急等待，刚进入聆听时先等睁大回落后再开始快眨
    switch (state_) {
        case EyeState::kListening: {
            const uint32_t elapsed = tick_ - state_enter_tick_;
            const uint32_t min_wait =
                (elapsed < kListenWideHoldTicks + kListenShrinkTicks)
                    ? (kListenWideHoldTicks + kListenShrinkTicks - elapsed + 2)
                    : kBlinkListenMin;
            next_blink_tick_ = tick_ + RandRange(min_wait, min_wait + (kBlinkListenMax - kBlinkListenMin));
            break;
        }
        case EyeState::kSpeaking:
            next_blink_tick_ = tick_ + RandRange(kBlinkTalkMin, kBlinkTalkMax);
            break;
        case EyeState::kStandby:
        default:
            next_blink_tick_ = tick_ + RandRange(kBlinkStandbyMin, kBlinkStandbyMax);
            break;
    }
}

void DeepbotEyesDisplay::ScheduleNextLook() {
    if (state_ == EyeState::kListening) {
        next_look_tick_ = tick_ + RandRange(kLookListenMin, kLookListenMax);
    } else {
        next_look_tick_ = tick_ + RandRange(kLookStandbyMin, kLookStandbyMax);
    }
}

int DeepbotEyesDisplay::BaseHeightForState() const {
    // 两套尺寸表按风格取：只有数字不同，时间曲线与律动逻辑完全共用，
    // 这样两种眼型的动态手感（睁大保持、平滑回落、说话脉动）保持一致。
    const bool circle = (style_ == EyeStyle::kCircle);
    const int h_listen = circle ? EYE_D_LISTEN : EYE_H_LISTEN;
    const int h_standby = circle ? EYE_D_STANDBY : EYE_H_STANDBY;
    const int h_talk = circle ? EYE_D_TALK : EYE_H_TALK;

    switch (state_) {
        case EyeState::kListening: {
            // 刚进入聆听时先睁大约 0.42s，随后在 0.36s 内平滑回落到正常大小
            const uint32_t elapsed = tick_ - state_enter_tick_;
            if (elapsed <= kListenWideHoldTicks) {
                return h_listen;
            }
            const uint32_t shrink_pos = elapsed - kListenWideHoldTicks;
            if (shrink_pos < kListenShrinkTicks) {
                const int diff = h_listen - h_standby;
                return h_listen - static_cast<int>(diff * shrink_pos / kListenShrinkTicks);
            }
            return h_standby;
        }
        case EyeState::kSpeaking: {
            // 说话时微眯缩小（比待机小一档），叠加 ±2px 的轻柔音节起伏，
            // 像带着笑意认真讲话
            const uint32_t idx = ((tick_ - state_enter_tick_) / 2) & 0x7;
            return h_talk + kTalkPulseTable[idx];
        }
        case EyeState::kStandby:
        default:
            return h_standby;
    }
}

int DeepbotEyesDisplay::BaseWidthForState() const {
    // 扁圆角矩形：宽度恒定 EYE_W（40），只靠高度变化表达扁/张开。
    if (style_ == EyeStyle::kCircle) {
        // 正圆：宽必须等于同状态的直径，否则就不是圆而是椭圆。
        // 直接复用高度表，保证「待机 36 / 聆听 40 / 说话 32」三档一致。
        return BaseHeightForState();
    }
    return EYE_W;
}

// ================= 情绪实现 =================

const char* DeepbotEyesDisplay::EmotionName(EyeEmotion emotion) {
    switch (emotion) {
        case EyeEmotion::kHappy:    return "happy";
        case EyeEmotion::kSad:      return "sad";
        case EyeEmotion::kAngry:    return "angry";
        case EyeEmotion::kCrying:   return "crying";
        case EyeEmotion::kFocus:    return "focus";
        case EyeEmotion::kSurprised: return "surprised";
        case EyeEmotion::kNone:
        default:                    return "none";
    }
}

int DeepbotEyesDisplay::EmotionHeight() const {
    // 返回 -1 表示当前无情绪，调用方应回退到状态表情。
    //
    // 注意：情绪期间不使用 state_enter_tick_ 算相位，而是直接用全局 tick_。
    // 因为情绪是即时叠加的（可能在任何状态下被触发），用全局 tick 才能保证
    // 一开始就落在颤动周期上，不会因为 state_enter_tick_ 过大而算出异常相位。
    switch (VisibleEmotion()) {
        case EyeEmotion::kFocus:
            return 27;  // 略眯，保留随舵机转动的视线。
        case EyeEmotion::kSurprised:
            return 42;  // 瞪大的眼睛仍由屏幕可用高度限幅。
        case EyeEmotion::kSad:
            return kEmoSadH;
        case EyeEmotion::kAngry:
            return kEmoAngryH;
        case EyeEmotion::kHappy: {
            // 用三角波改变弧深，避免从最大值骤跳到最小值。
            const uint32_t step = tick_ / kEmoHappyStepTicks;
            const uint32_t phase = step & 0x3;              // 0..3
            const uint32_t tri = (phase < 2) ? phase : (4 - phase);  // 0,1,2,1
            const int span = kEmoHappyMaxH - kEmoHappyMinH;  // 2
            return kEmoHappyMinH + static_cast<int>(tri) * span / 2;
        }
        case EyeEmotion::kCrying: {
            // 哭泣：返回**固定框高**，框内上方是抽噎的眼睛椭圆，
            // 下方留给泪水下落。抽噎的呼吸感由 DrawEyeShape 里的
            // CryEyeHeight() 控制椭圆高，不再让整个框上下缩张
            // （框一变泪滴就会跟着弹，看起来不像在流泪）。
            return kEmoCryFrameH;
        }
        case EyeEmotion::kNone:
        default:
            return -1;
    }
}

int DeepbotEyesDisplay::EmotionYOffset() const {
    switch (VisibleEmotion()) {
        case EyeEmotion::kHappy:    return kEmoHappyY;
        case EyeEmotion::kSad:      return kEmoSadY;
        case EyeEmotion::kAngry:    return kEmoAngryY;
        case EyeEmotion::kCrying:   return kEmoCryY;
        case EyeEmotion::kNone:
        default:                    return 0;
    }
}

DeepbotEyesDisplay::EyeShape DeepbotEyesDisplay::EmotionShape() const {
    switch (VisibleEmotion()) {
        case EyeEmotion::kHappy:
            return EyeShape::kSmileArc;
        case EyeEmotion::kCrying:
        case EyeEmotion::kSad:
        case EyeEmotion::kAngry:
        case EyeEmotion::kNone:
        default:
            return EyeShape::kOval;
    }
}

int DeepbotEyesDisplay::EmotionConverge() const {
    // 双眼前后距离也是情绪语言的一部分：
    //   生气：向内收（正数）→ 「盯着你」的聚焦感，这是生气与单纯眯眼的关键区别
    //   悲伤：向外散（负数）→ 失落、注意力涣散，与生气正好相反
    // 两者方向相反，配合高度差与倾斜方向，三重对比确保一眼能分开。
    switch (VisibleEmotion()) {
        case EyeEmotion::kAngry:
            return kEmoAngryConverge;
        case EyeEmotion::kSad:
            return -kEmoSadDiverge;
        default:
            return 0;
    }
}

int DeepbotEyesDisplay::EmotionShear() const {
    return (VisibleEmotion() == EyeEmotion::kSad) ? kEmoSadShear : 0;
}

void DeepbotEyesDisplay::ApplyEyeGeometry(int base_width, int height, int x_offset,
                                          int y_offset, EyeShape shape, int converge) {
    if (eye_left_ == nullptr || eye_right_ == nullptr) return;

    const int h = Clamp(height, EYE_MIN_H, eye_h_max_);

    // 圆形眨眼时保留基准直径作为宽度，只压缩高度。
    if (style_ == EyeStyle::kCircle) {
        const int w = (shape == EyeShape::kSmileArc)
                          ? EYE_SMILE_W
                          : Clamp(base_width, EYE_MIN_H, eye_h_max_);
        ApplyEyeGeometryWithWidth(w, h, x_offset, y_offset, shape, converge);
        return;
    }

    // 圆角矩形保持固定宽度。
    const int w = (shape == EyeShape::kSmileArc) ? EYE_SMILE_W : EYE_W;

    const int shear = EmotionShear();
    if (h == last_eye_h_ && w == last_eye_w_ && x_offset == last_eye_offset_ &&
        y_offset == last_eye_y_ &&
        shape == last_eye_shape_ && converge == last_eye_converge_ &&
        shear == last_eye_shear_) {
        return;
    }
    last_eye_h_ = h;
    last_eye_w_ = w;
    last_eye_offset_ = x_offset;
    last_eye_y_ = y_offset;
    last_eye_shape_ = shape;
    last_eye_converge_ = converge;
    last_eye_shear_ = shear;

    int y = eye_center_y_ - h / 2 + y_offset - kEyeVerticalLift;
    y = Clamp(y, zone_top_, kBlueZoneBottom - h);
    const int left_x = EYE_LEFT_CX - w / 2 + x_offset + converge;
    const int right_x = EYE_RIGHT_CX - w / 2 + x_offset - converge;

    lv_obj_set_size(eye_left_, w, h);
    lv_obj_set_pos(eye_left_, left_x, y);
    lv_obj_set_size(eye_right_, w, h);
    lv_obj_set_pos(eye_right_, right_x, y);

    // 尺寸没变但形状变了时必须手动失效（否则不会重绘）：
    // 半径 = h 的弧宽正好 2h，当 h = EYE_W/2 时（如 h=20）椭圆与弧线宽高相等，
    // set_size 不会触发失效，笑眼会“卡”在上一个形状上。
    lv_obj_invalidate(eye_left_);
    lv_obj_invalidate(eye_right_);
    // 内容全部由 EyeDrawCb 自绘（圆角矩形 / 粗 ∪ 弧），
    // 所以这里不再设置 radius / bg / border 等样式。
}

void DeepbotEyesDisplay::ApplyEyeGeometryWithWidth(int w, int h, int x_offset, int y_offset,
                                                   EyeShape shape, int converge) {
    const int shear = EmotionShear();
    if (h == last_eye_h_ && w == last_eye_w_ && x_offset == last_eye_offset_ &&
        y_offset == last_eye_y_ &&
        shape == last_eye_shape_ && converge == last_eye_converge_ &&
        shear == last_eye_shear_) {
        return;
    }
    last_eye_h_ = h;
    last_eye_w_ = w;
    last_eye_offset_ = x_offset;
    last_eye_y_ = y_offset;
    last_eye_shape_ = shape;
    last_eye_converge_ = converge;
    last_eye_shear_ = shear;

    int y = eye_center_y_ - h / 2 + y_offset - kEyeVerticalLift;
    y = Clamp(y, zone_top_, kBlueZoneBottom - h);
    const int left_x = EYE_LEFT_CX - w / 2 + x_offset + converge;
    const int right_x = EYE_RIGHT_CX - w / 2 + x_offset - converge;

    lv_obj_set_size(eye_left_, w, h);
    lv_obj_set_pos(eye_left_, left_x, y);
    lv_obj_set_size(eye_right_, w, h);
    lv_obj_set_pos(eye_right_, right_x, y);

    lv_obj_invalidate(eye_left_);
    lv_obj_invalidate(eye_right_);
}

void DeepbotEyesDisplay::EyeDrawCb(lv_event_t* e) {
    auto* self = static_cast<DeepbotEyesDisplay*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    lv_obj_t* obj = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (obj == nullptr || layer == nullptr) return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    // 形状存在成员里而不是 per-object：两只眼睛永远同步变形，
    // 存一份比给每个对象摹传 user_data 简单，也不会不同步。
    // mirror 告诉绘制层「这是右眼」：哭眼的泪滴要画在眼睛外侧，
    // 左右两眼必须镜像对称，否则两只眼都往同一边流泪会很怪。
    const bool mirror = (obj == self->eye_right_);
    self->DrawEyeShape(layer, coords, self->last_eye_shape_, mirror);
}

void DeepbotEyesDisplay::DrawEyeShape(lv_layer_t* layer, const lv_area_t& c,
                                      EyeShape shape, bool mirror) {
    const int w = lv_area_get_width(&c);
    const int h = lv_area_get_height(&c);
    if (w <= 0 || h <= 0) return;

    // 哭眼：框内上方是抽噎的实心椭圆，下方是一滴下落的泪。
    // 单独处理而不走下面的 kOval 分支，因为框高（含泪水区）
    // 不等于眼睛高，椭圆需要自己算高度并靠框顶摆放。
    if (VisibleEmotion() == EyeEmotion::kCrying) {
        const int eye_h = Clamp(CryEyeHeight(tick_), 1, h);
        const int half = w / 2;

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = PixelOn();
        dsc.opa = LV_OPA_COVER;
        dsc.width = 1;

        for (int row = 0; row < eye_h; ++row) {
            const int r = (style_ == EyeStyle::kCircle)
                              ? eye_geometry::CircleHalfWidth(w, eye_h, row)
                              : eye_geometry::EyeHalfWidth(w, eye_h, row);
            if (r <= 0) continue;
            dsc.p1.x = c.x1 + half - r;
            dsc.p1.y = c.y1 + row;
            dsc.p2.x = c.x1 + half + r - 1;
            dsc.p2.y = c.y1 + row;
            lv_draw_line(layer, &dsc);
        }

        // 2) 泪滴：从眼眶下缘开始向下滴落，落出框底后循环重来。
        // 左眼靠左、右眼靠右（镜像），两滴泪左右对称。
        const int tear_x = mirror ? (w - kEmoCryTearInset - kEmoCryTearW)
                                  : kEmoCryTearInset;
        const int tear_top = eye_h + 1;                       // 眼眶下缘留 1px 缝
        const int fall_span = h - tear_top - kEmoCryTearH;    // 可下落的最大距离
        int drop = 0;
        if (fall_span > 0) {
            // 比可下落距离多 4 档空档：泪水落出屏幕后稍停，再从眼眶重新渗出，
            // 不空档会变成连续往下流的棍子，看着不像泪。
            const uint32_t cycle = static_cast<uint32_t>(fall_span) + 4;
            const uint32_t pos = (tick_ / kEmoCryTearStepTicks) % cycle;
            if (pos > static_cast<uint32_t>(fall_span)) {
                return;  // 本轮泪水已落出框底，空档期不画
            }
            drop = static_cast<int>(pos);
        }

        const int tear_y = tear_top + drop;
        const int tear_h = (tear_y + kEmoCryTearH > h) ? (h - tear_y) : kEmoCryTearH;
        const int tear_half_max = kEmoCryTearW / 2;
        for (int row = 0; row < tear_h; ++row) {
            const int r = eye_geometry::TearHalfWidth(row, kEmoCryTearH, tear_half_max);
            if (r <= 0) continue;
            // 泪滴中心水平位置（镜像时在框内另一侧）
            const int center_x = tear_x + tear_half_max;
            const int lx = center_x - r;
            const int rx2 = center_x + r - 1;
            if (lx < 0 || rx2 >= w) continue;
            dsc.p1.x = c.x1 + lx;
            dsc.p1.y = c.y1 + tear_y + row;
            dsc.p2.x = c.x1 + rx2;
            dsc.p2.y = c.y1 + tear_y + row;
            lv_draw_line(layer, &dsc);
        }
        return;
    }

    if (shape == EyeShape::kOval) {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = PixelOn();  // 本板颜色反相，点亮像素用 PixelOn()
        dsc.opa = LV_OPA_COVER;
        dsc.width = 1;

        for (int row = 0; row < h; ++row) {
            const int half = (style_ == EyeStyle::kCircle)
                                 ? eye_geometry::CircleHalfWidth(w, h, row)
                                 : eye_geometry::EyeHalfWidth(w, h, row);
            if (half <= 0) continue;
            const int x0 = w / 2 - half;
            const int x1 = w / 2 + half - 1;
            if (x1 < x0) continue;

            dsc.p1.x = c.x1 + x0;
            dsc.p1.y = c.y1 + row;
            dsc.p2.x = c.x1 + x1;
            dsc.p2.y = c.y1 + row;
            lv_draw_line(layer, &dsc);
        }
        return;
    }

    if (VisibleEmotion() == EyeEmotion::kSad) {
        const int shear = EmotionShear();

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = PixelOn();
        dsc.opa = LV_OPA_COVER;
        dsc.width = 1;

        // 圆角半径与主眼型一致（高度×0.35），悲伤也是一颗倾斜的
        // 扁圆角矩形，不退回椭圆——否则只有悲伤一眼看得出不是同一个角色。
        const int radius = eye_geometry::EyeCornerRadius(w, h);
        for (int col = 0; col < w; ++col) {
            int top = 0;
            int bottom = 0;
            // mirror 保证左右两眼的外眼角各自向外垂，而不是同向斜。
            if (!eye_geometry::TiltedRoundedRectSpan(w, h, col, shear, mirror,
                                                     radius, &top, &bottom)) {
                continue;
            }
            dsc.p1.x = c.x1 + col;
            dsc.p1.y = c.y1 + top;
            dsc.p2.x = c.x1 + col;
            dsc.p2.y = c.y1 + bottom;
            lv_draw_line(layer, &dsc);
        }
        return;
    }

    // 按框尺寸求圆头弧的半径，使弧带始终贴合边界。
    if (shape == EyeShape::kSmileArc) {
        // 线宽 = h * 0.35（四舍五入）。
        // 注：必须随 h 等比缩放，否则颤动时只有弧高变、线宽不变，
        // 粗细比例会忽胖忽瘦，看起来像在“呼吸”而不是在笑。
        int thick = (h * kSmileThickPerMille + 500) / 1000;
        // 在 12–14 px 的笑眼框内固定线宽，动画只改变弧深。
        if (thick < 6) thick = 6;
        // 防御：线宽不能吞掉整个框，否则连空脸都没了，会变成一块黑块。
        // 留至少 3px 给弧的空脸（ry = h - thick >= 3）。
        if (thick > h - 3) thick = h - 3;
        const float rx = eye_geometry::SmileArcRx(w, thick);
        const float ry = eye_geometry::SmileArcRy(h, thick);

        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = PixelOn();
        dsc.opa = LV_OPA_COVER;
        dsc.width = 1;

        // 中心线采样点只算一次：happy 每 tick 颤动重绘是热路径，
        // 若每列都重算 cos/sin，40 列会重复 40 倍三角运算（实测约 12880 次/帧）。
        float xs[eye_geometry::kArcSamples + 1];
        float ys[eye_geometry::kArcSamples + 1];
        const int n = eye_geometry::ArcCenterline(rx, ry, w, h, thick, true, xs, ys,
                                                  eye_geometry::kArcSamples + 1);
        for (int col = 0; col < w; ++col) {
            int top = 0;
            int bottom = 0;
            if (!eye_geometry::ArcSpanFromPoints(xs, ys, n, h, thick, col, &top, &bottom)) {
                continue;
            }
            dsc.p1.x = c.x1 + col;
            dsc.p1.y = c.y1 + top;
            dsc.p2.x = c.x1 + col;
            dsc.p2.y = c.y1 + bottom;
            lv_draw_line(layer, &dsc);
        }
        return;
    }
}

void DeepbotEyesDisplay::SetStatus(const char* status) {
    // 关键修复：不按字符串判断状态。
    // 待机时 LvglDisplay::UpdateStatusBar 会用当前时间（如 "16:05"）调用本函数，
    // 若按 strcmp(status, Lang::Strings::STANDBY) 判断就会失败，
    // 从而落到"非眼睛"分支把 container_ / status_bar_ 恢复出来，
    // 屏幕于是显示默认 UI（小人 + 顶部 WiFi/时间 banner）。
    // 改为读取设备状态机，来源唯一且可靠。
    SyncStateFromDevice();
    const DeviceState dev_state = Application::GetInstance().GetDeviceState();
    const bool eyes_mode = ShouldShowEyes(dev_state);

    // 先让基类更新状态标签（内部会加锁解锁），保持框架状态一致。
    // 注意不能把它放在 DisplayLockGuard 作用域里：lvgl_port 的锁不可重入，会死锁。
    if (status != nullptr) {
        OledDisplay::SetStatus(status);
    }

    DisplayLockGuard lock(this);
    ApplyDisplayMode(eyes_mode);
    // 在持锁状态下刷新指示点（SyncStateFromDevice 已更新 state_）。
    // 之前把这一步放在 SyncStateFromDevice 内部，会在 SetStatus 的未持锁阶段
    // 操作 LVGL 对象，线程不安全，可能导致渲染错乱与任务异常。
    UpdateIndicator();
    // 眨眼/视线的重置已由 SyncStateFromDevice() 在状态真正变化时完成，
    // 这里不再强制 blink_pct_=0，以免每秒一次的 UpdateStatusBar 打断眨眼。
}

void DeepbotEyesDisplay::SetChatMessage(const char* role, const char* content) {
    // 眼睛界面下不显示聊天文字：完全忽略，也不改动基类的 content_right_ 等控件。
    (void)role;
    (void)content;
}

void DeepbotEyesDisplay::ClearPendingActions() {
    // cam 已接收的表情是工具效果，新一轮只能接续，不能丢掉还在排队的动作。
    if (style_ == EyeStyle::kCircle) return;
    // 只丢“还没轮到”的情绪，不动当前正在显示的那个。
    //
    // 为何不连当前一起清：
    //   调用时机是“新一轮回答开始播报”（收到 tts start）。此时屏幕上
    //   可能还在显示上一轮的情绪，保留它直到自然到期（最多 2 秒），
    //   比“一开播就立刻变脸”自然；而且新一轮的工具调用会发新情绪，
    //   自然接替。这里要解决的只是“排队太长导致越拖越久”的错位。
    DisplayLockGuard lock(this);
    if (emotion_queue_count_ > 0) {
        ESP_LOGI(TAG, "新一轮开始，丢弃 %d 个待播情绪", emotion_queue_count_);
        emotion_queue_head_ = 0;
        emotion_queue_count_ = 0;
    }
}

// 视线跟随头部转向：+1 往右看，-1 往左看，0 回自动漂移。
//
// 为何不在本函数里直接操作 LVGL 对象：
//   它会被传感器任务（20ms tick）调用，而传感器任务不持 LVGL 锁。
//   在非 LVGL 任务里直接调 lv_obj_* 会与 LVGL 任务竞争，导致花屏或崩溃。
//   这里只用原子变量发布方向，真正的几何刷写统一在 OnAnimTick（已持锁）里做。
void DeepbotEyesDisplay::SetGazeDirection(int dir) {
    const int normalized = (dir > 0) ? 1 : (dir < 0) ? -1 : 0;
    // 加日志方便实机排查：只在方向真正变化时打，避免 20ms 一次刷屏。
    const int previous = external_gaze_.exchange(normalized, std::memory_order_relaxed);
    if (normalized != previous) {
        ESP_LOGI(TAG, "外部视线: %s", normalized > 0 ? "往右看"
                                     : normalized < 0 ? "往左看"
                                                      : "恢复自动漂移");
    }
}

bool DeepbotEyesDisplay::PopNextEmotion() {
    if (emotion_queue_count_ <= 0) {
        return false;
    }
    emotion_ = emotion_queue_[emotion_queue_head_];
    emotion_queue_head_ = (emotion_queue_head_ + 1) % kEmotionQueueSize;
    --emotion_queue_count_;
    // 每个出队的情绪都重新计满 2 秒，不继承前一个的剩余时间
    emotion_until_tick_ = tick_ + kEmotionDurationTicks;
    // 强制失效缓存：连续两个情绪可能高度相同（如 sad 与 crying 的某些帧），
    // 不失效就会出现“轮到下一个了但屏幕没变化”。
    last_eye_h_ = -1;
    last_eye_y_ = -999;
    ESP_LOGI(TAG, "情绪出队执行: %s（剩余待执行 %d）", EmotionName(emotion_),
             emotion_queue_count_);
    return true;
}

void DeepbotEyesDisplay::SetConversationNeutral() {
    // cam 的对话状态与工具动画独立：切到待机或聆听只改底层眼型。
    // 保留其它眼型原有的状态重置行为；显式 SetEmotion("neutral") 仍可清除。
    if (style_ != EyeStyle::kCircle) Display::SetConversationNeutral();
}

void DeepbotEyesDisplay::SetTouchSmile(bool enabled) {
    DisplayLockGuard lock(this);
    if (touch_smile_ == enabled) return;
    touch_smile_ = enabled;
    // 工具表情优先；触摸只在没有工具表情时覆盖默认状态眼型。
    if (emotion_ == EyeEmotion::kNone) {
        last_eye_h_ = -1;
        last_eye_y_ = -999;
    }
}

void DeepbotEyesDisplay::StartHeadActionEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    // 独立于触摸与通用通知表情，不排第二条与动作不同步的队列。
    if (emotion != nullptr && strcmp(emotion, "focus") == 0) {
        head_emotion_ = EyeEmotion::kFocus;
    } else if (emotion != nullptr && strcmp(emotion, "surprised") == 0) {
        head_emotion_ = EyeEmotion::kSurprised;
    } else {
        head_emotion_ = ParseEmotion(emotion);
    }
    head_emotion_running_ = head_emotion_ != EyeEmotion::kNone;
    head_emotion_until_tick_ = tick_ + kEmotionDurationTicks;
    last_eye_h_ = -1;
    last_eye_y_ = -999;
}

void DeepbotEyesDisplay::FinishHeadActionEmotion() {
    DisplayLockGuard lock(this);
    // 长动作覆盖完整轨迹；最后一个短动作保留到原表情时长再结束。
    head_emotion_running_ = false;
}

DeepbotEyesDisplay::EyeEmotion DeepbotEyesDisplay::ParseEmotion(const char* emotion) {
    if (emotion == nullptr) return EyeEmotion::kNone;
    // 兼容旧消息的 laughing，与 happy 共用绘制逻辑。
    if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0) return EyeEmotion::kHappy;
    if (strcmp(emotion, "sad") == 0) return EyeEmotion::kSad;
    if (strcmp(emotion, "angry") == 0) return EyeEmotion::kAngry;
    if (strcmp(emotion, "crying") == 0) return EyeEmotion::kCrying;
    return EyeEmotion::kNone;
}

bool DeepbotEyesDisplay::HasPendingToolEmotion() {
    DisplayLockGuard lock(this);
    // 显示初始化失败时没有动画完成通知，不能因此把语音一直留在收尾状态。
    return anim_timer_ != nullptr && eye_left_ != nullptr && eye_right_ != nullptr &&
           (head_emotion_ != EyeEmotion::kNone || emotion_ != EyeEmotion::kNone || emotion_queue_count_ > 0);
}

void DeepbotEyesDisplay::SetEmotion(const char* emotion) {
    // 把服务端 / MCP 下发的情绪名转成内部枚举。
    //
    // 设计要点：
    // 1) 支持 4 种：sad / angry / happy / crying
    // 2) 旧词 "laughing" 仍接受，但降级为 happy（见下方分支的说明）
    // 3) 其它值（包括 "neutral"、空指针、不认识的词）一律当作“清除情绪”，
    //    回退到状态表情。这样服务端发 neutral 时能自然恢复正常。
    // 4) 情绪只维持 2 秒（kEmotionDurationTicks = 34 tick），到期由 OnAnimTick 自动清除。
    //    时长常量统一定义在头文件，此处只引用不重复写数。
    //
    // 线程安全：本函数可能从 MCP 工具线程或服务端消息线程调用，
    // 而情绪字段会被 LVGL 任务里的 OnAnimTick 读取，所以必须持锁。
    const auto parsed = ParseEmotion(emotion);

    DisplayLockGuard lock(this);

    // 保留显式 neutral 的兼容语义；cam 的对话状态重置不走这个分支。
    if (parsed == EyeEmotion::kNone) {
        if (emotion_queue_count_ > 0) {
            ESP_LOGI(TAG,
                     "忽略 neutral：队列还有 %d 个表情待播（不在状态切换时打断表情串）",
                     emotion_queue_count_);
            return;
        }
        // 走到这里说明队列必为空（>0 的情况上面已 return），
        // 所以只需停掉当前情绪，不需要清队列。
        const bool was_active = (emotion_ != EyeEmotion::kNone);
        emotion_ = EyeEmotion::kNone;
        emotion_until_tick_ = tick_;
        last_eye_h_ = -1;
        last_eye_y_ = -999;
        ESP_LOGI(TAG, "SetEmotion: %s -> 清除情绪（原状态%s）",
                 emotion == nullptr ? "(null)" : emotion,
                 was_active ? "活跃" : "空闲");
        if (was_active && style_ == EyeStyle::kCircle) Application::GetInstance().NotifyToolActivityDone();
        return;
    }

    // ---- 情况 2：当前无活跃情绪 -> 立即执行 ----
    const bool idle = (emotion_ == EyeEmotion::kNone || tick_ >= emotion_until_tick_);
    if (idle) {
        emotion_ = parsed;
        emotion_until_tick_ = tick_ + kEmotionDurationTicks;
        last_eye_h_ = -1;
        last_eye_offset_ = -999;
        last_eye_y_ = -999;
        ESP_LOGI(TAG, "SetEmotion: %s -> 立即执行 %s (%u ticks)",
                 emotion == nullptr ? "(null)" : emotion, EmotionName(parsed),
                 static_cast<unsigned>(kEmotionDurationTicks));
        return;
    }

    // ---- 情况 3：已有情绪在显示 -> 入队，等当前这个完整跑完 ----
    //
    // 这样“先笑再难过”两个动作都能完整各显示 2 秒，
    // 不会因为相隔 200ms 导致第一个几乎看不见。
    if (emotion_queue_count_ >= kEmotionQueueSize) {
        ESP_LOGW(TAG, "情绪队列已满(%d)，丢弃: %s", kEmotionQueueSize, EmotionName(parsed));
        return;
    }
    const int tail = (emotion_queue_head_ + emotion_queue_count_) % kEmotionQueueSize;
    emotion_queue_[tail] = parsed;
    ++emotion_queue_count_;
    const uint32_t remaining = (tick_ < emotion_until_tick_)
                                   ? (emotion_until_tick_ - tick_)
                                   : 0;
    ESP_LOGI(TAG, "SetEmotion: %s -> 排队等待（当前 %s 还剩 %u tick，队列 %d）",
             emotion == nullptr ? "(null)" : emotion, EmotionName(emotion_),
             static_cast<unsigned>(remaining), emotion_queue_count_);
}
