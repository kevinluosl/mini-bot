#include "ov_camera_watcher.h"
#include "application.h"
#include "ov_http_client.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

namespace {
constexpr const char* kTag = "OvWatch";

// 单次 HTTP 超时。推一张 10KB 的 JPEG 通常几十毫秒，8 秒足够宽松。
constexpr int kHttpTimeoutMs = 8000;

// 采样周期。用户要求"每秒发送一帧"。
constexpr int kFrameIntervalMs = 1000;
// OV 可能在重设时沿用上一轮的画面窗口；先积累三张网关实际接收的新帧。
constexpr unsigned kMonitorRearmFreshFrames = 3;
constexpr int64_t kHeapLogIntervalUs = 30000000;
constexpr size_t kMaxControlReplyBytes = 8 * 1024;
constexpr size_t kMaxResultReplyBytes = 96 * 1024;

void LogWatchHeap() {
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(kTag, "观察内存 internal=%zu min=%zu largest=%zu psram=%zu min=%zu largest=%zu stack_min=%u",
             heap_caps_get_free_size(internal), heap_caps_get_minimum_free_size(internal),
             heap_caps_get_largest_free_block(internal), heap_caps_get_free_size(psram),
             heap_caps_get_minimum_free_size(psram), heap_caps_get_largest_free_block(psram),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

// 仅在有在途问题时轮询结果，避免长轮询阻塞推帧。
constexpr int kResultReadTimeoutMs = 1500;
// 文档规定当前帧最多早于问题 2.5 秒；预留 0.5 秒给请求传输。
constexpr double kCurrentFrameMaxAgeSec = 2.0;
constexpr const char* kSessionExpired = "OV 观察会话已关闭";
constexpr size_t kMaxAnswerBytes = 4096;
constexpr int kViewTimeoutSec = 20;
constexpr const char* kDefaultQuestion = "详细描述画面内容";
// look_current 聚焦人物活动和可能的情绪，要求短答；内部 GetView 的通用问题不变。
constexpr const char* kLookCurrentQuestion = "画面中是否有人，如果有人简洁描述他在干什么，以及可能的情绪，不超过30个字";
// look_past 只问 OV 一次回看（time_scope=past）；实际可回看范围由网关决定。
// 明确要求无人时只答“没有人”，服务端按完整短句静默；有没有人仍由服务端 Jev 兜底判断，板端不解析。
constexpr const char* kLookPastQuestion = "总结过去一分钟画面前的人在做什么？如果没有人，就回答没有人。";
constexpr const char* kLookStreamLost = "没拿到画面：流式会话不可用，请稍后再看";
int ReadSequence(const cJSON* object, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value)) return -1;
    const double number = value->valuedouble;
    if (!std::isfinite(number) || number < 0 || number > std::numeric_limits<int>::max() ||
        number != std::floor(number)) return -1;
    return static_cast<int>(number);
}

std::string_view TextField(const cJSON* item, const char* key) {
    const auto* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, key));
    return value ? value : "";
}

}  // namespace

OvCameraWatcher::OvCameraWatcher(const camera_config_t& config,
                                 const std::string& base_url)
    : config_(config), base_url_(base_url) {
    // 去掉末尾斜杠，避免拼路径时出现双斜杠
    while (!base_url_.empty() && base_url_.back() == '/') {
        base_url_.pop_back();
    }
}

OvCameraWatcher::~OvCameraWatcher() {
    Stop();
    while (task_running_) vTaskDelay(pdMS_TO_TICKS(100));
}

bool OvCameraWatcher::Initialize() {
    if (initialized_) return true;

    const esp_err_t err = esp_camera_init(&config_);
    if (err != ESP_OK) {
        // 不 abort：摄像头坏了设备仍要能正常启动，只是观察功能不可用。
        ESP_LOGE(kTag, "摄像头初始化失败: 0x%x", err);
        return false;
    }
    initialized_ = true;
    ESP_LOGI(kTag, "摄像头就绪");
    return true;
}

std::expected<void, std::string> OvCameraWatcher::Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    return StartLocked();
}

std::expected<void, std::string> OvCameraWatcher::StartLocked() {
    if (!initialized_) {
        return std::unexpected("摄像头未初始化，无法开始观察");
    }
    if (watching_) return {};
    if (task_running_) {
        return std::unexpected("上次观察尚在关闭，请稍后再开启");
    }
    if (base_url_.empty()) return std::unexpected("未配置 OV 网关地址");
    if (!base_url_.starts_with("https://") || base_url_.size() <= sizeof("https://") - 1 ||
        base_url_.find_first_of("@?#\r\n\t ") != std::string::npos) {
        return std::unexpected("OV 网关必须使用不带凭据、查询或片段的 HTTPS 地址");
    }
    constexpr std::string_view key = CONFIG_MINI_BOT_OV_API_KEY;
    if (key.empty() || key.find_first_of("\r\n\t ") != std::string_view::npos) {
        return std::unexpected("未配置有效的 OV API Key");
    }

    // 清空上一轮的答案，避免"这次打开"却夹着上次的内容。
    {
        std::lock_guard<std::mutex> lock(answers_mutex_);
        answers_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_.clear();
    }

    watching_ = true;
    starting_ = true;
    task_running_ = true;

    // 栈 8192：任务里会做 HTTP、cJSON、日志格式化，还要拼 JSON 响应，
    // 给足余量避免溢出。优先级 5 低于音频/网络任务，不抢实时性。
    const BaseType_t ok = xTaskCreatePinnedToCore(
        TaskEntry, "cam_watch", 8192, this, 5, nullptr, 1);
    if (ok != pdPASS) {
        watching_ = false;
        starting_ = false;
        task_running_ = false;
        return std::unexpected("创建观察任务失败（内存不足）");
    }

    ESP_LOGI(kTag, "开始持续观察（QA，每秒一帧，仅按需问答）");
    return {};
}

OvCameraWatcher::MonitorStatus OvCameraWatcher::GetMonitorStatus() const {
    std::lock_guard<std::mutex> lock(monitor_mutex_);
    return monitor_;
}

void OvCameraWatcher::SetMonitor(MonitorStatus settings) {
    {
        std::scoped_lock lock(request_mutex_, monitor_mutex_);
        monitor_ = std::move(settings);
        ++monitor_revision_;
    }
    request_cv_.notify_all();
}

std::expected<void, std::string> OvCameraWatcher::StartMonitor(const std::string& condition,
                                                               const std::string& reminder) {
    if (condition.size() > 2048 || condition.find_first_not_of(" \t\r\n") == std::string::npos) {
        return std::unexpected("请填写简短、非空的监控条件");
    }
    if (reminder.size() > 2048) return std::unexpected("请填写简短的播报要求");
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    auto current = GetMonitorStatus();
    if (watching_ && current.condition == condition && current.reminder == reminder) return {};
    if (auto started = StartLocked(); !started) return started;
    SetMonitor({condition, reminder});
    return {};
}

void OvCameraWatcher::StopMonitor() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!GetMonitorStatus().condition.empty()) SetMonitor({});
}

void OvCameraWatcher::PublishMonitorEvent(const std::string& id, int seq, const std::string& text,
                                           const MonitorStatus& settings) {
    auto message = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(cJSON_CreateObject(), cJSON_Delete);
    if (!message) return;
    cJSON_AddStringToObject(message.get(), "jsonrpc", "2.0");
    cJSON_AddStringToObject(message.get(), "method", "notifications/camera/watch");
    auto* params = cJSON_AddObjectToObject(message.get(), "params");
    if (!params) return;
    cJSON_AddStringToObject(params, "watch_id", id.c_str());
    cJSON_AddNumberToObject(params, "seq", seq);
    if (seq == 0) {
        cJSON_AddBoolToObject(params, "stopped", true);
    } else {
        cJSON_AddStringToObject(params, "text", text.c_str());
        cJSON_AddStringToObject(params, "condition", settings.condition.c_str());
        // 播报要求随对应规则的命中回传，只由对话模型使用。
        if (!settings.reminder.empty()) {
            cJSON_AddStringToObject(params, "reminder", settings.reminder.c_str());
        }
    }
    if (auto* json = cJSON_PrintUnformatted(message.get())) {
        Application::GetInstance().SendServerMcpNotification(json, /*idle_only=*/false);
        cJSON_free(json);
    }
}

void OvCameraWatcher::Stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!GetMonitorStatus().condition.empty()) SetMonitor({});
    if (!watching_) return;

    watching_ = false;
    {
        std::lock_guard<std::mutex> guard(request_mutex_);
        pending_.reset();
    }
    request_cv_.notify_all();
    // MCP 在主循环执行，不能等在途 HTTP。网络资源由观察任务独占并释放。
    ESP_LOGI(kTag, "已请求停止观察");
}

void OvCameraWatcher::TaskEntry(void* arg) {
    auto* self = static_cast<OvCameraWatcher*>(arg);
    self->RunLoop();
    ESP_LOGI(kTag, "观察及网关会话清理已结束");
    // 到这里不再读取会话配置，允许下次 Start 写入新配置。
    self->starting_ = false;
    self->task_running_ = false;
    Application::GetInstance().NotifyToolActivityDone();
    vTaskDelete(nullptr);
}

void OvCameraWatcher::RunLoop() {
    // 整个观察任务共享一个客户端；Stop 只通知退出，不跨线程关闭连接。
    OvHttpClient http(base_url_);
    // get_view 可在建会话期间到达，建会话和首帧也计入它的等待上限。
    constexpr int kMaxFailures = 5;
    for (int attempt = 1; watching_; ++attempt) {
        auto result = OpenSession(http);
        if (result) {
            // 只等首次建会话完成，不等持续观察本身结束。
            starting_ = false;
            Application::GetInstance().NotifyToolActivityDone();
            break;
        }
        ESP_LOGW(kTag, "建会话失败 (%d/%d): %s", attempt, kMaxFailures, result.error().c_str());
        if (attempt >= kMaxFailures) {
            if (GetMonitorStatus().condition.empty()) {
                watching_ = false;
                break;
            }
            // 监控可以继续后台重连，但不能无限占着语音轮的初始化等待。
            if (starting_.exchange(false)) Application::GetInstance().NotifyToolActivityDone();
            attempt = kMaxFailures - 1;
        }
        // 失败退避仍可被 Stop 唤醒；期间的新提问不会提前触发下一次建连。
        std::unique_lock<std::mutex> lock(request_mutex_);
        request_cv_.wait_for(lock, std::chrono::seconds(2), [this] { return !watching_; });
    }
    constexpr int64_t kPollIntervalUs = 250000;
    int64_t start_us = esp_timer_get_time();
    int64_t next_frame_us = start_us, next_poll_us = 0;
    uint32_t monitor_revision = 0;
    MonitorStatus monitor;
    const auto load_monitor = [&] {
        // 配置与版本一起读取，避免停止/重启后把新条件记在旧版本上。
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        monitor_revision = monitor_revision_.load();
        monitor = monitor_;
    };
    load_monitor();
    std::string watch_id;
    bool watch_armed = false;
    bool monitor_submitted = false;
    std::optional<double> monitor_rearm_after_t;
    unsigned monitor_fresh_frames = 0;
    int64_t confirm_by_us = 0;
    const auto close_monitor = [&] {
        if (!watch_id.empty()) PublishMonitorEvent(watch_id, 0, {}, {});
        watch_id.clear();
        watch_armed = false;
        monitor_rearm_after_t.reset();
        monitor_fresh_frames = 0;
        confirm_by_us = 0;
    };
    const auto expire_monitor = [&] {
        close_monitor();
        if (!monitor_submitted) return;
        // 旧会话失效后不重发；只清除属于本次启用的配置。
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        if (monitor_revision == monitor_revision_.load()) monitor_ = {};
        monitor = {};
    };
    std::shared_ptr<ViewRequest> active;
    int64_t abandoned_query_until_us = 0;
    AnswerMatch match{};
    int result_after = 0;
    int consecutive_failures = 0;
    std::optional<double> latest_frame;
    bool stream_lost = false;
    int64_t last_heap_log_us = 0;
    while (watching_) {
        const int64_t now_us = esp_timer_get_time();
        if (!last_heap_log_us || now_us - last_heap_log_us >= kHeapLogIntervalUs) {
            LogWatchHeap();
            last_heap_log_us = now_us;
        }
        std::shared_ptr<ViewRequest> requested;
        {
            std::lock_guard<std::mutex> guard(request_mutex_);
            requested = pending_;
        }
        const auto wanted_revision = monitor_revision_.load();
        if (wanted_revision != monitor_revision) {
            // 尚未收到建立回执时，旧 auto 可能还在路由；直接换会话隔离迟到事件。
            const bool pending_monitor = confirm_by_us != 0;
            const bool confirmed_monitor = watch_armed;
            close_monitor();
            if ((confirmed_monitor || pending_monitor) && !ClearWatch(http)) stream_lost = true;
            if (pending_monitor) stream_lost = true;  // 隔离可能迟到的建立事件。
            load_monitor();
            monitor_submitted = false;
        }
        if (confirm_by_us && now_us >= confirm_by_us) {
            ESP_LOGW(kTag, "OV 监控未确认，已结束本次启用，不自动重发");
            stream_lost = true;
        }
        if (active && active != requested && !abandoned_query_until_us) {
            abandoned_query_until_us = now_us + 6000000;
        }
        bool query_abandoned = abandoned_query_until_us != 0;
        // /text 没有请求 ID。监控中先继续拉结果，给迟到的单次答案一点时间；
        // 一直等不到才换会话；恢复普通看图，但不会给新会话重发旧监控条件。
        if ((query_abandoned && (monitor.condition.empty() || now_us >= abandoned_query_until_us)) || stream_lost) {
            if (active && active == requested) FinishView(active, kSessionExpired, "stream_unavailable");
            stream_lost = false;
            expire_monitor();
            CloseSession(http);
            if (!watching_) break;
            if (!OpenSession(http)) {
                if (monitor.condition.empty()) {
                    watching_ = false;
                    break;
                }
                // 尚未尝试提交条件时，可以重连等待首帧；这不是重发监控。
                stream_lost = true;
                WaitForWork(requested, esp_timer_get_time() + 2000000, &monitor_revision);
                continue;
            }
            active.reset();
            query_abandoned = false;
            abandoned_query_until_us = 0;
            result_after = consecutive_failures = 0;
            latest_frame.reset();
            start_us = next_frame_us = esp_timer_get_time();
        }

        const auto submit = [&](double frame_t) {
            const double question_t = (esp_timer_get_time() - start_us) / 1000000.0;
            if (requested->time_scope == "current" && question_t - frame_t >= kCurrentFrameMaxAgeSec) return;
            {
                std::lock_guard<std::mutex> guard(request_mutex_);
                if (!watching_ || active || pending_ != requested ||
                    std::chrono::steady_clock::now() >= requested->deadline) return;
                active = requested;
            }
            match = {question_t};
            if (auto r = SendQuestion(http, active->question, match.question_t, active->time_scope); !r) {
                // 保留 active，等待线程收到错误后，下一次循环隔离旧会话。
                FinishView(active, "发送画面问题失败：" + r.error(), "");
            }
            next_poll_us = esp_timer_get_time();
        };
        const auto submit_monitor = [&] {
            if (!watching_ || stream_lost || active || confirm_by_us || monitor.condition.empty() ||
                monitor_revision != monitor_revision_.load()) return;
            const double t = (esp_timer_get_time() - start_us) / 1000000.0;
            // 重设只复用原提示词，不复用命中之前的画面。必要时再等一张更新的帧。
            if (monitor_rearm_after_t && (monitor_fresh_frames < kMonitorRearmFreshFrames ||
                                          !latest_frame || t - *latest_frame >= kCurrentFrameMaxAgeSec)) return;
            monitor_submitted = true;
            monitor_rearm_after_t.reset();
            if (SendQuestion(http, monitor.condition, t, "auto")) {
                std::lock_guard<std::mutex> lock(session_mutex_);
                watch_id = session_id_ + "-" + std::to_string(monitor_revision);
                confirm_by_us = esp_timer_get_time() + kViewTimeoutSec * 1000000LL;
                next_poll_us = esp_timer_get_time();
            } else {
                // 超时可能已送达。失败时回收会话，但不盲目重复同一次 POST。
                ESP_LOGW(kTag, "OV 监控提交失败，不自动重发");
                stream_lost = true;
            }
        };
        const auto poll = [&] {
            const bool has_query = active && !query_abandoned && std::chrono::steady_clock::now() < active->deadline;
            if (!watching_ || (active && !has_query && !query_abandoned)) return;
            match.query_pending = static_cast<bool>(active);
            match.monitor_registering = confirm_by_us != 0;
            match.monitor_accepted = false;
            match.status.clear();
            match.reports.clear();
            auto answer = CollectResults(http, result_after, match);
            next_poll_us = esp_timer_get_time() + (has_query || confirm_by_us ? kPollIntervalUs : 1000000);
            if (!answer) {
                ESP_LOGW(kTag, "OV 拉取结果失败: %s", answer.error().c_str());
                return;
            }
            if (match.monitor_accepted) {
                confirm_by_us = 0;
                watch_armed = true;
                std::lock_guard<std::mutex> lock(monitor_mutex_);
                if (monitor_revision == monitor_revision_.load()) monitor_.ready = true;
            }
            if (watch_armed && monitor_revision == monitor_revision_.load()) {
                for (const auto& [seq, text] : match.reports) {
                    if (!watching_ || monitor_revision != monitor_revision_.load()) break;
                    PublishMonitorEvent(watch_id, seq, text, monitor);
                }
                if (!match.reports.empty()) {
                    // model_report 是旧规则的终态。之后采集并被 OV 接受的帧才计入重设门槛。
                    watch_armed = false;
                    monitor_rearm_after_t = (esp_timer_get_time() - start_us) / 1000000.0;
                    monitor_fresh_frames = 0;
                    std::lock_guard<std::mutex> lock(monitor_mutex_);
                    if (monitor_revision == monitor_revision_.load()) monitor_.ready = false;
                }
            }
            if (match.status == "stream_unavailable") stream_lost = true;
            if (match.status == "unexpected_watch") {
                // 固定 scope 不应该进入提醒分支；已存在的真监控不由这次问答清理。
                if (monitor.condition.empty() && !ClearWatch(http)) stream_lost = true;
            } else if (confirm_by_us && !active && !answer->empty()) {
                // 网关接受了 /text，但并未把它路由成提醒；不把普通回答误当命中。
                ESP_LOGW(kTag, "OV 未建立监控规则，不自动重发");
                expire_monitor();
            }
            if (has_query && !answer->empty() && FinishView(active, *answer, match.status)) {
                ESP_LOGI(kTag, "OV answer seq=%d elapsed_ms=%lld", match.result_seq,
                         static_cast<long long>((esp_timer_get_time() - start_us) / 1000 - match.question_t * 1000));
                if (match.status == "ok") AddAnswer(*answer);
                active.reset();
            } else if (query_abandoned && !answer->empty()) {
                active.reset();
                abandoned_query_until_us = 0;
            }
        };
        const bool wants_current = requested && requested->time_scope == "current";
        // 单次看图仍在途时由 submit_monitor 拦住重设，避免 /text 的无 ID 结果串线。
        if (monitor_rearm_after_t) submit_monitor();
        // 已有被接受的画面时优先提交 current；下一帧仍按自己的节拍上传。
        if (!stream_lost && !active && !confirm_by_us && wants_current && latest_frame) submit(*latest_frame);
        if (!requested && !monitor_submitted && latest_frame) submit_monitor();
        // 单问用短轮询，已建立的提醒空闲时每秒取一次；没有任何工作时不拉结果。
        if (active && active == requested &&
            esp_timer_get_time() >= next_poll_us) {
            // 提交失败会清掉 pending；不要再查可能已进入旧会话的结果。
            bool still_pending;
            {
                std::lock_guard<std::mutex> guard(request_mutex_);
                still_pending = pending_ == active;
            }
            if (still_pending) poll();
        } else if (!stream_lost && (query_abandoned || watch_armed || confirm_by_us) &&
                   esp_timer_get_time() >= next_poll_us) {
            poll();
        }
        if (watching_ && !stream_lost && esp_timer_get_time() >= next_frame_us) {
            next_frame_us = esp_timer_get_time() + kFrameIntervalMs * 1000LL;
            auto frame = PushOneFrame(http, start_us);
            if (!frame) {
                ESP_LOGW(kTag, "本轮失败: %s", frame.error().c_str());
                if (frame.error() == kSessionExpired) {
                    if (active) FinishView(active, kSessionExpired, "stream_unavailable");
                    stream_lost = true;
                } else if (++consecutive_failures >= kMaxFailures) {
                    if (monitor.condition.empty()) {
                        watching_ = false;
                        break;
                    }
                    stream_lost = true;
                }
            } else {
                consecutive_failures = 0;
                if (frame->has_value()) {
                    latest_frame = **frame;
                    if (monitor_rearm_after_t && *latest_frame > *monitor_rearm_after_t &&
                        monitor_fresh_frames < kMonitorRearmFreshFrames) ++monitor_fresh_frames;
                }
            }
            if (!stream_lost && !active && !confirm_by_us && requested && frame && frame->has_value()) submit(**frame);
        }
        int64_t wake_us = next_frame_us;
        if ((active || watch_armed || confirm_by_us) && next_poll_us < wake_us) wake_us = next_poll_us;
        if (abandoned_query_until_us && abandoned_query_until_us < wake_us) wake_us = abandoned_query_until_us;
        if (!stream_lost) WaitForWork(requested, wake_us, &monitor_revision);
    }
    expire_monitor();

    {
        std::lock_guard<std::mutex> guard(request_mutex_);
        pending_.reset();
    }
    request_cv_.notify_all();
    ESP_LOGI(kTag, "观察任务退出");
    // 任务自己负责关会话：它最清楚本轮 HTTP 是否已经结束。
    // 若交给 Stop() 关，Stop 可能在任务还在用这个会话时就把会话删了，
    // 导致任务下一轮拿无效 session 推帧。关会话后局部客户端随任务释放。
    CloseSession(http);
}

std::expected<void, std::string> OvCameraWatcher::OpenSession(OvHttpClient& http) {
    // 实际摄像头帧率与每秒一次的上传节拍不是同一个概念。
    // 只发送公开参数，历史保留策略由网关决定。
    constexpr auto payload = R"({"input_mode":"live","mode":"qa","fps":30.0})";
    auto response = http.Request("/v1/glasses/sessions", HTTP_METHOD_POST,
                              kHttpTimeoutMs, kMaxControlReplyBytes, "application/json", payload);
    if (!response) return std::unexpected("连接网关失败: " + response.error());
    if (response->status != 201) {
        // 建会话响应可能包含凭据，任何分支都不要记录原始正文。
        return std::unexpected("建会话 HTTP " + std::to_string(response->status));
    }

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(response->body.c_str()), cJSON_Delete);
    const auto* id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root.get(), "session_id"));
    const std::string sid = id ? id : "";
    if (sid.empty() || sid.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
                           std::string::npos) {
        return std::unexpected("建会话响应缺少有效的 session_id");
    }

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_ = sid;
    }
    if (TextField(root.get(), "mode") != "qa") {
        CloseSession(http);
        return std::unexpected("OV 未建立 QA 会话");
    }
    ESP_LOGI(kTag, "会话已建立: %s (qa, fps=30)", sid.c_str());
    return {};
}

void OvCameraWatcher::CloseSession(OvHttpClient& http) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid.swap(session_id_);
    }
    if (sid.empty()) return;

    auto response = http.Request("/v1/glasses/sessions/" + sid, HTTP_METHOD_DELETE,
                                 3000, kMaxControlReplyBytes);
    if (!response || response->status < 200 || response->status >= 300) {
        ESP_LOGW(kTag, "OV 会话关闭未确认: %s",
                 (response ? std::to_string(response->status) : response.error()).c_str());
        return;
    }
    ESP_LOGI(kTag, "会话已关闭: %s", sid.c_str());
}

bool OvCameraWatcher::IsValidTimeScope(const std::string& time_scope) {
    return time_scope == "current" || time_scope == "past" || time_scope == "auto";
}

std::string OvCameraWatcher::GetView(const std::string& question, const std::string& time_scope) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kViewTimeoutSec);
    // 未知取值按默认看当下，避免把模型自造的参数传给网关。
    return RequestView(question, IsValidTimeScope(time_scope) ? time_scope : "current", deadline).text;
}

std::expected<std::string, std::string> OvCameraWatcher::LookCurrent(const std::string& question,
                                                                     std::string* session_id) {
    // 缺省或空白沿用本工具的默认问题；明确提供的提示词原样传给 OV。
    if (question.find_first_not_of(" \t\r\n") == std::string::npos) {
        return LookOnce(kLookCurrentQuestion, "current", session_id);
    }
    return LookOnce(question, "current", session_id);
}

std::expected<std::string, std::string> OvCameraWatcher::LookPast() {
    return LookOnce(kLookPastQuestion, "past");
}

std::expected<std::string, std::string> OvCameraWatcher::LookOnce(const std::string& question, const char* time_scope,
                                                                  std::string* session_id) {
    if (session_id) session_id->clear();
    // 实时优先：一次 look 只向 OV 提一个问题，最多等 20 秒。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kViewTimeoutSec);
    auto reply = RequestView(question, time_scope, deadline);
    if (reply.status == "ok") {
        if (session_id) *session_id = std::move(reply.session_id);
        return std::move(reply.text);
    }
    // 非 ok 一律当工具错误，服务端不会拿它判断有人无人，也不会当成画面线索接话。
    if (reply.status == "stream_unavailable") return std::unexpected(kLookStreamLost);
    return std::unexpected(reply.text.empty() ? "没拿到画面" : reply.text);
}

OvCameraWatcher::ViewReply OvCameraWatcher::RequestView(const std::string& question, const std::string& time_scope,
                                                         std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(request_mutex_);
    if (!watching_) return {"摄像头没有启动观察", ""};
    if (pending_) return {"正在等待上一个画面问题的答案", ""};
    auto request = std::make_shared<ViewRequest>();
    request->deadline = deadline;
    request->time_scope = time_scope;
    request->question = question.find_first_not_of(" \t\r\n") == std::string::npos ? kDefaultQuestion : question;
    pending_ = request;
    request_cv_.notify_all();  // 唤醒观察任务，不等下一次推帧。
    request_cv_.wait_until(lock, deadline, [&] { return pending_ != request || !watching_; });
    if (pending_ == request) {
        pending_.reset();
        request_cv_.notify_all();  // 观察任务发现超时后隔离旧会话。
    }
    if (!request->reply.empty()) return {request->reply, request->status, request->session_id};
    return {watching_ ? std::to_string(kViewTimeoutSec) + " 秒内未收到本次问题的画面描述"
                      : "摄像头已停止观察",
            ""};
}

void OvCameraWatcher::WaitForWork(const std::shared_ptr<ViewRequest>& snapshot, int64_t wake_us,
                                  const uint32_t* monitor_revision) {
    std::unique_lock<std::mutex> lock(request_mutex_);
    request_cv_.wait_for(lock, std::chrono::microseconds(wake_us - esp_timer_get_time()), [&] {
        return pending_ != snapshot || !watching_ || (monitor_revision && *monitor_revision != monitor_revision_.load());
    });
}

bool OvCameraWatcher::FinishView(const std::shared_ptr<ViewRequest>& request, const std::string& reply,
                                 const std::string& status) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }
    std::lock_guard<std::mutex> guard(request_mutex_);
    if (pending_ != request || !watching_ || std::chrono::steady_clock::now() >= request->deadline) return false;
    request->reply = reply;
    request->status = status;
    request->session_id = std::move(sid);  // 与答案一并交付，不能事后读取可能已重建的会话。
    pending_.reset();
    request_cv_.notify_all();
    return true;
}

std::expected<void, std::string> OvCameraWatcher::SendQuestion(OvHttpClient& http, const std::string& question, double t_sec,
                                                              const std::string& time_scope) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }
    if (sid.empty()) return std::unexpected("没有有效会话");

    cJSON* root = cJSON_CreateObject();
    char* printed = nullptr;
    // time_scope 由网关决定看当前帧还是回看；current / past 会跳过计数、提醒等自动分类。
    if (root && cJSON_AddStringToObject(root, "text", question.c_str()) &&
        cJSON_AddNumberToObject(root, "t", t_sec) &&
        cJSON_AddStringToObject(root, "time_scope", time_scope.c_str())) {
        printed = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    if (!printed) return std::unexpected("生成观察提示词 JSON 失败");
    std::unique_ptr<char, decltype(&cJSON_free)> body(printed, cJSON_free);
    ESP_LOGI(kTag, "OV question sent t=%.6f scope=%s", t_sec, time_scope.c_str());
    auto response = http.Request("/v1/glasses/sessions/" + sid + "/text", HTTP_METHOD_POST,
                              kHttpTimeoutMs, kMaxControlReplyBytes, "application/json", body.get());
    if (!response) return std::unexpected(response.error());
    if (response->status != 200) {
        return std::unexpected("观察提示词 HTTP " + std::to_string(response->status));
    }
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root_reply(cJSON_Parse(response->body.c_str()), cJSON_Delete);
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root_reply.get(), "accepted"))) {
        return std::unexpected("OV 未确认接收本次画面问题");
    }
    ESP_LOGI(kTag, "OV question accepted t=%.6f", t_sec);
    return {};
}

std::expected<std::optional<double>, std::string> OvCameraWatcher::PushOneFrame(OvHttpClient& http, int64_t session_start_us) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }
    if (sid.empty()) return std::unexpected("没有有效会话");

    const std::string session_path = "/v1/glasses/sessions/" + sid;

    auto frame = CaptureFreshFrame();
    if (!frame) return std::unexpected(frame.error());
    const double t_sec = (frame->captured_us - session_start_us) / 1000000.0;
    char path[48];
    snprintf(path, sizeof(path), "/frame?t=%.6f", t_sec);

    auto response = http.Request(session_path + path, HTTP_METHOD_POST, kHttpTimeoutMs,
                              kMaxControlReplyBytes, "image/jpeg", frame->jpeg);
    if (!response) return std::unexpected("推帧失败: " + response.error());
    if (response->status == 404) return std::unexpected(kSessionExpired);
    if (response->status != 200) {
        return std::unexpected("推帧 HTTP " + std::to_string(response->status));
    }

    // HTTP 200 只代表请求成功；服务端可能按秒采样，实际未接收这张图。
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> reply(cJSON_Parse(response->body.c_str()), cJSON_Delete);
    const auto* accepted = cJSON_GetObjectItem(reply.get(), "accepted");
    if (cJSON_IsTrue(accepted)) return t_sec;
    if (cJSON_IsFalse(accepted)) {
        ESP_LOGD(kTag, "OV frame skipped t=%.6f", t_sec);
        return std::nullopt;
    }
    return std::unexpected("OV 推帧响应缺少 accepted");
}

std::expected<void, std::string> OvCameraWatcher::ClearWatch(OvHttpClient& http) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }
    if (sid.empty()) return std::unexpected("没有有效会话");

    auto response = http.Request("/v1/glasses/sessions/" + sid + "/watch", HTTP_METHOD_DELETE,
                                 kResultReadTimeoutMs, kMaxControlReplyBytes);
    if (!response) return std::unexpected(response.error());
    if (response->status != 200) return std::unexpected("HTTP " + std::to_string(response->status));
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> reply(cJSON_Parse(response->body.c_str()), cJSON_Delete);
    const auto* stopped = cJSON_GetObjectItemCaseSensitive(reply.get(), "stopped");
    // false 表示当前没有活跃提醒，也符合清理完成的条件。
    if (!cJSON_IsBool(stopped)) return std::unexpected("响应缺少有效的 stopped 字段");
    return {};
}

std::expected<std::string, std::string> OvCameraWatcher::CollectResults(OvHttpClient& http, int& after, AnswerMatch& match) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }
    if (sid.empty()) return std::unexpected("没有有效的 QA 会话");
    auto response = http.Request("/v1/glasses/sessions/" + sid +
                              "/results?after=" + std::to_string(after) + "&wait=0", HTTP_METHOD_GET,
                              kResultReadTimeoutMs, kMaxResultReplyBytes);
    if (!response) return std::unexpected(response.error());
    if (response->status == 404) {
        match.status = "stream_unavailable";
        return kSessionExpired;
    }
    if (response->status != 200) return std::unexpected("读取 OV 回答 HTTP " + std::to_string(response->status));

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(response->body.c_str()), cJSON_Delete);
    const auto* results = cJSON_GetObjectItemCaseSensitive(root.get(), "results");
    if (!cJSON_IsArray(results)) return std::unexpected("OV 结果页格式异常");
    // /text 不返回可关联的 ID；单问题在途、已读 seq 和超时换会话共同隔离迟到结果。
    // 结果的 t 是画面时间，不是问题时间。不要用它拒绝正常的历史答案。
    int newest = after;
    const cJSON* chosen = nullptr;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, results) {
        const int seq = ReadSequence(item, "seq");
        if (seq < 0) return std::unexpected("OV 结果缺少有效的 seq");
        if (seq <= after) continue;
        if (seq > newest) newest = seq;
        const auto type = TextField(item, "type");
        if (type == "closed" || type == "error") {
            match.status = "stream_unavailable";
            return kSessionExpired;
        }
        const auto status = TextField(item, "status");
        if (type != "answer" || status.empty()) continue;
        if (status == "model_report") {
            // 命中和普通问题共享 seq，但永远不能作为 look 的答案。
            const auto text = TextField(item, "text");
            if ((!match.monitor_registering || match.monitor_accepted) && text.size() <= kMaxAnswerBytes &&
                text.find_first_not_of(" \t\r\n") != std::string_view::npos &&
                match.reports.size() < 8) match.reports.emplace_back(seq, std::string(text));
            continue;
        }
        if (status == "watch_accepted") {
            if (match.monitor_registering) {
                match.monitor_accepted = true;
                continue;
            }
            after = newest;
            match.status = "unexpected_watch";
            return "网关建立了异步提醒，不是单次画面回答";
        }
        if (!match.query_pending && !match.monitor_registering) continue;
        if (chosen) return std::unexpected("OV 返回多条答案，无法确认归属");
        chosen = item;
    }
    if (!chosen) {
        after = newest;
        return std::string{};
    }
    const auto text = TextField(chosen, "text");
    const auto status = TextField(chosen, "status");
    if (text.size() > kMaxAnswerBytes || text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        if (status == "stream_unavailable") {
            match.status = status;
            return kSessionExpired;
        }
        return std::unexpected("OV 回答正文缺失或过长");
    }
    after = newest;
    match.result_seq = ReadSequence(chosen, "seq");
    match.status = status;
    if (status == "ok") {
        // 文档规定视觉回答才有取证窗口；没有它的闲聊不能当作看到了画面。
        const auto* window = cJSON_GetObjectItemCaseSensitive(chosen, "source_window_s");
        const auto* first = cJSON_GetArrayItem(window, 0);
        const auto* last = cJSON_GetArrayItem(window, 1);
        if (!cJSON_IsArray(window) || cJSON_GetArraySize(window) != 2 ||
            !cJSON_IsNumber(first) || !cJSON_IsNumber(last) || !std::isfinite(first->valuedouble) ||
            !std::isfinite(last->valuedouble) || first->valuedouble < 0 || last->valuedouble < first->valuedouble) {
            match.status = "missing_evidence";
            return "视觉网关没有返回可用的画面证据";
        }
    }
    return std::string(text);
}

void OvCameraWatcher::AddAnswer(const std::string& text) {
    std::lock_guard<std::mutex> lock(answers_mutex_);
    answers_.push_back(text);
    // 只保留最新 kMaxAnswers 条：用户问"刚才看到什么"只需要最近的画面，
    // 无限累积会让返回给大模型的文本越来越长、且早期内容早已过时。
    if ((int)answers_.size() > kMaxAnswers) {
        answers_.erase(answers_.begin());
    }
}

std::string OvCameraWatcher::GetRecentAnswersJson() const {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }

    cJSON* root = cJSON_CreateObject();
    const bool watching = watching_;

    if (!watching) {
        // 未启动时必须明确告知，否则大模型会凭空编造画面内容回答用户。
        cJSON_AddBoolToObject(root, "watching", false);
        cJSON_AddStringToObject(root, "message", "摄像头没有启动观察");
        char* printed = cJSON_PrintUnformatted(root);
        std::string out = printed != nullptr ? printed : R"({"watching":false})";
        if (printed != nullptr) cJSON_free(printed);
        cJSON_Delete(root);
        return out;
    }

    cJSON_AddBoolToObject(root, "watching", true);

    std::lock_guard<std::mutex> lock(answers_mutex_);
    cJSON* arr = cJSON_AddArrayToObject(root, "answers");
    for (const auto& a : answers_) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(a.c_str()));
    }
    cJSON_AddNumberToObject(root, "count", (int)answers_.size());

    if (answers_.empty()) {
        // 已启动但还没拿到答案（可能刚启动，或网关尚未返回）。
        // 这也要如实说明，避免大模型把空数组误当成"什么也没有"。
        cJSON_AddStringToObject(root, "message",
                                "摄像头正在观察，但还没有收到画面描述");
    }

    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed != nullptr ? printed : R"({"watching":true})";
    if (printed != nullptr) cJSON_free(printed);
    cJSON_Delete(root);
    return out;
}

std::expected<std::string, std::string> OvCameraWatcher::CaptureJpeg() {
    auto frame = CaptureFreshFrame();
    if (!frame) return std::unexpected(frame.error());
    return std::move(frame->jpeg);
}

std::expected<OvCameraWatcher::CapturedFrame, std::string> OvCameraWatcher::CaptureFreshFrame() {
    if (!initialized_) return std::unexpected("摄像头未就绪");
    const auto requested_us = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(frame_mutex_);
    // 驱动的帧时间戳同样来自 esp_timer_get_time。双缓冲闲置时可能积压两张旧图。
    for (size_t i = 0; i <= config_.fb_count; ++i) {
        std::unique_ptr<camera_fb_t, decltype(&esp_camera_fb_return)> fb(
            esp_camera_fb_get(), esp_camera_fb_return);
        if (!fb) return std::unexpected("摄像头取帧失败");
        const auto timestamp = int64_t(fb->timestamp.tv_sec) * 1000000 + fb->timestamp.tv_usec;
        if (timestamp < requested_us) continue;
        if (fb->format != PIXFORMAT_JPEG || !fb->buf || !fb->len || fb->len > 128 * 1024)
            return std::unexpected("摄像头 JPEG 无效或过大");
        return CapturedFrame{std::string(reinterpret_cast<const char*>(fb->buf), fb->len), timestamp};
    }
    return std::unexpected("未取得本次调用的新画面");
}
