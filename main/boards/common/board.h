#ifndef BOARD_H
#define BOARD_H

#include <http.h>
#include <web_socket.h>
#include <mqtt.h>
#include <udp.h>
#include <string>
#include <functional>
#include <network_interface.h>

#include "led/led.h"
#include "backlight.h"
#include "camera.h"
#include "assets.h"

/**
 * Network events for unified callback
 */
enum class NetworkEvent {
    Scanning,              // Network is scanning (WiFi scanning, etc.)
    Connecting,            // Network is connecting (data: SSID/network name)
    Connected,             // Network connected successfully (data: SSID/network name)
    Disconnected,          // Network disconnected
    WifiConfigModeEnter,   // Entered WiFi configuration mode
    WifiConfigModeExit,    // Exited WiFi configuration mode
    // Cellular modem specific events
    ModemDetecting,        // Detecting modem (baud rate, module type)
    ModemErrorNoSim,       // No SIM card detected
    ModemErrorRegDenied,   // Network registration denied
    ModemErrorInitFailed,  // Modem initialization failed
    ModemErrorTimeout      // Operation timeout
};

// Power save level enumeration
enum class PowerSaveLevel {
    LOW_POWER,    // Maximum power saving (lowest power consumption)
    BALANCED,     // Medium power saving (balanced)
    PERFORMANCE,  // No power saving (maximum power consumption / full performance)
};

// Network event callback type (event, data)
// data contains additional info like SSID for Connecting/Connected events
using NetworkEventCallback = std::function<void(NetworkEvent event, const std::string& data)>;

void* create_board();
class AudioCodec;
class Display;
class Board {
private:
    Board(const Board&) = delete; // 禁用拷贝构造函数
    Board& operator=(const Board&) = delete; // 禁用赋值操作

protected:
    Board();
    std::string GenerateUuid();

    // 软件生成的设备唯一标识
    std::string uuid_;

public:
    static Board& GetInstance() {
        static Board* instance = static_cast<Board*>(create_board());
        return *instance;
    }

    virtual ~Board() = default;
    virtual std::string GetBoardType() = 0;
    virtual std::string GetUuid() { return uuid_; }
    virtual Backlight* GetBacklight() { return nullptr; }
    virtual Led* GetLed();
    virtual AudioCodec* GetAudioCodec() = 0;
    virtual bool GetTemperature(float& esp32temp);
    virtual Display* GetDisplay();
    virtual Camera* GetCamera();

    // 新一轮回答开始播报时回调（收到 tts start 的那一刻）。
    //
    // 用途：把上一轮还没跑完的“动作”全部作废，让新一轮接管。
    // 否则会出现这种错位：上一轮模型叫了 5 个表情（共 10 秒），用户
    // 第 4 秒就开口说下一句，结果屏幕还在慢慢播上一轮的表情，
    // 用户看到的表情与当前对话完全对不上。电机同理（上一轮叫的
    // “前进 3 秒”不应该在用户已经改口后还继续跑）。
    //
    // 为何不用“音频上行”作为信号：设备开着麦时音频是持续上行的
    // （是不是人声由服务端 VAD 判定），无法用来划分轮次。
    // 而服务端每轮只发一次 tts start（start_voice 里有 voice_started 守卫），
    // 所以它就是“新一轮开始”的准确信号。
    //
    // 默认实现只清显示层的待播动作；带电机等执行器的板子重写后
    // 把那些也一并停掉。
    virtual void OnNewUserTurn();
    // 查询工具回包后仍在后台运行的板级动作（不含传感器自动互动）。
    virtual bool HasPendingToolEffects() { return false; }
    // 提供合并动效工具的板型不再对外注册独立屏幕表情工具。
    virtual bool HasCombinedHeadEmotionTool() const { return false; }
    virtual NetworkInterface* GetNetwork() = 0;
    virtual void StartNetwork() = 0;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) { (void)callback; }
    // 联网后是否执行激活 / 检查更新 / 连接服务器流程。
    // 默认启用 OTA 和设备激活流程。只做 WiFi + 摄像头等本地验证的板子可返回 false，
    // 这样连上网络后直接进入待机，不会触发 OTA 和设备激活。
    virtual bool ShouldStartActivation() { return true; }
    // 框架拿到设备激活码时回调（默认空实现，不影响标准板子）。
    // 设备未在服务器绑定时，框架会提示注册；板子可据此展示 6 位注册码。
    virtual void OnActivationCode(const std::string& code, const std::string& message) {
        (void)code;
        (void)message;
    }
    virtual const char* GetNetworkStateIcon() = 0;
    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging);
    virtual std::string GetSystemInfoJson();
    virtual void SetPowerSaveLevel(PowerSaveLevel level) = 0;
    virtual std::string GetBoardJson() = 0;
    virtual std::string GetDeviceStatusJson() = 0;
};

#define DECLARE_BOARD(BOARD_CLASS_NAME) \
void* create_board() { \
    return new BOARD_CLASS_NAME(); \
}

#endif // BOARD_H
