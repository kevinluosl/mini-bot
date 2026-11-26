#ifndef CUSTOM_SERVER_AUDIO_H
#define CUSTOM_SERVER_AUDIO_H

#include "protocol.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

class Mqtt;
class Udp;

// MQTT 控制会话，UDP 传输语音。
class CustomServerAudio {
public:
    CustomServerAudio();
    ~CustomServerAudio();

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void ApplyMqtt(const std::string& endpoint, const std::string& client_id,
                   const std::string& username, const std::string& password,
                   const std::string& publish_topic, const std::string& subscribe_topic,
                   int keepalive);
    bool Start();
    void PollConnection();
    bool IsAudioChannelOpened() const;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet);
    void SendAbort();
    void CloseAudioChannel();
    bool IsPlaybackActive() const { return playback_active_.load(); }
    std::optional<uint32_t> ReplyGeneration() const;
    // 应用任务发送 MCP 回包，不可从 MQTT 接收回调同步调用。
    bool SendText(const std::string& text, uint32_t generation);
    bool SendMcpNotification(const std::string& payload, uint32_t generation);

private:
    bool EnsureConnected();
    bool ConnectBrokerLocked();
    bool MaintainConnectionLocked();
    bool SendUdpReadyLocked();
    void RequestResync();
    void RequestConnect();
    void HandleControl(const std::string& payload);
    bool OpenUdp();
    bool SendHelloLocked(uint32_t hello_id);
    bool SendTextLocked(const std::string& text);

    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    EventGroupHandle_t hello_event_ = nullptr;
    // 连接管理与发送任务持锁访问 mqtt_/udp_；MQTT 回调不能拿这把锁。
    mutable std::mutex send_mutex_;
    std::atomic<bool> udp_reconnect_needed_{true};
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_audio_;
    std::function<void(const cJSON* root)> on_json_;
    std::function<void()> on_closed_;
    // MQTT hello 回调只拿此锁，不拿 send_mutex_；握手等待时不持此锁。
    std::mutex hello_mutex_;
    uint32_t expected_hello_id_ = 0;
    std::atomic<uint32_t> active_hello_id_{0};
    std::atomic<uint32_t> server_epoch_{0};
    std::atomic<uint32_t> last_heartbeat_sec_{0};
    std::atomic<uint32_t> next_check_sec_{0};
    uint32_t udp_ready_token_ = 0;
    bool reconnect_broker_ = true;  // 只由持发送锁的任务读写。
    std::atomic<bool> broker_disconnected_{true};
    std::atomic<bool> maintenance_enabled_{false};
    std::string session_id_;
    std::string mqtt_host_;
    int mqtt_port_ = 1883;
    std::string mqtt_client_id_;
    std::string mqtt_username_;
    std::string mqtt_password_;
    std::string publish_topic_;
    std::string subscribe_topic_;
    int keepalive_ = 240;
    std::string udp_server_;
    int udp_port_ = 0;
    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    uint32_t upload_seq_ = 0;
    // 设备标识，填进上行 UDP 头的第 4~7 字节。
    // 服务器靠这段字节区分不同设备的上行音频——多台设备同连一个服务器时，
    // 若这段恒为 0，先建立的会话会把后来者的音频全部误收，导致聊天记录、
    // 记忆、工具列表全部串到别的设备名下。
    // 懒计算：首次发包时才读 MAC，避免构造函数在 WiFi 初始化前调用。
    uint32_t device_tag_ = 0;
    std::atomic<uint32_t> connection_generation_{0};
    bool listening_ = false;
    std::atomic<bool> playback_active_{false};
    std::atomic<bool> connect_started_{false};
    std::atomic<bool> abort_pending_{false};
};

#endif
