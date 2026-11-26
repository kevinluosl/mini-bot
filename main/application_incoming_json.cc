// 服务端下行 JSON 处理。
//
// 从 application.cc 拆分而来：该函数处理 tts/stt/llm/mcp/notify/alert/system
// 等全部下行消息，原内嵌为 InitializeProtocol 的 lambda 时使主流程达 261 行。
// 拆分到独立编译单元后，application.cc 只负责状态机与设备生命周期。
//
// 注意：本文件是该函数的唯一实现，application.h 中声明为 Application 成员，
// 因此仍可访问全部私有成员，行为与拆分前完全一致。

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "cjson_utils.h"
#include "display.h"
#include "mcp_server.h"
#include "text_glyph_payload.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>
#include <cstring>
#include <limits>
#include <utility>

// 沿用原来的 TAG，避免日志前缀变化导致历史日志无法对比排查。
#define TAG "Application"

void Application::CompleteServerListening(bool ready) {
    if (auto reply = std::exchange(server_listen_reply_, {})) reply(ready);
}

void Application::HandleIncomingJson(const cJSON* root) {
    // 由 InitializeProtocol 中的 on_json lambda 提取而来，逻辑未改动。
    // 提取原因：该 lambda 原有 178 行，嵌入后 InitializeProtocol 达 261 行，
    // 难以阅读与定位；且该 lambda 被 protocol_ 与 custom_server_audio_ 两处注册。
    auto display = Board::GetInstance().GetDisplay();

    // Parse JSON data
    auto type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        ESP_LOGW(TAG, "Incoming JSON message has no type");
        return;
    }
    if (strcmp(type->valuestring, "notify") == 0) {
        auto audio_url = cJSON_GetObjectItem(root, "audio_url");
        if (!cJSON_IsString(audio_url) || audio_url->valuestring[0] == '\0') {
            ESP_LOGW(TAG, "Notify message requires audio_url");
            return;
        }

        std::vector<NotifySubtitle> subtitles;
        auto subtitles_json = cJSON_GetObjectItem(root, "subtitles");
        if (subtitles_json != nullptr && !cJSON_IsArray(subtitles_json)) {
            ESP_LOGW(TAG, "Notify subtitles must be an array");
            return;
        }
        if (cJSON_IsArray(subtitles_json)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach (item, subtitles_json) {
                auto start_ms = cJSON_GetObjectItem(item, "start_ms");
                auto text = cJSON_GetObjectItem(item, "text");
                if (!cJSON_IsNumber(start_ms) || start_ms->valuedouble < 0 ||
                    start_ms->valuedouble > std::numeric_limits<uint32_t>::max() ||
                    !cJSON_IsString(text)) {
                    ESP_LOGW(TAG, "Ignoring invalid notify subtitle");
                    continue;
                }
                subtitles.push_back({.start_ms = static_cast<uint32_t>(start_ms->valuedouble),
                                     .text = text->valuestring});
            }
        }

        Schedule([this, url = std::string(audio_url->valuestring),
                  subtitles = std::move(subtitles)]() mutable {
            StartNotification(std::move(url), std::move(subtitles));
        });
    } else if (strcmp(type->valuestring, "tts") == 0) {
        auto state = cJSON_GetObjectItem(root, "state");
        if (!cJSON_IsString(state)) {
            return;
        }
        if (strcmp(state->valuestring, "start") == 0) {
            // 在收到 tts start 的第一时间（开放 UDP 接收前）同步重置解码器并提前预热开启 I2S 硬件，
            // 给功放留出充足的稳定时间，同时避免放到 main_loop 异步切换状态时把已到达的首帧清空
            audio_service_.ResetDecoder();
            audio_service_.PrewarmAudioOutput();
            Schedule([this]() {
                aborted_ = false;
                pending_speaking_exit_ = kDeviceStateUnknown;
                // 新一轮不继承旧轮收尾；是否保留已接受的效果由板端决定。
                Board::GetInstance().SetMusicPlayback(false);
                Board::GetInstance().OnNewUserTurn();
                SetDeviceState(kDeviceStateSpeaking);
            });
        } else if (strcmp(state->valuestring, "music_start") == 0 ||
                   strcmp(state->valuestring, "music_stop") == 0) {
            if (!cJSON_IsString(cJSON_GetObjectItem(root, "session_id"))) return;
            const auto generation = custom_server_audio_.ReplyGeneration();
            if (!generation) return;
            const bool active = strcmp(state->valuestring, "music_start") == 0;
            Schedule([this, generation, active]() {
                if (custom_server_audio_.ReplyGeneration() != generation) return;
                if (!active || (!aborted_ && GetDeviceState() == kDeviceStateSpeaking &&
                                pending_speaking_exit_ == kDeviceStateUnknown)) {
                    Board::GetInstance().SetMusicPlayback(active);
                }
            });
        } else if (strcmp(state->valuestring, "stop") == 0) {
            auto received_ms = esp_timer_get_time() / 1000;
            Schedule([this, received_ms]() {
                Board::GetInstance().SetMusicPlayback(false);
                auto state = GetDeviceState();
                if (state == kDeviceStateSpeaking) {
                    // tts stop 只表示服务端已结束播报；工具的本地效果可能还在运行。
                    // 若按键/唤醒已指定去聆听，不用迟到的 stop 覆盖该意图。
                    if (pending_speaking_exit_ == kDeviceStateUnknown) {
                        pending_speaking_exit_ = listening_mode_ == kListeningModeManualStop
                            ? kDeviceStateIdle : kDeviceStateListening;
                    }
                    ESP_LOGI(TAG, "处理 tts stop time=%lld ms，检查工具是否结束", received_ms);
                    TryFinishSpeaking();
                } else {
                    ESP_LOGW(TAG, "处理 tts stop time=%lld ms result=忽略 state=%d", received_ms,
                             state);
                }
            });
        } else if (strcmp(state->valuestring, "sentence_start") == 0) {
            // 提前开启音频输出硬件，让第一帧播放时硬件已就绪，避免"吃字"
            audio_service_.PrewarmAudioOutput();

            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, "<< %s", text->valuestring);
                Schedule([display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("assistant", message.c_str());
                });
            }
        }
    } else if (strcmp(type->valuestring, "listen") == 0) {
        const auto state = cJSON_GetObjectItem(root, "state");
        const auto request = cJSON_GetObjectItem(root, "request_id");
        const auto session = cJSON_GetObjectItem(root, "session_id");
        // 握手时也会收到 listen/start，但它没有 request_id，不能让板子上电就开麦。
        if (!cJSON_IsString(state) || strcmp(state->valuestring, "start") != 0 ||
            !cJSON_IsNumber(request) || request->valueint <= 0 || request->valuedouble != request->valueint ||
            !cJSON_IsString(session) || session->valuestring[0] == '\0') return;
        const auto generation = custom_server_audio_.ReplyGeneration();
        if (!generation) return;
        Schedule([this, id = request->valueint, session_id = std::string(session->valuestring), generation]() {
            if (custom_server_audio_.ReplyGeneration() != generation) return;
            CompleteServerListening(false);
            server_listen_reply_ = [this, id, session_id, generation](bool ready) {
                auto response = cJSON_CreateObject();
                if (!response) return;
                cJSON_AddStringToObject(response, "type", "listen");
                cJSON_AddStringToObject(response, "state", ready ? "ready" : "busy");
                cJSON_AddNumberToObject(response, "request_id", id);
                cJSON_AddStringToObject(response, "session_id", session_id.c_str());
                auto text = cJSON_PrintUnformatted(response);
                if (text) custom_server_audio_.SendText(text, *generation);
                cJSON_free(text);
                cJSON_Delete(response);
            };
            const auto current = GetDeviceState();
            if (current == kDeviceStateIdle) {
                Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
                SetListeningMode(GetDefaultListeningMode());
                // 状态事件会启动聆听音频；此时还不能确认 ready。
            } else if (current != kDeviceStateListening) {
                CompleteServerListening(false);
            } else if (!pending_listening_start_ && !play_popup_on_listening_ &&
                       audio_service_.IsAudioProcessorRunning()) {
                CompleteServerListening(true);
            }
        });
    } else if (strcmp(type->valuestring, "stt") == 0) {
        auto text = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(text)) {
            std::vector<TextGlyph> glyphs;
            uint8_t bpp = 0;
            if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                glyphs.clear();
            }
            ESP_LOGI(TAG, ">> %s", text->valuestring);
            Schedule([display, message = std::string(text->valuestring),
                      glyphs = std::move(glyphs), bpp]() {
                display->AddTextGlyphs(glyphs, bpp);
                display->SetChatMessage("user", message.c_str());
            });
        }
    } else if (strcmp(type->valuestring, "llm") == 0) {
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(emotion)) {
            Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                display->SetEmotion(emotion_str.c_str());
            });
        }
    } else if (strcmp(type->valuestring, "mcp") == 0) {
        auto payload = cJSON_GetObjectItem(root, "payload");
        auto session_id = cJSON_GetObjectItem(root, "session_id");
        if (cJSON_IsTrue(cJSON_GetObjectItem(root, "music_accompaniment"))) {
            // 伴奏仍使用 head.act 的动作名，但不走可排队的普通工具回调。
            const auto method = cJSON_GetObjectItem(payload, "method");
            const auto params = cJSON_GetObjectItem(payload, "params");
            const auto name = cJSON_GetObjectItem(params, "name");
            const auto args = cJSON_GetObjectItem(params, "arguments");
            const auto action = cJSON_GetObjectItem(args, "action");
            const auto generation = custom_server_audio_.ReplyGeneration();
            if (generation && cJSON_IsString(session_id) && cJSON_IsString(method) && strcmp(method->valuestring, "tools/call") == 0 &&
                cJSON_IsString(name) && strcmp(name->valuestring, "self.head.act") == 0 && cJSON_IsString(action)) {
                Schedule([this, generation, action = std::string(action->valuestring)]() {
                    if (custom_server_audio_.ReplyGeneration() == generation && !aborted_ &&
                        GetDeviceState() == kDeviceStateSpeaking) {
                        Board::GetInstance().TryMusicHeadAction(action);
                    }
                });
            }
            return;
        }
        if (cJSON_IsObject(payload)) {
            // 部分 MCP 回包在 MQTT 收包回调内同步生成。先切到应用任务再发布，
            // 避免重连持发送锁等待 MQTT 回调退出，而回调又在等同一把锁。
            auto send_response = [this, generation = custom_server_audio_.ReplyGeneration(),
                                  session = cJSON_IsString(session_id) ? std::string(session_id->valuestring) : std::string()](const std::string& json_str) {
                if (!generation) {
                    return;
                }
                cJSON* mqtt_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(mqtt_msg, "type", "mcp");
                if (!session.empty()) {
                    cJSON_AddStringToObject(mqtt_msg, "session_id", session.c_str());
                }
                cJSON* payload_obj = cJSON_ParseWithLength(json_str.data(), json_str.size());
                if (payload_obj != nullptr) {
                    cJSON_AddItemToObject(mqtt_msg, "payload", payload_obj);
                }
                char* msg_str = cJSON_PrintUnformatted(mqtt_msg);
                if (msg_str != nullptr) {
                    ESP_LOGI(TAG, "MCP 响应: %s", msg_str);
                    Schedule([this, generation = *generation, message = std::string(msg_str)]() {
                        if (!custom_server_audio_.SendText(message, generation)) {
                            ESP_LOGW(TAG, "MCP 回包未发出：连接已更换、断开或发布失败");
                        }
                    });
                    cJSON_free(msg_str);
                }
                cJSON_Delete(mqtt_msg);
            };
            McpServer::GetInstance().ParseMessage(payload, send_response);
        }
    } else if (strcmp(type->valuestring, "system") == 0) {
        auto command = cJSON_GetObjectItem(root, "command");
        if (cJSON_IsString(command)) {
            ESP_LOGI(TAG, "System command: %s", command->valuestring);
            if (strcmp(command->valuestring, "reboot") == 0) {
                // Do a reboot if user requests a OTA update
                Schedule([this]() { Reboot(); });
            } else {
                ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
            }
        }
    } else if (strcmp(type->valuestring, "alert") == 0) {
        auto status = cJSON_GetObjectItem(root, "status");
        auto message = cJSON_GetObjectItem(root, "message");
        auto emotion = cJSON_GetObjectItem(root, "emotion");
        if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
            Alert(status->valuestring, message->valuestring, emotion->valuestring,
                  Lang::Sounds::OGG_VIBRATION);
        } else {
            ESP_LOGW(TAG, "Alert command requires status, message and emotion");
        }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
    } else if (strcmp(type->valuestring, "custom") == 0) {
        auto payload = cJSON_GetObjectItem(root, "payload");
        CJsonStringUniquePtr root_json(cJSON_PrintUnformatted(root));
        ESP_LOGI(TAG, "Received custom message: %s", root_json ? root_json.get() : "");
        if (cJSON_IsObject(payload)) {
            CJsonStringUniquePtr payload_json(cJSON_PrintUnformatted(payload));
            if (payload_json) {
                Schedule(
                    [this, display, payload_str = std::string(payload_json.get())]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
            }
        } else {
            ESP_LOGW(TAG, "Invalid custom message format: missing payload");
        }
#endif
    } else {
        ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
    }
}
