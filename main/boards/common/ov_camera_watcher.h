#pragma once

#include "esp_camera.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class OvHttpClient;

// 同一传感器供两条独立链路使用：CaptureJpeg 单拍返回独立 JPEG；Start/Stop
// QA 会话持续推帧，仅在调用 GetView 或 Look 时提问。
// 两条链路通过 frame_mutex_ 串行取帧，不在持锁期间做网络 I/O。
class OvCameraWatcher {
public:
    // config   : esp32_camera 初始化参数（本板 PIXFORMAT_JPEG + VGA）
    // base_url : OV 网关根地址
    OvCameraWatcher(const camera_config_t& config, const std::string& base_url);
    ~OvCameraWatcher();

    // 初始化传感器。失败时对象仍可用，但 Start() 会直接报错。
    bool Initialize();
    bool IsReady() const { return initialized_; }

    std::expected<void, std::string> Start();
    // 停止观察并关闭网关会话。未在观察时什么都不做（幂等）。
    void Stop();
    bool IsWatching() const { return watching_; }
    bool HasPendingTransition() const { return starting_ || (!watching_ && task_running_); }

    // OV 专用观察查询和最近答案缓存，不是 DeepSeek 单拍入口。
    // time_scope 取 current / past / auto，原样传给网关决定看当前帧还是回看。
    std::string GetView(const std::string& question, const std::string& time_scope = "current");
    // look_current：只看当下（time_scope=current），不传问题时观察人物、活动和周围环境；
    // look_past：固定问题回看约一分钟画面前的人在做什么（time_scope=past）。
    // 两者都只问 OV 一次，成功时原样返回 OV 原话；
    // 流式会话失效、超时等非 ok 结果返回错误，由 MCP 工具如实报告。
    std::expected<std::string, std::string> LookCurrent(const std::string& question = {},
                                                        std::string* session_id = nullptr);
    std::expected<std::string, std::string> LookPast();
    static bool IsValidTimeScope(const std::string& time_scope);
    std::string GetRecentAnswersJson() const;

    // 单拍返回独立 JPEG，排除调用前积压在驱动中的旧帧。
    std::expected<std::string, std::string> CaptureJpeg();

private:
    static constexpr int kMaxAnswers = 5;

    static void TaskEntry(void* arg);
    void RunLoop();

    // 一整套建会话/插话/推帧/拉结果/关会话的实现
    std::expected<void, std::string> OpenSession(OvHttpClient& http);
    struct ViewRequest {
        std::string question;
        std::string reply;
        std::chrono::steady_clock::time_point deadline;
        std::string time_scope = "auto";
        // 采样任务在完成时写入网关 status，LookOnce 据此区分正常回答和工具错误。
        std::string status;
        std::string session_id;
    };
    struct ViewReply {
        std::string text;
        std::string status;  // 空表示本地失败（超时、未观察、忙）
        std::string session_id = {};
    };
    ViewReply RequestView(const std::string& question, const std::string& time_scope,
                          std::chrono::steady_clock::time_point deadline);
    // LookCurrent / LookPast 共用：提一次问题，把非 ok 结果转成工具错误。
    std::expected<std::string, std::string> LookOnce(const std::string& question, const char* time_scope,
                                                      std::string* session_id = nullptr);
    void WaitForWork(const std::shared_ptr<ViewRequest>& snapshot, int64_t wake_us);
    std::mutex request_mutex_;
    std::condition_variable request_cv_;
    std::shared_ptr<ViewRequest> pending_;
    bool FinishView(const std::shared_ptr<ViewRequest>& request, const std::string& reply,
                    const std::string& status);
    std::expected<void, std::string> SendQuestion(OvHttpClient& http, const std::string& question, double t_sec,
                                                  const std::string& time_scope);
    void CloseSession(OvHttpClient& http);
    struct CapturedFrame {
        std::string jpeg;
        int64_t captured_us;
    };
    std::expected<CapturedFrame, std::string> CaptureFreshFrame();
    // 返回已被网关接收的帧时间；采样过滤返回空，不能作为提问依据。
    std::expected<std::optional<double>, std::string> PushOneFrame(OvHttpClient& http, int64_t session_start_us);
    struct AnswerMatch {
        double question_t;
        int result_seq = 0;
        std::string status;
    };
    std::expected<std::string, std::string> CollectResults(OvHttpClient& http, int& after, AnswerMatch& match);

    void AddAnswer(const std::string& text);

    bool initialized_ = false;
    camera_config_t config_{};
    std::string base_url_;

    // 观察状态。watching_ 由 Start/Stop 与任务共同读写，用 portMUX 级别的
    // 原子变量即可（只做布尔切换，不需要复合操作）。
    std::atomic<bool> watching_{false};
    std::atomic<bool> starting_{false};
    std::atomic<bool> task_running_{false};
    std::mutex lifecycle_mutex_;

    // 会话 ID 受 session_mutex_ 保护，API Key 不写入日志或 URL。
    mutable std::mutex session_mutex_;
    std::string session_id_;

    // 答案环形缓冲，受 answers_mutex_ 保护。
    mutable std::mutex answers_mutex_;
    std::vector<std::string> answers_;

    mutable std::mutex frame_mutex_;
};
