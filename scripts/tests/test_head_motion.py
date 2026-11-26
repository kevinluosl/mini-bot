"""cam 板头部动作引擎宿主机单测：直接编译 deepbot_head_motion.cc，不依赖 ESP-IDF。

覆盖：动作清单与关键帧一一对应、两档角度和速度、限位与中心挪移、
表情动作回原朝向、姿态改写静止朝向、排队/满队/stop、按速度逐帧推进。
"""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / 'main/boards/deepbot-s3-cam-n16r8'

DRIVER = r'''
#include "deepbot_head_motion.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace deepbot_head;

// 与 config.h 一致：1 号 ±60，2 号抬头 60、低头 40。
static const Limits kLimits{30, 150, 30, 130};

// 播放到结束，返回总耗时（毫秒），并检查每一拍都在限位内、单拍位移不超过最快速度。
static int RunToEnd(HeadMotionPlayer& p, int& pan, int& tilt, int max_ms = 20000) {
    int elapsed = 0;
    while (elapsed < max_ms) {
        const int prev_pan = pan, prev_tilt = tilt;
        if (!p.Tick(20, pan, tilt)) return elapsed;
        assert(pan >= kLimits.pan_min && pan <= kLimits.pan_max);
        assert(tilt >= kLimits.tilt_min && tilt <= kLimits.tilt_max);
        // 最快档 250°/s × 20ms = 5°，四舍五入最多再多 1°。
        assert(std::abs(pan - prev_pan) <= 6 && std::abs(tilt - prev_tilt) <= 6);
        elapsed += 20;
    }
    std::fprintf(stderr, "动作超时未结束\n");
    std::abort();
}

int main() {
    // 1) 清单里每个动作（除 stop）都能展开，表情动作最后一帧回到起点，且不超 3 秒。
    for (auto name : ActionNames()) {
        if (name == "stop") continue;
        for (int lv = 1; lv <= 2; ++lv) {
            Motion m;
            assert(BuildMotion(name, lv, 90, 90, kLimits, m));
            assert(!m.frames.empty() && m.emotion && m.emotion[0]);
            if (m.return_home) {
                assert(m.frames.back().pan == 90 && m.frames.back().tilt == 90);
            }
            HeadMotionPlayer p(kLimits);
            assert(p.Enqueue(name, lv).empty());
            int pan = 90, tilt = 90;
            const int ms = RunToEnd(p, pan, tilt);
            if (std::getenv("HEAD_MOTION_DUMP")) {
                std::printf("%-16s L%d %5dms -> %d,%d\n", std::string(name).c_str(), lv, ms, pan, tilt);
            }
            assert(ms <= 3000);
        }
    }
    assert(!IsValidAction("spin") && IsValidAction("nod") && IsValidAction("stop"));

    // 音乐伴奏走相同轨迹，但不发起短表情；之后的普通动作仍照常发起。
    {
        HeadMotionPlayer p(kLimits);
        int pan = 90, tilt = 90;
        const char* emotion = nullptr;
        assert(p.Enqueue("nod", 1, false).empty());
        assert(p.Enqueue("shake", 1).empty());
        assert(p.Tick(20, pan, tilt, &emotion) && emotion == nullptr);
        while (p.ActiveName() == "nod") {
            assert(p.Tick(20, pan, tilt, &emotion) && emotion == nullptr);
        }
        assert(p.Tick(20, pan, tilt, &emotion) && emotion != nullptr);
    }

    // 2) 左右看两档幅度加大，俯仰角度和斜向动作的共用速度不变。
    const int pan_deg[] = {40, 55}, up_deg[] = {35, 50}, down_deg[] = {30, 38};
    const int pan_speed[] = {125, 150};
    for (int lv = 1; lv <= 2; ++lv) {
        Motion m;
        assert(BuildMotion("look_left", lv, 90, 70, kLimits, m) &&
               m.frames[0].pan == 90 - pan_deg[lv - 1] && m.frames[0].tilt == 70 &&
               m.frames[0].speed == pan_speed[lv - 1]);
        assert(BuildMotion("look_right", lv, 90, 70, kLimits, m) &&
               m.frames[0].pan == 90 + pan_deg[lv - 1] && m.frames[0].tilt == 70 &&
               m.frames[0].speed == pan_speed[lv - 1]);
        assert(BuildMotion("look_up", lv, 90, 90, kLimits, m) &&
               m.frames[0].tilt == 90 - up_deg[lv - 1] && m.frames[0].speed == 100);
        assert(BuildMotion("look_down", lv, 90, 90, kLimits, m) &&
               m.frames[0].tilt == 90 + down_deg[lv - 1] && m.frames[0].speed == 100);
        assert(BuildMotion("look_up_left", lv, 90, 90, kLimits, m) &&
               m.frames[0].pan == 90 - pan_deg[lv - 1] && m.frames[0].tilt == 90 - up_deg[lv - 1] &&
               m.frames[0].speed == 100);
    }
    // 越界强度收敛到 1..2：旧的 3 当作使劲，不报错。
    {
        Motion m;
        assert(BuildMotion("look_left", 3, 90, 90, kLimits, m) && m.frames[0].pan == 35 && m.level == 2);
        assert(BuildMotion("look_left", 9, 90, 90, kLimits, m) && m.frames[0].pan == 35 && m.level == 2);
        assert(BuildMotion("look_left", 0, 90, 90, kLimits, m) && m.frames[0].pan == 50 && m.level == 1);
    }

    // 3) 单轴姿态只改自己那一轴：先抬头再看左 = 左上。
    {
        HeadMotionPlayer p(kLimits);
        int pan = 90, tilt = 90;
        assert(p.Enqueue("look_up", 1).empty() && p.Enqueue("look_left", 1).empty());
        RunToEnd(p, pan, tilt);
        assert(pan == 50 && tilt == 55 && p.RestPan() == 50 && p.RestTilt() == 55);
        // look_center 回到正中。
        assert(p.Enqueue("look_center", 2).empty());
        RunToEnd(p, pan, tilt);
        assert(pan == 90 && tilt == 90 && p.RestPan() == 90 && p.RestTilt() == 90);
    }

    // 4) 表情动作以当前朝向为中心，做完回到原朝向，不改静止朝向。
    {
        HeadMotionPlayer p(kLimits);
        int pan = 90, tilt = 90;
        assert(p.Enqueue("look_right", 1).empty() && p.Enqueue("nod", 2).empty());
        int min_tilt = 999, max_tilt = -999;
        int elapsed = 0;
        while (p.Tick(20, pan, tilt)) {
            if (p.ActiveName() == "nod") {
                min_tilt = std::min(min_tilt, tilt);
                max_tilt = std::max(max_tilt, tilt);
                assert(pan == 130);  // 点头时横向保持在右边
            }
            elapsed += 20;
            assert(elapsed < 10000);
        }
        assert(max_tilt - min_tilt == 30);  // 使劲点头幅度 30°
        assert(pan == 130 && tilt == 90 && p.RestPan() == 130 && p.RestTilt() == 90);
    }

    // 5) 靠近限位：中心整体往回挪，幅度做完整、不越界；结束回到原朝向。
    {
        Motion m;
        // 向右看 2 档（145）再摇头幅度 34：中心挪到 116，动作完成后仍回到 145。
        assert(BuildMotion("shake", 2, 145, 90, kLimits, m));
        int lo = 999, hi = -999;
        for (size_t i = 0; i + 1 < m.frames.size(); ++i) {
            lo = std::min(lo, m.frames[i].pan);
            hi = std::max(hi, m.frames[i].pan);
        }
        assert(hi == 150 && lo == 82 && hi - lo == 68);
        assert(m.frames.back().pan == 145);
        // 低头到底再点头：低头行程只到 130，中心上挪，幅度仍是 30°。
        assert(BuildMotion("nod", 2, 90, 130, kLimits, m));
        int tlo = 999, thi = -999;
        for (size_t i = 0; i + 1 < m.frames.size(); ++i) {
            tlo = std::min(tlo, m.frames[i].tilt);
            thi = std::max(thi, m.frames[i].tilt);
        }
        assert(thi == 130 && thi - tlo == 30 && m.frames.back().tilt == 130);
        // 新幅度下所有动作从正中出发都不顶俯仰限位：最低头不超 128（40° 限位留 2°）。
        for (auto name : ActionNames()) {
            if (name == "stop") continue;
            assert(BuildMotion(name, 2, 90, 90, kLimits, m));
            for (const auto& k : m.frames) assert(k.tilt <= 128 && k.tilt >= 40);
        }
    }

    // 6) 排队：最多 4 个，第 5 个被拒；未知动作被拒；stop 清空并回中位。
    {
        HeadMotionPlayer p(kLimits);
        for (int i = 0; i < 4; ++i) assert(p.Enqueue("nod", 1).empty());
        assert(!p.Enqueue("shake", 1).empty());
        assert(!p.Enqueue("spin", 1).empty());
        int pan = 90, tilt = 90;
        assert(p.Enqueue("look_left", 2).empty() == false);  // 队列仍满
        p.Tick(20, pan, tilt);
        assert(p.Busy());
        assert(p.Enqueue("stop", 2).empty());
        assert(p.Busy() && p.RestPan() == 90 && p.RestTilt() == 90);
        RunToEnd(p, pan, tilt);
        assert(!p.Busy() && pan == 90 && tilt == 90);
    }

    // 7) 从中位看右，两档分别约 320/367ms，允许 20ms 节拍量化。
    for (int lv = 1; lv <= 2; ++lv) {
        HeadMotionPlayer p(kLimits);
        int pan = 90, tilt = 90;
        assert(p.Enqueue("look_right", lv).empty());
        const int ms = RunToEnd(p, pan, tilt);
        const int expected_ms = 1000 * pan_deg[lv - 1] / pan_speed[lv - 1];
        assert(pan == 90 + pan_deg[lv - 1] && ms >= expected_ms && ms <= expected_ms + 20);
    }
    // 传感器让实际俯仰偏离静止朝向时，回正仍不超过原来的 100°/s。
    {
        HeadMotionPlayer p(kLimits);
        int pan = 90, tilt = 30;
        assert(p.Enqueue("look_right", 2).empty());
        assert(p.Tick(20, pan, tilt) && tilt == 32);
        RunToEnd(p, pan, tilt);
        assert(pan == 145 && tilt == 90);
    }
    // 左右摆动、偏头均扩大幅度，但动作仍受限位约束，并能从侧边归回。
    {
        struct Sweep { const char* name; int pan1, pan2, tilt1, tilt2; };
        const Sweep sweeps[] = {{"shake", -24, -34, 0, 0}, {"look_around", -35, -48, -12, -18},
                                {"dance", -23, -27, -8, -8}, {"curious", 18, 24, -16, -22},
                                {"think", -23, -30, -20, -28}, {"shy", 18, 24, 25, 32},
                                {"listen", 18, 24, 10, 14}};
        for (const auto& s : sweeps) {
            for (int lv = 1; lv <= 2; ++lv) {
                Motion m;
                assert(BuildMotion(s.name, lv, 90, 90, kLimits, m));
                assert(m.frames.front().pan == 90 + (lv == 1 ? s.pan1 : s.pan2));
                assert(m.frames.front().tilt == 90 + (lv == 1 ? s.tilt1 : s.tilt2));
                for (int start_pan : {30, 150}) {
                    for (int start_tilt : {30, 130}) {
                        HeadMotionPlayer p(kLimits);
                        int pan = start_pan, tilt = start_tilt;
                        assert(p.Enqueue(s.name, lv).empty());
                        assert(RunToEnd(p, pan, tilt) <= 3000);
                        assert(pan == start_pan && tilt == start_tilt);
                    }
                }
            }
        }
    }
    // 张望不仅俯仰抬高，还有两站高度差，之后恢复原朝向。
    for (int level : {1, 2}) {
        Motion m;
        assert(BuildMotion("look_around", level, 90, 90, kLimits, m));
        assert(m.frames.size() == 3);
        assert(m.frames[0].tilt == (level == 1 ? 78 : 72));
        assert(m.frames[1].tilt == (level == 1 ? 84 : 81));
        assert(m.frames.back().tilt == 90);
    }
    // 每个动作仅在实际播放时发布一次表情，连续两次同名也各有一个开始事件。
    {
        HeadMotionPlayer p(kLimits);
        assert(p.Enqueue("happy", 2).empty());
        assert(p.Enqueue("crying", 2).empty());
        assert(p.Enqueue("crying", 1).empty());
        int pan = 90, tilt = 90;
        const char* emotion = nullptr;
        std::vector<std::string_view> started;
        for (int i = 0; i < 450 && p.Tick(20, pan, tilt, &emotion); ++i) {
            if (emotion) started.emplace_back(emotion);
        }
        assert(!p.Busy() && emotion == nullptr);
        assert((started == std::vector<std::string_view>{"happy", "crying", "crying"}));
        assert(p.Enqueue("angry", 2).empty());
        assert(p.Tick(20, pan, tilt, &emotion) && std::string_view(emotion) == "angry");
        assert(p.Enqueue("stop", 1).empty());
        assert(p.Tick(20, pan, tilt, &emotion) && std::string_view(emotion) == "neutral");
        RunToEnd(p, pan, tilt);
        assert(!p.Busy() && pan == 90 && tilt == 90);
    }
    // 8) 低速每拍不足 1° 也能走到（浮点位置累计，不被整数截断卡住）。
    {
        HeadMotionPlayer p(kLimits);
        int pan = 90, tilt = 90;
        assert(p.Enqueue("think", 1).empty());
        int elapsed = 0, prev = 90;
        bool moved = false;
        while (p.Tick(5, pan, tilt)) {  // 5ms 一拍 × 50°/s = 0.25°/拍
            if (pan != prev) moved = true;
            prev = pan;
            elapsed += 5;
            assert(elapsed < 10000);
        }
        assert(moved && pan == 90 && tilt == 90);
    }
    std::puts("OK");
    return 0;
}
'''


class HeadMotionTests(unittest.TestCase):
    def test_engine(self):
        with tempfile.TemporaryDirectory() as tmp:
            driver = Path(tmp) / 'driver.cc'
            driver.write_text(DRIVER)
            exe = Path(tmp) / 'head_motion'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I', str(BOARD), str(driver), str(BOARD / 'deepbot_head_motion.cc'),
                            '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertIn('OK', result.stdout)


if __name__ == '__main__':
    unittest.main()
