"""Host-side tests for the production GetView wait and OV observation loop (no ESP network)."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
WATCH = ROOT / 'main/boards/common/ov_camera_watcher.cc'


class CameraViewWaitTests(unittest.TestCase):
    def test_stop_returns_while_worker_is_busy_and_does_not_hold_voice(self):
        source = WATCH.read_text()
        header = WATCH.with_suffix('.h').read_text()
        stop = source[source.index('void OvCameraWatcher::Stop()'):
                      source.index('void OvCameraWatcher::TaskEntry(')]
        transition = next(line for line in header.splitlines() if 'bool HasPendingTransition()' in line)
        harness = r'''
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
unsigned delay_calls = 0;
void vTaskDelay(int) { ++delay_calls; }
struct OvCameraWatcher {
    std::atomic<bool> watching_{true}, starting_{true}, task_running_{true};
    std::mutex lifecycle_mutex_, request_mutex_, answers_mutex_;
    struct MonitorStatus { std::string condition = "举手"; } monitor;
    MonitorStatus GetMonitorStatus() { return monitor; }
    void SetMonitor(MonitorStatus value) { monitor = std::move(value); monitor.condition.clear(); }
    std::condition_variable request_cv_;
    std::shared_ptr<int> pending_ = std::make_shared<int>(1);
    std::string answers_;
    void Stop();
'''
        driver = r'''
int main() {
    OvCameraWatcher watch;
    assert(watch.HasPendingTransition());
    watch.Stop();
    // 假设网络任务一直没退出：Stop 仍必须立即回执，并解除语音状态等待。
    assert(watch.task_running_ && !watch.watching_ && !watch.pending_ && watch.monitor.condition.empty());
    assert(!watch.HasPendingTransition() && delay_calls == 0);
    watch.Stop();
    assert(watch.task_running_ && delay_calls == 0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'stop_test'
            subprocess.run([os.environ.get('CXX', 'c++'), '-x', 'c++', '-std=c++23', '-pthread',
                            '-o', str(exe), '-'], input=harness + transition + '\n};\n' + stop + driver,
                           text=True, check=True)
            subprocess.run([str(exe)], check=True, timeout=5)

    def test_deadline_and_request_identity(self):
        source = WATCH.read_text()
        methods = source[source.index('bool OvCameraWatcher::IsValidTimeScope('):
                         source.index('std::expected<void, std::string> OvCameraWatcher::SendQuestion(')]
        constants = '\n'.join(line for line in source.splitlines()
                              if line.startswith(('constexpr int kViewTimeoutSec',
                                                  'constexpr const char* kDefaultQuestion',
                                                  'constexpr const char* kLookCurrentQuestion')))
        constants += '\n' + source[source.index('constexpr const char* kLookPastQuestion'):source.index('int ReadSequence(')]
        # Compile the actual wait/completion methods; replace only the board and its network task.
        harness = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
using namespace std::chrono_literals;
int64_t esp_timer_get_time() { return 0; }
struct OvCameraWatcher {
    struct ViewRequest { std::string question, reply; std::chrono::steady_clock::time_point deadline;
                         std::string time_scope, status, session_id; };
    struct ViewReply { std::string text, status, session_id; };
    std::mutex session_mutex_; std::string session_id_ = "ov-123";
    std::mutex request_mutex_; std::condition_variable request_cv_;
    std::shared_ptr<ViewRequest> pending_; std::atomic<bool> watching_{false};
    static bool IsValidTimeScope(const std::string&);
    std::string GetView(const std::string&, const std::string& = "current");
    std::expected<std::string, std::string> LookCurrent(const std::string& = {}, std::string* = nullptr);
    std::expected<std::string, std::string> LookPast();
    std::expected<std::string, std::string> LookOnce(const std::string&, const char*, std::string* = nullptr);
    std::atomic<uint32_t> monitor_revision_{0};
    void WaitForWork(const std::shared_ptr<ViewRequest>&, int64_t, const uint32_t* = nullptr);
    ViewReply RequestView(const std::string&, const std::string&, std::chrono::steady_clock::time_point);
    bool FinishView(const std::shared_ptr<ViewRequest>&, const std::string&, const std::string&);
    std::shared_ptr<ViewRequest> take() {
        for (;;) {
            {std::lock_guard<std::mutex> l(request_mutex_); if(pending_) return pending_;}
            std::this_thread::sleep_for(1ms);
        }
    }
};
'''
        driver = r'''
int main() {
    OvCameraWatcher w;
    assert(w.GetView("") == "摄像头没有启动观察"); w.watching_ = true;
    auto start = std::chrono::steady_clock::now();
    auto first = std::async(std::launch::async,[&]{ return w.GetView(" \t"); });
    auto expired = w.take(); assert(expired->question == "详细描述画面内容" && expired->time_scope == "current");
    assert(w.GetView("other") == "正在等待上一个画面问题的答案");
    assert(first.get() == "20 秒内未收到本次问题的画面描述");
    auto duration = std::chrono::steady_clock::now() - start;
    assert(duration >= 19900ms && duration < 22s);
    auto second = std::async(std::launch::async,[&]{ return w.GetView("杯子是什么颜色？"); });
    auto current = w.take(); assert(current->question == "杯子是什么颜色？");
    assert(!w.FinishView(expired,"旧回答","ok"));
    assert(second.wait_for(10ms) == std::future_status::timeout);
    assert(w.FinishView(current,"蓝色","ok"));
    assert(second.wait_for(500ms) == std::future_status::ready && second.get() == "蓝色");
    // get_view 原样传 past / auto，未知取值回落到看当下。
    const std::pair<const char*, const char*> scopes[] = {{"past","past"}, {"auto","auto"}, {"recent","current"}};
    for (const auto& item : scopes) {
        const std::string scope = item.first, expect = item.second;
        auto asked = std::async(std::launch::async,[&]{ return w.GetView("刚才做了什么", scope); });
        auto r = w.take(); assert(r->time_scope == expect);
        assert(w.FinishView(r,"回答","ok") && asked.get() == "回答");
    }
    // look_current 询问当前人物活动与可能情绪，原样返回 OV 回答。
    std::string sid;
    auto look = std::async(std::launch::async,[&]{ return w.LookCurrent({}, &sid); });
    auto glance = w.take();
    assert(glance->time_scope == "current" &&
           glance->question == "画面中是否有人，如果有人简洁描述他在干什么，以及可能的情绪，不超过30个字");
    assert(w.FinishView(glance,"桌上有一个蓝色杯子","ok"));
    w.session_id_ = "ov-new";  // 元数据必须来自完成这条回答的会话。
    auto seen = look.get();
    assert(sid == "ov-123");
    assert(seen && *seen == "桌上有一个蓝色杯子");
    {std::lock_guard<std::mutex> l(w.request_mutex_); assert(!w.pending_);}
    // 自定义问题原样传递，只看当下；空白仍使用 look_current 的默认问题。
    const std::string custom = " 只描述桌上的杯子，不描述人物。 ";
    const std::string look_default = "画面中是否有人，如果有人简洁描述他在干什么，以及可能的情绪，不超过30个字";
    for (const auto& question : {custom, std::string(" \t\r\n")}) {
        look = std::async(std::launch::async,[&]{ return w.LookCurrent(question); });
        glance = w.take();
        assert(glance->time_scope == "current" &&
               glance->question == (question == custom ? custom : look_default));
        assert(w.FinishView(glance,"一个蓝色杯子","ok"));
        seen = look.get();
        assert(seen && *seen == "一个蓝色杯子");
    }
    // look_past 只问 OV 一次回看，固定问题，成功时原话原样返回。
    look = std::async(std::launch::async,[&]{ return w.LookPast(); });
    glance = w.take();
    assert(glance->time_scope == "past" &&
           glance->question == "总结过去一分钟画面前的人在做什么？如果没有人，就回答没有人。");
    assert(w.FinishView(glance,"先举杯喝水，又放下杯子，现在在打字","ok"));
    seen = look.get();
    assert(seen && *seen == "先举杯喝水，又放下杯子，现在在打字");
    // 只发了一个问题：完成后没有第二个在途请求。
    {std::lock_guard<std::mutex> l(w.request_mutex_); assert(!w.pending_);}
    // 无人短句也原样返回，是否静默由服务端判断。
    look = std::async(std::launch::async,[&]{ return w.LookPast(); });
    assert(w.FinishView(w.take(),"没有人","ok"));
    seen = look.get(); assert(seen && *seen == "没有人");
    // 流式会话失效：两个 look 都是工具错误，不补问。
    for (bool past : {false, true}) {
        look = std::async(std::launch::async,[&]{ return past ? w.LookPast() : w.LookCurrent(); });
        assert(w.FinishView(w.take(),"流式会话不可用","stream_unavailable"));
        seen = look.get();
        assert(!seen && seen.error().find("没拿到画面") == 0);
        {std::lock_guard<std::mutex> l(w.request_mutex_); assert(!w.pending_);}
    }
    // 其他非 ok（如证据不足）也是工具错误，带上 OV 原因。
    look = std::async(std::launch::async,[&]{ return w.LookPast(); });
    assert(w.FinishView(w.take(),"画面太暗","insufficient_evidence"));
    seen = look.get(); assert(!seen && seen.error() == "画面太暗");
    // 本地未观察：同样是工具错误。
    w.watching_ = false;
    seen = w.LookCurrent(); assert(!seen && seen.error() == "摄像头没有启动观察");
    seen = w.LookPast(); assert(!seen && seen.error() == "摄像头没有启动观察");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'wait_test'
            subprocess.run([os.environ.get('CXX', 'c++'), '-x', 'c++', '-std=c++23', '-pthread',
                            '-o', str(exe), '-'], input=harness + constants + '\n' + methods + driver,
                           text=True, check=True)
            subprocess.run([str(exe)], check=True, timeout=25)

    def test_current_bypasses_frame_tick_and_past_keeps_confirmed_frame(self):
        source = WATCH.read_text()
        loop = source[source.index('void OvCameraWatcher::RunLoop()'):
                      source.index('std::expected<void, std::string> OvCameraWatcher::OpenSession(')]
        # Execute the production loop with a deterministic clock and instrumented transport.
        # A question arrives during an upload, followed by a skipped and a failed frame.
        harness = r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
constexpr int kFrameIntervalMs = 1000;
constexpr unsigned kMonitorRearmFreshFrames = 3;
constexpr double kCurrentFrameMaxAgeSec = 2.0;
constexpr const char* kSessionExpired = "closed";
constexpr int kViewTimeoutSec = 20;
constexpr int64_t kHeapLogIntervalUs = 30000000;
void LogWatchHeap() {}  // 芯片内存采样不参与帧时序判断。
int64_t now_us = 0;
int64_t esp_timer_get_time() { return now_us; }
void vTaskDelay(int64_t ms) { now_us += ms * 1000; }
template <class T> struct Result {
    bool ok;
    T value{};
    explicit operator bool() const { return ok; }
    std::string error() const { return "test error"; }
    T* operator->() { return &value; }
    T& operator*() { return value; }
};
struct Application {
    static Application& GetInstance() { static Application app; return app; }
    void NotifyToolActivityDone() { ++notifications; }
    unsigned notifications = 0;
};
struct OvHttpClient { explicit OvHttpClient(const std::string&) {} };
struct OvCameraWatcher {
    std::string base_url_ = "https://unit.test";
    struct ViewRequest { std::string question; std::chrono::steady_clock::time_point deadline;
                         std::string time_scope = "past"; };
    struct AnswerMatch {
        double question_t; int result_seq = 0; std::string status;
        bool query_pending, monitor_registering, monitor_accepted;
        std::vector<std::pair<int, std::string>> reports;
    };
    struct MonitorStatus { std::string condition, reminder; bool ready = false; } monitor_;
    std::mutex monitor_mutex_, session_mutex_;
    std::atomic<uint32_t> monitor_revision_{0};
    std::string session_id_ = "fixture";
    MonitorStatus GetMonitorStatus() { return monitor_; }
    bool stop_on_hit = false;
    void PublishMonitorEvent(const std::string&, int seq, const std::string& text, const MonitorStatus& settings) {
        if (seq) delivered_reminders.push_back(settings.reminder);
        events.push_back(seq ? "report:" + text : "monitor:stop");
        if (seq && stop_on_hit) { monitor_ = {}; ++monitor_revision_; }
    }
    bool ClearWatch(OvHttpClient&) { events.push_back("clear"); return true; }
    bool watching_ = true;
    std::atomic<bool> starting_{true};
    std::mutex request_mutex_;
    std::condition_variable request_cv_;
    std::shared_ptr<ViewRequest> pending_;
    std::vector<std::string> events, delivered_reminders;
    unsigned frames = 0, polls = 0;
    double capture_t = 0;
    bool current = false, inject_current = true, stale_current = false, cancel_answer = false, mode_monitor = false;
    bool missing_look = false, recovered_look = false;
    enum class Setup { kAccept, kNoAck, kPostFails };
    Setup setup = Setup::kAccept;
    unsigned monitor_posts = 0, session_opens = 0;
    unsigned frames_after_hit = 0;
    std::string first_monitor_prompt;
    int64_t second_open_at = 0;
    unsigned fast_polls = 0;
    void WaitForWork(const std::shared_ptr<ViewRequest>& snapshot, int64_t wake_us, const uint32_t*) {
        if (pending_ != snapshot || !watching_) return;
        if (current && inject_current && frames == 1) {
            assert(wake_us == 1000000);
            now_us = stale_current ? 2400000 : 400000;
            pending_ = std::make_shared<ViewRequest>(ViewRequest{"question", std::chrono::steady_clock::time_point::max(), "current"});
            inject_current = false;
            return;
        }
        if (wake_us > now_us) now_us = wake_us;
    }
    std::string answer_status = "ok";
    void RunLoop();
    OvHttpClient* transport = nullptr;
    Result<int> OpenSession(OvHttpClient& http) {
        if (transport) assert(transport == &http);
        transport = &http;
        if (++session_opens == 2) second_open_at = now_us;
        events.push_back("open");
        return {true};
    }
    void CloseSession(OvHttpClient& http) { assert(transport == &http); events.push_back("close"); }
    Result<std::optional<double>> PushOneFrame(OvHttpClient& http, int64_t start) { assert(transport == &http);
        if (stale_current && frames == 1) assert(now_us == 2400000);
        else if (!cancel_answer && answer_status == "ok" && !stale_current && session_opens == 1) {
            assert(now_us == int64_t(frames) * 1000000);
        }
        capture_t = (esp_timer_get_time() - start) / 1000000.0;
        now_us += 50000;
        events.push_back("frame" + std::to_string(++frames));
        if (mode_monitor) {
            if (missing_look) {
                if (frames == 13) watching_ = false;
                if (session_opens == 2 && !recovered_look) {
                    recovered_look = true;
                    pending_ = std::make_shared<ViewRequest>(ViewRequest{"question", std::chrono::steady_clock::time_point::max(), "current"});
                }
            } else if (setup != Setup::kAccept) {
                if (frames == 27) watching_ = false;
            } else {
                if (frames == 25) { monitor_.condition.clear(); ++monitor_revision_; }
                if (frames == 27) { monitor_.condition = " 看到有人举手，就提醒我看过去。 "; ++monitor_revision_; }
                if (frames == 30) watching_ = false;
            }
            if (polls && ++frames_after_hit == 1) return {true, std::nullopt}; // HTTP 200，OV 跳过。
            if (polls && frames_after_hit == 2) return {true, capture_t - 3}; // 假设接受了晚到的旧帧。
            if (polls && frames_after_hit > 2) events.push_back("monitor:fresh");
            return {true, capture_t};
        }
        if (current) {
            if (frames == (stale_current ? 3u : 2u)) watching_ = false;
            return {true, capture_t};
        }
        switch (frames) {
        case 1:
            pending_ = std::make_shared<ViewRequest>(ViewRequest{"question", std::chrono::steady_clock::time_point::max()});
            break;
        case 2: return {true, std::nullopt};  // HTTP succeeded but OV skipped the frame.
        case 3: return {false};
        case 6: watching_ = false; break;
        }
        return {true, capture_t};
    }
    Result<int> SendQuestion(OvHttpClient& http, const std::string& question, double t, const std::string& scope) { assert(transport == &http);
        events.push_back("ask");
        if (mode_monitor && scope == "auto") {
            assert(question == " 看到有人举手，就提醒我看过去。 ");
            if (!monitor_posts) first_monitor_prompt = question;
            assert(question == first_monitor_prompt);
            ++monitor_posts;
            events.push_back("monitor:post");
            return {setup != Setup::kPostFails};
        }
        if (mode_monitor) {
            assert(scope == "current" && question == "question");  // 禁止额外发恢复检查。
            return {true};
        }
        // 采样任务把请求里的 time_scope 原样交给网关。
        assert(question == "question" && t > capture_t);
        if (current) {
            assert(scope == "current");
            if (stale_current) assert(t == 2.45 && frames == 2);
            else assert((t == 0.4 || t == 0.05) && frames == 1);
        } else { assert(scope == "past" && t == 3.05 && frames == 4); }
        return {true};
    }
    Result<std::string> CollectResults(OvHttpClient& http, int& after, AnswerMatch& match) { assert(transport == &http);
        ++after;
        if (mode_monitor) {
            if (match.monitor_registering) {
                if (setup == Setup::kNoAck) return {true, ""};
                events.push_back("accepted"); match.monitor_accepted = true;
                if (frames == 1) {
                    pending_ = std::make_shared<ViewRequest>(ViewRequest{"question", std::chrono::steady_clock::time_point::max(), "current"});
                }
            } else if (match.query_pending) {
                if (missing_look) {
                    if (session_opens == 1) {
                        pending_.reset();  // 第一问已结束，模拟网关永不返回它的答案。
                        return {true, ""};
                    }
                    match.status = "ok";  // 新会话的普通问题正常回答。
                    return {true, "answer"};
                }
                events.push_back("abandoned-look");
                if (pending_) {
                    pending_.reset();
                    return {true, ""};
                }
                match.status = "ok";
                frames_after_hit = 0;
                match.reports.emplace_back(after - 1, "命中");
                ++polls;
                return {true, "late look"};
            } else if (monitor_posts == 2 && polls == 1 && frames >= 4 && frames < 25) {
                // 真网关首条命中后移除规则；只有收到下一次 POST 才能再命中。
                frames_after_hit = 0;
                match.reports.emplace_back(after, "命中");
                ++polls;
            }
            return {true, ""};
        }
        if (current) {
            assert(frames == (stale_current ? 2u : 1u));
            assert(now_us == static_cast<int64_t>(match.question_t * 1000000) + fast_polls * 250000);
            events.push_back("fast-poll");
            match.status = answer_status;
            return {true, ++fast_polls == 2 ? "answer" : ""};
        }
        // past 也走独立的轮询节拍；跳过未被网关接收的帧之后，只提交一次。
        assert(match.question_t == 3.05 && frames == 4);
        events.push_back("poll");
        if (cancel_answer) {
            // 模拟调用线程已超时；刚到的回答必须被丢弃，不能进缓存或归给下一问。
            pending_.reset();
            return {true, "late"};
        }
        match.status = answer_status;
        return {true, ++polls == 1 ? "answer" : ""};
    }
    bool FinishView(const std::shared_ptr<ViewRequest>& request, const std::string& reply, const std::string& status) {
        // 网关 status 随答案交给等待方，look 靠它区分正常回答和工具错误。
        if (pending_ != request) return false;
        assert(reply == "answer" && status == answer_status);
        pending_.reset();
        return true;
    }
    void AddAnswer(const std::string& answer) { events.push_back(answer); }
};
'''
        driver = r'''
int main() {
    OvCameraWatcher w;
    w.RunLoop();
    const std::vector<std::string> expected = {
        "open", "frame1", "frame2", "frame3", "frame4", "ask", "poll", "answer", "frame5", "frame6", "close"
    };
    assert(w.events == expected);
    assert(w.watching_ == false && !w.starting_ && Application::GetInstance().notifications == 1);
    // 流式会话失效：答案交付后，下一拍先关旧会话、再建新会话，然后继续推帧。
    // 模拟时钟是全局的，第二个场景从 0 重新计时。
    now_us = 0;
    OvCameraWatcher lost;
    lost.answer_status = "stream_unavailable";
    lost.RunLoop();
    const std::vector<std::string> reopened = {
        "open", "frame1", "frame2", "frame3", "frame4", "ask", "poll",
        "close", "open", "frame5", "frame6", "close"
    };
    assert(lost.events == reopened);
    now_us = 0;
    OvCameraWatcher cancelled;
    cancelled.cancel_answer = true;
    cancelled.RunLoop();
    assert(cancelled.events == reopened);
    now_us = 0;
    OvCameraWatcher stale;
    stale.current = stale.stale_current = true;
    stale.RunLoop();
    const std::vector<std::string> after_fresh_frame = {
        "open", "frame1", "frame2", "ask", "fast-poll", "fast-poll", "answer", "frame3", "close"
    };
    assert(stale.events == after_fresh_frame);
    now_us = 0;
    OvCameraWatcher monitored;
    monitored.mode_monitor = true;
    monitored.monitor_.condition = " 看到有人举手，就提醒我看过去。 ";
    monitored.monitor_.reminder = "请看向举手的人";
    monitored.RunLoop();
    assert((monitored.delivered_reminders == std::vector<std::string>{"请看向举手的人", "请看向举手的人"}));
    for (const auto& item : {"report:命中", "abandoned-look", "monitor:stop", "clear", "frame30"}) {
        assert(std::count(monitored.events.begin(), monitored.events.end(), item) >= 1);
    }
    assert(monitored.polls == 2);
    assert(std::count(monitored.events.begin(), monitored.events.end(), "report:命中") == 2);
    assert(std::count(monitored.events.begin(), monitored.events.end(), "open") == 1);
    assert(std::count(monitored.events.begin(), monitored.events.end(), "close") == 1);
    assert(std::count(monitored.events.begin(), monitored.events.end(), "late look") == 0);
    // 首次启用 + 两次命中各补发一次 + 用户明确重新启用一次。
    assert(monitored.monitor_posts == 4);
    const auto cleared = std::find(monitored.events.begin(), monitored.events.end(), "clear");
    assert(std::count(monitored.events.begin(), cleared, "accepted") == 3);
    assert(std::count(cleared, monitored.events.end(), "accepted") == 1);
    for (auto it = monitored.events.begin(); it != monitored.events.end(); ++it) {
        if (*it == "report:命中") {
            // 被跳过的帧和命中前采集的帧都不算；收到三张新帧就重设，不等播报。
            const auto post = std::find(it + 1, cleared, "monitor:post");
            assert(post != cleared);
            assert(std::count(it + 1, post, "monitor:fresh") == kMonitorRearmFreshFrames);
        }
    }
    now_us = 0;
    OvCameraWatcher stopped;
    stopped.mode_monitor = stopped.stop_on_hit = true;
    stopped.monitor_.condition = " 看到有人举手，就提醒我看过去。 ";
    stopped.RunLoop();
    // 上报时并发停用，不重发；下一次 POST 只来自手动重新启用。
    assert(stopped.monitor_posts == 2);
    const auto stop = std::find(stopped.events.begin(), stopped.events.end(), "monitor:stop");
    assert(std::count(stopped.events.begin(), stop, "monitor:post") == 1);
    assert(std::count(stopped.events.begin(), stopped.events.end(), "report:命中") == 1);
    for (const auto setup : {OvCameraWatcher::Setup::kNoAck, OvCameraWatcher::Setup::kPostFails}) {
        now_us = 0;
        OvCameraWatcher unconfirmed;
        unconfirmed.mode_monitor = true;
        unconfirmed.setup = setup;
        unconfirmed.monitor_.condition = " 看到有人举手，就提醒我看过去。 ";
        unconfirmed.RunLoop();
        assert(unconfirmed.monitor_posts == 1);
        assert(unconfirmed.session_opens == 2 && unconfirmed.monitor_.condition.empty());
        assert(!unconfirmed.monitor_.ready);
    }
    // 看图永不回包时，有界等待后恢复看图会话，但不能重发旧监控条件。
    now_us = 0;
    OvCameraWatcher missing;
    missing.mode_monitor = missing.missing_look = true;
    missing.monitor_.condition = " 看到有人举手，就提醒我看过去。 ";
    missing.RunLoop();
    assert(missing.session_opens == 2 && missing.second_open_at >= 6000000);
    assert(missing.monitor_.condition.empty() && !missing.monitor_.ready);
    assert(missing.monitor_posts == 1);
    assert(std::count(missing.events.begin(), missing.events.end(), "accepted") == 1);
    assert(std::count(missing.events.begin(), missing.events.end(), "answer") == 1);
    for (bool before_first_frame : {false, true}) {
        now_us = 0;
        OvCameraWatcher current;
        current.current = true;
        if (before_first_frame) {
            current.inject_current = false;
            current.pending_ = std::make_shared<OvCameraWatcher::ViewRequest>(OvCameraWatcher::ViewRequest{
                "question", std::chrono::steady_clock::time_point::max(), "current"});
        }
        current.RunLoop();
        const std::vector<std::string> fast = {
            "open", "frame1", "ask", "fast-poll", "fast-poll", "answer", "frame2", "close"
        };
        assert(current.events == fast);
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'ov_loop_test'
            subprocess.run([os.environ.get('CXX', 'c++'), '-x', 'c++', '-std=c++17', '-pthread',
                            '-o', str(exe), '-'], input=harness + loop + driver, text=True, check=True)
            subprocess.run([str(exe)], check=True, timeout=5)

    def test_qa_reads_documented_results_and_isolates_non_answers(self):
        source = WATCH.read_text()
        # 使用生产解析器和拉取函数，只替换网络；测试数据不含诊断字段或顶层 cursor。
        helpers = ('namespace {\n' + source[source.index('constexpr int kResultReadTimeoutMs'):
                                             source.index('OvCameraWatcher::OvCameraWatcher(')])
        methods = source[source.index('std::expected<std::string, std::string> OvCameraWatcher::CollectResults('):
                         source.index('void OvCameraWatcher::AddAnswer(')]
        header = WATCH.with_suffix('.h').read_text()
        match = header[header.index('    struct AnswerMatch {'):header.index('    void AddAnswer(')]
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        if not (cjson / 'cJSON.c').is_file():
            self.skipTest('ESP-IDF managed cJSON source is not installed')
        harness = r'''
#include <cassert>
#include <cmath>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "cJSON.h"
constexpr size_t kMaxResultReplyBytes = 96 * 1024;
enum HttpMethod { HTTP_METHOD_GET };
struct OvHttpReply { int status; std::string body; };
struct Reply { int after; int status; std::string body; };
std::deque<Reply> replies;
struct OvHttpClient {
    std::expected<OvHttpReply, std::string> Request(
        const std::string& path, HttpMethod method, int timeout, size_t limit) {
        assert(method == HTTP_METHOD_GET && timeout == 1500 && limit == kMaxResultReplyBytes);
        assert(!replies.empty());
        auto reply = std::move(replies.front()); replies.pop_front();
        assert(path == "/v1/glasses/sessions/abc/results?after=" + std::to_string(reply.after) + "&wait=0");
        return OvHttpReply{reply.status, std::move(reply.body)};
    }
};
struct OvCameraWatcher {
    std::string session_id_ = "abc";
    std::mutex session_mutex_;
'''
        driver = r'''
int main() {
    OvCameraWatcher w;
    OvHttpClient http;
    int cursor = 0;
    OvCameraWatcher::AnswerMatch match{11};
    replies = {
        {0, 200, R"({"results":[{"seq":1,"type":"warning"},{"seq":2,"type":"answer","status":"model_report","text":"异步提醒"}]})"},
        {2, 200, R"({"results":[{"seq":2,"type":"answer","status":"ok","text":"旧答案"},{"seq":3,"type":"answer","t":9,"status":"ok","text":"蓝色","source_window_s":[3,9]}]})"},
        {3, 200, R"({"results":[{"seq":4,"type":"answer","status":"insufficient_evidence","text":"没看到画面"}]})"},
        {4, 200, R"({"results":[{"seq":5,"type":"answer","status":"ok","text":"闲聊"}]})"},
        {5, 200, R"({"results":[{"seq":6,"type":"answer","status":"ok","text":"A"},{"seq":7,"type":"answer","status":"ok","text":"B"}]})"},
        {5, 200, R"({"results":[{"seq":6,"type":"answer","status":"stream_unavailable","text":"流丢失"}]})"},
        {6, 200, R"({"results":[{"seq":7,"type":"closed"}]})"},
        {6, 404, R"({"detail":"not found"})"},
        {6, 200, R"({"results":[{"seq":7,"type":"answer","status":"watch_accepted","text":"提醒开启"}]})"},
        {7, 200, R"({"results":[{"type":"answer","status":"ok","text":"缺少序号"}]})"},
        {7, 200, R"({"results":[{"seq":8,"type":"answer","status":"model_report","text":"旧监控"},{"seq":9,"type":"answer","status":"watch_accepted","text":"已开启"},{"seq":10,"type":"answer","status":"model_report","text":"命中"}]})"},
        {10, 200, R"({"results":[{"seq":11,"type":"answer","status":"model_report","text":"另一次命中"},{"seq":12,"type":"answer","status":"ok","text":"单次回答","source_window_s":[10,11]}]})"}
    };
    assert(w.CollectResults(http, cursor, match).value().empty() && cursor == 2);
    assert(w.CollectResults(http, cursor, match).value() == "蓝色" && cursor == 3 && match.status == "ok");
    assert(w.CollectResults(http, cursor, match).value() == "没看到画面" && cursor == 4 && match.status == "insufficient_evidence");
    assert(w.CollectResults(http, cursor, match) && cursor == 5 && match.status == "missing_evidence");
    assert(!w.CollectResults(http, cursor, match) && cursor == 5);
    assert(w.CollectResults(http, cursor, match).value() == "流丢失" && cursor == 6 && match.status == "stream_unavailable");
    for (int i = 0; i < 2; ++i) {
        match.status.clear();
        assert(w.CollectResults(http, cursor, match) && match.status == "stream_unavailable" && cursor == 6);
    }
    assert(w.CollectResults(http, cursor, match) && match.status == "unexpected_watch" && cursor == 7);
    assert(!w.CollectResults(http, cursor, match) && cursor == 7);
    match = {0}; match.query_pending = false; match.monitor_registering = true;
    assert(w.CollectResults(http, cursor, match).value().empty());
    assert(match.monitor_accepted && cursor == 10 && match.reports.size() == 1 && match.reports[0].first == 10);
    match = {11};
    assert(w.CollectResults(http, cursor, match).value() == "单次回答");
    assert(match.reports.size() == 1 && match.reports[0].first == 11 && cursor == 12 && replies.empty());
}
'''
        with tempfile.TemporaryDirectory() as directory:
            obj, exe = (Path(directory) / name for name in ('cjson.o', 'ov_answer_test'))
            subprocess.run([os.environ.get('CC', 'cc'), '-c', str(cjson / 'cJSON.c'), '-o', str(obj)],
                           check=True)
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++23', '-pthread', '-I', str(cjson),
                            '-x', 'c++', '-', '-x', 'none', str(obj), '-o', str(exe)],
                           input=harness + match + '\n};\n' + helpers + methods + driver,
                           text=True, check=True)
            subprocess.run([str(exe)], check=True, timeout=5)
