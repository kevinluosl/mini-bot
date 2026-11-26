"""宿主机执行真实收尾方法；另检查 MCP、屏幕和板级完成通知接线。"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2] / 'main'


class ToolCompletionTest(unittest.TestCase):
    def test_finish_speaking_waits_for_callbacks_and_board_effects(self):
        app = (ROOT / 'application.cc').read_text()
        methods = app[app.index('void Application::BeginToolExecution()'):
                      app.index('ListeningMode Application::GetDefaultListeningMode()')]
        harness = r'''
#include <atomic>
#include <cassert>
#include "device_state.h"
#define ESP_LOGI(...) ((void)0)
#define MAIN_EVENT_TOOL_ACTIVITY_DONE (1 << 14)
static unsigned events;
void xEventGroupSetBits(int, int bits) { events |= bits; }
enum ListeningMode { kListeningModeAutoStop };
struct Board {
    static Board& GetInstance() { static Board board; return board; }
    bool head = false, emotion = false, camera = false;
    bool HasPendingToolEffects() { return head || emotion || camera; }
};
struct Application {
    int event_group_ = 0;
    std::atomic<unsigned> executing_tools_{0};
    DeviceState pending_speaking_exit_ = kDeviceStateUnknown;
    DeviceState state = kDeviceStateSpeaking;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    DeviceState GetDeviceState() { return state; }
    void SetDeviceState(DeviceState next) { state = next; }
    void BeginToolExecution();
    void EndToolExecution();
    void NotifyToolActivityDone();
    void TryFinishSpeaking();
    void SetListeningMode(ListeningMode);
};
'''
        driver = r'''
int main() {
    Application app;
    auto& board = Board::GetInstance();
    app.BeginToolExecution(); // 已排队但尚未运行
    app.BeginToolExecution(); // 仍在后台等待的另一个工具
    app.pending_speaking_exit_ = kDeviceStateListening;
    app.TryFinishSpeaking();
    assert(app.state == kDeviceStateSpeaking);
    board.head = board.emotion = board.camera = true;
    app.EndToolExecution();
    assert(events == 0);
    app.EndToolExecution();
    assert(events == MAIN_EVENT_TOOL_ACTIVITY_DONE);
    app.TryFinishSpeaking();
    assert(app.state == kDeviceStateSpeaking);
    board.head = false;
    app.TryFinishSpeaking();
    assert(app.state == kDeviceStateSpeaking);
    board.emotion = false;
    app.TryFinishSpeaking();
    assert(app.state == kDeviceStateSpeaking);
    board.camera = false;
    app.TryFinishSpeaking();
    assert(app.state == kDeviceStateListening && app.pending_speaking_exit_ == kDeviceStateUnknown);

    app.state = kDeviceStateSpeaking;
    app.pending_speaking_exit_ = kDeviceStateIdle;
    board.head = true;
    app.SetListeningMode(kListeningModeAutoStop);
    assert(app.state == kDeviceStateSpeaking && app.pending_speaking_exit_ == kDeviceStateListening);
    board.head = false;
    app.TryFinishSpeaking();
    assert(app.state == kDeviceStateListening);
    app.state = kDeviceStateSpeaking;
    app.TryFinishSpeaking(); // 上一轮重复通知不得结束新一轮
    assert(app.state == kDeviceStateSpeaking);
    app.pending_speaking_exit_ = kDeviceStateIdle;
    app.TryFinishSpeaking();
    assert(app.state == kDeviceStateIdle);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'test'
            subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-I', str(ROOT),
                            '-x', 'c++', '-o', str(exe), '-'], input=harness + methods + driver,
                           text=True, check=True)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_sensor_notifications_use_current_mqtt_session_and_reject_busy_states(self):
        app = (ROOT / 'application.cc').read_text().split('void Application::SendServerMcpNotification', 1)[1].split('\n}', 1)[0]
        self.assertIn('state == kDeviceStateIdle || state == kDeviceStateListening', app)
        self.assertEqual(app.count('if (!can_notify()) return;'), 2)
        self.assertLess(app.index('if (!can_notify()) return;'), app.index('ReplyGeneration()'))
        scheduled = app.split('Schedule(', 1)[1]
        self.assertLess(scheduled.index('if (!can_notify()) return;'), scheduled.index('SendMcpNotification(payload, generation)'))
        protocol = (ROOT / 'protocols/custom_server_audio.cc').read_text().split(
            'bool CustomServerAudio::SendMcpNotification', 1)[1].split('\n}', 1)[0]
        self.assertIn('std::try_to_lock', protocol)
        self.assertIn('generation != connection_generation_.load()', protocol)
        self.assertIn('session = session_id_', protocol)
        self.assertIn('session_id\\\":\\\"" + session', protocol)

    def test_completion_signals_follow_effect_and_reset_is_not_a_clear(self):
        incoming = (ROOT / 'application_incoming_json.cc').read_text()
        start, rest = incoming.split('strcmp(state->valuestring, "stop")', 1)
        self.assertIn('pending_speaking_exit_ = kDeviceStateUnknown;', start)
        stop = rest.split('strcmp(state->valuestring, "sentence_start")', 1)[0]
        self.assertIn('TryFinishSpeaking();', stop)
        self.assertNotIn('SetDeviceState(', stop)
        mcp = (ROOT / 'mcp_server.cc').read_text().split('void McpServer::DoToolCall', 1)[1]
        self.assertEqual(mcp.count('.BeginToolExecution();'), 2)
        self.assertEqual(mcp.count('.EndToolExecution();'), 3)  # 包含后台任务创建失败
        eyes = (ROOT / 'display/deepbot_eyes_display.cc').read_text()
        neutral = eyes.split('void DeepbotEyesDisplay::SetConversationNeutral()', 1)[1].split(
            'void DeepbotEyesDisplay::SetTouchSmile', 1)[0]
        self.assertIn('if (style_ != EyeStyle::kCircle) Display::SetConversationNeutral();', neutral)
        touch = eyes.split('void DeepbotEyesDisplay::SetTouchSmile', 1)[1].split(
            'void DeepbotEyesDisplay::StartHeadActionEmotion', 1)[0]
        self.assertNotIn('emotion_ =', touch.replace('emotion_ ==', ''))
        self.assertNotIn('emotion_queue_', touch)
        pending = eyes.split('bool DeepbotEyesDisplay::HasPendingToolEmotion', 1)[1].split(
            'void DeepbotEyesDisplay::SetEmotion', 1)[0]
        self.assertIn('anim_timer_ != nullptr && eye_left_ != nullptr && eye_right_ != nullptr', pending)
        expire = eyes.split('"情绪到期:', 1)[1].split('SyncStateFromDevice();', 1)[0]
        self.assertLess(expire.index('PopNextEmotion();'), expire.index('NotifyToolActivityDone();'))
        board = (ROOT / 'boards/deepbot-s3-cam-n16r8/deepbot_s3_cam_board.cc').read_text()
        drive = board.split('// ---- 5. 驱动舵机', 1)[1].split('vTaskDelay', 1)[0]
        self.assertLess(drive.index('SetBothServoAngles('), drive.index('head_final_output_pending_ = false;'))
        self.assertLess(drive.index('head_final_output_pending_ = false;'), drive.index('NotifyToolActivityDone();'))
        self.assertIn('head_motion_.Busy() || head_final_output_pending_', board)
        self.assertLess(drive.index('StartHeadActionEmotion(started_emotion)'), drive.index('SetBothServoAngles('))
        self.assertLess(drive.index('SetBothServoAngles('), drive.index('FinishHeadActionEmotion()'))
        self.assertLess(drive.index('FinishHeadActionEmotion()'), drive.index('head_final_output_pending_ = false;'))
        self.assertIn('head_emotion_ != EyeEmotion::kNone', pending)
        self.assertIn('!head_emotion_running_ && tick_ >= head_emotion_until_tick_', expire)
        registry = (ROOT / 'mcp_server.cc').read_text()
        self.assertIn('!Board::GetInstance().HasCombinedHeadEmotionTool()) {\n        AddTool("self.screen.set_emotion",', registry)
        self.assertIn('bool HasCombinedHeadEmotionTool() const override { return true; }', board)
