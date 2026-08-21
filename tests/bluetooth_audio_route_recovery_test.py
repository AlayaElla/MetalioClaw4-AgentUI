import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ADAPTER = (
    ROOT
    / "main"
    / "display"
    / "agent_ui"
    / "apps"
    / "bluetooth"
    / "bluetooth_adapter.cc"
).read_text(encoding="utf-8")
CODEC = (ROOT / "main" / "boards" / "common" / "bt_audio_codec.cc").read_text(
    encoding="utf-8"
)
ROUTE = (ROOT / "main" / "boards" / "common" / "audio_output_route.cc").read_text(
    encoding="utf-8"
)


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


class BluetoothAudioRouteRecoveryTest(unittest.TestCase):
    def test_connection_defaults_to_music_route_and_defers_profile_commands(self):
        connected = function_body(
            ADAPTER,
            "    void HandleSlcConnectSuccess()",
            "    struct ScoValidationArgs",
        )
        self.assertIn("requested_profile.store(AudioProfile::Music)", connected)
        self.assertIn("ActivateBluetoothOutput(AudioProfile::Music)", connected)
        self.assertNotIn("SetAudioProfile(AudioProfile::Music)", connected)
        self.assertNotIn("StartProfileCommand", connected)
        self.assertNotIn("SetAudioProfile(AudioProfile::Call)", connected)
        self.assertIn("profile commands will run when PCM playback starts", connected)
        self.assertIn("HandleCodecOutputChanged", connected)

    def test_music_start_sequence_is_synchronized_to_pcm_playback(self):
        playback = function_body(
            ADAPTER,
            "    void HandleCodecOutputChanged(",
            "    void HandleSlcConnectSuccess()",
        )
        self.assertIn("enabled", playback)
        self.assertIn("AudioOutputTarget::BluetoothSpeaker", playback)
        self.assertIn("requested_profile.load() != AudioProfile::Music", playback)
        self.assertIn("StartProfileCommand(AudioProfile::Music)", playback)
        self.assertIn("upstream music ", playback)
        self.assertIn("start sequence at the playback boundary", playback)
        self.assertIn("s_codec_change_handler(enabled, s_target", ROUTE)

    def test_sco_event_activates_output_before_mic_validation(self):
        sco_ready = function_body(
            ADAPTER, "    void HandleScoReady()", "    void HandleDeviceDisconnected("
        )
        self.assertIn("ValidateAudioInput(AudioProfile::Call, true)", sco_ready)
        activate = sco_ready.index("ActivateBluetoothOutput(AudioProfile::Call)")
        validate = sco_ready.index("ValidateAudioInput(AudioProfile::Call, true)")
        self.assertLess(activate, validate)

        validation = function_body(
            ADAPTER,
            "    static void ScoValidationTask(void* parameter)",
            "    void ValidateAudioInput(",
        )
        self.assertNotIn("AudioOutput_SetTarget", validation)
        self.assertNotIn("SetOutputTransportEnabled", validation)

    def test_failed_mic_probe_does_not_change_profile_or_mute_output(self):
        validation = function_body(
            ADAPTER,
            "    static void ScoValidationTask(void* parameter)",
            "    void ValidateAudioInput(",
        )
        failure = validation[validation.index("profile remains active") :]
        self.assertNotIn("StartProfileCommand", failure)
        self.assertNotIn("requested_profile.store", failure)
        self.assertNotIn("SetOutputTransportEnabled", failure)
        self.assertNotIn("AudioOutput_SetTarget", failure)

    def test_music_profile_activates_output_without_input_probe(self):
        profile_task = function_body(
            ADAPTER,
            "    static void ProfileTask(void* parameter)",
            "    void StartProfileCommand(",
        )
        music_activation = profile_task.index(
            "ActivateBluetoothOutput(AudioProfile::Music)"
        )
        music_command = profile_task.index('uart.sendString("AT+BTSCO=0')
        self.assertGreater(music_activation, music_command)
        self.assertNotIn("ResumeWakeWord()", profile_task)
        self.assertNotIn("ValidateAudioInput(AudioProfile::Music", profile_task)
        self.assertNotIn("ValidateAudioInput(AudioProfile::Music", ADAPTER)

    def test_profile_uart_commands_are_logged_individually(self):
        profile_task = function_body(
            ADAPTER,
            "    static void ProfileTask(void* parameter)",
            "    void StartProfileCommand(",
        )
        for command in ("AT+PP=1", "AT+BTSCO=1", "AT+BTSCO=0"):
            self.assertIn(f'TX: {command}', profile_task)

    def test_i2s_logs_expected_clock_and_runtime_pause(self):
        self.assertIn("I2S config: port=0 role=slave", CODEC)
        self.assertIn("expected_ws=%dHz expected_bclk=%dHz", CODEC)
        self.assertIn("clock_absent_ms=", CODEC)
        self.assertIn("WS clock probe ready", CODEC)
        self.assertIn("observed_ws_edges=", CODEC)
        self.assertIn('"absent"', CODEC)
        self.assertIn('"present_dma_stalled"', CODEC)

    def test_bluetooth_route_drives_i2s_clock_and_local_route_restores_slave(self):
        transport = function_body(
            CODEC,
            "bool BTAudioCodec::SetOutputTransportEnabled(bool enabled)",
            "void BTAudioCodec::DeleteI2sChannels()",
        )
        self.assertIn("AudioOutputTarget::BluetoothSpeaker", transport)
        self.assertIn("I2S_ROLE_MASTER", transport)
        self.assertIn("I2S_ROLE_SLAVE", transport)
        self.assertIn("SetI2sClockRole(desired_role)", transport)
        self.assertIn("output_transport_requested_.store(ready", transport)
        self.assertIn("I2S clock role configured: role=%s", CODEC)

        read = function_body(CODEC, "int BTAudioCodec::Read(", "\n    size_t bytes_read;")
        self.assertIn("std::lock_guard<std::mutex> lock(data_if_mutex_)", read)

    def test_route_activation_does_not_guess_module_volume_or_send_burst(self):
        activate = function_body(
            ADAPTER,
            "    bool ActivateBluetoothOutput(AudioProfile profile)",
            "    void UseLocalRoute(bool disable_setting)",
        )
        self.assertNotIn("module_volume.store", activate)
        self.assertNotIn("ScheduleVolumeSync", activate)
        self.assertNotIn("AT+VOLUP", activate)

    def test_profile_selection_keeps_output_suspended_until_validation(self):
        set_profile = function_body(
            ADAPTER, "    void SetAudioProfile(AudioProfile profile)", "};\n\nAdapter&"
        )
        self.assertIn("SuspendOutputForRouteChange()", set_profile)
        self.assertIn("AudioOutputTarget::LocalSpeaker", set_profile)
        self.assertNotIn("AudioOutputTarget::BluetoothSpeaker", set_profile)

    def test_mode_switch_gates_pcm_while_clock_role_changes(self):
        transport = function_body(
            CODEC,
            "bool BTAudioCodec::SetOutputTransportEnabled(bool enabled)",
            "BTAudioCodecDuplex::BTAudioCodecDuplex(",
        )
        self.assertIn("output_transport_requested_.exchange(", transport)
        self.assertIn("false, std::memory_order_acq_rel", transport)
        self.assertIn("SetI2sClockRole(desired_role)", transport)
        self.assertIn("output_transport_requested_.store(ready", transport)
        self.assertIn(
            "if (!output_transport_requested_.load", CODEC
        )
        self.assertIn("duplex channels remain", CODEC)
        self.assertIn('"enabled, expected_ws=', CODEC)

        write = function_body(
            CODEC, "int BTAudioCodec::Write(", "int BTAudioCodec::Read("
        )
        timeout = write[write.index("error == ESP_ERR_TIMEOUT") :]
        self.assertIn("PCM may remain queued in DMA", timeout)
        self.assertIn("copied=%u requested=%u", timeout)
        self.assertIn("continue;", timeout)
        self.assertIn("while (output_transport_requested_.load", write)

        handle_line = function_body(
            ADAPTER, "    void HandleLine(const std::string& raw_line)", "    void OnUartData("
        )
        mode1 = handle_line.index('line.find("SET MODE 1")')
        resume = handle_line.index("SetOutputTransportEnabled(true)", mode1)
        self.assertGreater(resume, mode1)


if __name__ == "__main__":
    unittest.main()
