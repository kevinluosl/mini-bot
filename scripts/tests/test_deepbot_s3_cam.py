import json
import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main/boards/deepbot-s3-cam-n16r8"
SOURCE = BOARD / "deepbot_s3_cam_board.cc"
HEADER = BOARD / "config.h"
# 眼睛 UI 实现在本工程的 display 组件里，也要做括号和语法检查。
EYES_SOURCE = ROOT / "main/display/deepbot_eyes_display.cc"
EYES_HEADER = ROOT / "main/display/deepbot_eyes_display.h"


class DeepbotS3CamBoardTests(unittest.TestCase):
    def test_audio_defaults_keep_wake_sound_and_device_aec(self):
        """重建配置后仍播放本地唤醒提示音，并保留设备端 AEC。"""
        defaults = (ROOT / "sdkconfig.defaults.esp32s3").read_text(encoding="utf-8").splitlines()
        self.assertIn("CONFIG_SEND_WAKE_WORD_DATA=n", defaults)
        self.assertIn("CONFIG_USE_DEVICE_AEC=y", defaults)
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        block = kconfig.split("config USE_DEVICE_AEC", 1)[1].split("config ", 1)[0]
        self.assertIn("depends on USE_AUDIO_PROCESSOR", block)
        self.assertIn("default y", block)

    def test_board_is_registered_once(self):
        config = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(config["target"], "esp32s3")
        self.assertEqual(config["type"], "deepbot-s3-cam-n16r8")
        self.assertEqual(config["builds"][0]["name"], "deepbot-s3-cam-n16r8")

        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        self.assertEqual(cmake.count('set(BOARD_DIR "deepbot-s3-cam-n16r8")'), 1)
        self.assertEqual(set(re.findall(r"^\s*config (BOARD_TYPE_\w+)", kconfig, re.M)),
                         {"BOARD_TYPE_DEEPBOT_S3_CAM_N16R8"})
        self.assertEqual({p.name for p in BOARD.parent.iterdir() if p.is_dir()},
                         {"common", BOARD.name})
        self.assertEqual(kconfig.count("config BOARD_TYPE_DEEPBOT_S3_CAM_N16R8"), 1)
        # 共享眼睛组件必须登记进构建源列表，否则板级链接时找不到实现
        self.assertIn('"display/deepbot_eyes_display.cc"', cmake)

    def test_wiring_matches_confirmed_camera_and_display(self):
        header = HEADER.read_text(encoding="utf-8")
        # 摄像头沿用已验证的 DVP 并口接线，一个都不能改：改错会整帧花屏或全黑。
        expected_camera = {
            "CAMERA_PIN_D0": "GPIO_NUM_11",
            "CAMERA_PIN_D1": "GPIO_NUM_9",
            "CAMERA_PIN_D2": "GPIO_NUM_8",
            "CAMERA_PIN_D3": "GPIO_NUM_10",
            "CAMERA_PIN_D4": "GPIO_NUM_12",
            "CAMERA_PIN_D5": "GPIO_NUM_18",
            "CAMERA_PIN_D6": "GPIO_NUM_17",
            "CAMERA_PIN_D7": "GPIO_NUM_16",
            "CAMERA_PIN_XCLK": "GPIO_NUM_15",
            "CAMERA_PIN_PCLK": "GPIO_NUM_13",
            "CAMERA_PIN_VSYNC": "GPIO_NUM_6",
            "CAMERA_PIN_HREF": "GPIO_NUM_7",
            "CAMERA_PIN_SIOD": "GPIO_NUM_4",
            "CAMERA_PIN_SIOC": "GPIO_NUM_5",
        }
        for name, gpio in expected_camera.items():
            self.assertIsNotNone(
                re.search(rf"#define\s+{name}\s+{gpio}\b", header),
                f"{name} 必须连接到 {gpio}",
            )

        # 屏幕已换成 1.3 寸 128x64 单色 I2C OLED（4 针：VCC/GND/SCL/SDA）。
        # 实测接线：SDA=GPIO21、SCL=GPIO47（复用原彩屏的 RST / DC 脚）。
        self.assertIsNotNone(re.search(r"#define\s+DISPLAY_SDA_PIN\s+GPIO_NUM_21\b", header))
        self.assertIsNotNone(re.search(r"#define\s+DISPLAY_SCL_PIN\s+GPIO_NUM_47\b", header))
        self.assertIsNotNone(re.search(r"#define\s+DISPLAY_WIDTH\s+128\b", header))
        self.assertIsNotNone(re.search(r"#define\s+DISPLAY_HEIGHT\s+64\b", header))

        # 旧 SPI 彩屏的引脚宏必须彻底消失：留着会被误认为还在用并口屏，
        # 也会让 7 线接线的历史误导后来者。
        for gone in (
            "DISPLAY_CLK_PIN",
            "DISPLAY_MOSI_PIN",
            "DISPLAY_MISO_PIN",
            "DISPLAY_DC_PIN",
            "DISPLAY_CS_PIN",
            "DISPLAY_RST_PIN",
            "DISPLAY_BACKLIGHT_PIN",
            "DISPLAY_SPI_HOST",
        ):
            self.assertNotIn(gone, header, f"{gone} 是旧 SPI 彩屏残留，应删除")

        # 摄像头与屏幕的引脚不能重叠（重叠会让两路外设互相干扰）
        camera_pins = re.findall(r"#define\s+CAMERA_PIN_\w+\s+(GPIO_NUM_\d+)", header)
        display_pins = re.findall(r"#define\s+DISPLAY_(?:SDA|SCL)_PIN\s+(GPIO_NUM_\d+)", header)
        self.assertEqual(len(display_pins), 2)
        self.assertEqual(len(set(camera_pins)), len(camera_pins), "摄像头引脚不能重复")
        self.assertEqual(len(set(display_pins)), len(display_pins), "屏幕引脚不能重复")
        self.assertFalse(set(camera_pins) & set(display_pins), "摄像头与屏幕引脚不能重叠")

        # 驱动必须是 SH1106：SSD1306 驱动用 0x20/0x21/0x22 设寻址，
        # 这三条 SH1106 不支持，会导致画面冻结不刷新（已实测）。
        self.assertIsNotNone(
            re.search(r"#define\s+DISPLAY_USE_SH1106\s+1\b", header),
            "本板是 SH1106，必须用 SH1106 驱动",
        )

    def test_servos_stay_idle_until_explicit_tool_or_serial_command(self):
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")

        # 38/39 是本板空闲 GPIO：不能和摄像头、OLED、绑带脚冲突。
        self.assertIn("#define SERVO_1_GPIO GPIO_NUM_38", header)
        self.assertIn("#define SERVO_2_GPIO GPIO_NUM_39", header)
        camera_pins = set(re.findall(r"#define\s+CAMERA_PIN_\w+\s+GPIO_NUM_(\d+)", header))
        display_pins = set(re.findall(r"#define\s+DISPLAY_(?:SDA|SCL)_PIN\s+GPIO_NUM_(\d+)", header))
        servo_pins = {"38", "39"}
        self.assertFalse(servo_pins & camera_pins, "舵机 GPIO 与摄像头引脚不能重叠")
        self.assertFalse(servo_pins & display_pins, "舵机 GPIO 与 OLED 引脚不能重叠")
        self.assertFalse(servo_pins & {"0", "3", "45", "46"}, "不能使用启动绑带脚")

        # 50Hz 周期 20ms，1500us 是常见舵机中位（约 90 度）。
        self.assertIn("#define SERVO_PWM_FREQ_HZ 50", header)
        self.assertIn("LEDC_TIMER_2", source)
        self.assertIn("LEDC_CHANNEL_4", source)
        self.assertIn("LEDC_CHANNEL_5", source)
        # 上电只能将 GPIO 设为高阻输入，不得配置 LEDC 定时器/通道或输出 PWM。
        self.assertIn("config.mode = GPIO_MODE_INPUT;", source)
        self.assertIn("InitializeServos();", source)
        init_start = source.index("void InitializeServos()")
        # 只取 InitializeServos 本身；再往后是传感器任务，它在运行时本就会驱动舵机。
        init_end = source.index("void InitializeSensors()", init_start)
        init_body = source[init_start:init_end]
        self.assertNotIn("ledc_", init_body)
        self.assertNotIn("SetBothServoAngles(", init_body)
        self.assertNotIn("ServoSelfTest", source)
        self.assertNotIn("ServoPositionDemo", source)
        # 上电自动动作相关开关已彻底移除，避免有人改回 1 又让舵机开机乱动。
        for gone in ("SERVO_SELF_TEST_ON_BOOT", "SERVO_POSITION_DEMO_ON_BOOT"):
            self.assertNotIn(gone, header)
            self.assertNotIn(gone, source)

        # 摄像头临时排查开关已移除，摄像头必须正常初始化。
        self.assertNotIn("CAMERA_DISABLE_FOR_SERVO_TEST", header)
        self.assertNotIn("CAMERA_DISABLE_FOR_SERVO_TEST", source)
        # esp_camera_init 已下沉到通用组件 OvCameraWatcher，
        # 板级代码只负责构造对象并提供相机参数。
        self.assertIn("OvCameraWatcher(camera_config, CONFIG_MINI_BOT_OV_BASE_URL)", source)
        kconfig = (ROOT / "main/Kconfig.projbuild").read_text(encoding="utf-8")
        self.assertRegex(kconfig, r'config MINI_BOT_OV_BASE_URL\n[^\n]+\n    default ""')
        self.assertIn("camera_ready_ = camera_->Initialize()", source)

        # PWM timer/channel 只在第一次需要转头时由 SetBothServoAngles 惰性初始化。
        self.assertIn("EnsureServoChannel(1, pulse1)", source)
        self.assertIn("EnsureServoChannel(2, pulse2)", source)
        self.assertIn("config.duty = duty;", source)
        self.assertIn("ledc_set_duty(kServoMode, channel, duty)", source)
        self.assertIn("ledc_update_duty(kServoMode, channel)", source)
        self.assertNotIn("ledc_set_duty_and_update", source)

        # 串口 UART0 REPL 只保留头部动作命令，与工具走同一入队路径。
        self.assertIn('.command = "head"', source)
        self.assertIn("self->EnqueueHeadAction(argv[1], level)", source)
        for command in ("esp_console_new_repl_uart", "esp_console_start_repl"):
            self.assertIn(command, source)
        # 旧的调试命令和 7 个旧工具已删除，只剩一个动作工具。
        for gone in ('.command = "servo"', '.command = "pwmt"', '.command = "servotest"',
                     "self.servo.", "SetServoPower", "SetServoTargetAngles", "MeasurePwmRepeated"):
            self.assertNotIn(gone, source)
        self.assertEqual(source.count('AddTool("self.head.act"'), 1)
        self.assertNotIn("self.motor.", source)
        # 脉宽范围必须是 1000~2000us。
        #
        self.assertIn("#define SERVO_MIN_PULSE_US 1000", header)
        self.assertIn("#define SERVO_MAX_PULSE_US 2000", header)
        self.assertIn("kServoMinPulseUs = SERVO_MIN_PULSE_US", source)
        self.assertIn("kServoMaxPulseUs = SERVO_MAX_PULSE_US", source)

        # 加强断言：脉宽跨度不得超过 1000us。
        # 跨度越大，同样的逻辑角度范围要求的物理行程越大，越容易顶死。
        # 这条能在有人"为了让摆动更明显"重新放宽脉宽时立刻拦住。
        import re as _re
        _min = int(_re.search(r"#define SERVO_MIN_PULSE_US (\d+)", header).group(1))
        _max = int(_re.search(r"#define SERVO_MAX_PULSE_US (\d+)", header).group(1))
        self.assertLessEqual(
            _max - _min, 1000,
            f"脉宽跨度 {_max - _min}us 超过 1000us，逻辑 90° 会要求超出行程的物理角度，"
            f"两端会顶死堵转（这正是哒哒声的成因）")
        # 安全区间：两端都应落在 1000~2000us 这个 SG90 通用范围内
        self.assertGreaterEqual(_min, 1000, "下限低于 1000us 可能超出机械行程")
        self.assertLessEqual(_max, 2000, "上限高于 2000us 可能超出机械行程")

        # 自检工具已删除，构造路径也不得触发任何动作。
        ctor = source[source.index("DeepbotS3CamBoard()") : source.index("AudioCodec* GetAudioCodec()")]
        self.assertNotIn("EnqueueHeadAction(", ctor)
        self.assertNotIn("ServoPositionDemo", source)

        # 引脚探测绝不能驱动输出电平。
        self.assertNotIn("gpio_set_level", source)
        self.assertIn("GPIO_MODE_INPUT", source)

        # 通道初始 duty 必须直接使用显式请求的目标值，不能在启动时输出中位或 duty=0。
        self.assertIn("config.duty = duty;", source)
        self.assertNotIn("config.duty = kHomeDuty;", source)
        self.assertNotIn("config.duty = 0;", source)

        # duty 仅在显式动作后写入，必须 set + update 两步。

        # 14 位分辨率下 1500us home duty 约为 1228 ticks。
        duty = (1500 * ((1 << 14) - 1)) // 20000
        self.assertEqual(duty, 1228)

    def test_servo_safety_limit_and_direction_invert(self):
        """舵机安全限位与 2 号舵机方向反转。

        背景：
        - 2 号舵机物理安装方向与程序假定相反（发“向下”却抬头），
          所以必须在脉宽层做镜像，而上层逻辑角度语义保持不变。
        - 机械结构限制横向/俯仰行程，超过会堵转发热，
          必须在唯一的写 PWM 入口 SetBothServoAngles 里统一收敛，不能依赖每个调用方自觉。
        """
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")

        # 中位 90 度
        self.assertIn("#define SERVO_CENTER_ANGLE 90", header)
        self.assertIn("#define SERVO_1_MAX_OFFSET_DEG 60", header)

        #
        self.assertIn("#define SERVO_2_MAX_OFFSET_UP_DEG", header)
        self.assertIn("#define SERVO_2_MAX_OFFSET_DOWN_DEG", header)

        # 分轴+分方向限位必须真正作用于代码（动作引擎的 Limits 也按同一组常量构造）
        self.assertIn("SERVO_CENTER_ANGLE - SERVO_2_MAX_OFFSET_UP_DEG, SERVO_CENTER_ANGLE + "
                      "SERVO_2_MAX_OFFSET_DOWN_DEG}}", source)

        import re as _re
        _up = int(_re.search(r"#define SERVO_2_MAX_OFFSET_UP_DEG (\d+)", header).group(1))
        _down = int(_re.search(r"#define SERVO_2_MAX_OFFSET_DOWN_DEG (\d+)", header).group(1))
        self.assertLess(_down, _up,
                        f"低头 {_down}° 应小于抬头 {_up}°：低头实测 45° 即顶死，"
                        f"抬头是客户明确要求的 60°；两者相等会把抬头误限制，"
                        f"表现为「抬头转不到位」")

        # ClampForAxis 必须对 2 号用**不对称**的上下限，
        # 不能简单地 中位±offset（那正是导致抬头被压到 40° 的写法）
        clamp_start = source.index("static int ClampForAxis(int servo_id, int angle)")
        clamp_end = source.index("bool SetBothServoAngles(", clamp_start)
        clamp_body = source[clamp_start:clamp_end]
        self.assertIn("SERVO_CENTER_ANGLE - SERVO_2_MAX_OFFSET_UP_DEG", clamp_body)
        self.assertIn("SERVO_CENTER_ANGLE + SERVO_2_MAX_OFFSET_DOWN_DEG", clamp_body)

        # 限位必须写在唯一的写 PWM 入口 SetBothServoAngles 内部
        body_start = source.index("bool SetBothServoAngles(")
        body_end = source.index("void StepServoBothTo(", body_start)
        body = source[body_start:body_end]
        self.assertIn("ClampForAxis", source)
        self.assertIn("angle1 = ClampForAxis(1, angle1);", body)
        self.assertIn("angle2 = ClampForAxis(2, angle2);", body)
        # 只剩一个写 PWM 的入口：单轴旧入口已删除。
        self.assertNotIn("bool SetServoAngle(", source)

        # 2 号舵机方向反转：脉宽镜像，只作用于 2 号
        self.assertIn("#define SERVO_2_INVERT 1", header)
        self.assertIn("if (SERVO_2_INVERT)", body)
        self.assertIn("pulse2 = kServoMinPulseUs + kServoMaxPulseUs - pulse2;", body)

        # 镜像必须发生在脉宽计算之后（顺序错了就变成改逻辑角度）
        self.assertLess(body.index("uint32_t pulse2 = to_pulse(angle2);"),
                        body.index("pulse2 = kServoMinPulseUs + kServoMaxPulseUs - pulse2;"))

    def test_touch_and_infrared_sensor_wiring(self):
        """触摸与红外传感器引脚、电平极性、反应参数。"""
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")

        # 三个引脚按实际接线：触摸 14，右红外 46，左红外 3
        self.assertRegex(header, r"#define\s+TOUCH_GPIO\s+GPIO_NUM_14\b")
        self.assertRegex(header, r"#define\s+INFRARED_RIGHT_GPIO\s+GPIO_NUM_46\b")
        self.assertRegex(header, r"#define\s+INFRARED_LEFT_GPIO\s+GPIO_NUM_3\b")

        # GPIO45 是 VDD_SPI 绑带脚，绝对不能用，防止有人后续改成 45
        self.assertNotIn("GPIO_NUM_45", header)

        # 极性可配，且默认按低电平有效
        self.assertIn("#define INFRARED_ACTIVE_LOW 1", header)

        # 传感器引脚不得与摄像头/OLED/音频/舵机冲突
        all_pins = re.findall(r"#define\s+\w*GPIO\w*\s+GPIO_NUM_(\d+)", header)
        sensor_pins = {14, 46, 3}
        others = [int(p) for p in all_pins]
        for pin in sensor_pins:
            # 每个传感器引脚在头文件里只应出现一次定义
            self.assertEqual(others.count(pin), 1, f"GPIO{pin} 被重复定义")

        # 实现层必须存在：初始化、任务、非阻塞步进、左右映射
        for fn in ("void InitializeSensors()", "void SensorLoop()", "bool StartSensorTask()"):
            self.assertIn(fn, source)
        # 两个轴必须同帧更新：用批量步进接口，而不是分别步进两次。
        # 分别调用时，两次之间会插一条 10ms 的 UART 日志，
        # 导致 1 号与 2 号先后生效（实测时间戳差正好 10ms）。
        self.assertIn("StepServoBothTo(target1, target2", source)
        self.assertNotIn("StepServoTo(1,", source)
        self.assertNotIn("StepServoTo(2,", source)
        self.assertIn(
            "target1 = SERVO_CENTER_ANGLE +\n"
            "                    (face_right ? REACT_IR_TURN_DEG : -REACT_IR_TURN_DEG);",
            source)
        self.assertIn("target2 = SERVO_CENTER_ANGLE - REACT_IR_TILT_DEG;", source)

        import re as _re2
        _tilt = int(_re2.search(r"#define REACT_IR_TILT_DEG (\d+)", header).group(1))
        self.assertEqual(_tilt, 40,
                         f"红外抬头应为 40°（当前 {_tilt}°）；"
                         f"20° 时会被转头动作盖过，观感像没抬头")
        # 且必须在抬头行程内（否则会被 ClampForAxis 截断，实际抬不到 40°）
        _up = int(_re2.search(r"#define SERVO_2_MAX_OFFSET_UP_DEG (\d+)", header).group(1))
        self.assertLessEqual(_tilt, _up,
                             f"抬头 {_tilt}° 超出抬头行程上限 {_up}°，会被截断")

    def test_sensors_have_debounce_both_directions(self):
        """红外/触摸的触发与释放都必须做去抖。

        演变（都是客户实机反馈驱动的）：
          1. 最早触发/释放都 200ms —— 起因是误判「GPIO3 自发误触发」，
             导致「挡一下要等半天才动」。
          2. 随后改为触发 1 拍即生效（≈20ms）+ 释放 3 拍。
          3. 2026-11-17 客户反馈「还是加上防抖」：
             红外边缘抖动会让头部在触发瞬间抽动，所以触发也补上 3 拍。

        现在两个方向都是 3 拍（≈60ms）：60ms 属于人类感知的“瞬时”，
        不会有迟钝感，但足以滤掉红外模块的边缘抖动。
        """
        source = SOURCE.read_text(encoding="utf-8")

        # 两个方向的去抖常量都必须存在
        self.assertIn("kTriggerDebounceTicks", source)
        self.assertIn("kReleaseDebounceTicks", source)

        # 触发与释放都要能累积计数到阈值
        body_start = source.index("const auto debounce = [](bool raw, bool& state, int& stable)")
        body = source[body_start:body_start + 700]
        self.assertIn("kTriggerDebounceTicks", body,
                      "触发方向必须参与去抖判定")
        self.assertIn("kReleaseDebounceTicks", body,
                      "释放方向必须参与去抖判定")
        # 关键：必须用累积计数后统一置位，不能某一方向直接 state=true 跳过确认
        self.assertIn("++stable", body)
        self.assertNotIn("state = true;", body,
                         "触发不得跳过累积确认直接置位（那就是无去抖）")

        # 旧的 200ms 红外语义常量不得残留
        self.assertNotIn("kIrDebounceTicks", source)

    def test_tool_angle_is_not_reset_by_sensor_task(self):
        """头部姿态动作转到的朝向不能被传感器任务自动拉回中位。

        客户 2026-11-17 要求：「舵机工具执行转动后不要自动归位，
        只有红外和触摸才会自动归位」。

        现在由头部动作播放器记住「静止朝向」：
          * look_left 等姿态动作改写静止朝向（引擎单测覆盖）
          * 无触发时传感器任务以静止朝向为目标
          * 红外/触摸只临时接管，结束后回到静止朝向
        """
        source = SOURCE.read_text(encoding="utf-8")
        # 旧基线变量与 update_base 参数已随旧工具一起删除
        self.assertNotIn("servo_base_angle", source)
        self.assertNotIn("update_base", source)
        # 无动作时目标取播放器的静止朝向
        self.assertIn("target1 = head_motion_.RestPan();", source)
        self.assertIn("target2 = head_motion_.RestTilt();", source)
        # 播放器跨任务访问必须加锁（工具在主线程入队，传感器任务推进）
        self.assertIn("xSemaphoreTake(head_motion_mutex_, portMAX_DELAY);", source)
        self.assertEqual(source.count("xSemaphoreTake(head_motion_mutex_"),
                         source.count("xSemaphoreGive(head_motion_mutex_"))

    def test_reactions_use_absolute_angles(self):
        """红外/触摸的动作目标必须是绝对角度，不叠加工具基线。

        客户 2026-11-17：「左转 45 度就永远是距离中间 45 度，右转 45 度
        也永远是往右的 45 度，角度改为绝对坐标」。

        旧写法 base + 偏移的问题：
          工具把脸设到 60° 后，右转变 60+20=80°，不再指向固定的“右侧”；
          基线一变同一动作结果就跟着变，行为不可预测。

        现在：
          左红外 → SERVO_MIN_ANGLE（45°）
          右红外 → SERVO_MAX_ANGLE（135°）
          触摸摆动 → 围绕 SERVO_CENTER_ANGLE（90°）往复
        这三处都必须直接引用常量，不能再出现 servo_base_angle*_ 参与运算。
        """
        source = SOURCE.read_text(encoding="utf-8")
        header = HEADER.read_text(encoding="utf-8")

        # REACT_IR_TURN_DEG 现在是「绝对偏移幅度 45°」，
        # 与旧版「相对基线 +20°」的语义不同（虽然名字相同）。
        # 关键区别在于使用处是 90 ± 它，而不是 base + 它。
        self.assertIn("#define REACT_IR_TURN_DEG 45", header)
        self.assertNotIn("RestPan() +", source,
                         "不得在静止朝向上做加法（那是旧的相对坐标写法）")

        # 红外转向 = 绝对左右限位
        # 绝对角度 = 90 ± REACT_IR_TURN_DEG（45°），不再用工具上限常量：
        # 工具上限是 60°，尚未验证能安静到位；红外是自动高频动作，
        # 一旦顶死会持续哒哒响，所以自动动作只用已验证安静的 45°。
        self.assertIn(
            "target1 = SERVO_CENTER_ANGLE +\n"
            "                    (face_right ? REACT_IR_TURN_DEG : -REACT_IR_TURN_DEG);",
            source)

        # 触摸摆动围绕绝对中位，不能叠加基线
        self.assertIn(
            "target1 = SERVO_CENTER_ANGLE + swing_direction * REACT_TOUCH_SWING_X_DEG;",
            source,
            "触摸摆动必须围绕绝对中位 90°，不能叠加工具基线")
        self.assertIn(
            "target2 = SERVO_CENTER_ANGLE + swing_direction * REACT_TOUCH_SWING_Y_DEG;",
            source)

        # 动作目标计算区不得出现基线叠加（基线只用于“无触发时的停留点”）
        body_start = source.index("} else if (head_touched) {")
        body_end = source.index("// ---- 3. 处理状态切换时的副作用 ----")
        body = source[body_start:body_end]
        self.assertNotIn("RestPan()", body,
                         "红外/触摸目标不得叠加静止朝向（那是旧的相对坐标写法）")
        self.assertNotIn("RestTilt()", body)

    def test_touch_reaction_swings_and_clears_emotion(self):
        """摸头用明显的双轴动作；只有按下沿通知云端，松手只复位本地效果。"""
        source = SOURCE.read_text(encoding="utf-8")
        header = HEADER.read_text(encoding="utf-8")

        edge = source.split("if (head_touched != was_touch)", 1)[1].split("// 只有抢到控制权", 1)[0]
        self.assertIn('display_->SetTouchSmile(head_touched)', edge)
        self.assertNotIn('display_->SetEmotion(', edge)
        self.assertNotIn('kEmotionRefreshTicks', source)
        self.assertNotIn('SendServerMcpNotification', edge)
        self.assertIn('#define REACT_TOUCH_SWING_X_DEG 20', header)
        self.assertIn('#define REACT_TOUCH_SWING_Y_DEG 12', header)

        # 触摸必须驱动两个轴摆动，不能只动一个
        self.assertIn("REACT_TOUCH_SWING_X_DEG", source)
        self.assertIn("REACT_TOUCH_SWING_Y_DEG", source)

        # 上电静默期：TTP223 自校准期间输电平不可信
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(2000))", source)
        self.assertIn("传感器基线", source)

    def test_sensor_output_uses_only_the_winning_trigger(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn('sensor_focus.Reset(touch_on, ir_left_on, ir_right_on)', source)
        self.assertIn('sensor_focus.Tick(touch_on, ir_left_on, ir_right_on)', source)
        for name, kind in (('head_touched', 'Head'), ('face_left', 'Left'), ('face_right', 'Right')):
            self.assertIn(f'{name} = interaction.active == SensorInput::k{kind};', source)
        edge = source.split('if (interaction.started && !motion_active)', 1)[1].split('// 红外状态边沿日志', 1)[0]
        self.assertIn('"method":"notifications/touch/head"', edge)
        for side in ('right', 'left'):
            self.assertIn(f'"method":"notifications/infrared/turn","params":{{"side":"{side}"}}', edge)
        self.assertEqual(edge.count('SendServerMcpNotification(notification)'), 1)
        self.assertEqual(source.count('SendServerMcpNotification('), 1)

    def test_sensor_task_starts_after_display(self):
        """传感器任务必须在 OLED 初始化之后启动。

        触摸反应会调用 display_->SetTouchSmile，若 display_ 还是 nullptr，
        整个反应逻辑会静默失效。构造函数里的顺序是硬约束。
        """
        source = SOURCE.read_text(encoding="utf-8")
        ctor_start = source.index("DeepbotS3CamBoard()")
        ctor_end = source.index("AudioCodec* GetAudioCodec()", ctor_start)
        ctor = source[ctor_start:ctor_end]

        self.assertIn("InitializeDisplay();", ctor)
        self.assertIn("InitializeSensors();", ctor)
        self.assertIn("StartSensorTask();", ctor)
        self.assertLess(ctor.index("InitializeDisplay();"), ctor.index("StartSensorTask();"))
        self.assertLess(ctor.index("InitializeSensors();"), ctor.index("StartSensorTask();"))

    def test_servo_moves_smoothly_without_snap(self):
        """传感器驱动的舵机动作必须平滑步进，不能一帧直达。

        客户 2026-11-17 反馈：「转动的时候速度减慢一点，现在太快了」。

        根因不是步进值太大，而是 StepServoBothTo 里曾有一个
        「差值 ≥ kSnapDeg(10°) 一帧直通」的分支：
        红外转 45° 在单帧（20ms）内瞬间完成，舵机瞬间大角度跳动，
        冲量大、有响声，观感像“抖一下”而不是“看过去”。

        现在删除直通，所有幅度统一按 kStepDeg 平滑逼近。
        根据 step_deg=2°/tick、tick=20ms 估算，45° 约需 23 拍 ≈460ms，
        接近真实头部转动速度。
        """
        source = SOURCE.read_text(encoding="utf-8")

        step_start = source.index("void StepServoBothTo(")
        step_end = source.index("void RegisterCameraTools()", step_start)
        step_body = source[step_start:step_end]

        # 不得存在“大跳变直接到位”的分支
        self.assertNotIn("kSnapDeg", step_body,
                         "不得保留一帧直通分支（那是转动过快的原因）")

        # 必须真的按步长逐步靠近
        self.assertIn("step_toward", step_body)
        self.assertIn("servo1_angle_", step_body)

        # 步进与周期必须存在且为合理值
        import re as _re
        tick = int(_re.search(r"kTickMs = (\d+)", source).group(1))
        step = int(_re.search(r"kStepDeg = (\d+)", source).group(1))
        self.assertGreater(step, 0)
        self.assertGreater(tick, 0)

        # 速度上限：不超过 150°/秒，否则 45° 转向会在 300ms 内完成，过快。
        # 这条能在有人调大 kStepDeg 或调小 kTickMs 时拦住。
        deg_per_sec = step * 1000.0 / tick
        self.assertLessEqual(
            deg_per_sec, 150,
            f"当前转速 {deg_per_sec:.0f}°/s 偏快（step={step}°/tick, tick={tick}ms），"
            f"45° 转向会在 {45 / deg_per_sec * 1000:.0f}ms 内完成，客户会感觉太快")
        # 下限：也不该慢到迟钝，45° 转向不应超过 1.2 秒
        self.assertGreaterEqual(
            deg_per_sec, 40,
            f"当前转速 {deg_per_sec:.0f}°/s 过慢，45° 转向要 "
            f"{45 / deg_per_sec * 1000:.0f}ms，会显得迟钝")

    def test_head_act_tool_queues_motion_instead_of_writing_pwm(self):
        """self.head.act 只入队，由传感器任务按关键帧速度驱动，不直接写 PWM。

        旧接口只能设一个目标角度，模型做不出点头、摇头；
        现在动作由固件播放器按关键帧完整播放，工具立即返回。
        """
        source = SOURCE.read_text(encoding="utf-8")
        start = source.index("void RegisterHeadTools()")
        end = source.index("void InitializeServoConsole()", start)
        tools = source[start:end]
        # 参数只有动作名和 1（普通）/2（使劲）两档，默认普通；动作清单来自引擎，不在两处手写
        self.assertIn('Property("action", kPropertyTypeString', tools)
        self.assertIn('Property("level", kPropertyTypeInteger, 1, 1, 2)', tools)
        self.assertIn("level 1 普通、2 使劲", tools)
        self.assertIn("做表情或头部动作", tools)
        self.assertNotIn("眼睛表情", tools)
        self.assertIn("deepbot_head::ActionNames()", tools)
        # 只入队，不写 PWM、不阻塞等待动作完成
        self.assertIn("EnqueueHeadAction(action, level)", tools)
        for forbidden in ("SetBothServoAngles(", "WriteServoPulse", "vTaskDelay"):
            self.assertNotIn(forbidden, tools)
        # 失败用工具错误返回，成功返回空文本（服务端不会为一次点头再让模型说话）
        self.assertIn("return std::unexpected(std::move(error));", tools)
        self.assertIn("return ReturnValue(std::string{});", tools)

        # 传感器任务：动作优先于触摸与红外，动作拍直接写角度，其余仍平滑逼近
        loop = source[source.index("void SensorLoop()"):source.index("static void SensorTaskEntry")]
        self.assertIn("motion_active = head_motion_.Tick(kTickMs, pan, tilt, &started_emotion);", loop)
        self.assertLess(loop.index("if (motion_active) {"), loop.index("} else if (head_touched) {"))
        self.assertIn("SetBothServoAngles(target1, target2, /*verbose=*/false);", loop)
        self.assertIn("StepServoBothTo(target1, target2, kStepDeg);", loop)
        # 头部动作时眼神跟随横向朝向
        self.assertIn("kGazeFollowDeg", loop)

        # 新增引擎源文件随板级目录 GLOB 编译，并登记到代码地图
        self.assertTrue((SOURCE.parent / "deepbot_head_motion.cc").exists())
        self.assertTrue((SOURCE.parent / "deepbot_head_motion.h").exists())
        self.assertIn('#include "deepbot_head_motion.h"', source)

    def test_both_servos_update_in_same_frame(self):
        """两个舵机必须同帧更新，而不是先后动。

        背景（已实测）：分别调两次 SetServoAngle 时，
        中间会插一条 ESP_LOGI，115200 波特率下占 UART 约 10ms，
        日志时间戳差正好 10ms，观感上就是“先转头再抬头”。

        正确做法：先把两个角度都算成脉宽，再背靠背写两个 LEDC 通道，
        中间不做任何 IO。两个通道共享 TIMER_2，所以同一周期边界同时生效。
        """
        source = SOURCE.read_text(encoding="utf-8")

        # 批量接口必须存在
        self.assertIn("bool SetBothServoAngles(", source)

        body_start = source.index("bool SetBothServoAngles(")
        body_end = source.index("void StepServoBothTo(", body_start)
        body = source[body_start:body_end]

        # 关键约束：两个 WriteServoPulse 必须背靠背，中间不能有日志/延时
        write1 = body.index("WriteServoPulse(kServo1Channel")
        write2 = body.index("WriteServoPulse(kServo2Channel")
        self.assertLess(write1, write2, "1 号应先写，便于阅读")
        between = body[write1:write2]
        self.assertNotIn("ESP_LOG", between, "两次写之间不得插日志，否则又变成先后动")
        self.assertNotIn("vTaskDelay", between, "两次写之间不得延时")

        # 日志必须在两次写之后，而且只允许一条合并日志
        log_pos = body.find("ESP_LOGI")
        self.assertGreater(log_pos, write2, "日志应在两个通道都写完后才输出")
        self.assertIn("if (verbose)", body, "步进路径必须能关日志（否则 50 条/秒）")

        # 步进路径必须用 verbose=false
        step_start = source.index("void StepServoBothTo(")
        step_end = source.index("void RegisterCameraTools()", step_start)
        step_body = source[step_start:step_end]
        self.assertIn("verbose=*/false", step_body)

        # 旧的单轴写入口已删除：传感器任务每拍只调一次 SetBothServoAngles，
        # 两轴（包括 look_up_left 这类斜向动作）都在同一次调用里背靠背写入。
        self.assertNotIn("bool SetServoAngle(", source)
        loop = source[source.index("void SensorLoop()"):source.index("static void SensorTaskEntry")]
        self.assertNotIn("WriteServoPulse", loop, "传感器任务不得绕过批量接口单独写某一轴")

    def test_audio_preserves_cam_simplex_aec_wiring(self):
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        expected = {
            "AUDIO_I2S_MIC_GPIO_WS": 1,
            "AUDIO_I2S_MIC_GPIO_SCK": 2,
            "AUDIO_I2S_MIC_GPIO_DIN": 40,
            "AUDIO_I2S_SPK_GPIO_DOUT": 41,
            "AUDIO_I2S_SPK_GPIO_BCLK": 42,
            "AUDIO_I2S_SPK_GPIO_LRCK": 48,
        }
        for name, gpio in expected.items():
            self.assertRegex(header, rf"#define\s+{name}\s+GPIO_NUM_{gpio}\b")

        # 所有音频 pin 都必须与摄像头、OLED 和舵机不冲突。
        def pin(name):
            return re.search(rf"#define\s+{name}\s+GPIO_NUM_(\d+)", header).group(1)
        audio_pins = {pin(name) for name in expected}
        camera_pins = set(re.findall(r"#define\s+CAMERA_PIN_\w+\s+GPIO_NUM_(\d+)", header))
        display_pins = {pin("DISPLAY_SDA_PIN"), pin("DISPLAY_SCL_PIN")}
        servo_pins = {pin("SERVO_1_GPIO"), pin("SERVO_2_GPIO")}
        self.assertFalse(audio_pins & camera_pins)
        self.assertFalse(audio_pins & display_pins)
        self.assertFalse(audio_pins & servo_pins)

        self.assertIn("AUDIO_I2S_METHOD_SIMPLEX", header)
        self.assertIn("AUDIO_INPUT_REFERENCE true", header)
        self.assertIn("#define AUDIO_INPUT_SAMPLE_RATE 16000", header)
        self.assertIn("#define AUDIO_OUTPUT_SAMPLE_RATE 24000", header)
        self.assertIn("NoAudioCodecSimplexAec", source)
        self.assertNotIn("DummyAudioCodec", source)
        self.assertIn("AUDIO_I2S_SPK_GPIO_BCLK", source)
        self.assertIn("AUDIO_I2S_MIC_GPIO_DIN", source)

    def test_display_uses_i2c_oled_and_shared_eyes_ui(self):
        source = SOURCE.read_text(encoding="utf-8")
        header = HEADER.read_text(encoding="utf-8")
        self.assertIn("NoAudioCodecSimplexAec", source)
        self.assertIn("void StartNetwork() override", source)
        self.assertIn("InitializeCamera", source)
        self.assertIn("PIXFORMAT_JPEG", source)
        self.assertIn("FRAMESIZE_VGA", source)
        self.assertNotIn("FRAMESIZE_QVGA", source)

        # ---- 屏幕初始化：I2C + 单色面板 ----
        self.assertIn("i2c_new_master_bus", source)
        self.assertIn("esp_lcd_new_panel_io_i2c", source)
        self.assertIn(".bits_per_pixel = 1", source)

        # ---- 显示极性：必须在数据层取反 ----
        # esp_lvgl_port 单色转换固定输出“背景=位1(亮)、内容=位0(暗)”，
        # 本板屏幕是“位1点亮”，于是背景亮（蓝底黑眼）。
        # 实测面板命令 0xA6/0xA7 在这块屏上**均无效**，必须靠数据层取反。
        self.assertIn("DISPLAY_PIXEL_INVERT", header)
        self.assertRegex(header, r"#define\s+DISPLAY_PIXEL_INVERT\s+1\b")
        self.assertIn("InvertColorTx", source)
        self.assertIn("panel_io_->tx_color = InvertColorTx", source)
        # 取反不能在命令通道上做（否则 0xB0 页地址等命令会被破坏）
        self.assertIn("lcd_cmd >= 0", source)

        # 必须用 SH1106 驱动：SSD1306 驱动用 0x20/0x21/0x22 设寻址，
        # 这三条 SH1106 不支持，会导致画面冻结不刷新（已实测）。
        self.assertRegex(
            header,
            r"#define\s+DISPLAY_USE_SH1106\s+1\b",
            "本板是 SH1106，用 SSD1306 驱动会导致画面不刷新",
        )
        # 驱动按宏二选一，默认走 SH1106
        self.assertIn("#if DISPLAY_USE_SH1106", source)
        self.assertIn("esp_lcd_new_panel_sh1106", source)
        self.assertIn("esp_lcd_new_panel_ssd1306", source)

        # ---- 旧 SPI 彩屏路径必须彻底移除 ----
        for gone in (
            "esp_lcd_new_panel_ili9341",
            "spi_bus_initialize",
            "CameraLcdDisplay",
            "SpiLcdDisplay",
            "UpdateCameraFrame",
            "decode_buf_",
            "esp_lcd_panel_draw_bitmap",
        ):
            self.assertNotIn(gone, source, f"{gone} 属于旧彩屏实现，应删除")

        self.assertIn('#include "display/deepbot_eyes_display.h"', source)
        self.assertIn("new DeepbotEyesDisplay", source)
        # 单色屏整屏同色，眼睛用满 64 行；zone_top=0 是板级显式传入的
        self.assertRegex(
            source,
            r"new DeepbotEyesDisplay\([^;]*DISPLAY_MIRROR_Y,\s*0,",
            "单色屏必须显式传 zone_top=0，否则会沿用双色屏默认的 16",
        )

        self.assertIn("EyeStyle::kCircle", source,
                      "cam 板必须显式传 EyeStyle::kCircle，否则眼睛不是圆形")

        # ---- 本地不显示摄像头画面 ----
        # 用户要求：屏幕上不显示摄像头内容，只显示联网状态与眼睛表情。
        self.assertNotIn("PreviewTask", source)
        self.assertNotIn("jpeg_dec_open", source)
        self.assertNotIn("esp_camera_fb_get", source)
        # 但摄像头本身仍要初始化，后面要推给云端 OV 网关
        watcher = (ROOT / "main/boards/common/ov_camera_watcher.cc").read_text(encoding="utf-8")
        self.assertIn("camera_ready_ = camera_->Initialize();", source)
        self.assertIn("esp_camera_init(&config_)", watcher)

        # ---- 联网状态与注册码 ----
        # 界面提示交给框架状态机与眼睛 UI 的白名单，板级不再自绘 banner
        self.assertNotIn("SetWifiBanner", source)
        self.assertNotIn("SetConfigHint", source)
        self.assertIn("bool ShouldStartActivation() override { return true; }", source)
        self.assertIn("void OnActivationCode(const std::string& code", source)
        self.assertIn("display_->ShowPairingCode(code);", source)
        self.assertNotIn("ShowNotification(code.c_str()", source)
        board_header = (ROOT / "main/boards/common/board.h").read_text(encoding="utf-8")
        self.assertIn("virtual void OnActivationCode(const std::string& code", board_header)
        application = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn("Board::GetInstance().OnActivationCode(code, message)", application)

        # ---- 网络事件回调必须链式包裹 ----
        # Application::Initialize() 先注册自己的回调驱动状态机，回调是单变量，
        # 直接赋值会把它覆盖掉，设备就再也不会进入待命。
        self.assertIn("NetworkEventCallback app_callback = network_event_callback_", source)
        self.assertIn("NetworkEvent::Connected", source)

        board_config = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        ota_entries = [
            item
            for item in board_config["builds"][0]["sdkconfig_append"]
            if item.startswith("CONFIG_OTA_URL=")
        ]
        self.assertEqual(len(ota_entries), 1, "只能配置一个 OTA 地址")
        self.assertEqual(
            ota_entries[0], 'CONFIG_OTA_URL="http://43.142.28.216:8765/deepbot/ota/"'
        )
        defaults = (ROOT / "sdkconfig.defaults.esp32s3").read_text(encoding="utf-8").splitlines()
        self.assertEqual([line for line in defaults if line.startswith("CONFIG_OTA_URL=")], ota_entries)

    def test_eyes_ui_shows_text_while_connecting_then_eyes(self):
        # 用户要求：开机先正常显示联网过程，连上服务器后才显示眼睛表情。
        # 这条契约由共享眼睛 UI 的白名单实现，这里锁死白名单内容，
        # 防止以后把 connecting/activating 也放进眼睛模式导致"一上电就是眼睛"。
        eyes = EYES_SOURCE.read_text(encoding="utf-8")
        block = eyes.split("bool DeepbotEyesDisplay::ShouldShowEyes")[1]
        block = block.split("}")[0] + "}"
        for state in (
            "kDeviceStateIdle",
            "kDeviceStateListening",
            "kDeviceStateSpeaking",
            "kDeviceStateNotifying",
        ):
            self.assertIn(state, block, f"{state} 应显示眼睛")
        # 联网阶段必须走文字提示
        for state in ("kDeviceStateStarting", "kDeviceStateConnecting", "kDeviceStateActivating"):
            self.assertNotIn(state, block, f"{state} 不应显示眼睛，应交回文字提示")
        # 白名单之外的默认分支返回 false（显示文字）
        self.assertIn("return false;", block)

        # 启动文字由眼睛组件绘制，不依赖 OLED 基类切换板型。
        self.assertIn("void DeepbotEyesDisplay::ShowBrandText", eyes)
        self.assertIn('lv_label_set_text(emotion_label_, "mini-bot")', eyes)
        self.assertIn("LvglTheme", eyes)
        self.assertIn("text_font()", eyes)
        oled = (ROOT / "main/display/oled_display.cc").read_text(encoding="utf-8")
        self.assertNotIn("CONFIG_BOARD_TYPE_", oled)

    def test_pairing_code_replaces_brand_until_activation_finishes(self):
        eyes = EYES_SOURCE.read_text(encoding="utf-8")
        show = eyes.split("void DeepbotEyesDisplay::ShowPairingCode", 1)[1].split("\nvoid ", 1)[0]
        render = eyes.split("void DeepbotEyesDisplay::UpdateStartupTextLocked", 1)[1].split("\nvoid ", 1)[0]
        brand = eyes.split("void DeepbotEyesDisplay::ShowBrandText", 1)[1].split("\nvoid ", 1)[0]
        mode = eyes.split("void DeepbotEyesDisplay::ApplyDisplayMode", 1)[1].split("\nvoid ", 1)[0]
        self.assertIn('code.find_first_not_of("0123456789")', show)
        self.assertIn('code.size() != 6', show)
        self.assertLess(show.index('DisplayLockGuard'), show.index('std::memcpy(pairing_code_'))
        self.assertIn('UpdateStartupTextLocked();', show)
        self.assertIn('UpdateStartupTextLocked();', brand)
        self.assertIn('lv_label_set_text(emotion_label_, pairing_code_);', render)
        self.assertIn('lv_obj_center(emotion_label_);', render)
        self.assertNotIn('ShowNotification', show)
        clearing = mode.index("pairing_code_[0] = '\\0';")
        self.assertLess(mode.index('if (eyes_mode) {'), clearing)
        self.assertLess(clearing, mode.index('UpdateStartupTextLocked();', clearing))

    def test_one_class_per_source_file(self):
        for path in sorted(BOARD.glob("*.cc")) + sorted(BOARD.glob("*.h")):
            text = path.read_text(encoding="utf-8")
            # enum class 是强类型枚举，不是这条文件组织约束要计数的类声明。
            declarations = re.findall(r"\b(?:enum\s+)?class\s+\w+", text)
            class_count = sum(item.startswith("class") for item in declarations)
            self.assertLessEqual(
                class_count, 1, f"{path.name} 含 {class_count} 个类，应拆分为一个类一个文件"
            )

    def test_board_source_has_balanced_braces(self):
        for path in (SOURCE, EYES_SOURCE):
            source = path.read_text(encoding="utf-8")
            self.assertEqual(source.count("{"), source.count("}"), f"{path.name} 括号不平衡")
        display = (ROOT / "main/display/oled_display.cc").read_text(encoding="utf-8")
        self.assertEqual(display.count("{"), display.count("}"))
        self.assertFalse((ROOT / "main/display/lcd_display.cc").exists())

    def test_board_source_passes_syntax_check(self):
        compiler = "g++"
        for target in (SOURCE, EYES_SOURCE):
            result = subprocess.run(
                [compiler, "-std=c++23", "-fsyntax-only", "-I", str(BOARD), "-I",
                 str(ROOT / "main"), str(target)],
                text=True,
                capture_output=True,
                check=False,
            )
            # 主机编译器没有 ESP-IDF 头文件，语法通过或只因缺失 ESP 头失败都接受；
            # 但源码自身的括号、类型和纯虚调用错误必须先暴露出来。
            self.assertNotIn("pure virtual", result.stderr, f"{target.name}: {result.stderr[:400]}")
            self.assertNotIn(
                "expected declaration", result.stderr, f"{target.name}: {result.stderr[:400]}"
            )

    # ---- OV 眼镜网关摄像头持续观察组件 ----

    def test_camera_watcher_component_contract(self):
        """OvCameraWatcher 的接口与生命周期约束。

        关键设计：本组件故意**不**继承项目的 Camera 抽象类。
        Camera 对应 self.camera.take_photo（拍一张问一次）；而本板要的是
        「持续观察 + 随时取最近答案」。若继承 Camera，McpServer 会自动
        挂上一个语义不符的 take_photo 工具，让大模型选错工具。
        """
        header = (ROOT / "main/boards/common/ov_camera_watcher.h").read_text(encoding="utf-8")
        source = (ROOT / "main/boards/common/ov_camera_watcher.cc").read_text(encoding="utf-8")

        # 不能继承 Camera
        self.assertNotIn("public Camera", header,
                         "不该继承 Camera，否则会多注册一个语义不符的 take_photo")
        # 不实现 Camera 的纯虚函数
        self.assertNotIn("SetExplainUrl", header)
        self.assertNotIn("Explain(", header)

        # 必备接口：初始化 / 启动 / 停止 / 取答案 / 抓帧
        for method in ("bool Initialize();", "std::expected<void, std::string> Start();",
                       "void Stop();", "std::string GetRecentAnswersJson() const;",
                       "CaptureJpeg();"):
            self.assertIn(method, header)

        # 构造不调 esp_camera_init，初始化单独一个方法（方便延后到 WiFi 就绪）
        ctor_start = source.index("OvCameraWatcher::OvCameraWatcher")
        ctor_end = source.index("bool OvCameraWatcher::Initialize()")
        self.assertNotIn("esp_camera_init(&", source[ctor_start:ctor_end],
                         "构造函数不得初始化传感器")
        self.assertIn("esp_camera_init(&config_)", source[ctor_end:ctor_end + 600])

        # 帧缓冲必须及时归还，否则 PSRAM 会泄漏
        self.assertIn("decltype(&esp_camera_fb_return)", source)
        self.assertIn("esp_camera_fb_get(), esp_camera_fb_return", source)
        self.assertIn("~OvCameraWatcher()", source)

    def test_camera_watcher_observes_once_per_second(self):
        """持续观察必须每秒推一帧，且缓存最近 5 条答案。"""
        source = (ROOT / "main/boards/common/ov_camera_watcher.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/boards/common/ov_camera_watcher.h").read_text(encoding="utf-8")

        # 采样周期固定 1000ms
        self.assertIn("kFrameIntervalMs = 1000", source)

        # 下一帧按实际开始时间计，结果轮询不能重置帧节拍。
        self.assertIn('next_frame_us = esp_timer_get_time() + kFrameIntervalMs * 1000LL;', source)
        self.assertIn('int64_t wake_us = next_frame_us;', source)

        # 缓存上限 5 条
        self.assertIn("kMaxAnswers = 5", header)
        # 超限时裁掉最旧的一条（环形缓冲语义）
        self.assertIn("answers_.erase(answers_.begin())", source)
        self.assertIn("answers_.push_back", source)

        # 连续采样不能退回旧的逐帧简答模式；新提示词及其发送位置由协议测试校验。
        self.assertNotIn('画面里有什么？请用一句话简略回答。', source)

    def test_camera_watcher_reports_not_started(self):
        """未启动观察时必须明确告诉大模型“没有启动摄像头”。

        为什么要专门守这一条：
        若未启动时返回空数组或不返字段，大模型会以为“摄像头看过了但没东西”，
        从而向用户编造或错误推演画面内容。必须给出明确文案。
        """
        source = (ROOT / "main/boards/common/ov_camera_watcher.cc").read_text(encoding="utf-8")

        self.assertIn("摄像头没有启动观察", source)
        # 未启动分支必须先判断 watching，再走取答案逻辑
        body_start = source.index("std::string OvCameraWatcher::GetRecentAnswersJson()")
        body = source[body_start:body_start + 2500]
        self.assertIn("if (!watching)", body)
        self.assertLess(body.index("if (!watching)"), body.index("answers_"),
                        "必须先判未启动，再读答案缓存")

        # 已启动但还没答案时也要如实说明，不能让大模型当“什么都没看到”
        self.assertIn("还没有收到画面描述", source)

    def test_camera_watcher_is_thread_safe_and_idempotent(self):
        """共享状态必须有锁保护；Start/Stop 必须可重复调用。

        采样跑在独立任务里，MCP 工具在主线程读缓存，两边并发访问；
        没有锁会读到半写的 vector 导致崩溃。
        """
        source = (ROOT / "main/boards/common/ov_camera_watcher.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/boards/common/ov_camera_watcher.h").read_text(encoding="utf-8")

        # 三把锁：答案 / 会话 / 帧
        for m in ("answers_mutex_", "session_mutex_", "frame_mutex_"):
            self.assertIn(m, header)
            self.assertIn(f"std::lock_guard<std::mutex> lock({m})", source)

        # 幂等：已在观察时 Start 直接返回成功；未观察时 Stop 直接返回
        start = source.split('OvCameraWatcher::Start(', 1)[1].split('\n}', 1)[0]
        stop = source.split('OvCameraWatcher::Stop()', 1)[1].split('\n}', 1)[0]
        for method in (start, stop):
            self.assertIn('std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_)', method)
        self.assertIn('return StartLocked();', start)
        start_locked = source.split('OvCameraWatcher::StartLocked()', 1)[1].split('\n}', 1)[0]
        self.assertLess(start_locked.index('if (watching_) return {};'), start_locked.index('xTaskCreatePinnedToCore'))
        self.assertLess(stop.index('if (!watching_) return;'), stop.index('watching_ = false;'))

        # 任务必须跑在独立 FreeRTOS 任务里，不能占主线程
        self.assertIn("xTaskCreatePinnedToCore", source)

    def test_camera_gateway_protocol_order(self):
        """当前画面与回看共用公开 QA 会话接口。"""
        source = (ROOT / "main/boards/common/ov_camera_watcher.cc").read_text(encoding="utf-8")

        for path in ("/v1/glasses/sessions", "/text", "/frame?t=", "/results?after=", "DELETE"):
            self.assertIn(path, source)

        # 启用阶段只建会话；开始推帧后才允许看图或提交监控条件。
        loop = source.split('void OvCameraWatcher::RunLoop() {', 1)[1].split('\n}', 1)[0]
        setup, frames = loop.split('while (watching_)', 1)
        self.assertNotIn('SendQuestion(', setup)
        self.assertIn('PushOneFrame(http, start_us)', frames)
        upload = source.split('OvCameraWatcher::PushOneFrame(', 1)[1].split('\n}', 1)[0]
        self.assertIn('CaptureFreshFrame()', upload)
        self.assertIn('frame->captured_us - session_start_us', upload)
        self.assertIn('if (cJSON_IsTrue(accepted)) return t_sec;', upload)
        capture = source.split('OvCameraWatcher::CaptureFreshFrame()', 1)[1]
        self.assertIn('if (timestamp < requested_us) continue;', capture)
        open_session = source.split('OvCameraWatcher::OpenSession(', 1)[1].split('\n}', 1)[0]
        payload = re.search(r'constexpr auto payload = R"\((\{[^\n]*\})\)";', open_session)
        self.assertIsNotNone(payload)
        config = json.loads(payload.group(1))
        self.assertEqual((config['input_mode'], config['mode'], config['fps']), ('live', 'qa', 30.0))
        self.assertEqual(set(config), {'input_mode', 'mode', 'fps'})
        self.assertEqual(source.count('SendQuestion('), 3)  # 定义、用户查询和共用的监控提交入口
        self.assertIn('SendQuestion(http, monitor.condition, t, "auto")', frames)
        self.assertNotIn('CLEARED', frames)
        self.assertIn('SendQuestion(http, active->question, match.question_t, active->time_scope)', frames)
        self.assertIn('cJSON_AddStringToObject(root, "time_scope", time_scope.c_str())', source)
        self.assertIn('if (!watching_ || (active && !has_query && !query_abandoned)) return;', frames)
        self.assertIn('CollectResults(http, result_after, match)', frames)
        self.assertIn('question_t - frame_t >= kCurrentFrameMaxAgeSec', frames)
        self.assertLess(frames.index('if (!stream_lost && !active && !confirm_by_us && wants_current && latest_frame)'),
                        frames.index('PushOneFrame(http, start_us)'))
        self.assertIn('if ((query_abandoned && (monitor.condition.empty() || now_us >= abandoned_query_until_us)) || stream_lost)', frames)
        # 流式会话失效后必须重建会话，否则同一会话的回看会一直失败。
        self.assertIn('if (match.status == "stream_unavailable") stream_lost = true;', frames)
        self.assertIn('CloseSession(http);', frames)
        board = SOURCE.read_text()
        # 只留两个 look 供模型调用；串口问答不受影响。
        self.assertNotIn('"self.camera.get_view"', board)
        ov = board.split('auto add_look = ', 1)[1].split('// 本地调试取图', 1)[0]
        self.assertNotIn('Property("time_scope"', ov)
        start_watch = board.split('mcp.AddTool("self.camera.start_watch",', 1)[1].split('mcp.AddTool("self.camera.stop_watch",', 1)[0]
        self.assertIn('PropertyList()', start_watch)
        self.assertIn('camera_->Start()', start_watch)
        self.assertNotIn('notifications/camera/watch', start_watch)
        start_monitor = board.split('"self.camera.start_monitor"', 1)[1].split('"self.camera.stop_monitor"', 1)[0]
        self.assertIn('PropertyList({Property("condition", kPropertyTypeString),', start_monitor)
        self.assertIn('Property("reminder", kPropertyTypeString, std::string{}).SetMaxLength(2048)', start_monitor)
        self.assertIn('args["reminder"].value<std::string>()', start_monitor)
        self.assertIn('cJSON_AddStringToObject(params, "reminder", settings.reminder.c_str())', source)
        self.assertNotIn('monitor.reminder', frames)  # 循环只将 condition 发给 OV。
        self.assertNotIn('silent_if_negative', start_monitor)
        self.assertIn('"self.camera.stop_monitor"', board)
        self.assertIn('"monitor_ready"', board)
        self.assertIn('cJSON_AddStringToObject(camera, "monitor_reminder", monitor.reminder.c_str())', board)
        self.assertNotIn('Property("prompt"', start_watch)
        self.assertNotIn('camera_->Start()', ov)
        self.assertNotIn('CaptureJpeg()', ov)
        # 仅 look_current 声明可选 question，look_past 保持无参数。
        self.assertNotIn('"self.camera.look"', board)
        self.assertNotIn('Look()', source)
        look = ov
        self.assertIn('if (!past) {\n                properties.AddProperty(Property("question", kPropertyTypeString, std::string{}));', look)
        self.assertIn('std::make_unique<McpTool>(name, description, properties,', look)
        self.assertIn('auto seen = past ? camera_->LookPast()', look)
        self.assertIn(': camera_->LookCurrent(arguments["question"].value<std::string>(), &session_id);', look)
        # 失败以工具错误返回，不当作画面线索。
        self.assertIn('if (!seen) return std::unexpected(seen.error());', look)
        self.assertIn('tool->set_background(true)', look)
        self.assertIn('tool->set_result_mime_type("text/plain")', look)
        self.assertIn('add_look("self.camera.look_current",', look)
        self.assertIn('add_look("self.camera.look_past",', look)
        self.assertNotIn('不先说话', look)
        self.assertIn('无人时不播报结果', look)
        # 当前问题可覆盖默认值，回看仍用固定问题；两者各自锁定时间范围。
        self.assertIn('return LookOnce(kLookCurrentQuestion, "current", session_id);', source)
        self.assertIn('return LookOnce(question, "current", session_id);', source)
        self.assertIn('"画面中是否有人，如果有人简洁描述他在干什么，以及可能的情绪，不超过30个字"', source)
        self.assertIn('return LookOnce(kLookPastQuestion, "past");', source)
        self.assertIn('"总结过去一分钟画面前的人在做什么？如果没有人，就回答没有人。"', source)
        once = source.split('OvCameraWatcher::LookOnce(', 1)[1].split('\n}', 1)[0]
        self.assertEqual(once.count('RequestView('), 1)
        self.assertNotIn('【现在】', source)
        self.assertIn('timestamp < requested_us', source)
        self.assertIn('详细描述画面内容', source)
        self.assertIn('wait_until(', source)
        self.assertIn('kViewTimeoutSec = 20', source)
        self.assertEqual(source.count('"/text"'), 1)

        # 推帧必须声明为 JPEG 并发送裸字节，不读取其他请求的同名调用。
        self.assertIn('kMaxControlReplyBytes, "image/jpeg", frame->jpeg', upload)

        # 插话内容用 cJSON 构造，避免文本里的引号产生非法 JSON
        self.assertIn('cJSON_AddStringToObject(root, "text"', source)

        # 会话必须被关闭，避免网关侧会话堆积
        self.assertIn("CloseSession(http)", source)

        # 公开接口使用已处理的 seq；不依赖旧网关的诊断通道。
        collect = source.split('OvCameraWatcher::CollectResults(', 1)[1].split('\n}', 1)[0]
        self.assertIn('"/results?after=" + std::to_string(after) + "&wait=0"', collect)
        for private_field in ('/logs?', 'X-Session-Log-Token', 'task_template', 'question_log_seq'):
            self.assertNotIn(private_field, source)

    def test_ov_logs_observation_memory_and_reset_reason(self):
        source = (ROOT / "main/boards/common/ov_camera_watcher.cc").read_text()
        self.assertNotIn("CreateHttp(", source)
        self.assertIn("kHeapLogIntervalUs = 30000000", source)
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", source)
        self.assertIn("heap_caps_get_largest_free_block(internal)", source)
        self.assertIn("esp_reset_reason()", (ROOT / "main/main.cc").read_text())

    def test_cam_explicitly_selects_circle_eyes(self):
        shared_header = EYES_HEADER.read_text(encoding="utf-8")
        self.assertIn("EyeStyle style = EyeStyle::kRoundedRect", shared_header)
        self.assertIn("EyeStyle::kCircle", SOURCE.read_text(encoding="utf-8"))

    def test_circle_blink_closes_vertically(self):
        """cam 板圆形眼的眨眼必须是上下合拢，不是整体缩小。

        客户反馈：「眨眼不对，应该是上下闭合，现在好像是收缩到中间」。
        原因是早期实现让宽度跟随眨眼高度（w = h），眼睛圆着往中心缩，
        看起来像镜头缩图。

        正确做法：宽度取不受眨眼影响的基准宽度（圆形下为状态直径），
        只压缩高度 → 圆变横向椭圆再变横线，即上下眼皮合拢。
        """
        display = (ROOT / "main/display/deepbot_eyes_display.cc").read_text(encoding="utf-8")

        # 圆形分支必须用 base_width（基准直径），不能用来会被压缩的 h。
        # 注意要从 ApplyEyeGeometry 函数体里定位：
        # BaseWidthForState 里也有一个同样的 if，先出现的是那个。
        func_start = display.index("void DeepbotEyesDisplay::ApplyEyeGeometry(int base_width")
        branch_start = display.index("if (style_ == EyeStyle::kCircle) {", func_start)
        branch = display[branch_start:branch_start + 900]
        self.assertIn("Clamp(base_width, EYE_MIN_H, eye_h_max_)", branch,
                      "圆形宽度必须取基准直径，否则眨眼会变成整体缩小")
        self.assertNotIn("Clamp(h,", branch, "圆形宽度不得取会被眨眼压缩的 h")

        # 眨眼计算必须保留 base_h 不变，只改 h
        tick_start = display.index("void DeepbotEyesDisplay::OnAnimTick(")
        tick = display[tick_start:display.index("void DeepbotEyesDisplay::ScheduleNextBlink(",
                                                tick_start)]
        self.assertIn("base_h = BaseHeightForState();", tick)
        self.assertIn("base_w = BaseWidthForState();", tick)
        self.assertIn("h = EYE_MIN_H + (base_h - EYE_MIN_H) * (100 - blink_pct_) / 100;", tick,
                      "眨眼只能压高度，不能动宽度")
        self.assertIn("ApplyEyeGeometry(base_w, h,", tick,
                      "必须把基准宽与压缩后高度分开传入")

    def test_camera_does_not_expose_take_photo(self):
        """不再向 McpServer 提供 Camera 对象，避免 take_photo 与观察工具冲突。"""
        source = SOURCE.read_text(encoding="utf-8")

        # GetCamera 必须返回 nullptr
        self.assertIn("Camera* GetCamera() override { return nullptr; }", source)
        self.assertNotIn("return camera_; }", source)

        # 四个观察工具必须注册，旧 MCP 入口不再上报。
        for tool in ("self.camera.start_watch", "self.camera.stop_watch",
                     "self.camera.look_current", "self.camera.look_past"):
            self.assertIn(tool, source)
        self.assertNotIn('"self.camera.get_view"', source)
        self.assertNotIn('"self.camera.look"', source)

        # 旧的拍照问答路径必须彻底移除
        # RegisterCameraTools 负责注册这些观察工具，不是已移除的拍照入口。
        for gone in ("self.camera.ask", "AskCameraAboutPicture"):
            self.assertNotIn(gone, source)

    def test_camera_tool_registered_before_tool_scan(self):
        """摄像头对象必须在板子构造函数里创建，早于工具注册扫描。"""
        source = SOURCE.read_text(encoding="utf-8")
        ctor_start = source.index("DeepbotS3CamBoard()")
        ctor_end = source.index("AudioCodec* GetAudioCodec()", ctor_start)
        ctor = source[ctor_start:ctor_end]

        # 构造函数里必须同步创建对象并注册工具
        self.assertIn("CreateCameraObject();", ctor)
        self.assertIn("RegisterCameraTools();", ctor)
        # 网络事件回调里只应做传感器初始化，不应重复创建对象
        net_start = source.index("void StartNetwork()")
        net_body = source[net_start:]
        self.assertIn("CameraInitTask", net_body)
        self.assertNotIn("CreateCameraObject()", net_body,
                         "对象创建必须早于工具注册，不能推迟到网络回调")


if __name__ == "__main__":
    unittest.main()
