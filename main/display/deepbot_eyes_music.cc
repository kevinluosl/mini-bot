#include "deepbot_eyes_display.h"

#include <algorithm>

namespace {

constexpr int kNoteWidth = 27;
constexpr int kNoteHeight = 27;
constexpr int kEyeWidth = 28;
constexpr int kEyeHeight = 42;
constexpr int kBaselineOffset = 3;

// 60 ms 一帧：抬起、回落、轻压后收稳。右眼晚 4 帧，双音符沿用原来的节奏。
constexpr int kBounce[] = {0, -2, -5, -8, -9, -8, -6, -3, 0, 2, 2, 1, 0, 0, 0, 0};
constexpr uint32_t kPeriod = sizeof(kBounce) / sizeof(kBounce[0]);
constexpr uint32_t kRightDelay = 4;

int NoteOffset(uint32_t elapsed, bool right_eye) {
    const uint32_t phase = (elapsed % kPeriod + (right_eye ? kPeriod - kRightDelay : 0)) % kPeriod;
    return kBounce[phase];
}

struct Span {
    int left;
    int right;
};

// 一对连梁八分音符（♫）：上方斜梁连接两根符干，两个圆润的符头错开六像素。
// 保留原来的 27 像素高度，横向填满眼框；中间的留白使单色屏也能辨认两个音符。
constexpr Span kNote[kNoteHeight][2] = {
    {{23, 26}}, {{20, 26}}, {{17, 26}}, {{14, 26}}, {{11, 26}}, {{8, 26}},
    {{8, 26}}, {{8, 22}, {23, 26}}, {{8, 19}, {23, 26}}, {{8, 16}, {23, 26}},
    {{8, 13}, {23, 26}}, {{8, 11}, {23, 26}}, {{8, 11}, {23, 26}}, {{8, 11}, {23, 26}},
    {{8, 11}, {20, 26}}, {{8, 11}, {17, 26}}, {{8, 11}, {16, 26}}, {{8, 11}, {15, 25}},
    {{8, 11}, {15, 24}}, {{8, 11}, {16, 22}}, {{5, 11}, {18, 20}},
    {{2, 11}}, {{1, 11}}, {{0, 10}}, {{0, 9}}, {{1, 7}}, {{3, 5}},
};

}  // namespace

void DeepbotEyesDisplay::SetMusicPlayback(bool active) {
    DisplayLockGuard lock(this);
    if (music_playback_ == active) return;
    music_playback_ = active;
    if (active) music_enter_tick_ = tick_;
    // 音符是独立覆盖层，退出时让底层表情重绘，不改普通动作或计时。
    last_eye_h_ = -1;
}

void DeepbotEyesDisplay::AnimateMusicEyes() {
    ApplyEyeGeometryWithWidth(kEyeWidth, std::min(kEyeHeight, eye_h_max_), 0, 0, EyeShape::kMusicNote, 0);
    // 对象的位置和大小不动，只重绘位移变化的一只眼，避免刷新整屏。
    const uint32_t elapsed = tick_ - music_enter_tick_;
    if (elapsed > 0) {
        if (NoteOffset(elapsed, false) != NoteOffset(elapsed - 1, false)) lv_obj_invalidate(eye_left_);
        if (NoteOffset(elapsed, true) != NoteOffset(elapsed - 1, true)) lv_obj_invalidate(eye_right_);
    }
}

void DeepbotEyesDisplay::DrawMusicNote(lv_layer_t* layer, const lv_area_t& coords, bool right_eye) {
    const int w = lv_area_get_width(&coords);
    const int h = lv_area_get_height(&coords);
    if (w < kNoteWidth || h < kNoteHeight) return;
    const int x = coords.x1 + (w - kNoteWidth) / 2;
    // 分配更多向上的空间；小视口宁可缩短跳幅，也不裁掉符头或符尾。
    const int room = h - kNoteHeight;
    const int y = coords.y1 + std::clamp(room / 2 + kBaselineOffset +
        NoteOffset(tick_ - music_enter_tick_, right_eye), 0, room);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = lv_color_black();  // 与主眼型一致，OLED 驱动反相后点亮。
    line.opa = LV_OPA_COVER;
    line.width = 1;
    for (int row = 0; row < kNoteHeight; ++row) {
        for (const auto& span : kNote[row]) {
            if (span.left == span.right) continue;  // 零初始化的空段。
            line.p1.x = x + span.left;
            line.p1.y = y + row;
            line.p2.x = x + span.right;
            line.p2.y = y + row;
            lv_draw_line(layer, &line);
        }
    }
}
