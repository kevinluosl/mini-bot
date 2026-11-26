#include "ov_camera_watcher.h"
#include "application.h"

#include <cJSON.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

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
constexpr int64_t kHeapLogIntervalUs = 30000000;
constexpr size_t kMaxControlReplyBytes = 8 * 1024;
constexpr size_t kMaxResultReplyBytes = 96 * 1024;

struct OvHttpReply {
    int status = 0;
    std::string body;
    size_t limit = 0;
    bool too_large = false;
};

// OV 全部 I/O 已在同一个观察任务中串行执行。使用同步 HTTP，避开第三方
// EspTcp 的接收任务与客户端销毁竞态；JPEG 由调用者持有，不再拼进一份大请求字符串。
std::expected<OvHttpReply, std::string> RequestOv(
    const std::string& url, esp_http_client_method_t method, int timeout_ms,
    size_t reply_limit = kMaxControlReplyBytes, std::string_view content_type = {},
    std::string_view body = {}, const char* logs_token = nullptr) {
    OvHttpReply reply;
    reply.limit = reply_limit;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.method = method;
    config.timeout_ms = timeout_ms;
    // 不跟随重定向，避免问答诊断凭据被带到其他地址。
    config.disable_auto_redirect = true;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    config.user_data = &reply;
    config.event_handler = [](esp_http_client_event_t* event) -> esp_err_t {
        if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) return ESP_OK;
        auto& response = *static_cast<OvHttpReply*>(event->user_data);
        if (response.too_large) return ESP_OK;
        const auto length = static_cast<size_t>(event->data_len);
        if (length > response.limit - response.body.size()) {
            response.too_large = true;
            return ESP_OK;
        }
        response.body.append(static_cast<const char*>(event->data), length);
        return ESP_OK;
    };
    std::unique_ptr<esp_http_client, decltype(&esp_http_client_cleanup)> client(
        esp_http_client_init(&config), esp_http_client_cleanup);
    if (!client) return std::unexpected("创建 OV HTTP 客户端失败");
    if ((!content_type.empty() && esp_http_client_set_header(client.get(), "Content-Type", content_type.data()) != ESP_OK) ||
        (logs_token && esp_http_client_set_header(client.get(), "X-Session-Log-Token", logs_token) != ESP_OK) ||
        (!body.empty() && (body.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                          esp_http_client_set_post_field(client.get(), body.data(), static_cast<int>(body.size())) != ESP_OK))) {
        return std::unexpected("设置 OV HTTP 请求失败");
    }
    const auto err = esp_http_client_perform(client.get());
    if (err != ESP_OK) return std::unexpected(esp_err_to_name(err));
    if (reply.too_large) return std::unexpected("OV HTTP 响应超过内存限制");
    reply.status = esp_http_client_get_status_code(client.get());
    return reply;
}

void LogWatchHeap() {
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(kTag, "观察内存 internal=%zu min=%zu largest=%zu psram=%zu min=%zu largest=%zu stack_min=%u",
             heap_caps_get_free_size(internal), heap_caps_get_minimum_free_size(internal),
             heap_caps_get_largest_free_block(internal), heap_caps_get_free_size(psram),
             heap_caps_get_minimum_free_size(psram), heap_caps_get_largest_free_block(psram),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

// QA 有在途问题时才查诊断与结果，使用短请求以免阻塞下一帧。
constexpr int kResultReadTimeoutMs = 1500;
constexpr size_t kMaxAnswerBytes = 4096;
constexpr int kViewTimeoutSec = 20;
constexpr const char* kDefaultQuestion = "详细描述画面内容";
// look_current 聚焦人物活动和可能的情绪，要求短答；内部 GetView 的通用问题不变。
constexpr const char* kLookCurrentQuestion = "画面中是否有人，如果有人简洁描述他在干什么，以及可能的情绪，不超过30个字";
// look_past 只问 OV 一次回看（time_scope=past）。会话最多保留 60 帧、每秒一帧，约一分钟。
// 明确要求无人时只答“没有人”，服务端按完整短句静默；有没有人仍由服务端 Jev 兜底判断，板端不解析。
constexpr const char* kLookPastQuestion = "总结过去一分钟画面前的人在做什么？如果没有人，就回答没有人。";
constexpr const char* kLookStreamLost = "没拿到画面：流式会话不可用，已重新建立观察会话，请稍后再看";
// 板端发送微秒精度的会话时间，仅容许 JSON 数字转换的亚微秒误差。
constexpr double kTimestampToleranceSec = 0.00000051;

int ReadSequence(const cJSON* object, const char* key) {
    const auto* value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(value)) return -1;
    const double number = value->valuedouble;
    if (!std::isfinite(number) || number < 0 || number > std::numeric_limits<int>::max() ||
        number != std::floor(number)) return -1;
    return static_cast<int>(number);
}

struct EvidenceWindow {
    double first;
    double last;
};

std::optional<EvidenceWindow> ReadEvidenceWindow(const cJSON* item, const char* key) {
    const auto* window = cJSON_GetObjectItemCaseSensitive(item, key);
    if (!cJSON_IsArray(window) || cJSON_GetArraySize(window) != 2) return std::nullopt;
    const auto* first = cJSON_GetArrayItem(window, 0);
    const auto* last = cJSON_GetArrayItem(window, 1);
    if (!cJSON_IsNumber(first) || !cJSON_IsNumber(last) ||
        !std::isfinite(first->valuedouble) || !std::isfinite(last->valuedouble) ||
        first->valuedouble < 0 || last->valuedouble < first->valuedouble) return std::nullopt;
    return EvidenceWindow{first->valuedouble, last->valuedouble};
}

std::string_view TextField(const cJSON* item, const char* key) {
    const auto* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, key));
    return value ? value : "";
}

bool IsAnswerError(std::string_view status) {
    return status == "insufficient_evidence" || status == "busy" || status == "stream_unavailable" ||
           status == "vision_timeout" || status == "vision_queue_timeout" ||
           status == "vision_release_unconfirmed" || status == "clarification" ||
           status == "ambiguous_reference" || status == "invalid_visual_format";
}

bool HasValidEvidence(const cJSON* item, double frame_t, double question_t, std::string_view type,
                      bool from_results) {
    const auto source = ReadEvidenceWindow(item, "source_window_s");
    if (!source) return false;
    if (type == "current_qa_v1") {
        return source->first >= frame_t - kTimestampToleranceSec &&
               source->last <= question_t + kTimestampToleranceSec;
    }
    const bool history = type == "history_history_v1" || type == "history_replay_v1" ||
                         type == "history_inventory_v1" || type == "history_attribute_v1" ||
                         type == "history_action_count_v1" || type == "history_appearance_count_v1";
    if (!history) return false;
    // 未指定时段的流式回看会一直读到作答时刻，19200 实测取证终点比提问晚约 1 秒。
    // 超出本轮等待上限的证据不可能属于本轮问题。
    if (source->last > question_t + kViewTimeoutSec) return false;
    // 诊断页只有实际取证窗口；已关联的回看答案还须在结果页核对请求窗口。
    if (!from_results) return true;
    const auto requested = ReadEvidenceWindow(item, "requested_window_s");
    if (!requested || source->first < requested->first - kTimestampToleranceSec ||
        source->last > requested->last + kTimestampToleranceSec) return false;
    // 指定时段的回看：请求窗口在提问前结束，证据落在请求窗口内即可。
    if (requested->last <= question_t + kTimestampToleranceSec) return true;
    // 流式回看：请求窗口随流延伸到作答时刻，必须覆盖本轮确认的新帧，排除旧的总结。
    return TextField(item, "coverage") == "stream" && source->last >= frame_t - kTimestampToleranceSec;
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
    ESP_LOGI(kTag, "摄像头就绪，网关=%s", base_url_.c_str());
    return true;
}

std::expected<void, std::string> OvCameraWatcher::Start() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!initialized_) {
        return std::unexpected("摄像头未初始化，无法开始观察");
    }
    if (watching_) return {};
    if (task_running_) {
        return std::unexpected("上次观察尚在关闭，请稍后再开启");
    }
    if (base_url_.empty()) {
        return std::unexpected("未配置 OV 网关地址");
    }

    // 清空上一轮的答案，避免"这次打开"却夹着上次的内容。
    {
        std::lock_guard<std::mutex> lock(answers_mutex_);
        answers_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_.clear();
        logs_token_.clear();
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

void OvCameraWatcher::Stop() {
    std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
    if (!watching_) return;

    watching_ = false;
    {
        std::lock_guard<std::mutex> guard(request_mutex_);
        pending_.reset();
    }
    request_cv_.notify_all();

    // 等任务自己退出，但不在主线程上久等。
    //
    // 本函数可能被 MCP 工具（主线程）或串口命令调用，阻塞太久会卡住音频与显示。
    // 所以只等 2 秒：任务正常情况下在 delay 或短 HTTP 里，能很快看到
    // watching_ 变 false 并退出；即使这一轮正在跑长 HTTP（超时上限几秒），
    // 它也会在自己退出时调 CloseSession() 收尾，不需要 Stop 帮它兜。
    for (int i = 0; i < 20 && task_running_; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 网关 I/O 完全由采样任务串行管理，Stop 不删除仍在使用的会话。
    ESP_LOGI(kTag, "已停止观察，缓存 %d 条答案",
             [this] {
                 std::lock_guard<std::mutex> lock(answers_mutex_);
                 return (int)answers_.size();
             }());
}

void OvCameraWatcher::TaskEntry(void* arg) {
    auto* self = static_cast<OvCameraWatcher*>(arg);
    self->RunLoop();
    // 到这里不再读取会话配置，允许下次 Start 写入新配置。
    self->starting_ = false;
    self->task_running_ = false;
    Application::GetInstance().NotifyToolActivityDone();
    vTaskDelete(nullptr);
}

void OvCameraWatcher::RunLoop() {
    // get_view 可在建会话期间到达，建会话和首帧也计入它的等待上限。
    constexpr int kMaxFailures = 5;
    for (int attempt = 1; watching_; ++attempt) {
        auto result = OpenSession();
        if (result) {
            // 只等首次建会话完成，不等持续观察本身结束。
            starting_ = false;
            Application::GetInstance().NotifyToolActivityDone();
            break;
        }
        ESP_LOGW(kTag, "建会话失败 (%d/%d): %s", attempt, kMaxFailures, result.error().c_str());
        if (attempt >= kMaxFailures) {
            watching_ = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    constexpr int64_t kPollIntervalUs = 250000;
    int64_t start_us = esp_timer_get_time();
    int64_t next_frame_us = start_us, next_poll_us = 0;
    std::shared_ptr<ViewRequest> active;
    AnswerMatch match{};
    int logs_after = 0, result_after = 0;
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
        // 不确定网关是否已收到的提问仍可能迟到。先换会话，再接新问题。
        if ((active && active != requested) || stream_lost) {
            stream_lost = false;
            CloseSession();
            if (!watching_ || !OpenSession()) {
                watching_ = false;
                break;
            }
            active.reset();
            logs_after = result_after = consecutive_failures = 0;
            latest_frame.reset();
            start_us = next_frame_us = esp_timer_get_time();
        }

        const auto submit = [&](double frame_t) {
            {
                std::lock_guard<std::mutex> guard(request_mutex_);
                if (!watching_ || !requested || active || pending_ != requested ||
                    std::chrono::steady_clock::now() >= requested->deadline) return;
                active = requested;
            }
            match = {(esp_timer_get_time() - start_us) / 1000000.0, frame_t, requested->current_only};
            if (auto r = SendQuestion(active->question, match.question_t, active->time_scope); !r) {
                // 保留 active，等待线程收到错误后，下一次循环隔离旧会话。
                FinishView(active, "发送画面问题失败：" + r.error(), "");
            }
            next_poll_us = esp_timer_get_time();
        };
        const auto poll = [&](bool current) {
            if (!watching_ || std::chrono::steady_clock::now() >= active->deadline) return;
            auto answer = current ? CollectCurrentResult(result_after, match) : CollectResults(logs_after, match);
            next_poll_us = esp_timer_get_time() + kPollIntervalUs;
            if (!answer) {
                ESP_LOGW(kTag, "OV 拉取结果失败: %s", answer.error().c_str());
            } else if (!answer->empty() && FinishView(active, *answer, match.status)) {
                if (match.result_seq > result_after) result_after = match.result_seq;
                ESP_LOGI(kTag, "OV answer seq=%d elapsed_ms=%lld", match.result_seq,
                         static_cast<long long>((esp_timer_get_time() - start_us) / 1000 - match.question_t * 1000));
                if (match.status == "ok") AddAnswer(*answer);
                stream_lost = match.status == "stream_unavailable";
                active.reset();
            }
        };
        // 已有被接受的画面时优先提交 current；下一帧仍按自己的节拍上传。
        const bool wants_current = requested && requested->time_scope == "current";
        if (!active && wants_current && latest_frame) submit(*latest_frame);
        // 用独立的短轮询节拍取答，不使用会卡住推帧的长轮询。
        if (active && active == requested && active->time_scope == "current" &&
            esp_timer_get_time() >= next_poll_us) {
            // 提交失败会清掉 pending；不要再查可能已进入旧会话的结果。
            bool still_pending;
            {
                std::lock_guard<std::mutex> guard(request_mutex_);
                still_pending = pending_ == active;
            }
            if (still_pending) poll(true);
        }
        if (watching_ && !stream_lost && esp_timer_get_time() >= next_frame_us) {
            next_frame_us = esp_timer_get_time() + kFrameIntervalMs * 1000LL;
            auto frame = PushOneFrame(start_us);
            if (!frame) {
                ESP_LOGW(kTag, "本轮失败: %s", frame.error().c_str());
                if (++consecutive_failures >= kMaxFailures) {
                    watching_ = false;
                    break;
                }
            } else {
                consecutive_failures = 0;
                if (frame->has_value()) latest_frame = **frame;
            }
            if (active && active == requested && active->time_scope != "current") poll(false);
            if (!active && requested && frame && frame->has_value()) submit(**frame);
        }
        int64_t wake_us = next_frame_us;
        if (active && active->time_scope == "current" && next_poll_us < wake_us) wake_us = next_poll_us;
        if (!stream_lost) WaitForWork(requested, wake_us);
    }

    {
        std::lock_guard<std::mutex> guard(request_mutex_);
        pending_.reset();
    }
    request_cv_.notify_all();
    ESP_LOGI(kTag, "观察任务退出");
    // 任务自己负责关会话：它最清楚本轮 HTTP 是否已经结束。
    // 若交给 Stop() 关，Stop 可能在任务还在用这个会话时就把会话删了，
    // 导致任务下一轮拿无效 session 推帧。CloseSession 是幂等的，
    // 两边都调也安全（sid 为空时直接返回）。
    CloseSession();
}

std::expected<void, std::string> OvCameraWatcher::OpenSession() {
    // 实际摄像头帧率与每秒一次的上传节拍不是同一个概念。
    // 每秒一帧时保留 60 帧、超出后压到 45 帧：“刚才”约覆盖 45–60 秒。
    constexpr auto payload = R"({"input_mode":"live","mode":"qa","fps":30.0,"answer_memory":false,"max_visual_frames":60,"compact_to_visual_frames":45})";
    auto response = RequestOv(base_url_ + "/v1/glasses/sessions", HTTP_METHOD_POST,
                              kHttpTimeoutMs, kMaxControlReplyBytes, "application/json", payload);
    if (!response) return std::unexpected("连接网关失败: " + response.error());
    if (response->status != 200 && response->status != 201) {
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

    const auto token = TextField(root.get(), "logs_token");
    const auto logs_path = TextField(root.get(), "logs_url");
    bool valid_token = !token.empty();
    for (const unsigned char ch : token) {
        if (ch < 0x21 || ch > 0x7e) valid_token = false;
    }
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_id_ = sid;
    }
    // 只允许已知的同源路径，避免把凭据发往响应中指定的其他地址。
    if (TextField(root.get(), "mode") != "qa" || !valid_token ||
        logs_path != "/v1/glasses/sessions/" + sid + "/logs") {
        CloseSession();
        return std::unexpected("OV 未返回有效的 QA 会话诊断凭据");
    }
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        logs_token_ = token;
    }
    ESP_LOGI(kTag, "会话已建立: %s (qa, fps=30, 回看 60/45 帧)", sid.c_str());
    return {};
}

void OvCameraWatcher::CloseSession() {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid.swap(session_id_);
        logs_token_.clear();
    }
    if (sid.empty()) return;

    auto response = RequestOv(base_url_ + "/v1/glasses/sessions/" + sid, HTTP_METHOD_DELETE, 3000);
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
    return RequestView(question, false, IsValidTimeScope(time_scope) ? time_scope : "current", deadline).text;
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
    auto reply = RequestView(question, false, time_scope, deadline);
    if (reply.status == "ok") {
        if (session_id) *session_id = std::move(reply.session_id);
        return std::move(reply.text);
    }
    // 非 ok 一律当工具错误，服务端不会拿它判断有人无人，也不会当成画面线索接话。
    if (reply.status == "stream_unavailable") return std::unexpected(kLookStreamLost);
    return std::unexpected(reply.text.empty() ? "没拿到画面" : reply.text);
}

OvCameraWatcher::ViewReply OvCameraWatcher::RequestView(const std::string& question, bool current_only,
                                                         const std::string& time_scope,
                                                         std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(request_mutex_);
    if (!watching_) return {"摄像头没有启动观察", ""};
    if (pending_) return {"正在等待上一个画面问题的答案", ""};
    auto request = std::make_shared<ViewRequest>();
    request->deadline = deadline;
    request->current_only = current_only;
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

void OvCameraWatcher::WaitForWork(const std::shared_ptr<ViewRequest>& snapshot, int64_t wake_us) {
    std::unique_lock<std::mutex> lock(request_mutex_);
    request_cv_.wait_for(lock, std::chrono::microseconds(wake_us - esp_timer_get_time()),
                        [&] { return pending_ != snapshot || !watching_; });
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

std::expected<void, std::string> OvCameraWatcher::SendQuestion(const std::string& question, double t_sec,
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
    auto response = RequestOv(base_url_ + "/v1/glasses/sessions/" + sid + "/text", HTTP_METHOD_POST,
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

std::expected<std::optional<double>, std::string> OvCameraWatcher::PushOneFrame(int64_t session_start_us) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }
    if (sid.empty()) return std::unexpected("没有有效会话");

    const std::string session_path = base_url_ + "/v1/glasses/sessions/" + sid;

    auto frame = CaptureFreshFrame();
    if (!frame) return std::unexpected(frame.error());
    const double t_sec = (frame->captured_us - session_start_us) / 1000000.0;
    char path[48];
    snprintf(path, sizeof(path), "/frame?t=%.6f", t_sec);

    auto response = RequestOv(session_path + path, HTTP_METHOD_POST, kHttpTimeoutMs,
                              kMaxControlReplyBytes, "image/jpeg", frame->jpeg);
    if (!response) return std::unexpected("推帧失败: " + response.error());
    if (response->status != 200) {
        return std::unexpected("推帧 HTTP " + std::to_string(response->status));
    }

    // HTTP 200 只代表请求成功；服务端可能按秒采样，实际未接收这张图。
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> reply(cJSON_Parse(response->body.c_str()), cJSON_Delete);
    const auto* accepted = cJSON_GetObjectItem(reply.get(), "accepted");
    if (cJSON_IsTrue(accepted)) return t_sec;
    const auto reason = TextField(reply.get(), "reason");
    if (cJSON_IsFalse(accepted) && (reason == "live_1hz_sampling" || reason == "live_wall_rate")) {
        ESP_LOGD(kTag, "OV frame skipped t=%.6f", t_sec);
        return std::nullopt;
    }
    return std::unexpected("OV 未确认接收推帧: " + std::string(reason.empty() ? "未返回接收原因" : reason));
}

bool OvCameraWatcher::ReadAnswerLogPage(const std::string& body, int& after, AnswerMatch& match) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
    const int next_after = ReadSequence(root.get(), "next_after");
    const auto* events = cJSON_GetObjectItemCaseSensitive(root.get(), "events");
    if (next_after < after || !cJSON_IsArray(events)) return false;
    const cJSON* event = nullptr;
    cJSON_ArrayForEach(event, events) {
        const int log_seq = ReadSequence(event, "log_seq");
        if (log_seq <= after || log_seq > next_after) continue;
        const auto type = TextField(event, "event");
        if (type == "question" && match.question_log_seq == 0) {
            const auto* t = cJSON_GetObjectItemCaseSensitive(event, "question_t");
            if (cJSON_IsNumber(t) && std::abs(t->valuedouble - match.question_t) < kTimestampToleranceSec) {
                match.question_log_seq = log_seq;
            }
        } else if (type == "answer" && match.question_log_seq > 0 &&
                   ReadSequence(event, "question_log_seq") == match.question_log_seq) {
            const int seq = ReadSequence(event, "seq");
            const auto task = TextField(event, "task_template");
            const auto status = TextField(event, "status");
            // 异步提醒或持续计数不是单次问答回包。
            if (seq <= match.result_seq || task == "event_alert_v1" || task == "cumulative_count_v1" ||
                task == "cancel_live_task" || status == "watch_accepted" || status == "model_report") continue;
            // look 只接受本轮新帧；网关误路由到回看时不能用历史画面决定是否静默。
            if (status == "ok" && match.current_only && task != "current_qa_v1" && task != "chat") continue;
            if (status == "ok" && task != "chat" &&
                !HasValidEvidence(event, match.frame_t, match.question_t, task, false)) continue;
            match.result_seq = seq;
            match.task_template = task;
            match.status = status;
        }
    }
    after = next_after;
    return true;
}

std::expected<std::string, std::string> OvCameraWatcher::SelectAnswer(const std::string& body,
                                                                     const AnswerMatch& match) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
    const int cursor = ReadSequence(root.get(), "cursor");
    const auto* results = cJSON_GetObjectItemCaseSensitive(root.get(), "results");
    if (cursor < 0 || !cJSON_IsArray(results)) return std::unexpected("OV 结果页格式异常");
    if (match.result_seq <= 0 || cursor < match.result_seq) return std::string{};
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, results) {
        if (ReadSequence(item, "seq") != match.result_seq || TextField(item, "type") != "answer" ||
            TextField(item, "task_template") != match.task_template || TextField(item, "status") != match.status) continue;
        const auto text = TextField(item, "text");
        if (text.size() > kMaxAnswerBytes || text.find_first_not_of(" \t\r\n") == std::string_view::npos) continue;
        if (match.status == "ok") {
            if (match.task_template == "chat") {
                return "视觉网关未提供画面证据，无法确认这次画面问题。";
            }
            if (HasValidEvidence(item, match.frame_t, match.question_t, match.task_template, true)) {
                return std::string(text);
            }
        } else if (IsAnswerError(match.status)) {
            return std::string(text);
        }
    }
    return std::string{};
}

std::expected<std::string, std::string> OvCameraWatcher::CollectCurrentResult(int& after, AnswerMatch& match) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
    }
    if (sid.empty()) return std::unexpected("没有有效的 QA 会话");
    auto response = RequestOv(base_url_ + "/v1/glasses/sessions/" + sid +
                              "/results?after=" + std::to_string(after) + "&wait=0", HTTP_METHOD_GET,
                              kResultReadTimeoutMs, kMaxResultReplyBytes, {}, {}, nullptr);
    if (!response) return std::unexpected(response.error());
    if (response->status != 200) return std::unexpected("读取 OV 回答 HTTP " + std::to_string(response->status));

    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(response->body.c_str()), cJSON_Delete);
    const int cursor = ReadSequence(root.get(), "cursor");
    const auto* results = cJSON_GetObjectItemCaseSensitive(root.get(), "results");
    if (cursor < after || !cJSON_IsArray(results)) return std::unexpected("OV 结果页格式异常");
    const cJSON* chosen = nullptr;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, results) {
        const int seq = ReadSequence(item, "seq");
        const auto status = TextField(item, "status");
        const auto text = TextField(item, "text");
        if (seq <= after || seq > cursor || TextField(item, "type") != "answer" ||
            TextField(item, "task_template") != "current_qa_v1" || (status != "ok" && !IsAnswerError(status)) ||
            text.size() > kMaxAnswerBytes || text.find_first_not_of(" \t\r\n") == std::string_view::npos) continue;
        // 网关不回传问题 ID。单问题会话若出现多个当前答案，不能随意挑一条。
        if (chosen) return std::unexpected("OV 返回多条当前答案，无法确认归属");
        chosen = item;
    }
    if (!chosen) {
        after = cursor;
        return std::string{};
    }
    // 提问和取证时序由 harness 负责；不拿结果中的帧时间当作问题时间。
    match.result_seq = ReadSequence(chosen, "seq");
    match.status = TextField(chosen, "status");
    after = cursor;
    return std::string(TextField(chosen, "text"));
}

std::expected<std::string, std::string> OvCameraWatcher::CollectResults(int& logs_after, AnswerMatch& match) {
    std::string sid, token;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        sid = session_id_;
        token = logs_token_;
    }
    if (sid.empty() || token.empty()) return std::unexpected("没有有效的 QA 会话");
    const std::string path = base_url_ + "/v1/glasses/sessions/" + sid;
    const auto fetch = [&](const std::string& suffix, bool diagnostics) -> std::expected<std::string, std::string> {
        auto response = RequestOv(path + suffix, HTTP_METHOD_GET, kResultReadTimeoutMs,
                                  kMaxResultReplyBytes, {}, {}, diagnostics ? token.c_str() : nullptr);
        if (!response) return std::unexpected("读取 OV 回答失败: " + response.error());
        if (response->status != 200) {
            return std::unexpected("读取 OV 回答 HTTP " + std::to_string(response->status));
        }
        return std::move(response->body);
    };
    {
        auto logs = fetch("/logs?after=" + std::to_string(logs_after) + "&limit=100", true);
        if (!logs) return std::unexpected(logs.error());
        if (!ReadAnswerLogPage(*logs, logs_after, match)) return std::unexpected("OV 诊断页格式异常");
    }
    if (match.result_seq <= 0) return std::string{};
    auto results = fetch("/results?after=" + std::to_string(match.result_seq - 1) + "&wait=0", false);
    if (!results) return std::unexpected(results.error());
    return SelectAnswer(*results, match);
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
