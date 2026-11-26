#include "custom_server_audio.h"

#include "audio_service.h"
#include "board.h"
#include "system_info.h"

#include <mqtt.h>
#include <udp.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <arpa/inet.h>
#include <cstring>
#include <string>

#define TAG "CustomAudio"
#define CUSTOM_SERVER_HELLO_EVENT (1 << 0)
#define CUSTOM_SERVER_DISCONNECTED_EVENT (1 << 1)
static constexpr size_t kAudioHeaderSize = 16;

namespace {

constexpr uint32_t kHeartbeatSeconds = 10;
constexpr uint32_t kHeartbeatTimeoutSeconds = 35;
constexpr uint32_t kRetrySeconds = 3;

uint32_t NowSeconds() { return static_cast<uint32_t>(esp_timer_get_time() / 1000000); }

uint32_t ReadId(const cJSON* root, const char* key) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valueint <= 0 || item->valuedouble != item->valueint) return 0;
    return static_cast<uint32_t>(item->valueint);
}

// 把 MAC 压缩成 32 位设备标识，写进上行 UDP 头的第 4~7 字节。
//
// 为什么需要：服务器的会话归属判定看 header[4:8]。这个字段以前一直留空，
// 所有设备发出去都是 00000000，于是服务器无法区分：先建立的会话会把
// 后来的设备的音频全部吃掉，表现是新板子说话、却记在旧板子名下，
// 连工具列表和聊天记录都用错的。
//
// 取 MAC 的后 4 字节：前 3 字节是厂商 OUI（同批板子相同），
// 后 3 字节才是设备唯一，多取 1 字节可抵消 MAC 相邻问题。
//
// 返回 0 时回退到 1：00000000 必须留给“未升级的旧固件”，
// 否则新旧固件撞在一起又分不开了。
uint32_t MakeDeviceTag() {
    const std::string mac = SystemInfo::GetMacAddress();  // "aa:bb:cc:dd:ee:ff"
    std::string hex;
    hex.reserve(12);
    for (char c : mac) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            hex.push_back(c);
        }
    }
    if (hex.size() > 8) {
        hex = hex.substr(hex.size() - 8);
    }
    uint32_t tag = 0;
    for (char c : hex) {
        // 手写 hex 转换，不依赖 tolower：避免为一个小工具函数引入 <cctype>，
        // 也避开了 std::tolower 返回 int 需要强转的麻烦。
        uint32_t v;
        if (c <= '9') {
            v = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a') {
            v = static_cast<uint32_t>(c - 'a' + 10);
        } else {
            v = static_cast<uint32_t>(c - 'A' + 10);
        }
        tag = (tag << 4) | v;
    }
    return tag == 0 ? 1u : tag;
}

}  // namespace

CustomServerAudio::CustomServerAudio() { hello_event_ = xEventGroupCreate(); }

CustomServerAudio::~CustomServerAudio() {
    ++connection_generation_;
    mqtt_.reset();
    udp_.reset();
    vEventGroupDelete(hello_event_);
}

void CustomServerAudio::OnIncomingAudio(
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_audio_ = std::move(callback);
}

void CustomServerAudio::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_json_ = std::move(callback);
}

void CustomServerAudio::OnAudioChannelClosed(std::function<void()> callback) {
    on_closed_ = std::move(callback);
}

void CustomServerAudio::ApplyMqtt(const std::string& endpoint, const std::string& client_id,
                                  const std::string& username, const std::string& password,
                                  const std::string& publish_topic, const std::string& subscribe_topic,
                                  int keepalive) {
    auto pos = endpoint.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= endpoint.size()) {
        ESP_LOGE(TAG, "MQTT endpoint 不正确: %s", endpoint.c_str());
        return;
    }
    mqtt_host_ = endpoint.substr(0, pos);
    mqtt_port_ = std::stoi(endpoint.substr(pos + 1));
    mqtt_client_id_ = client_id;
    mqtt_username_ = username;
    mqtt_password_ = password;
    publish_topic_ = publish_topic;
    subscribe_topic_ = subscribe_topic;
    keepalive_ = keepalive > 0 ? keepalive : 240;
    connect_started_.store(false);
    listening_ = false;
    ESP_LOGI(TAG, "已收到注册下发的 MQTT %s topic=%s", endpoint.c_str(), publish_topic_.c_str());
}

std::optional<uint32_t> CustomServerAudio::ReplyGeneration() const {
    const auto generation = connection_generation_.load();
    if (!udp_reconnect_needed_.load()) {
        return generation;
    }
    return std::nullopt;
}

bool CustomServerAudio::SendText(const std::string& text, uint32_t generation) {
    // 明知旧会话已失效时，不让应用任务空等整段重连握手。
    if (generation != connection_generation_.load() || udp_reconnect_needed_.load()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(send_mutex_);
    return generation == connection_generation_.load() && !udp_reconnect_needed_.load() && SendTextLocked(text);
}

bool CustomServerAudio::SendMcpNotification(const std::string& payload, uint32_t generation) {
    // 通知不是必须回包；连接忙或已切换时丢弃，不能让摸头阻塞应用主循环。
    std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || generation != connection_generation_.load() ||
        udp_reconnect_needed_.load() || !udp_) return false;
    std::string session;
    {
        std::lock_guard<std::mutex> hello_lock(hello_mutex_);
        session = session_id_;
    }
    if (session.empty()) return false;
    // session_id 由服务端生成（十六进制）；新会话会拒绝缺失或过期的 ID。
    return SendTextLocked("{\"type\":\"mcp\",\"session_id\":\"" + session +
                          "\",\"payload\":" + payload + "}");
}

bool CustomServerAudio::SendTextLocked(const std::string& text) {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        return false;
    }
    ESP_LOGI(TAG, "MQTT send: %u bytes", unsigned(text.size()));
    return mqtt_->Publish(publish_topic_, text);
}

void CustomServerAudio::HandleControl(const std::string& payload) {
    auto root = cJSON_ParseWithLength(payload.data(), payload.size());
    if (root == nullptr) {
        ESP_LOGE(TAG, "MQTT 控制消息不是 JSON");
        return;
    }
    auto type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && strcmp(type->valuestring, "session_resync") == 0) {
        const auto epoch = ReadId(root, "server_epoch");
        if (epoch && (epoch != server_epoch_.load() || ReadId(root, "hello_id") == active_hello_id_.load())) {
            RequestResync();
        }
        cJSON_Delete(root);
        return;
    }
    if (cJSON_IsString(type) && strcmp(type->valuestring, "session_pong") == 0) {
        if (ReadId(root, "server_epoch") == server_epoch_.load() && server_epoch_.load() &&
            ReadId(root, "hello_id") == active_hello_id_.load()) {
            last_heartbeat_sec_.store(NowSeconds());
        }
        cJSON_Delete(root);
        return;
    }
    if (cJSON_IsString(type) && strcmp(type->valuestring, "hello") == 0) {
        std::lock_guard<std::mutex> handshake(hello_mutex_);
        const auto epoch = ReadId(root, "server_epoch");
        const auto hello_id = ReadId(root, "hello_id");
        const auto token = ReadId(root, "udp_ready_token");
        // 旧服务没有扩展字段；新版应答必须对应当前尝试，且有本次 UDP 报到凭据。
        if (!expected_hello_id_ || ((epoch || hello_id || token) &&
            (!epoch || !token || hello_id != expected_hello_id_))) {
            cJSON_Delete(root);
            return;
        }
        auto audio_params = cJSON_GetObjectItem(root, "audio_params");
        if (cJSON_IsObject(audio_params)) {
            auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
            auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
            if (cJSON_IsNumber(sample_rate)) {
                server_sample_rate_ = sample_rate->valueint;
            }
            if (cJSON_IsNumber(frame_duration)) {
                server_frame_duration_ = frame_duration->valueint;
            }
        }
        auto udp = cJSON_GetObjectItem(root, "udp");
        auto server = cJSON_IsObject(udp) ? cJSON_GetObjectItem(udp, "server") : nullptr;
        auto port = cJSON_IsObject(udp) ? cJSON_GetObjectItem(udp, "port") : nullptr;
        auto session = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsString(server) && cJSON_IsString(session) && cJSON_IsNumber(port) &&
            port->valueint > 0 && port->valueint <= UINT16_MAX) {
            active_hello_id_.store(hello_id);
            server_epoch_.store(epoch);
            udp_ready_token_ = token;
            expected_hello_id_ = 0;
            session_id_ = session->valuestring;
            udp_server_ = server->valuestring;
            udp_port_ = port->valueint;
            ESP_LOGI(TAG, "MQTT hello: UDP %s:%d, %d Hz", udp_server_.c_str(), udp_port_,
                     server_sample_rate_);
            xEventGroupSetBits(hello_event_, CUSTOM_SERVER_HELLO_EVENT);
        } else {
            ESP_LOGE(TAG, "MQTT hello 缺少 UDP 地址");
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "system") == 0 && ReadId(root, "hello_id")) {
        // 只有匹配本次待完成握手的解绑通知可越过业务消息门控；迟到包不得重启已恢复的会话。
        auto command = cJSON_GetObjectItem(root, "command");
        auto reason = cJSON_GetObjectItem(root, "reason");
        bool retry_activation = false;
        {
            std::lock_guard<std::mutex> handshake(hello_mutex_);
            if (ReadId(root, "hello_id") == expected_hello_id_ && !broker_disconnected_.load() &&
                cJSON_IsString(command) && strcmp(command->valuestring, "reboot") == 0 &&
                cJSON_IsString(reason) && strcmp(reason->valuestring, "binding_revoked") == 0) {
                expected_hello_id_ = 0;
                retry_activation = true;
            }
        }
        if (retry_activation && on_json_) {
            // 先结束旧握手的等待，再让应用主循环走正常重启、OTA、显示码流程。
            xEventGroupSetBits(hello_event_, CUSTOM_SERVER_DISCONNECTED_EVENT);
            on_json_(root);
        }
    } else if (udp_reconnect_needed_.load() || broker_disconnected_.load()) {
        // 握手期间不执行上一轮迟到的工具或播放控制。
    } else if (server_epoch_.load() && cJSON_IsString(cJSON_GetObjectItem(root, "session_id")) &&
               session_id_ != cJSON_GetObjectItem(root, "session_id")->valuestring) {
        // MQTT 回调串行处理 hello 和业务消息，不让迟到的旧会话 stop/MCP 影响新会话。
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "tts") == 0) {
        auto state = cJSON_GetObjectItem(root, "state");
        if (cJSON_IsString(state) && strcmp(state->valuestring, "start") == 0) {
            // 先调用 on_json_（内部会 ResetDecoder + PrewarmAudioOutput），再打开 playback_active_ 接收 UDP 音频包，
            // 防止首帧 UDP 音频比异步状态切换先到却被滞后的 ResetDecoder() 清空导致首字吞音/跳帧
            if (on_json_ != nullptr) {
                on_json_(root);
            }
            playback_active_.store(true);
            cJSON_Delete(root);
            return;
        } else if (cJSON_IsString(state) && strcmp(state->valuestring, "stop") == 0) {
            // 恢复协议原有 stop 门控；服务端负责发送前的缓冲排空等待。
            playback_active_.store(false);
            ESP_LOGI(TAG, "收到 tts stop time=%lld ms", esp_timer_get_time() / 1000);
        }
        if (on_json_ != nullptr) {
            on_json_(root);
        }
    } else if (cJSON_IsString(type) && strcmp(type->valuestring, "goodbye") == 0) {
        auto session = cJSON_GetObjectItem(root, "session_id");
        playback_active_.store(false);
        ESP_LOGI(TAG, "收到 goodbye，结束本轮，保持 UDP session=%s", cJSON_IsString(session) ? session->valuestring : "");
        if (on_closed_ != nullptr) {
            on_closed_();
        }
    } else if (on_json_ != nullptr) {
        on_json_(root);
    }
    cJSON_Delete(root);
}

bool CustomServerAudio::OpenUdp() {
    if (udp_server_.empty() || udp_port_ <= 0) {
        return false;
    }
    auto network = Board::GetInstance().GetNetwork();
    auto udp = network->CreateUdp(2);
    // 每个接收任务独享序号与音频格式，不与新连接共享。
    udp->OnMessage([this, rate = server_sample_rate_, duration = server_frame_duration_,
                    sequence_seen = uint32_t{0}, missing = uint32_t{0}, late = uint32_t{0},
                    last_log_us = int64_t{0}](const std::string& data) mutable {
        if (data.size() < kAudioHeaderSize || static_cast<uint8_t>(data[0]) != 0x01) {
            ESP_LOGW(TAG, "UDP 语音包格式不对 bytes=%u", static_cast<unsigned>(data.size()));
            return;
        }
        uint16_t payload_len = 0;
        uint32_t sequence = 0;
        memcpy(&payload_len, data.data() + 2, sizeof(payload_len));
        memcpy(&sequence, data.data() + 12, sizeof(sequence));
        payload_len = ntohs(payload_len);
        sequence = ntohl(sequence);
        if (data.size() != kAudioHeaderSize + payload_len || on_audio_ == nullptr) {
            return;
        }
        if (udp_reconnect_needed_.load() || broker_disconnected_.load()) {
            return;
        }
        const bool late_packet = sequence != 0 && sequence <= sequence_seen;
        if (late_packet) {
            ++late;
        } else if (sequence != 0) {
            if (sequence_seen != 0) {
                missing += sequence - sequence_seen - 1;
            }
            sequence_seen = sequence;
        }
        const auto now = esp_timer_get_time();
        if ((missing || late) && now - last_log_us >= 5000000) {
            ESP_LOGW(TAG, "UDP 下行序号异常 seq=%lu gaps=%lu late_drop=%lu",
                     static_cast<unsigned long>(sequence), static_cast<unsigned long>(missing),
                     static_cast<unsigned long>(late));
            missing = late = 0;
            last_log_us = now;
        }
        if (late_packet) {
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = rate;
        packet->frame_duration = duration;
        packet->payload.assign(data.begin() + kAudioHeaderSize, data.end());
        on_audio_(std::move(packet));
    });
    if (auto connected = udp->Connect(udp_server_, udp_port_); !connected) {
        ESP_LOGE(TAG, "UDP 连接失败: %s", connected.error().ToString().c_str());
        return false;
    }
    udp_ = std::move(udp);
    ESP_LOGI(TAG, "UDP 语音通道已打开 %s:%d", udp_server_.c_str(), udp_port_);
    return true;
}

void CustomServerAudio::CloseAudioChannel() {
    std::function<void()> closed;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        maintenance_enabled_.store(false);
        playback_active_.store(false);
        listening_ = false;
        udp_reconnect_needed_.store(true);
        ++connection_generation_;
        udp_.reset();
        closed = on_closed_;
    }
    ESP_LOGI(TAG, "语音通道已关闭");
    if (closed) {
        closed();
    }
}

bool CustomServerAudio::SendHelloLocked(uint32_t hello_id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", 3);
#if CONFIG_BOARD_TYPE_DEEPBOT_S3_CAM_N16R8
    cJSON_AddBoolToObject(root, "proactive_listen", true);
    cJSON_AddBoolToObject(root, "music_playback", true);
#endif
    cJSON_AddNumberToObject(root, "hello_id", hello_id);
    cJSON_AddStringToObject(root, "transport", "udp");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    char* json = cJSON_PrintUnformatted(root);
    std::string hello(json);
    cJSON_free(json);
    cJSON_Delete(root);
    return SendTextLocked(hello);
}

bool CustomServerAudio::ConnectBrokerLocked() {
    if (!reconnect_broker_ && !broker_disconnected_.load() && mqtt_ && mqtt_->IsConnected()) {
        return true;
    }
    reconnect_broker_ = true;
    mqtt_.reset();  // 旧 MQTT 回调已退出后才清标记。
    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(3);
    if (!mqtt_) return false;
    mqtt_->SetKeepAlive(keepalive_);
    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        if (topic == subscribe_topic_) HandleControl(payload);
    });
    mqtt_->OnDisconnected([this]() {
        // 回调只标记并唤醒；不可拿发送锁或回收 MQTT/UDP。
        broker_disconnected_.store(true);
        udp_reconnect_needed_.store(true);
        playback_active_.store(false);
        xEventGroupSetBits(hello_event_, CUSTOM_SERVER_DISCONNECTED_EVENT);
        next_check_sec_.store(0);
        RequestConnect();
    });
    broker_disconnected_.store(false);
    if (auto status = mqtt_->Connect(mqtt_host_, mqtt_port_, mqtt_client_id_, mqtt_username_, mqtt_password_); !status) {
        ESP_LOGW(TAG, "MQTT 连接失败: %s", status.error().ToString().c_str());
        return false;
    }
    if (!mqtt_->Subscribe(subscribe_topic_) || broker_disconnected_.load()) return false;
    reconnect_broker_ = false;
    return true;
}

bool CustomServerAudio::SendUdpReadyLocked() {
    if (!udp_ready_token_) return true;  // 旧服务不认识报到包。
    if (!device_tag_) device_tag_ = MakeDeviceTag();
    // 16 字节，无音频；只用设备标识和本次 hello 的 UDP 凭据认领端口。
    std::string packet(kAudioHeaderSize, '\0');
    packet[0] = 1;
    const uint32_t tag = htonl(device_tag_);
    const uint32_t token = htonl(udp_ready_token_);
    memcpy(packet.data() + 4, &tag, sizeof(tag));
    memcpy(packet.data() + 8, &token, sizeof(token));
    return udp_ && udp_->Send(packet) == static_cast<int>(packet.size());
}

bool CustomServerAudio::EnsureConnected() {
    if (mqtt_host_.empty() || publish_topic_.empty() || subscribe_topic_.empty()) return false;
    udp_reconnect_needed_.store(true);
    ++connection_generation_;
    playback_active_.store(false);
    abort_pending_.store(false);
    listening_ = false;
    const bool had_udp = udp_ != nullptr;
    udp_.reset();
    if (had_udp && on_closed_) on_closed_();
    xEventGroupClearBits(hello_event_, CUSTOM_SERVER_HELLO_EVENT | CUSTOM_SERVER_DISCONNECTED_EVENT);
    if (!ConnectBrokerLocked()) return false;

    upload_seq_ = 0;
    const uint32_t hello_id = (esp_random() & 0x7fffffffU) | 1U;
    {
        std::lock_guard<std::mutex> handshake(hello_mutex_);
        expected_hello_id_ = hello_id;
    }
    const bool sent = SendHelloLocked(hello_id);
    const EventBits_t bits = sent ? xEventGroupWaitBits(hello_event_, CUSTOM_SERVER_HELLO_EVENT |
        CUSTOM_SERVER_DISCONNECTED_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000)) : 0;
    {
        std::lock_guard<std::mutex> handshake(hello_mutex_);
        expected_hello_id_ = 0;
    }
    if (!(bits & CUSTOM_SERVER_HELLO_EVENT) || broker_disconnected_.load() || !OpenUdp()) return false;
    if (!SendUdpReadyLocked()) {
        ESP_LOGW(TAG, "UDP 就绪包未发出，将重试会话");
        return false;
    }
    last_heartbeat_sec_.store(NowSeconds());
    udp_reconnect_needed_.store(false);
    const auto listen = std::string("{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\",\"hello_id\":") +
                        std::to_string(active_hello_id_.load()) + "}";
    listening_ = SendTextLocked(listen);
    if (!listening_ || broker_disconnected_.load()) {
        udp_reconnect_needed_.store(true);
        return false;
    }
    ESP_LOGI(TAG, "语音会话已恢复 epoch=%lu，MQTT 和 UDP 已打通", static_cast<unsigned long>(server_epoch_.load()));
    return true;
}

bool CustomServerAudio::MaintainConnectionLocked() {
    const auto epoch = server_epoch_.load();
    if (udp_reconnect_needed_.load() || broker_disconnected_.load() || !mqtt_ || !mqtt_->IsConnected() || !udp_ ||
        (epoch && NowSeconds() - last_heartbeat_sec_.load() >= kHeartbeatTimeoutSeconds)) {
        return EnsureConnected();
    }
    if (abort_pending_.exchange(false) && !SendTextLocked("{\"type\":\"abort\"}")) abort_pending_.store(true);
    if (!epoch) return true;  // 旧服务器没有应用层心跳。
    if (!SendUdpReadyLocked()) {
        udp_reconnect_needed_.store(true);
        return false;
    }
    return SendTextLocked(std::string("{\"type\":\"session_ping\",\"server_epoch\":") + std::to_string(epoch) +
                          ",\"hello_id\":" + std::to_string(active_hello_id_.load()) + "}");
}

void CustomServerAudio::RequestResync() {
    if (udp_reconnect_needed_.exchange(true)) return;
    playback_active_.store(false);
    next_check_sec_.store(0);
    RequestConnect();
}

void CustomServerAudio::RequestConnect() {
    if (!maintenance_enabled_.load() || connect_started_.exchange(true)) return;
    if (xTaskCreate([](void* arg) {
            auto* self = static_cast<CustomServerAudio*>(arg);
            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(self->send_mutex_);
                if (self->maintenance_enabled_.load()) ok = self->MaintainConnectionLocked();
                self->next_check_sec_.store(NowSeconds() + (ok ? kHeartbeatSeconds : kRetrySeconds));
                // 持锁直到复位运行标记，保证 CloseAudioChannel 不与未完成的任务交错。
                self->connect_started_.store(false);
            }
            vTaskDelete(nullptr);
        }, "custom_mqtt", 16384, this, 4, nullptr) != pdPASS) {
        next_check_sec_.store(NowSeconds() + kRetrySeconds);
        connect_started_.store(false);
        ESP_LOGE(TAG, "无法创建 MQTT 连接任务");
    }
}

bool CustomServerAudio::Start() {
    maintenance_enabled_.store(true);
    next_check_sec_.store(0);
    RequestConnect();
    return true;
}

void CustomServerAudio::PollConnection() {
    if (maintenance_enabled_.load() && NowSeconds() >= next_check_sec_.load()) RequestConnect();
}

bool CustomServerAudio::IsAudioChannelOpened() const {
    std::lock_guard<std::mutex> lock(send_mutex_);
    return !udp_reconnect_needed_.load() && mqtt_ && mqtt_->IsConnected() && udp_;
}

bool CustomServerAudio::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (packet == nullptr || packet->payload.empty()) {
        return true;
    }
    std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || !listening_ || udp_reconnect_needed_.load() || udp_ == nullptr) {
        return true;
    }
    uint32_t seq = ++upload_seq_;
    // 懒计算设备标识：首次发包时读 MAC。放这里而不是构造函数，
    // 是因为部分平台（P4 走 esp_wifi_get_mac）要求 WiFi 已初始化，
    // 而构造函数可能在那之前就被调用了。
    if (device_tag_ == 0) {
        device_tag_ = MakeDeviceTag();
    }
    std::string frame(kAudioHeaderSize + packet->payload.size(), '\0');
    frame[0] = 1;
    uint16_t payload_len = htons(static_cast<uint16_t>(packet->payload.size()));
    uint32_t timestamp = htonl(seq * 60);
    uint32_t sequence = htonl(seq);
    uint32_t device_tag = htonl(device_tag_);
    memcpy(frame.data() + 2, &payload_len, sizeof(payload_len));
    // 第 4~7 字节：设备标识。服务器用它判断这个包属于哪个会话，
    // 没有它多台设备就会串台。
    memcpy(frame.data() + 4, &device_tag, sizeof(device_tag));
    memcpy(frame.data() + 8, &timestamp, sizeof(timestamp));
    memcpy(frame.data() + 12, &sequence, sizeof(sequence));
    memcpy(frame.data() + kAudioHeaderSize, packet->payload.data(), packet->payload.size());
    int sent = udp_->Send(frame);
    if (sent > 0) {
        if (seq == 1 || seq % 25 == 0) {
            ESP_LOGI(TAG, "UDP 上传 seq=%u opus=%u", seq,
                     static_cast<unsigned>(packet->payload.size()));
        }
    } else {
        ESP_LOGW(TAG, "UDP 上传失败，保持通道，稍后重试");
    }
    return true;
}

void CustomServerAudio::SendAbort() {
    playback_active_.store(false);
    abort_pending_.store(true);
    std::unique_lock<std::mutex> lock(send_mutex_, std::try_to_lock);
    if (!lock.owns_lock() || udp_reconnect_needed_.load() || !SendTextLocked("{\"type\":\"abort\"}")) {
        return;
    }
    abort_pending_.store(false);
    ESP_LOGI(TAG, "已请求停止播放");
}
