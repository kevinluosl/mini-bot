"""Host-side tests for the production GetView wait and OV observation loop (no ESP network)."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
WATCH = ROOT / 'main/boards/common/ov_camera_watcher.cc'


class CameraViewWaitTests(unittest.TestCase):
    def test_deadline_and_request_identity(self):
        source = WATCH.read_text()
        methods = source[source.index('bool OvCameraWatcher::IsValidTimeScope('):
                         source.index('std::expected<void, std::string> OvCameraWatcher::SendQuestion(')]
        constants = '\n'.join(line for line in source.splitlines()
                              if line.startswith(('constexpr int kViewTimeoutSec',
                                                  'constexpr const char* kDefaultQuestion',
                                                  'constexpr const char* kLookCurrentQuestion')))
        constants += '\n' + source[source.index('constexpr const char* kLookPastQuestion'):source.index('// 板端发送微秒精度')]
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
                         bool current_only; std::string time_scope, status, session_id; };
    struct ViewReply { std::string text, status, session_id; };
    std::mutex session_mutex_; std::string session_id_ = "ov-123";
    std::mutex request_mutex_; std::condition_variable request_cv_;
    std::shared_ptr<ViewRequest> pending_; std::atomic<bool> watching_{false};
    static bool IsValidTimeScope(const std::string&);
    std::string GetView(const std::string&, const std::string& = "current");
    std::expected<std::string, std::string> LookCurrent(const std::string& = {}, std::string* = nullptr);
    std::expected<std::string, std::string> LookPast();
    std::expected<std::string, std::string> LookOnce(const std::string&, const char*, std::string* = nullptr);
    void WaitForWork(const std::shared_ptr<ViewRequest>&, int64_t);
    ViewReply RequestView(const std::string&, bool, const std::string&, std::chrono::steady_clock::time_point);
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
        auto r = w.take(); assert(!r->current_only && r->time_scope == expect);
        assert(w.FinishView(r,"回答","ok") && asked.get() == "回答");
    }
    // look_current 询问当前人物活动与可能情绪，原样返回 OV 回答。
    std::string sid;
    auto look = std::async(std::launch::async,[&]{ return w.LookCurrent({}, &sid); });
    auto glance = w.take();
    assert(!glance->current_only && glance->time_scope == "current" &&
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
        assert(!glance->current_only && glance->time_scope == "current" &&
               glance->question == (question == custom ? custom : look_default));
        assert(w.FinishView(glance,"一个蓝色杯子","ok"));
        seen = look.get();
        assert(seen && *seen == "一个蓝色杯子");
    }
    // look_past 只问 OV 一次回看，固定问题，成功时原话原样返回。
    look = std::async(std::launch::async,[&]{ return w.LookPast(); });
    glance = w.take();
    assert(!glance->current_only && glance->time_scope == "past" &&
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
                      source.index('std::expected<void, std::string> OvCameraWatcher::OpenSession()')]
        # Execute the production loop with a deterministic clock and instrumented transport.
        # A question arrives during an upload, followed by a skipped and a failed frame.
        harness = r'''
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
struct OvCameraWatcher {
    struct ViewRequest { std::string question; std::chrono::steady_clock::time_point deadline; bool current_only = false;
                         std::string time_scope = "past"; };
    struct AnswerMatch { double question_t, frame_t; bool current_only; int result_seq = 0; std::string status; };
    bool watching_ = true, starting_ = true;
    std::mutex request_mutex_;
    std::condition_variable request_cv_;
    std::shared_ptr<ViewRequest> pending_;
    std::vector<std::string> events;
    unsigned frames = 0, polls = 0;
    double capture_t = 0;
    bool current = false, inject_current = true;
    unsigned fast_polls = 0;
    void WaitForWork(const std::shared_ptr<ViewRequest>& snapshot, int64_t wake_us) {
        if (pending_ != snapshot || !watching_) return;
        if (current && inject_current && frames == 1) {
            assert(wake_us == 1000000);
            now_us = 400000;
            pending_ = std::make_shared<ViewRequest>(ViewRequest{"question", std::chrono::steady_clock::time_point::max(), false, "current"});
            inject_current = false;
            return;
        }
        if (wake_us > now_us) now_us = wake_us;
    }
    std::string answer_status = "ok";
    void RunLoop();
    Result<int> OpenSession() { events.push_back("open"); return {true}; }
    void CloseSession() { events.push_back("close"); }
    Result<std::optional<double>> PushOneFrame(int64_t start) {
        assert(now_us == int64_t(frames) * 1000000 ||
               (answer_status == "stream_unavailable" && frames == 5 && now_us == 4050000));
        capture_t = (esp_timer_get_time() - start) / 1000000.0;
        now_us += 50000;
        events.push_back("frame" + std::to_string(++frames));
        if (current) {
            if (frames == 2) watching_ = false;
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
    Result<int> SendQuestion(const std::string& question, double t, const std::string& scope) {
        events.push_back("ask");
        // 采样任务把请求里的 time_scope 原样交给网关。
        assert(question == "question" && t > capture_t);
        if (current) { assert(scope == "current" && (t == 0.4 || t == 0.05) && frames == 1); }
        else { assert(scope == "past"); }
        return {true};
    }
    Result<std::string> CollectCurrentResult(int&, AnswerMatch& match) {
        assert(current && frames == 1 && match.frame_t == 0);
        assert(now_us == static_cast<int64_t>(match.question_t * 1000000) + fast_polls * 250000);
        events.push_back("fast-poll");
        match.status = answer_status;
        return {true, ++fast_polls == 2 ? "answer" : ""};
    }
    Result<std::string> CollectResults(int& after, AnswerMatch& match) {
        ++after;
        // 后续采样推进时，仍按首次发问时确认的帧和发问时间匹配。
        assert(match.frame_t == 3.0 && match.question_t == 3.05);
        events.push_back("poll");
        match.status = answer_status;
        return {true, ++polls == 1 ? "answer" : ""};
    }
    bool FinishView(const std::shared_ptr<ViewRequest>& request, const std::string& reply, const std::string& status) {
        // 网关 status 随答案交给等待方，look 靠它区分正常回答和工具错误。
        assert(pending_ == request && reply == "answer" && status == answer_status);
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
        "open", "frame1", "frame2", "frame3", "frame4", "ask", "frame5", "poll", "answer", "frame6", "close"
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
        "open", "frame1", "frame2", "frame3", "frame4", "ask", "frame5", "poll",
        "close", "open", "frame6", "close"
    };
    assert(lost.events == reopened);
    for (bool before_first_frame : {false, true}) {
        now_us = 0;
        OvCameraWatcher current;
        current.current = true;
        if (before_first_frame) {
            current.inject_current = false;
            current.pending_ = std::make_shared<OvCameraWatcher::ViewRequest>(OvCameraWatcher::ViewRequest{
                "question", std::chrono::steady_clock::time_point::max(), false, "current"});
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

    def test_qa_only_reads_results_for_the_correlated_question(self):
        source = WATCH.read_text()
        # 只提取与问答关联有关的常量和解析器，不把 ESP-IDF HTTP/堆实现编进宿主测试。
        helpers = ('namespace {\n' + source[source.index('constexpr int kResultReadTimeoutMs'):
                                             source.index('OvCameraWatcher::OvCameraWatcher(')])
        methods = source[source.index('bool OvCameraWatcher::ReadAnswerLogPage('):source.index('void OvCameraWatcher::AddAnswer(')]
        header = WATCH.with_suffix('.h').read_text()
        match = header[header.index('    struct AnswerMatch {'):header.index('    void AddAnswer(')]
        cjson = ROOT / 'managed_components/espressif__cjson/cJSON'
        if not (cjson / 'cJSON.c').is_file():
            self.skipTest('ESP-IDF managed cJSON source is not installed')
        # 使用生产解析器及 HTTP 拉取函数，只用脚本替代传输。
        harness = r'''
#include <cassert>
#include <cmath>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "cJSON.h"
constexpr size_t kMaxResultReplyBytes = 96 * 1024;
enum HttpMethod { HTTP_METHOD_GET };
struct OvHttpReply { int status; std::string body; };
struct Reply { std::string suffix; std::string body; };
std::deque<Reply> replies;
std::expected<OvHttpReply, std::string> RequestOv(
    const std::string& url, HttpMethod method, int timeout, size_t limit,
    std::string_view content_type, std::string_view body, const char* token) {
    assert(method == HTTP_METHOD_GET && timeout == 1500 && limit == kMaxResultReplyBytes);
    assert(content_type.empty() && body.empty() && !replies.empty());
    auto reply = std::move(replies.front()); replies.pop_front();
    assert(url == "http://unit.test/v1/glasses/sessions/abc" + reply.suffix);
    if (reply.suffix.starts_with("/logs?")) assert(token && std::string_view(token) == "test-token");
    else assert(!token);
    return OvHttpReply{200, std::move(reply.body)};
}
struct OvCameraWatcher {
    std::string session_id_ = "abc", base_url_ = "http://unit.test", logs_token_ = "test-token";
    std::mutex session_mutex_;
'''
        driver = r'''
int main() {
    OvCameraWatcher w;
    int cursor = 4;
    OvCameraWatcher::AnswerMatch direct{11, 10.9};
    replies = {
        {"/results?after=4&wait=0", R"({"cursor":6,"results":[
            {"seq":4,"type":"answer","task_template":"current_qa_v1","status":"ok","text":"旧答案"},
            {"seq":6,"type":"caption","text":"不是问答"}
        ]})"},
        {"/results?after=6&wait=0", R"({"cursor":7,"results":[
            {"seq":7,"type":"answer","task_template":"current_qa_v1","status":"ok","t":10.5,"text":"蓝色"}
        ]})"},
        {"/results?after=7&wait=0", R"({"cursor":9,"results":[
            {"seq":8,"type":"answer","task_template":"current_qa_v1","status":"ok","text":"甲"},
            {"seq":9,"type":"answer","task_template":"current_qa_v1","status":"ok","text":"乙"}
        ]})"},
    };
    assert(w.CollectCurrentResult(cursor, direct).value().empty() && cursor == 6);
    assert(w.CollectCurrentResult(cursor, direct).value() == "蓝色" && cursor == 7);
    assert(direct.result_seq == 7 && direct.status == "ok");
    assert(!w.CollectCurrentResult(cursor, direct) && cursor == 7);
    int after = 0;
    replies = {
        {"/logs?after=0&limit=100", R"({"next_after":10,"events":[
            {"log_seq":9,"event":"question","question_t":8},
            {"log_seq":10,"event":"question","question_t":10.1}
        ]})"},
        {"/logs?after=10&limit=100", R"({"next_after":14,"events":[
            {"log_seq":11,"event":"answer","question_log_seq":9,"seq":2,"status":"ok","task_template":"current_qa_v1","source_window_s":[10,10]},
            {"log_seq":12,"event":"answer","question_log_seq":10,"seq":3,"status":"ok","task_template":"current_qa_v1","source_window_s":[9,9]},
            {"log_seq":13,"event":"answer","question_log_seq":10,"seq":4,"status":"ok","task_template":"current_qa_v1","source_window_s":[10,10]},
            {"log_seq":14,"event":"answer","question_log_seq":9,"seq":5,"status":"ok","task_template":"current_qa_v1","source_window_s":[10,10]}
        ]})"},
        {"/results?after=3&wait=0", R"({"cursor":5,"results":[
            {"seq":4,"type":"answer","status":"ok","task_template":"current_qa_v1","t":10,"source_window_s":[10,10],"text":"当前是蓝色"},
            {"seq":5,"type":"answer","status":"ok","task_template":"current_qa_v1","source_window_s":[10,10],"text":"上一问迟到"}
        ]})"},
        {"/logs?after=14&limit=100", R"({"next_after":16,"events":[
            {"log_seq":15,"event":"question","question_t":20},
            {"log_seq":16,"event":"answer","question_log_seq":15,"seq":6,"status":"ok","task_template":"history_history_v1","source_window_s":[14.5,19]}
        ]})"},
        {"/results?after=5&wait=0", R"({"cursor":6,"results":[
            {"seq":6,"type":"answer","status":"ok","task_template":"history_history_v1","t":19,"source_window_s":[14.5,19],"requested_window_s":[14,20],"text":"没有看到喝水动作"}
        ]})"},
    };
    OvCameraWatcher::AnswerMatch current{10.1, 10};
    assert(w.CollectResults(after, current).value().empty() && after == 10);
    assert(w.CollectResults(after, current).value() == "当前是蓝色" && after == 14);
    OvCameraWatcher::AnswerMatch replay{20, 19.9};
    assert(w.CollectResults(after, replay).value() == "没有看到喝水动作" && after == 16);
    assert(replies.empty() && replay.question_log_seq == 15 && replay.result_seq == 6);
    assert(w.SelectAnswer(R"({"cursor":6,"results":[
        {"seq":6,"type":"answer","status":"ok","task_template":"history_history_v1","source_window_s":[13.9,19],"requested_window_s":[14,20],"text":"超出窗口"}
    ]})", replay).value().empty());
    assert(w.SelectAnswer(R"({"cursor":6,"results":[
        {"seq":6,"type":"answer","status":"ok","task_template":"history_history_v1","source_window_s":[15,19],"text":"缺少窗口"}
    ]})", replay).value().empty());
    assert(!w.ReadAnswerLogPage(R"({"next_after":1,"events":[]})", after, replay));
    assert(after == 16);
    OvCameraWatcher::AnswerMatch look{30, 29.9, true};
    assert(w.ReadAnswerLogPage(R"({"next_after":19,"events":[
        {"log_seq":17,"event":"question","question_t":30},
        {"log_seq":18,"event":"answer","question_log_seq":17,"seq":7,"status":"ok","task_template":"current_qa_v1","source_window_s":[29.9,29.9]},
        {"log_seq":19,"event":"answer","question_log_seq":17,"seq":8,"status":"ok","task_template":"history_history_v1","source_window_s":[25,29]}
    ]})", after, look));
    assert(after == 19 && look.result_seq == 7);

    // 19200 实测：未指定时段的流式回看读到作答时刻，取证终点晚于提问，请求窗口与取证窗口一致。
    OvCameraWatcher::AnswerMatch stream{77.807901, 77.705024};
    assert(w.ReadAnswerLogPage(R"({"next_after":21,"events":[
        {"log_seq":20,"event":"question","question_t":77.807901},
        {"log_seq":21,"event":"answer","question_log_seq":20,"seq":9,"status":"ok","task_template":"history_history_v1","source_window_s":[16.805042,78.958972]}
    ]})", after, stream));
    assert(stream.result_seq == 9);
    assert(w.SelectAnswer(R"({"cursor":9,"results":[
        {"seq":9,"type":"answer","status":"ok","task_template":"history_history_v1","source_window_s":[16.805042,78.958972],"requested_window_s":[16.805042,78.958972],"coverage":"stream","text":"先红后蓝"}
    ]})", stream).value() == "先红后蓝");
    // 非流式覆盖、或没有覆盖到本轮确认新帧的总结，都不能当作本轮回看。
    assert(w.SelectAnswer(R"({"cursor":9,"results":[
        {"seq":9,"type":"answer","status":"ok","task_template":"history_history_v1","source_window_s":[16.8,78.9],"requested_window_s":[16.8,78.9],"text":"无覆盖标记"}
    ]})", stream).value().empty());
    assert(w.SelectAnswer(R"({"cursor":9,"results":[
        {"seq":9,"type":"answer","status":"ok","task_template":"history_history_v1","source_window_s":[10,77.6],"requested_window_s":[10,78],"coverage":"stream","text":"旧总结"}
    ]})", stream).value().empty());
    // 取证终点远超本轮等待上限，不可能属于本轮问题。
    OvCameraWatcher::AnswerMatch late{30, 29.9};
    assert(w.ReadAnswerLogPage(R"({"next_after":23,"events":[
        {"log_seq":22,"event":"question","question_t":30},
        {"log_seq":23,"event":"answer","question_log_seq":22,"seq":10,"status":"ok","task_template":"history_history_v1","source_window_s":[1,51]}
    ]})", after, late));
    assert(late.result_seq == 0);
    // 当前帧答案仍须在提问前取证。
    OvCameraWatcher::AnswerMatch future_frame{40, 39.9, true};
    assert(w.ReadAnswerLogPage(R"({"next_after":25,"events":[
        {"log_seq":24,"event":"question","question_t":40},
        {"log_seq":25,"event":"answer","question_log_seq":24,"seq":11,"status":"ok","task_template":"current_qa_v1","source_window_s":[40.5,40.5]}
    ]})", after, future_frame));
    assert(future_frame.result_seq == 0);
    // 流式会话失效立即返回，不空等到超时。
    OvCameraWatcher::AnswerMatch lost{50, 49.9};
    assert(w.ReadAnswerLogPage(R"({"next_after":27,"events":[
        {"log_seq":26,"event":"question","question_t":50},
        {"log_seq":27,"event":"answer","question_log_seq":26,"seq":12,"status":"stream_unavailable","task_template":"history_history_v1"}
    ]})", after, lost));
    assert(lost.result_seq == 12 && w.SelectAnswer(R"({"cursor":12,"results":[
        {"seq":12,"type":"answer","status":"stream_unavailable","task_template":"history_history_v1","text":"流式会话不可用"}
    ]})", lost).value() == "流式会话不可用");
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
