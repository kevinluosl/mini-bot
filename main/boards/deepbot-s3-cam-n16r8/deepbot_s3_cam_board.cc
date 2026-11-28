#include "wifi_board.h"
#include "application.h"
#include "codecs/no_audio_codec.h"
#include "display/deepbot_eyes_display.h"
#include "display/display.h"
#include "config.h"
#include "mcp_server.h"
#include "system_info.h"

#include <algorithm>
#include <cJSON.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <esp_camera.h>
#include <mbedtls/base64.h>
#include "ov_camera_watcher.h"
#include "deepbot_head_motion.h"
#include "deepbot_sensor_focus.h"
#include <esp_console.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lcd_panel_io.h>
// tx_color 取反需要直接访问 esp_lcd_panel_io_t 的成员，
// 而 esp_lcd_panel_io.h 只给了前向声明，完整结构在 interface 头里。
#include <esp_lcd_panel_io_interface.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#if DISPLAY_USE_SH1106
#include <esp_lcd_panel_sh1106.h>
#else
// SSD1306 用 IDF 官方驱动（esp_lcd_panel_vendor.h 已包含其声明）。
// 它比 SH1106 第三方驱动多一个好处：invert_color() 是真正实现的，能反色。
#include <esp_lcd_panel_ssd1306.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define TAG "DeepbotS3Cam"

// SH1106 的 tx_color 只反转像素字节；命令和参数原样透传。LVGL 刷新线程独占缓冲。
#if DISPLAY_PIXEL_INVERT
static esp_err_t (*s_real_tx_color)(esp_lcd_panel_io_t*, int, const void*, size_t) = nullptr;

static esp_err_t InvertColorTx(esp_lcd_panel_io_t* io, int lcd_cmd, const void* color,
                               size_t color_size) {
    if (s_real_tx_color == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // 命令 / 命令参数不取反，只有像素数据（lcd_cmd == -1）才处理
    if (lcd_cmd >= 0 || color == nullptr || color_size == 0) {
        return s_real_tx_color(io, lcd_cmd, color, color_size);
    }
    static uint8_t inverted[1024];
    if (color_size > sizeof(inverted)) {
        // 超预期尺寸就不取反，避免越界；宁可选面极性错也不要崩溃
        return s_real_tx_color(io, lcd_cmd, color, color_size);
    }
    const auto* src = static_cast<const uint8_t*>(color);
    for (size_t i = 0; i < color_size; ++i) {
        inverted[i] = static_cast<uint8_t>(~src[i]);
    }
    return s_real_tx_color(io, lcd_cmd, inverted, color_size);
}
#endif

// 摄像头采集分辨率（VGA 640x480）。
#define CAMERA_SRC_W 640
#define CAMERA_SRC_H 480

// 初始化外设并协调传感器、动作和显示。
class DeepbotS3CamBoard : public WifiBoard {
private:
    DeepbotEyesDisplay* display_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    std::atomic<bool> camera_ready_{false};
    OvCameraWatcher* camera_ = nullptr;
    bool servos_ready_ = false;  // GPIO 已配置；各 LEDC 通道仅在显式命令后创建
    bool servo_timer_ready_ = false;
    bool servo1_channel_ready_ = false;
    bool servo2_channel_ready_ = false;
    esp_console_repl_t* servo_console_repl_ = nullptr;
    int servo1_angle_ = 90;
    int servo2_angle_ = 90;

    // ===== 头部动作播放器（替代旧的「基线角度」）=====
    //
    // 旧接口只能设一个目标角度，模型做不出点头、摇头这类往返动作。
    // 现在 self.head.act 把动作排进播放器，由传感器任务每 20ms 推进一拍。
    // 播放器同时记住「静止朝向」：姿态动作（look_left 等）改写它，
    // 红外/触摸结束后头回到这里，语义与旧基线一致（工具设的朝向不会被自动拉回中位）。
    //
    // 互斥：MCP 工具在应用主线程入队，传感器任务在另一任务推进，
    // 播放器内部有 deque，必须加锁。临界区只有入队/推进几微秒，不会卡住采样节奏。
    deepbot_head::HeadMotionPlayer head_motion_{deepbot_head::Limits{
        SERVO_CENTER_ANGLE - SERVO_1_MAX_OFFSET_DEG, SERVO_CENTER_ANGLE + SERVO_1_MAX_OFFSET_DEG,
        SERVO_CENTER_ANGLE - SERVO_2_MAX_OFFSET_UP_DEG, SERVO_CENTER_ANGLE + SERVO_2_MAX_OFFSET_DOWN_DEG}};
    SemaphoreHandle_t head_motion_mutex_ = xSemaphoreCreateMutex();
    bool music_playback_ = false;  // 与播放器共用互斥；停止时丢弃尚未开始的伴奏。
    std::string pending_music_action_;
    bool head_final_output_pending_ = false;  // 与播放器共用互斥；最后一拍写入 PWM 后才清除。

    // 传感器反应任务句柄（触摸 / 红外）。
    // 单独一个任务而不是放在主循环：主循环要处理音频与网络，
    // 传感器需要在互动时保持 20ms 的采样节奏。
    TaskHandle_t sensor_task_ = nullptr;

    static constexpr ledc_mode_t kServoMode = LEDC_LOW_SPEED_MODE;
    static constexpr ledc_timer_t kServoTimer = LEDC_TIMER_2;
    static constexpr ledc_channel_t kServo1Channel = LEDC_CHANNEL_4;
    static constexpr ledc_channel_t kServo2Channel = LEDC_CHANNEL_5;
    static constexpr uint32_t kServoResolutionBits = 14;
    static constexpr uint32_t kServoDutyMax = (1U << kServoResolutionBits) - 1U;
    static constexpr uint32_t kServoPeriodUs = 1000000U / SERVO_PWM_FREQ_HZ;
    static constexpr uint32_t kServoMinPulseUs = SERVO_MIN_PULSE_US;
    static constexpr uint32_t kServoMaxPulseUs = SERVO_MAX_PULSE_US;

    // 把脉宽（微秒）换算成 LEDC duty 并写到指定通道。
    // ledc_set_duty 只写寄存器，ledc_update_duty 才真正生效，两个都要调。
    esp_err_t WriteServoPulse(ledc_channel_t channel, uint32_t pulse_us) {
        const uint32_t duty = (pulse_us * kServoDutyMax) / kServoPeriodUs;
        esp_err_t err = ledc_set_duty(kServoMode, channel, duty);
        if (err == ESP_OK) {
            err = ledc_update_duty(kServoMode, channel);
        }
        return err;
    }

    // 首次显式收到控制命令时才初始化指定 PWM 通道。
    // 通道配置直接使用目标脉宽，避免从 duty=0（非法 0us 脉冲）开始。
    bool EnsureServoChannel(int servo_id, uint32_t pulse_us) {
        if (servo_id != 1 && servo_id != 2) return false;
        if (!servo_timer_ready_) {
            ledc_timer_config_t timer = {};
            timer.speed_mode = kServoMode;
            timer.duty_resolution = static_cast<ledc_timer_bit_t>(kServoResolutionBits);
            timer.timer_num = kServoTimer;
            timer.freq_hz = SERVO_PWM_FREQ_HZ;
            timer.clk_cfg = LEDC_AUTO_CLK;
            const esp_err_t err = ledc_timer_config(&timer);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "显式控制时初始化舵机定时器失败: %s", esp_err_to_name(err));
                return false;
            }
            servo_timer_ready_ = true;
        }

        bool& channel_ready = servo_id == 1 ? servo1_channel_ready_ : servo2_channel_ready_;
        if (channel_ready) return true;

        const gpio_num_t gpio = servo_id == 1 ? SERVO_1_GPIO : SERVO_2_GPIO;
        const ledc_channel_t channel = servo_id == 1 ? kServo1Channel : kServo2Channel;
        const uint32_t duty = (pulse_us * kServoDutyMax) / kServoPeriodUs;
        ledc_channel_config_t config = {};
        config.gpio_num = gpio;
        config.speed_mode = kServoMode;
        config.channel = channel;
        config.timer_sel = kServoTimer;
        config.duty = duty;  // 第一帧就是用户请求的目标值，不先输出中位或 0us
        config.hpoint = 0;
        const esp_err_t err = ledc_channel_config(&config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "舵机%d GPIO%d 通道初始化失败: %s", servo_id, gpio,
                     esp_err_to_name(err));
            return false;
        }

        // 开始输出 PWM 后，清掉启动时设的弱下拉，并把引脚驱动能力提到最强。
        //
        // 为什么：MG90S 的信号输入端阻抗可能低于 SG90，
        // 弱下拉 + 默认中等驱动能力会让 3.3V 高电平的上升沿变缓、
        // 高电平幅度被拉低，导致 MG90S 认不出脉冲（SG90 却能识别）。
        // 这两个设置只影响本引脚，不影响其它外设。
        gpio_set_pull_mode(gpio, GPIO_FLOATING);
        gpio_set_drive_capability(gpio, GPIO_DRIVE_CAP_3);
        ESP_LOGI(TAG, "舵机%d GPIO%d 已启动 PWM（浮动输入、最大驱动能力）",
                 servo_id, gpio);

        channel_ready = true;
        return true;
    }

    // 把一个角度按指定轴的行程收敛到安全范围。
    // 所有角度入口（头部动作、传感器反应）都必须过这一层，
    // 不依赖调用方自觉，避免任何路径把舵机顶到机械极限。
    //
    // 2 号俯仰的上下限**不对称**：逻辑角度小于中位 = 抬头（行程较大），
    // 大于中位 = 低头（行程较小，实测 45° 即顶死）。
    // 所以不能简单地用 中位±offset 的对称写法。
    static int ClampForAxis(int servo_id, int angle) {
        if (servo_id == 1) {
            return std::clamp(angle, SERVO_CENTER_ANGLE - SERVO_1_MAX_OFFSET_DEG,
                              SERVO_CENTER_ANGLE + SERVO_1_MAX_OFFSET_DEG);
        }
        const int up_limit = SERVO_CENTER_ANGLE - SERVO_2_MAX_OFFSET_UP_DEG;    // 抬头极限
        const int down_limit = SERVO_CENTER_ANGLE + SERVO_2_MAX_OFFSET_DOWN_DEG; // 低头极限
        return std::clamp(angle, up_limit, down_limit);
    }

    // 同时设置两个舵机的角度。
    //
    // 两个角度都算成脉宽后，连续写两个 LEDC 通道，中间不做任何 IO、不打日志。
    // 两个通道共享同一个 LEDC 定时器（TIMER_2），该周期边界上同时生效，
    // 斜向动作（如看左上）两轴真正同步，不会“先转头再抬头”。
    //
    // verbose=false 用于平滑步进路径：传感器任务每 20ms 调一次，
    // 若每次都打日志会变成 50 条/秒，把串口和时序一起堵死。
    bool SetBothServoAngles(int angle1, int angle2, bool verbose = true) {
        if (!servos_ready_) {
            ESP_LOGW(TAG, "舵机 GPIO 尚未就绪，拒绝角度命令");
            return false;
        }

        // 先按各自轴的行程限位，再算脉宽。
        // 1 号与 2 号行程不同，必须分别收敛（理由见 ClampForAxis）。
        angle1 = ClampForAxis(1, angle1);
        angle2 = ClampForAxis(2, angle2);

        const auto to_pulse = [](int angle) -> uint32_t {
            return kServoMinPulseUs +
                (static_cast<uint32_t>(angle) * (kServoMaxPulseUs - kServoMinPulseUs)) / 180U;
        };
        uint32_t pulse1 = to_pulse(angle1);
        uint32_t pulse2 = to_pulse(angle2);
        // 2 号舵机方向反转：镜像整段脉宽。
        // 镜像后“逻辑角度增大”对应“脉宽减小”，物理上就是反向，
        // 而 servo2_angle_ 仍记录逻辑角度，上层不必关心安装方向。
        if (SERVO_2_INVERT) {
            pulse2 = kServoMinPulseUs + kServoMaxPulseUs - pulse2;
        }

        // 首次调用时才会真正创建通道，之后直接返回
        if (!EnsureServoChannel(1, pulse1)) return false;
        if (!EnsureServoChannel(2, pulse2)) return false;

        // ---- 关键：两个写操作背靠背执行，中间不插任何 IO ----
        const esp_err_t err1 = WriteServoPulse(kServo1Channel, pulse1);
        const esp_err_t err2 = WriteServoPulse(kServo2Channel, pulse2);
        if (err1 != ESP_OK || err2 != ESP_OK) {
            ESP_LOGE(TAG, "双舵机同时设置失败: %s / %s",
                     esp_err_to_name(err1), esp_err_to_name(err2));
            return false;
        }

        servo1_angle_ = angle1;
        servo2_angle_ = angle2;
        if (verbose) {
            ESP_LOGI(TAG, "双舵机同时：1号=%d°(%uus) 2号=%d°(%uus)",
                     angle1, pulse1, angle2, pulse2);
        }
        return true;
    }

    // 双轴按相同步长逼近目标，避免大角度指令单拍跳变。
    void StepServoBothTo(int target1, int target2, int step_deg) {
        target1 = ClampForAxis(1, target1);
        target2 = ClampForAxis(2, target2);

        const auto step_toward = [step_deg](int current, int target) -> int {
            if (current == target) return target;
            int next = current + (target > current ? step_deg : -step_deg);
            // 防止步进越过目标后来回振荡
            if ((target > current && next > target) ||
                (target < current && next < target)) {
                next = target;
            }
            return next;
        };

        const int next1 = step_toward(servo1_angle_, target1);
        const int next2 = step_toward(servo2_angle_, target2);
        // 两个都已到位就不写寄存器，减少无意义的 LEDC 更新
        if (next1 == servo1_angle_ && next2 == servo2_angle_) return;

        SetBothServoAngles(next1, next2, /*verbose=*/false);
    }

    // 观察工具只管理推帧会话，look 工具才提交问题。
    void RegisterCameraTools() {
        auto& mcp = McpServer::GetInstance();

        mcp.AddTool("self.camera.start_watch",
                    "开始持续上传画面，无需参数。不自动生成画面描述；之后用 look_current 看当下，或用 look_past 回看。",
                    PropertyList(),
                    [this](const PropertyList&) -> ToolResult {
                        if (camera_ == nullptr || !camera_ready_) {
                            return std::unexpected("摄像头未就绪，无法开始观察");
                        }
                        if (auto r = camera_->Start(); !r) {
                            return std::unexpected("开始观察失败: " + r.error());
                        }
                        return ReturnValue(std::string("已启动持续观察"));
                    });

        mcp.AddTool("self.camera.stop_watch",
                    "停止持续观察画面，后台关闭视觉网关会话。"
                    "用户说“不用看了”“关上眼睛”时调用。",
                    PropertyList(),
                    [this](const PropertyList&) -> ToolResult {
                        if (camera_ == nullptr) {
                            return std::unexpected("摄像头未就绪");
                        }
                        camera_->Stop();
                        return ReturnValue(std::string("已请求停止观察和监控提醒，不再接受画面查询"));
                    });

        mcp.AddTool(std::make_unique<McpTool>("self.camera.start_monitor",
            "用户说‘看到某情况时提醒我’才开启持续监控提醒；不是问当前或过去。自动开启上传画面，"
            "condition 原样发给视觉网关；语音只写‘看到什么情况，就提醒’，不要添加建议或播报内容。"
            "用户明确指定怎么提醒时，将其原话单独写入 reminder；这段文字只在命中后用于播报。"
            "视觉网关每次命中后，会先等网关接收新画面，再用原条件恢复监控，不用再次启用。"
            "用户说‘取消提醒’用 stop_monitor；不要创建周期定时任务。",
            PropertyList({Property("condition", kPropertyTypeString),
                          Property("reminder", kPropertyTypeString, std::string{}).SetMaxLength(2048)}),
            [this](const PropertyList& args) -> ToolResult {
                if (!camera_ || !camera_ready_) return std::unexpected("摄像头未就绪");
                auto result = camera_->StartMonitor(args["condition"].value<std::string>(),
                                                    args["reminder"].value<std::string>());
                if (!result) return std::unexpected(result.error());
                return ReturnValue(std::string("已请求启用监控提醒，等待网关确认；尚未检测到目标。"));
            }));
        mcp.AddTool(std::make_unique<McpTool>("self.camera.stop_monitor",
            "关闭监控提醒，不停止持续上传。用户只要求取消提醒时调用。",
            PropertyList(), [this](const PropertyList&) -> ToolResult {
                if (!camera_ || !camera_ready_) return std::unexpected("摄像头未就绪");
                camera_->StopMonitor();
                return ReturnValue(std::string("已停用监控提醒，持续观察状态不变。"));
            }));

        // 只给当前画面工具开放自定义 question；回看继续使用板端固定问题。
        // 非 ok 以工具错误返回，服务端不拿它做有人判断，也不当画面线索。
        auto add_look = [this, &mcp](const char* name, const char* description, bool past) {
            PropertyList properties;
            if (!past) {
                properties.AddProperty(Property("question", kPropertyTypeString, std::string{}));
            }
            auto tool = std::make_unique<McpTool>(name, description, properties,
                [this, past](const PropertyList& arguments) -> ToolResult {
                    if (!camera_ || !camera_ready_) return std::unexpected("摄像头未就绪");
                    std::string session_id;
                    auto seen = past ? camera_->LookPast()
                                     : camera_->LookCurrent(arguments["question"].value<std::string>(), &session_id);
                    if (!seen) return std::unexpected(seen.error());
                    if (past) return ReturnValue(std::move(*seen));
                    return ReturnValue(McpTextWithMeta{std::move(*seen), "ov_session_id", std::move(session_id)});
                });
            tool->set_background(true);
            tool->set_result_mime_type("text/plain");
            mcp.AddTool(std::move(tool));
        };
        add_look("self.camera.look_current",
                 "在已开启的观察会话中看一眼当前画面。question 为可选字符串；"
                 "有明确问题就原样传入 question；省略时，简短描述画面里的人、活动和可能的情绪。"
                 "用户说‘这个’‘这样’指着眼前的东西或动作，或提出具体的当前画面问题时调用。", false);
        add_look("self.camera.look_past",
                 "在已开启的观察会话中回看过去约一分钟，无参数，总结画面前的人在做什么，无人时不播报结果。"
                 "用户没有具体问题、想知道刚才在做什么时调用。", true);

        // 本地调试取图统一使用串口 camdump，不再注册开发机 HTTP 上传工具。
    }

    // 头部动作异步播放；返回空文本，避免为动作回执另起一轮播报。
    void RegisterHeadTools() {
        std::string actions;
        for (auto name : deepbot_head::ActionNames()) {
            if (!actions.empty()) actions += "/";
            actions += name;
        }
        McpServer::GetInstance().AddTool("self.head.act",
            "做表情或头部动作，配套效果自动同步。立即返回，可边说边做。action 取 " + actions + "。"
            "look_* 转过去并保持（look_around 抬头扫视后回原朝向）；look_center 回正；stop 立刻停下回正；"
            "其余动作做完回到原朝向：点头 nod、摇头 shake、好奇 curious、思考 think、害羞 shy、"
            "低落 sad、开心 happy、惊讶 surprised、致谢 bow、犯困 sleepy、摇摆 dance、倾听 listen、"
            "生气 angry、垂头抽泣 crying。"
            "level 1 普通、2 使劲。一轮可连发几个，按顺序执行，最多排 4 个。",
            PropertyList({
                Property("action", kPropertyTypeString, std::string{"nod"}),
                // 只有两档：三档时模型分不清“中”和“强”，默认普通。
                Property("level", kPropertyTypeInteger, 1, 1, 2),
            }),
            [this](const PropertyList& p) -> ToolResult {
                const std::string action = p["action"].value<std::string>();
                const int level = p["level"].value<int>();
                std::string error = EnqueueHeadAction(action, level);
                if (!error.empty()) return std::unexpected(std::move(error));
                return ReturnValue(std::string{});
            });
    }

    // 工具与串口共用的入队入口。返回空串表示成功，否则是中文错误原因。
    std::string EnqueueHeadAction(const std::string& action, int level) {
        if (!servos_ready_) return "舵机未就绪";
        if (head_motion_mutex_ == nullptr || sensor_task_ == nullptr) return "头部动作未初始化";
        std::string error;
        xSemaphoreTake(head_motion_mutex_, portMAX_DELAY);
        error = head_motion_.Enqueue(action, level);
        xSemaphoreGive(head_motion_mutex_);
        if (error.empty()) {
            ESP_LOGI(TAG, "头部动作入队：%s 强度%d", action.c_str(), level);
        } else {
            ESP_LOGW(TAG, "头部动作被拒：%s（%s）", action.c_str(), error.c_str());
        }
        return error;
    }

    // 舵机 PWM 与摄像头 XCLK 的 LEDC 资源已隔离：摄像头用 TIMER_1/CHANNEL_1，
    // 舵机用 TIMER_2/CHANNEL_4/5，互不影响。
    //
    // UART0（CH340）命令行：head（头部动作）、sensor、camwatch、camdump。
    // 启动 REPL 本身不初始化 PWM，不会让舵机动作。
    // 旧的 servo / pwmt / servotest 调试命令已删除：舵机硬件早已验证，
    // 日常只需要按动作名测试，和 self.head.act 走同一条入队路径。
    void InitializeServoConsole() {
        const esp_console_cmd_t head_cmd = {
            .command = "head",
            .help = "head <动作> [强度1-2]：同步执行头部动作和表情；head list 列出动作",
            .hint = nullptr,
            .func = nullptr,
            .argtable = nullptr,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                auto* self = static_cast<DeepbotS3CamBoard*>(context);
                if (argc < 2 || std::strcmp(argv[1], "list") == 0) {
                    for (auto name : deepbot_head::ActionNames()) {
                        printf("%.*s ", static_cast<int>(name.size()), name.data());
                    }
                    printf("\nusage: head <动作> [1|2]\n");
                    return argc < 2 ? 1 : 0;
                }
                int level = 1;
                if (argc >= 3) {
                    if (argv[2][0] < '1' || argv[2][0] > '2' || argv[2][1] != '\0') {
                        printf("强度只能是 1（普通）或 2（使劲）\n");
                        return 1;
                    }
                    level = argv[2][0] - '0';
                }
                const std::string error = self->EnqueueHeadAction(argv[1], level);
                printf("head %s %d: %s\n", argv[1], level, error.empty() ? "OK" : error.c_str());
                return error.empty() ? 0 : 1;
            },
            .context = this,
        };
        esp_err_t err = esp_console_cmd_register(&head_cmd);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "注册 UART head 命令失败: %s", esp_err_to_name(err));
            return;
        }

        // sensor：打印触摸与两路红外的当前电平及解析结果。
        //
        // 为什么需要：红外模块型号很多，输出极性不一（有的有物体输出低，
        // 有的输出高）。接好后先看这里的原始电平，就能确认到底该不该
        // 把 INFRARED_ACTIVE_LOW 置 1，不用猜。
        // 同时能看到触摸是否有响应，用于确认 TTP223 已正常上电。
        const esp_console_cmd_t sensor_cmd = {
            .command = "sensor",
            .help = "sensor：打印触摸 / 右红外 / 左红外 的原始电平与解析状态",
            .hint = nullptr,
            .func = nullptr,
            .argtable = nullptr,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                (void)context; (void)argc; (void)argv;
                const int touch = gpio_get_level(TOUCH_GPIO);
                const int right = gpio_get_level(INFRARED_RIGHT_GPIO);
                const int left = gpio_get_level(INFRARED_LEFT_GPIO);
                const bool r_trig = ReadInfraredTriggered(INFRARED_RIGHT_GPIO);
                const bool l_trig = ReadInfraredTriggered(INFRARED_LEFT_GPIO);
                printf("触摸 GPIO%d : 电平=%d  %s\n", TOUCH_GPIO, touch,
                       touch ? "被触摸" : "未触摸");
                printf("右红外 GPIO%d: 电平=%d  %s\n", INFRARED_RIGHT_GPIO, right,
                       r_trig ? "检测到物体" : "无物体");
                printf("左红外 GPIO%d : 电平=%d  %s\n", INFRARED_LEFT_GPIO, left,
                       l_trig ? "检测到物体" : "无物体");
                printf("（极性：INFRARED_ACTIVE_LOW=%d，若逻辑与实际相反请改 config.h）\n",
                       (int)INFRARED_ACTIVE_LOW);
                return 0;
            },
            .context = this,
        };
        err = esp_console_cmd_register(&sensor_cmd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "注册 sensor 命令失败: %s", esp_err_to_name(err));
        }

        // camwatch：本地控制持续观察。
        //
        // 用法：camwatch start | camwatch stop | camwatch view | camwatch
        // 不带参数时默认打印当前状态与已缓存的答案。
        //
        // 为什么需要：MCP 工具只能由服务端/大模型主动调用，
        // 想单独验证“持续观察→网关→答案缓存”这段是否通，
        // 串口 view 是只读诊断：不模拟 MCP 提问，不消耗一次视觉问答。
        const esp_console_cmd_t camwatch_cmd = {
            .command = "camwatch",
            .help = "camwatch start|stop|view|ask [问题] [scope]|look|past：启动/停止观察、查状态，或看现在/回看刚才",
            .hint = nullptr,
            .func = nullptr,
            .argtable = nullptr,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                auto* self = static_cast<DeepbotS3CamBoard*>(context);
                if (self->camera_ == nullptr || !self->camera_ready_) {
                    printf("摄像头未就绪（等 WiFi 连上后再试）\n");
                    return 1;
                }

                const char* action = (argc >= 2) ? argv[1] : "view";

                if (std::strcmp(action, "start") == 0) {
                    if (auto r = self->camera_->Start(); !r) {
                        printf("启动失败: %s\n", r.error().c_str());
                        return 1;
                    }
                    printf("已启动持续观察，每秒一帧，缓存最近 5 条答案\n");
                    return 0;
                }
                if (std::strcmp(action, "stop") == 0) {
                    self->camera_->Stop();
                    printf("已请求停止观察\n");
                    return 0;
                }
                // 自定义时间范围只留在本地串口调试，不向模型开放。
                // camwatch ask <问题> [current|past|auto]，第三个参数省略时看当下。
                if (std::strcmp(action, "ask") == 0) {
                    printf("%s\n", self->camera_->GetView(argc > 2 ? argv[2] : "",
                                                          argc > 3 ? argv[3] : "current").c_str());
                    return 0;
                }
                // look = look_current，past = look_past，与 MCP 工具走同一条提问路径。
                if (std::strcmp(action, "look") == 0 || std::strcmp(action, "past") == 0) {
                    const auto seen = std::strcmp(action, "past") == 0 ? self->camera_->LookPast()
                                                                         : self->camera_->LookCurrent();
                    printf("%s%s\n", seen ? "" : "失败：", seen ? seen->c_str() : seen.error().c_str());
                    return 0;
                }
                if (std::strcmp(action, "view") == 0) {
                    printf("当前状态: %s\n",
                           self->camera_->IsWatching() ? "观察中" : "未启动");
                    printf("缓存答案: %s\n",
                           self->camera_->GetRecentAnswersJson().c_str());
                    return 0;
                }
                printf("未知参数 %s，可用: start / stop / view / ask / look / past\n", action);
                return 1;
            },
            .context = this,
        };
        err = esp_console_cmd_register(&camwatch_cmd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "注册 camwatch 命令失败: %s", esp_err_to_name(err));
        }

        // camdump：把设备真正拍到的画面通过串口传回本机。
        //
        // 为什么需要：云端网关的识别结果可能不可靠（实测三张差异极大的图
        // 返回了同一个答案），这时必须能拿到设备原始 JPEG 肉眼确认，
        // 才能区分是“摄像头拍错了”还是“网关识别错了”。
        //
        // 格式：首行 CAMDUMP_BEGIN <字节数> <base64长度>，中间每行 76 字符，
        // 末行 CAMDUMP_END。本机脚本按这两个标记提取并解码即可。
        const esp_console_cmd_t camdump_cmd = {
            .command = "camdump",
            .help = "camdump：拍一张并以 base64 输出到串口，便于本地取回检查画面",
            .hint = nullptr,
            .func = nullptr,
            .argtable = nullptr,
            .func_w_context = [](void* context, int argc, char** argv) -> int {
                auto* self = static_cast<DeepbotS3CamBoard*>(context);
                if (self->camera_ == nullptr || !self->camera_ready_) {
                    printf("摄像头未就绪\n");
                    return 1;
                }
                auto jpeg = self->camera_->CaptureJpeg();
                if (!jpeg) {
                    printf("拍照失败: %s\n", jpeg.error().c_str());
                    return 1;
                }
                const size_t len = jpeg->size();
                const auto* data = reinterpret_cast<const unsigned char*>(jpeg->data());

                // base64 膨胀到 4/3，再留 4 字节对齐余量
                const size_t b64_cap = ((len + 2) / 3) * 4 + 4;
                auto* b64 = static_cast<unsigned char*>(malloc(b64_cap));
                if (b64 == nullptr) {
                    printf("内存不足（%u 字节）\n", (unsigned)b64_cap);
                    return 1;
                }
                size_t b64_len = 0;
                const int rc = mbedtls_base64_encode(b64, b64_cap, &b64_len, data, len);
                if (rc != 0) {
                    printf("base64 编码失败: %d\n", rc);
                    free(b64);
                    return 1;
                }

                printf("CAMDUMP_BEGIN %u %u\n", (unsigned)len, (unsigned)b64_len);
                // 每行 76 字符，避免单行过长被串口缓冲截断
                constexpr size_t kLine = 76;
                for (size_t i = 0; i < b64_len; i += kLine) {
                    const size_t n = (b64_len - i < kLine) ? (b64_len - i) : kLine;
                    fwrite(b64 + i, 1, n, stdout);
                    fputc('\n', stdout);
                }
                printf("CAMDUMP_END\n");
                fflush(stdout);
                free(b64);
                return 0;
            },
            .context = this,
        };
        err = esp_console_cmd_register(&camdump_cmd);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "注册 camdump 命令失败: %s", esp_err_to_name(err));
        }

        esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
        repl_config.prompt = "cam-servo>";
        repl_config.max_cmdline_length = 128;
        esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
        err = esp_console_new_repl_uart(&uart_config, &repl_config, &servo_console_repl_);
        if (err == ESP_OK) err = esp_console_start_repl(servo_console_repl_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "启动 UART servo REPL 失败: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "UART 命令已启用（115200 baud）：head / sensor / camwatch / camdump");
        // 说明：曾尝试同时开一个 USB-Serial-JTAG 控制台，让原生 USB 也能发命令，
        // 但 esp_console_new_repl_usb_serial_jtag 受 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG 保护，
        // 而本板 primary console 是 UART0（该宏未开），编译不过。
        // 要改只能把 primary console 切到 USB，代价是 UART0 失去日志，得不偿失。
        // 因此维持分工：UART0(CH340) 发命令，原生 USB 看日志+烧录。
    }

    // 上电只把两根信号脚设为高阻输入并开启弱下拉，避免信号悬空；不初始化 LEDC、不输出 PWM。
    // 只有头部动作或传感器互动第一次需要转头时，EnsureServoChannel 才创建 PWM 通道。
    void InitializeServos() {
        const gpio_num_t pins[] = {SERVO_1_GPIO, SERVO_2_GPIO};
        for (gpio_num_t pin : pins) {
            gpio_reset_pin(pin);
            gpio_config_t config = {};
            config.pin_bit_mask = 1ULL << pin;
            config.mode = GPIO_MODE_INPUT;
            config.pull_up_en = GPIO_PULLUP_DISABLE;
            config.pull_down_en = GPIO_PULLDOWN_ENABLE;
            config.intr_type = GPIO_INTR_DISABLE;
            const esp_err_t err = gpio_config(&config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "舵机信号 GPIO%d 高阻初始化失败: %s", pin,
                         esp_err_to_name(err));
                return;
            }
        }
        servos_ready_ = true;
        ESP_LOGI(TAG,
                 "舵机已就绪但未输出 PWM：GPIO%d/GPIO%d 保持高阻；"
                 "仅头部动作或传感器互动会启动舵机",
                 SERVO_1_GPIO, SERVO_2_GPIO);
    }

    void InitializeSensors() {
        struct PinInit {
            gpio_num_t pin;
            const char* name;
            bool pull_up;
        };
        // TTP223 自身输出推挽，不需要上下拉；
        // 两个红外模块输出可能开路（板上已有上拉），这里开内部上拉更塌实，
        // 保证无模块接入时读到“无触发”电平而不是悬空乱跳。
        const PinInit pins[] = {
            {TOUCH_GPIO, "触摸", false},
            {INFRARED_RIGHT_GPIO, "右红外", true},
            {INFRARED_LEFT_GPIO, "左红外", true},
        };

        for (const auto& p : pins) {
            gpio_reset_pin(p.pin);
            gpio_config_t cfg = {};
            cfg.pin_bit_mask = 1ULL << p.pin;
            cfg.mode = GPIO_MODE_INPUT;
            cfg.pull_up_en = p.pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
            cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
            cfg.intr_type = GPIO_INTR_DISABLE;
            const esp_err_t err = gpio_config(&cfg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "%s GPIO%d 输入初始化失败: %s", p.name, p.pin,
                         esp_err_to_name(err));
                return;
            }
        }
        ESP_LOGI(TAG, "传感器就绪：触摸=GPIO%d 右红外=GPIO%d 左红外=GPIO%d",
                 TOUCH_GPIO, INFRARED_RIGHT_GPIO, INFRARED_LEFT_GPIO);
    }

    // 读取红外是否检测到物体。
    //
    // 模块电平极性因型号而异，所以用宏 INFRARED_ACTIVE_LOW 控制，
    // 默认按常见的“有物体输出低”处理。
    static bool ReadInfraredTriggered(gpio_num_t pin) {
        const int level = gpio_get_level(pin);
#if INFRARED_ACTIVE_LOW
        return level == 0;
#else
        return level != 0;
#endif
    }

    // 传感器任务主循环。
    //
    // 设计要点：
    // 1) 非阻塞：每个 tick 只让舵机向目标走一小步（StepServoTo），
    //    而不是阻塞式插值过去，否则采样节奏会被打断、漏掉触摸与红外输入。
    // 2) 去抖：连续若干次一致才认可状态变化（触摸短、红外长）。
    //    机械与红外模块的边沿常伴噪声，不去抖会出现头部抽搐。
    // 3) 优先级：触摸 > 红外。两者同时发生时按“被摸头”处理，
    //    因为那是人主动的互动，不应该被旁边经过的物体抢走。
    // 4) 只在本任务内驱动舵机与表情，不与其他任务竞争。
    void SensorLoop() {
        constexpr int kTickMs = 20;              // 采样周期
        constexpr int kStepDeg = 2;              // 每 tick 最大步进（平滑度）

        // 触发和释放各需连续 3 拍确认，以滤除红外边缘抖动。
        constexpr int kTriggerDebounceTicks = 3;  // 60ms，触发需连续确认
        constexpr int kReleaseDebounceTicks = 3;  // 60ms，释放需连续确认

        int touch_stable = 0;      // 稳定的触摸计数
        int ir_right_stable = 0;
        int ir_left_stable = 0;
        bool touch_on = false;     // 去抖后的当前状态
        bool ir_right_on = false;
        bool ir_left_on = false;

        int swing_direction = 1;   // 摆动方向：+1 / -1
        int swing_elapsed_ms = 0;
        DeepbotSensorFocus sensor_focus;
        using SensorInput = DeepbotSensorFocus::Input;
        bool was_touch = false;    // 上一 tick 实际生效的摸头状态
        // 上一 tick 已记录过的红外状态，避免每 20ms 重复打印日志
        bool ir_right_logged = false;
        bool ir_left_logged = false;

        // 低频诊断：每 3 秒打印一次三个传感器的原始电平。
        //
        // 为什么需要：TTP223 模块有 A/B 焊盘可以选“高有效/低有效/自锁”，
        // 出厂不一定是我们假定的高有效。没有原始电平就无法区分
        // “模块没接好”“极性反了”“逻辑写错”这三种情况。
        // 3 秒一次而不是每 tick，避免刷屏影响时序。
        constexpr int kDiagEveryTicks = 150;  // 150 * 20ms = 3s
        int diag_counter = 0;

        ESP_LOGI(TAG, "传感器任务启动（触摸/红外反应逻辑已启用）");

        // 上电静默期：两个原因，都必须等。
        // 1) TTP223 上电有短暂自校准，期间输出电平不可信，
        //    不等就可能在上电瞬间被当成“被触摸”而误动作。
        // 2) 避免舵机在 OLED / 网络还没起来时就跳位置，
        //    看起来像开机抽搐。
        vTaskDelay(pdMS_TO_TICKS(2000));

        // 静默结束后重新同步一次状态，把静默期的变化当成基线，
        // 不当作“新事件”触发动作。
        touch_on = gpio_get_level(TOUCH_GPIO) != 0;
        ir_right_on = ReadInfraredTriggered(INFRARED_RIGHT_GPIO);
        ir_left_on = ReadInfraredTriggered(INFRARED_LEFT_GPIO);
        sensor_focus.Reset(touch_on, ir_left_on, ir_right_on);
        ESP_LOGI(TAG, "传感器基线：触摸=%d 右红外=%d 左红外=%d",
                 touch_on, ir_right_on, ir_left_on);
        // 基线就是起点，不算“变化”，否则上电会先打一条误导性的红外变化日志
        ir_right_logged = ir_right_on;
        ir_left_logged = ir_left_on;

        while (true) {
            // ---- 1. 采样并去抖 ----
            const bool touch_raw = gpio_get_level(TOUCH_GPIO) != 0;
            const bool ir_right_raw = ReadInfraredTriggered(INFRARED_RIGHT_GPIO);
            const bool ir_left_raw = ReadInfraredTriggered(INFRARED_LEFT_GPIO);

            // 低频诊断：直接打印 gpio_get_level 的原始值（未经极性转换）
            if (++diag_counter >= kDiagEveryTicks) {
                diag_counter = 0;
                ESP_LOGI(TAG, "传感器原始电平：触摸GPIO%d=%d 右红外GPIO%d=%d 左红外GPIO%d=%d",
                         TOUCH_GPIO, gpio_get_level(TOUCH_GPIO),
                         INFRARED_RIGHT_GPIO, gpio_get_level(INFRARED_RIGHT_GPIO),
                         INFRARED_LEFT_GPIO, gpio_get_level(INFRARED_LEFT_GPIO));
            }

            // 三路独立计数，触发和释放可分别调参。
            const auto debounce = [](bool raw, bool& state, int& stable) {
                if (raw == state) {
                    stable = 0;  // 状态一致，计数清零
                    return;
                }
                // 目标状态与本拍读数不一致，累积确认拍数
                if (++stable < (raw ? kTriggerDebounceTicks : kReleaseDebounceTicks)) {
                    return;
                }
                state = raw;
                stable = 0;
            };
            debounce(touch_raw, touch_on, touch_stable);
            debounce(ir_right_raw, ir_right_on, ir_right_stable);
            debounce(ir_left_raw, ir_left_on, ir_left_stable);

            // 头部工具优先；传感器临时接管，释放后回到最近一次工具设定的朝向。
            int target1 = SERVO_CENTER_ANGLE;   // 1 号：横向
            int target2 = SERVO_CENTER_ANGLE;   // 2 号：俯仰
            bool motion_active = false;
            bool finishing = false;
            const char* started_emotion = nullptr;
            if (head_motion_mutex_ != nullptr) {
                int pan = servo1_angle_;
                int tilt = servo2_angle_;
                xSemaphoreTake(head_motion_mutex_, portMAX_DELAY);
                if (!pending_music_action_.empty()) {
                    if (music_playback_ && !head_motion_.Busy() && !head_final_output_pending_) {
                        // 不生成伴奏表情，避免末拍跨过音乐停止时闪出另一张脸。
                        head_motion_.Enqueue(pending_music_action_, 1, /*show_emotion=*/false);
                    }
                    pending_music_action_.clear();
                }
                motion_active = head_motion_.Tick(kTickMs, pan, tilt, &started_emotion);
                finishing = motion_active && !head_motion_.Busy();
                head_final_output_pending_ = finishing;
                if (motion_active) {
                    target1 = pan;
                    target2 = tilt;
                } else {
                    target1 = head_motion_.RestPan();
                    target2 = head_motion_.RestTilt();
                }
                xSemaphoreGive(head_motion_mutex_);
            }

            const auto interaction = sensor_focus.Tick(touch_on, ir_left_on, ir_right_on);
            const bool head_touched = interaction.active == SensorInput::kHead;
            const bool face_right = interaction.active == SensorInput::kRight;
            const bool face_left = interaction.active == SensorInput::kLeft;

            if (motion_active) {
                // 动作播放器已给出本拍角度，传感器不接管舵机。
            } else if (head_touched) {
                // ---- 触摸：微笑 + 像被摸头一样轻微摆动 ----
                //
                // 每半个周期翻转一次摆动方向，形成往返。
                // 只改目标角度、不直接写舵机，由 StepServoBothTo 平滑追上去。
                swing_elapsed_ms += kTickMs;
                if (swing_elapsed_ms >= REACT_TOUCH_SWING_MS) {
                    swing_elapsed_ms = 0;
                    swing_direction = -swing_direction;
                }
                // 摸头摆动围绕机械中位，不叠加工具设定的朝向。
                target1 = SERVO_CENTER_ANGLE + swing_direction * REACT_TOUCH_SWING_X_DEG;
                // 逻辑角度增大为低头：第一拍偏头时先收下巴。
                target2 = SERVO_CENTER_ANGLE + swing_direction * REACT_TOUCH_SWING_Y_DEG;
            } else if (face_left || face_right) {
                // 朝唯一接管的红外侧转头，自动动作不使用舵机的最大行程。
                target1 = SERVO_CENTER_ANGLE +
                    (face_right ? REACT_IR_TURN_DEG : -REACT_IR_TURN_DEG);
                // 逻辑角度减小为抬头。
                target2 = SERVO_CENTER_ANGLE - REACT_IR_TILT_DEG;
            }

            // ---- 3. 处理状态切换时的副作用 ----
            if (head_touched != was_touch) {
                was_touch = head_touched;
                // 触摸只维护自己的微笑；松手不能清掉工具触发的表情。
                if (display_ != nullptr) display_->SetTouchSmile(head_touched);
                if (!head_touched) {
                    swing_elapsed_ms = 0;
                    swing_direction = 1;
                    ESP_LOGI(TAG, "触摸结束 → 恢复静止朝向");
                } else {
                    ESP_LOGI(TAG, "检测到触摸 → 害羞摆头");
                }
            }

            // 只有抢到控制权的边沿发一次通知；忙时或被挡住的边沿不会补报。
            if (interaction.started && !motion_active) {
                const char* notification = head_touched
                    ? R"({"jsonrpc":"2.0","method":"notifications/touch/head"})"
                    : face_right
                        ? R"({"jsonrpc":"2.0","method":"notifications/infrared/turn","params":{"side":"right"}})"
                        : R"({"jsonrpc":"2.0","method":"notifications/infrared/turn","params":{"side":"left"}})";
                Application::GetInstance().SendServerMcpNotification(notification);
            }

            // 红外状态边沿日志。
            //
            // 为什么要打：红外触发时头会转向 + 抬头，但舵机是平滑步进的，
            // 单看结果很难分清“没检测到”和“检测到了但方向不对”。
            // 把原始电平与解析结果一起打出来，接错极性或模块没上电时一眼能看出。
            if (ir_right_on != ir_right_logged || ir_left_on != ir_left_logged) {
                // 只在电平边沿记录目标和当前角度，便于区分限位与机械问题。
                ESP_LOGI(TAG, "红外变化：右=%d(电平%d) 左=%d(电平%d) "
                              "目标=[1号%d° 2号%d°] 当前=[1号%d° 2号%d°]",
                         ir_right_on, gpio_get_level(INFRARED_RIGHT_GPIO),
                         ir_left_on, gpio_get_level(INFRARED_LEFT_GPIO),
                         target1, target2, servo1_angle_, servo2_angle_);
                ir_right_logged = ir_right_on;
                ir_left_logged = ir_left_on;
            }

            // 眼神随接管侧或头部动作更新；小幅摆动不切换视线。
            constexpr int kGazeFollowDeg = 10;
            if (display_ != nullptr) {
                if (motion_active || interaction.active == SensorInput::kNone) {
                    const int offset = target1 - SERVO_CENTER_ANGLE;
                    display_->SetGazeDirection(offset > kGazeFollowDeg ? +1
                                               : offset < -kGazeFollowDeg ? -1 : 0);
                } else if (face_right) {
                    display_->SetGazeDirection(+1);   // 头右转 → 眼往右看
                } else if (face_left) {
                    display_->SetGazeDirection(-1);   // 头左转 → 眼往左看
                } else {
                    display_->SetGazeDirection(0);    // 触摸 → 恢复自动漂移
                }
            }

            // ---- 5. 驱动舵机（两个轴同帧更新）----
            //
            // 必须用批量版：分别写两个轴时中间插一条日志，1 号与 2 号就会先后生效，
            // 看起来像“先转头再抬头”，而不是斜着一次性到位。
            // 动作播放器已按自己的速度算好本拍角度（每拍 ≤5°），直接写；
            // 其余情况仍按 kStepDeg 平滑逼近。
            // 只在动作开播时切换眼型；不要在舵机锁里拿显示锁。
            if (started_emotion != nullptr && display_ != nullptr) {
                display_->StartHeadActionEmotion(started_emotion);
            }
            if (motion_active) {
                if (target1 != servo1_angle_ || target2 != servo2_angle_) {
                    SetBothServoAngles(target1, target2, /*verbose=*/false);
                }
            } else {
                StepServoBothTo(target1, target2, kStepDeg);
            }
            if (finishing) {
                if (display_ != nullptr) display_->FinishHeadActionEmotion();
                xSemaphoreTake(head_motion_mutex_, portMAX_DELAY);
                head_final_output_pending_ = false;
                xSemaphoreGive(head_motion_mutex_);
                Application::GetInstance().NotifyToolActivityDone();
            }

            vTaskDelay(pdMS_TO_TICKS(kTickMs));
        }
    }

    static void SensorTaskEntry(void* arg) {
        static_cast<DeepbotS3CamBoard*>(arg)->SensorLoop();
    }

    // 创建传感器任务。
    //
    // 栈 4096：触摸边沿会更新显示（涉及 LVGL 锁），
    // 给足余量避免溢出。优先级 4 低于音频/网络任务，不抢实时性。
    bool StartSensorTask() {
        if (sensor_task_ != nullptr) return true;
        const BaseType_t ok = xTaskCreatePinnedToCore(
            SensorTaskEntry, "sensor_react", 4096, this, 4, &sensor_task_, 1);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "传感器任务创建失败");
            sensor_task_ = nullptr;
            return false;
        }
        return true;
    }

    // ---- 1.3 寸 128x64 单色 OLED (I2C 4 针：VCC / GND / SCL / SDA)----
    void InitializeDisplay() {
        ESP_LOGI(TAG, "OLED 初始化: I2C SDA=GPIO%d SCL=GPIO%d addr=0x%02X", DISPLAY_SDA_PIN,
                 DISPLAY_SCL_PIN, DISPLAY_I2C_ADDR);

        i2c_master_bus_config_t bus_config = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));

        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = DISPLAY_I2C_ADDR,
            .scl_speed_hz = DISPLAY_I2C_SCL_HZ,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;  // 4 针模块没有独立复位脚
        panel_config.bits_per_pixel = 1;            // 单色：1bit/像素

#if DISPLAY_USE_SH1106
        // 1.3 寸 128x64 绝大多数是 SH1106。它的显存是 132 列，写数据要 +2 列偏移，
        // 驱动内部已处理；用 SSD1306 驱动会整体错位 2 像素并留黑边。
        esp_lcd_panel_sh1106_config_t sh1106_config = {};
        panel_config.vendor_config = &sh1106_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_));
#else
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_));
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            // 屏没接好 / 没通电 / 地址不对时不要卡死在断言里，
            // 退回无显示继续启动，便于从日志区分接线问题与驱动问题。
            ESP_LOGE(TAG, "OLED 初始化失败（检查 VCC 是否 3.3V、SDA=GPIO%d / SCL=GPIO%d、地址 0x%02X）",
                     DISPLAY_SDA_PIN, DISPLAY_SCL_PIN, DISPLAY_I2C_ADDR);
            display_ = nullptr;
            return;
        }

        // 面板显示极性：靠像素取反解决，不依赖面板命令。
        //
        // 实测这块屏对 0xA6/0xA7 均无反应（两次烧录验证，I2C 无报错），
        // 所以改用替换 tx_color 在数据层取反的方案（见文件顶部 InvertColorTx）。
        // 效果：背景位=1 -> 取反熄灭（黑底），内容位=0 -> 取反点亮（亮眼睛）。
#if DISPLAY_PIXEL_INVERT
        s_real_tx_color = panel_io_->tx_color;
        panel_io_->tx_color = InvertColorTx;
        ESP_LOGI(TAG, "OLED 像素取反已启用（背景熄灭/内容点亮）");
#endif

        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

        // 单色屏用满 64 行，并选圆形眼型。
        display_ = new DeepbotEyesDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                          DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, 0,
                                          DeepbotEyesDisplay::EyeStyle::kCircle);
        ESP_LOGI(TAG, "OLED 已初始化，眼睛界面就绪");
    }

    // 创建对象不操作硬件；连上 Wi-Fi 后由异步任务初始化 OV2640。
    void CreateCameraObject() {
        camera_config_t camera_config = {
            .pin_pwdn = CAMERA_PIN_PWDN,
            .pin_reset = CAMERA_PIN_RESET,
            .pin_xclk = CAMERA_PIN_XCLK,
            .pin_sccb_sda = CAMERA_PIN_SIOD,
            .pin_sccb_scl = CAMERA_PIN_SIOC,

            .pin_d7 = CAMERA_PIN_D7,
            .pin_d6 = CAMERA_PIN_D6,
            .pin_d5 = CAMERA_PIN_D5,
            .pin_d4 = CAMERA_PIN_D4,
            .pin_d3 = CAMERA_PIN_D3,
            .pin_d2 = CAMERA_PIN_D2,
            .pin_d1 = CAMERA_PIN_D1,
            .pin_d0 = CAMERA_PIN_D0,
            .pin_vsync = CAMERA_PIN_VSYNC,
            .pin_href = CAMERA_PIN_HREF,
            .pin_pclk = CAMERA_PIN_PCLK,

            .xclk_freq_hz = XCLK_FREQ_HZ,
            // 摄像头和舵机分用 LEDC 定时器，避免 PWM 配置互相覆盖。
            .ledc_timer = LEDC_TIMER_1,
            .ledc_channel = LEDC_CHANNEL_1,

            .pixel_format = PIXFORMAT_JPEG,
            .frame_size = FRAMESIZE_VGA,
            .jpeg_quality = 12,
            .fb_count = 2,
            .fb_location = CAMERA_FB_IN_PSRAM,
            .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
            .sccb_i2c_port = 1,
        };

        camera_ = new OvCameraWatcher(camera_config, CONFIG_MINI_BOT_OV_BASE_URL);
        ESP_LOGI(TAG, "摄像头对象已创建，传感器等 WiFi 连上后再初始化");
    }

    // 真正初始化传感器。由 CameraInitTask 在 WiFi 连上后调用。
    void InitializeCamera() {
        if (camera_ == nullptr) {
            ESP_LOGE(TAG, "摄像头对象不存在，无法初始化传感器");
            return;
        }
        camera_ready_ = camera_->Initialize();
        if (camera_ready_) {
            ESP_LOGI(TAG, "摄像头初始化成功"
                        "（工具 self.camera.start_watch / stop_watch / look_current / look_past 已可用）");
        } else {
            ESP_LOGE(TAG, "摄像头初始化失败（工具调用会返回错误，不影响其他功能）");
        }
    }

    // 摄像头初始化放在独立任务里：初始化耗时较长，放在网络事件回调里会阻塞事件分发。
    static void CameraInitTask(void* param) {
        auto* board = static_cast<DeepbotS3CamBoard*>(param);
        board->InitializeCamera();
        vTaskDelete(nullptr);
    }

public:
    bool HasCombinedHeadEmotionTool() const override { return true; }

    void SetMusicPlayback(bool active) override {
        if (head_motion_mutex_ != nullptr) {
            xSemaphoreTake(head_motion_mutex_, portMAX_DELAY);
            music_playback_ = active;
            if (!active) pending_music_action_.clear();
            xSemaphoreGive(head_motion_mutex_);
        }
        if (display_ != nullptr) display_->SetMusicPlayback(active);
    }

    void TryMusicHeadAction(const std::string& action) override {
        if (!servos_ready_ || head_motion_mutex_ == nullptr || sensor_task_ == nullptr) return;
        if (action != "nod" && action != "shake" && action != "dance") return;
        xSemaphoreTake(head_motion_mutex_, portMAX_DELAY);
        // 单拍交接槽，不排队；普通工具若先占用播放器，下拍也会丢掉伴奏。
        if (music_playback_ && pending_music_action_.empty() &&
            !head_motion_.Busy() && !head_final_output_pending_) pending_music_action_ = action;
        xSemaphoreGive(head_motion_mutex_);
    }

    bool HasPendingToolEffects() override {
        if (head_motion_mutex_ != nullptr) {
            xSemaphoreTake(head_motion_mutex_, portMAX_DELAY);
            const bool moving = head_motion_.Busy() || head_final_output_pending_;
            xSemaphoreGive(head_motion_mutex_);
            if (moving) return true;
        }
        // 只等开启阶段；关闭时后台清理，不能推迟本轮退出 Speaking。
        if (camera_ != nullptr && camera_->HasPendingTransition()) return true;
        // 不同时持舵机锁和显示锁，避免跨任务相互等待。
        return display_ != nullptr && display_->HasPendingToolEmotion();
    }
    DeepbotS3CamBoard() {
        ESP_LOGI(TAG, "板级初始化开始");
        InitializeServos();  // 仅将信号 GPIO 配成高阻输入，不输出 PWM
        RegisterHeadTools();  // self.head.act：点头、摇头、看左右等语义动作
        // 必须在这里创建摄像头对象并把工具注册好。
        // 注：本板不再向 McpServer 提供 Camera 对象（GetCamera 返回 nullptr），
        // 所以 self.camera.take_photo 不会出现，避免与持续观察工具语义冲突。
        CreateCameraObject();
        RegisterCameraTools();  // 持续观察：start_watch / stop_watch / get_view
        InitializeServoConsole();  // 串口 head 命令只有显式输入才驱动舵机
        InitializeDisplay();       // 必须在传感器任务之前：触摸反应需要更新表情
        InitializeSensors();       // 触摸 / 左红外 / 右红外，只配输入不动作
        StartSensorTask();         // 开启后才会有“被摸头”与“感知物体”的行为
        ESP_LOGI(TAG, "板级初始化完成，等待 WiFi 连接");
    }

    AudioCodec* GetAudioCodec() override {
        // 麦克风与功放各用独立 I²S 控制器，播放参考音频供 AFE 回声消除。
        static NoAudioCodecSimplexAec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    std::string GetDeviceStatusJson() override {
        auto base = WifiBoard::GetDeviceStatusJson();
        auto root = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(cJSON_Parse(base.c_str()), cJSON_Delete);
        if (!root) return base;
        auto* camera = cJSON_AddObjectToObject(root.get(), "camera");
        if (!camera) return base;
        const bool watching = camera_ && camera_->IsWatching();
        const auto monitor = camera_ ? camera_->GetMonitorStatus() : OvCameraWatcher::MonitorStatus{};
        cJSON_AddBoolToObject(camera, "observation_enabled", watching);
        cJSON_AddBoolToObject(camera, "monitor_enabled", watching && !monitor.condition.empty());
        cJSON_AddBoolToObject(camera, "monitor_ready", watching && monitor.ready);
        cJSON_AddStringToObject(camera, "monitor_condition", monitor.condition.c_str());
        // 回显当前规则的播报内容，和提醒条件分离。
        cJSON_AddStringToObject(camera, "monitor_reminder", monitor.reminder.c_str());
        auto json = std::unique_ptr<char, decltype(&cJSON_free)>(cJSON_PrintUnformatted(root.get()), cJSON_free);
        return json ? std::string(json.get()) : base;
    }
    Display* GetDisplay() override { return display_; }

    // 供 McpServer 注册标准工具 self.camera.take_photo。
    // 固定返回 nullptr：不再向上层提供“拍一张问一次”的相机对象。
    //
    // 为什么：McpServer 只要看到 GetCamera() 非空就会自动注册
    // self.camera.take_photo 工具。而本板已改成持续观察模式
    // （start_watch / stop_watch / get_view），take_photo 的语义不同、
    // 两个工具同时存在会让大模型选错。所以这里明确返回空，
    // 由 RegisterCameraTools() 自己注册符合持续观察语义的工具。
    Camera* GetCamera() override { return nullptr; }

    bool ShouldStartActivation() override { return true; }

    // 配对码是待绑定画面的主内容，不放在顶部会超时的通知栏。
    void OnActivationCode(const std::string& code, const std::string& /*message*/) override {
        if (display_ != nullptr) display_->ShowPairingCode(code);
    }

    void StartNetwork() override {
        // WiFi 连上后再初始化摄像头：此时才有意义（后续要连云端推流），
        // 也不至于在开机最忙的时候和 WiFi 抢时间。
        NetworkEventCallback app_callback = network_event_callback_;
        network_event_callback_ = [this, app_callback](NetworkEvent event, const std::string& data) {
            if (event == NetworkEvent::Connected && !camera_ready_) {
                ESP_LOGI(TAG, "WiFi 已连接，准备摄像头");
                xTaskCreatePinnedToCore(CameraInitTask, "cam_init", 8192, this, 3, nullptr, 1);
            }
            // 界面提示（配网 / 连接中 / 注册码）统一交给框架与眼睛 UI 处理，
            // 这里不再单独绘制 banner，避免和状态机抢屏幕。
            if (app_callback) {
                app_callback(event, data);
            }
        };

        ESP_LOGI(TAG, "启动 WiFi 配网");
        WifiBoard::StartNetwork();
    }
};

DECLARE_BOARD(DeepbotS3CamBoard);
