"""音频补丁精简后的源码契约回归；实际 C++ 语法和链接由 build.py 验证。"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class AudioPlaybackContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = (ROOT / "main/application.cc").read_text()
        cls.header = (ROOT / "main/application.h").read_text()
        cls.protocol = (ROOT / "main/protocols/custom_server_audio.cc").read_text()
        # 下行 JSON（含 tts start/stop）处理已从 application.cc 拆分到独立编译单元，
        # 契约断言跟随实现位置，避免因重构而误报。
        cls.incoming = (ROOT / "main/application_incoming_json.cc").read_text()

    def test_speaking_restores_original_wake_word_policy(self):
        handler = self.app.split("void Application::HandleStateChangedEvent()", 1)[1]
        block = handler.split("case kDeviceStateSpeaking:", 1)[1].split("case kDeviceStateNotifying:", 1)[0]
        self.assertIn("EnableWakeWordDetection(audio_service_.IsAfeWakeWord())", block)
        self.assertNotIn("input_reference()", block)
        # 注释中可描述 ResetDecoder，但不能在异步 speaking 状态中再次执行它。
        code = "\n".join(line for line in block.splitlines() if not line.strip().startswith("//"))
        self.assertNotIn("audio_service_.ResetDecoder();", code)

    def test_no_extra_blocking_stop_state(self):
        self.assertNotIn("pending_tts_stop_", self.app + self.header)
        self.assertNotIn("vTaskDelay(pdMS_TO_TICKS(90))", self.app)
        self.assertNotIn("SetPlaybackActive(", self.app)

    def test_start_prepares_before_opening_audio_gate(self):
        start = self.protocol.split('strcmp(state->valuestring, "start") == 0', 1)[1]
        start = start.split('strcmp(state->valuestring, "stop")', 1)[0]
        self.assertLess(start.index("on_json_(root)"), start.index("playback_active_.store(true)"))
        start_handler = self.incoming.split('strcmp(state->valuestring, "start") == 0', 1)[1]
        start_handler = start_handler.split('strcmp(state->valuestring, "stop")', 1)[0]
        self.assertLess(start_handler.index("ResetDecoder();"), start_handler.index("Schedule("))
        self.assertLess(start_handler.index("PrewarmAudioOutput();"), start_handler.index("Schedule("))

    def test_proactive_listen_ack_follows_normal_audio_setup(self):
        # 普通握手回包不带 request_id，不能触发开麦。
        handler = self.incoming.split('strcmp(type->valuestring, "listen") == 0', 1)[1]
        handler = handler.split('strcmp(type->valuestring, "stt") == 0', 1)[0]
        self.assertIn('!cJSON_IsNumber(request)', handler)
        idle = handler.split('if (current == kDeviceStateIdle)', 1)[1].split('} else if', 1)[0]
        self.assertLess(idle.index('PowerSaveLevel::PERFORMANCE'), idle.index('SetListeningMode(GetDefaultListeningMode())'))
        self.assertNotIn('CompleteServerListening(true)', idle)
        self.assertIn('current != kDeviceStateListening', handler)
        self.assertIn('CompleteServerListening(false)', handler)
        listening = self.app.split('void Application::StartListeningAudio()', 1)[1].split('\n}', 1)[0]
        ready_check = 'CompleteServerListening(audio_service_.IsAudioProcessorRunning())'
        self.assertLess(listening.index('EnableVoiceProcessing(true)'), listening.index(ready_check))
        self.assertNotIn('CompleteServerListening(true)', listening)
        self.assertNotIn('preserve_idle', self.incoming)

    def test_stop_restores_protocol_gate(self):
        stop = self.protocol.split('strcmp(state->valuestring, "stop") == 0', 1)[1]
        stop = stop.split("if (on_json_", 1)[0]
        self.assertIn("playback_active_.store(false)", stop)

    def test_prewarm_updates_activity_and_abort_resets_decoder(self):
        audio = (ROOT / "main/audio/audio_service.cc").read_text()
        prewarm = audio.split("void AudioService::PrewarmAudioOutput()", 1)[1]
        self.assertIn("last_output_time_ = std::chrono::steady_clock::now()", prewarm)
        abort = self.app.split("void Application::AbortSpeaking(", 1)[1].split("void Application::", 1)[0]
        self.assertIn("audio_service_.ResetDecoder();", abort)

    def test_output_power_and_write_share_lock_and_driver_error_is_recoverable(self):
        audio = (ROOT / "main/audio/audio_service.cc").read_text()
        output = audio.split("void AudioService::AudioOutputTask()", 1)[1].split("void AudioService::OpusCodecTask()", 1)[0]
        power = audio.split("void AudioService::CheckAndUpdateAudioPowerState()", 1)[1].split("void AudioService::SetModelsList", 1)[0]
        warm = audio.split("void AudioService::PrewarmAudioOutput()", 1)[1].split("bool AudioService::IsAfeWakeWord", 1)[0]
        self.assertLess(output.index("lock.unlock()"), output.index("power_lock(output_power_mutex_)"))
        self.assertLess(output.index("power_lock(output_power_mutex_)"), output.index("codec_->OutputData"))
        self.assertLess(power.index("lock(output_power_mutex_, std::try_to_lock)"), power.index("codec_->EnableOutput(false)"))
        self.assertLess(warm.index("lock(output_power_mutex_)"), warm.index("last_output_time_ ="))
        codec = (ROOT / "main/audio/codecs/no_audio_codec.cc").read_text()
        write = codec.split("int NoAudioCodec::Write", 1)[1].split("void NoAudioCodec::AppendReference", 1)[0]
        self.assertIn("if (err != ESP_OK)", write)
        self.assertNotIn("ESP_ERROR_CHECK", write)
        self.assertNotIn("portMAX_DELAY", write)

    def test_mcp_reply_is_deferred_and_serialized_with_mqtt_reconnect(self):
        reply = self.incoming.split("auto send_response = ", 1)[1].split("ParseMessage(payload, send_response)", 1)[0]
        self.assertIn("generation = custom_server_audio_.ReplyGeneration()", reply)
        self.assertLess(reply.index("Schedule([this, generation = *generation"),
                        reply.index("custom_server_audio_.SendText(message, generation)"))
        send = self.protocol.split("bool CustomServerAudio::SendText(", 1)[1].split(
            "bool CustomServerAudio::SendTextLocked(", 1)[0]
        checked = send.split("lock(send_mutex_)", 1)[1]
        self.assertIn("generation == connection_generation_.load()", checked)
        self.assertIn("!udp_reconnect_needed_.load() && SendTextLocked(text)", checked)
        connector = self.protocol.split("bool CustomServerAudio::EnsureConnected()", 1)[1].split(
            "void CustomServerAudio::RequestConnect()", 1)[0]
        self.assertLess(connector.index("udp_reconnect_needed_.store(true)"), connector.index("++connection_generation_"))
        self.assertLess(connector.index("++connection_generation_"), connector.index("udp_.reset()"))
        self.assertIn("const bool sent = SendHelloLocked(hello_id)", connector)
        self.assertLess(connector.index("!OpenUdp()"), connector.index("!SendUdpReadyLocked()"))
        self.assertLess(connector.index("!SendUdpReadyLocked()"), connector.index("listening_ = SendTextLocked(listen)"))
        # 握手已经持锁，不能再进对外带锁的 SendText。
        hello = self.protocol.split("bool CustomServerAudio::SendHelloLocked(uint32_t hello_id)", 1)[1].split(
            "bool CustomServerAudio::ConnectBrokerLocked()", 1)[0]
        self.assertIn("return SendTextLocked(hello)", hello)

    def test_udp_disconnect_defers_destruction_to_locked_connection_task(self):
        disconnect = self.protocol.split("mqtt_->OnDisconnected", 1)[1].split("});", 1)[0]
        self.assertNotIn("udp_.reset()", disconnect)
        self.assertIn("udp_reconnect_needed_.store(true)", disconnect)
        self.assertIn("RequestConnect()", disconnect)
        sender = self.protocol.split("bool CustomServerAudio::SendAudio", 1)[1].split("void CustomServerAudio::SendAbort", 1)[0]
        self.assertLess(sender.index("lock(send_mutex_"), sender.index("udp_->Send"))
        self.assertIn("udp_reconnect_needed_.load()", sender)
        connector = self.protocol.split("void CustomServerAudio::RequestConnect", 1)[1].split("bool CustomServerAudio::Start", 1)[0]
        self.assertLess(connector.index("lock(self->send_mutex_)"), connector.index("self->MaintainConnectionLocked()"))
        broker = self.protocol.split("bool CustomServerAudio::ConnectBrokerLocked()", 1)[1].split(
            "bool CustomServerAudio::SendUdpReadyLocked()", 1)[0]
        self.assertLess(broker.index("return true;"), broker.index("mqtt_.reset()"))
        self.assertIn("custom_server_audio_.PollConnection()", self.app)
        self.assertIn("kHeartbeatTimeoutSeconds", self.protocol)
        self.assertIn("expected_hello_id_", self.protocol)
