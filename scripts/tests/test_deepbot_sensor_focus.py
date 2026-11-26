"""Exercise the exact MCU sensor arbitration on the host."""
import subprocess
import tempfile
import unittest
from pathlib import Path

BOARD = Path(__file__).resolve().parents[2] / 'main/boards/deepbot-s3-cam-n16r8'


class SensorFocusTests(unittest.TestCase):
    def test_first_edge_wins_and_blocked_inputs_are_not_replayed(self):
        program = r'''
#include <cassert>
#include "deepbot_sensor_focus.h"

int main() {
    using Input = DeepbotSensorFocus::Input;
    DeepbotSensorFocus focus;
    const auto tick = [&](bool head, bool left, bool right, Input expected, bool started) {
        const auto result = focus.Tick(head, left, right);
        assert(result.active == expected && result.started == started);
    };

    focus.Reset(true, false, false);
    tick(true, false, false, Input::kNone, false);  // 上电电平不是边沿
    tick(false, false, false, Input::kNone, false);
    tick(false, true, false, Input::kLeft, true);
    tick(true, true, true, Input::kLeft, false);  // 右脸和摸头都不能抢左脸
    tick(true, false, true, Input::kNone, false); // 左脸释放，不接手仍按住的两路
    tick(false, false, true, Input::kNone, false);
    tick(true, false, true, Input::kHead, true);  // 重新摸头时可抢到空闲位
    tick(true, true, true, Input::kHead, false);  // 摸头先到，红外也不抢
    tick(false, true, true, Input::kNone, false);
    tick(false, false, false, Input::kNone, false);
    tick(true, true, true, Input::kRight, true);  // 同拍三路：右红外优先
    tick(true, true, false, Input::kNone, false);
    tick(false, false, false, Input::kNone, false);
    tick(true, true, false, Input::kLeft, true);  // 同拍左红外与头：左优先
    tick(false, true, false, Input::kLeft, false);
    tick(false, false, true, Input::kRight, true); // 释放当拍出现新边沿，立即接管
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'sensor_focus.cc', Path(tmp) / 'sensor_focus'
            src.write_text(program)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(BOARD),
                            str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
