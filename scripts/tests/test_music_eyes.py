"""采样真实音符绘制函数；宿主侧替身只记录 LVGL 交给 OLED 的线段。"""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

DISPLAY = Path(__file__).resolve().parents[2] / 'main/display'

LVGL_STUB = r'''
#pragma once
#include <cassert>
#include <cstdint>
#include <set>
#include <utility>

struct lv_area_t { int x1, y1, x2, y2; };
struct lv_layer_t {
    lv_area_t clip;
    std::set<std::pair<int, int>> pixels;
};
struct lv_draw_line_dsc_t {
    struct { int x, y; } p1, p2;
    int color, opa, width;
};
constexpr int LV_OPA_COVER = 255;
inline int lv_color_black() { return 0; }
inline int lv_area_get_width(const lv_area_t* a) { return a->x2 - a->x1 + 1; }
inline int lv_area_get_height(const lv_area_t* a) { return a->y2 - a->y1 + 1; }
inline void lv_draw_line_dsc_init(lv_draw_line_dsc_t* line) { *line = {}; }
inline void lv_draw_line(lv_layer_t* l, const lv_draw_line_dsc_t* d) {
    assert(d->p1.y == d->p2.y && d->width == 1 && d->color == lv_color_black());
    assert(l->clip.y1 <= d->p1.y && d->p1.y <= l->clip.y2);
    assert(l->clip.x1 <= d->p1.x && d->p1.x < d->p2.x && d->p2.x <= l->clip.x2);
    for (int x = d->p1.x; x <= d->p2.x; ++x) l->pixels.emplace(x, d->p1.y);
}
inline void lv_obj_invalidate(int* count) { ++*count; }

class DeepbotEyesDisplay;
struct DisplayLockGuard { explicit DisplayLockGuard(DeepbotEyesDisplay*) {} };

class DeepbotEyesDisplay {
public:
    enum class EyeShape { kMusicNote };
    bool music_playback_ = false;
    uint32_t music_enter_tick_ = 0, tick_ = 0;
    int eye_h_max_ = 56, last_eye_h_ = 0;
    int left_dirty = 0, right_dirty = 0;
    int* eye_left_ = &left_dirty;
    int* eye_right_ = &right_dirty;
    void ApplyEyeGeometryWithWidth(int w, int h, int x, int y, EyeShape shape, int converge) {
        assert(w > 0 && h <= eye_h_max_ && x == 0 && y == 0 && converge == 0);
        assert(shape == EyeShape::kMusicNote);
    }
    void SetMusicPlayback(bool active);
    void AnimateMusicEyes();
    void DrawMusicNote(lv_layer_t*, const lv_area_t&, bool);
};
'''

DRIVER = r'''
#include "deepbot_eyes_display.h"
#include <algorithm>
#include <vector>

using Pixels = std::set<std::pair<int, int>>;

static Pixels Render(DeepbotEyesDisplay& display, lv_area_t clip, bool right) {
    lv_layer_t layer{clip, {}};
    display.DrawMusicNote(&layer, clip, right);
    return layer.pixels;
}

static int Top(const Pixels& pixels) {
    assert(!pixels.empty());
    return std::min_element(pixels.begin(), pixels.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; })->second;
}

static Pixels Normalize(const Pixels& pixels) {
    Pixels normalized;
    const int top = Top(pixels);
    for (const auto& [x, y] : pixels) normalized.emplace(x, y - top);
    return normalized;
}

int main() {
    DeepbotEyesDisplay display;
    const lv_area_t clip{12, 8, 39, 49};
    display.tick_ = 123;
    display.SetMusicPlayback(true);
    const auto baseline = Render(display, clip, false);
    const auto note = Normalize(baseline);
    // 连梁横贯两侧；下方保留两根符干、两个分开的符头和中缝。
    for (int x = 20; x <= 38; ++x) assert(note.count({x, 6}));
    assert(note.count({21, 13}) && note.count({36, 13}) && !note.count({28, 13}));
    assert(!note.count({17, 16}) && note.count({31, 16}) && !note.count({26, 16}));
    assert(note.count({17, 22}) && !note.count({31, 22}));
    std::vector<int> left, right;
    for (int frame = 0; frame < 32; ++frame) {
        display.tick_ = 123 + frame;
        display.SetMusicPlayback(true);  // 重复通知不能把动画重置为静止。
        display.AnimateMusicEyes();
        const auto l = Render(display, clip, false);
        const auto r = Render(display, clip, true);
        assert(Normalize(l) == note && Normalize(r) == note);
        left.push_back(Top(l));
        right.push_back(Top(r));
        assert(Normalize(Render(display, {12, 8, 39, 37}, true)) == note);
    }
    // 换轮廓不换动画：原有跳幅、落地回弹、周期与右眼延迟全部保持。
    constexpr int offsets[] = {0, -2, -5, -8, -9, -8, -6, -3, 0, 2, 2, 1, 0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
        assert(left[i] == left[0] + offsets[i]);
        assert(left[i] == left[i + 16]);
        assert(left[i] == right[i + 4]);
    }
    assert(display.left_dirty > 0 && display.right_dirty > 0);
    display.SetMusicPlayback(false);
    assert(display.last_eye_h_ == -1 && !display.music_playback_);
    display.tick_ += 77;
    display.SetMusicPlayback(true);
    assert(Render(display, clip, false) == baseline);
}
'''


class MusicEyesTests(unittest.TestCase):
    def test_beamed_notes_keep_shape_and_original_bounce(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / 'deepbot_eyes_display.h').write_text(LVGL_STUB)
            (work / 'deepbot_eyes_music.cc').write_bytes((DISPLAY / 'deepbot_eyes_music.cc').read_bytes())
            (work / 'driver.cc').write_text(DRIVER)
            subprocess.run([
                os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                'deepbot_eyes_music.cc', 'driver.cc', '-o', 'music_eyes_test',
            ], cwd=work, check=True)
            subprocess.run([str(work / 'music_eyes_test')], cwd=work, check=True)
