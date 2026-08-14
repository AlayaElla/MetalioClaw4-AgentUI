#include "hermes_voice_session.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include <cJSON.h>
#include <mbedtls/base64.h>
#include <decoder/esp_audio_dec.h>
#include <decoder/esp_audio_dec_default.h>
#include <esp_vad.h>

#include "boards/common/board.h"
#include "network_interface.h"
#include "web_socket.h"

namespace hermes_voice {
namespace {
void WriteLe16(std::string* out, uint16_t value) { out->push_back(value & 0xff); out->push_back(value >> 8); }
void WriteLe32(std::string* out, uint32_t value) { WriteLe16(out, value & 0xffff); WriteLe16(out, value >> 16); }
uint16_t ReadLe16(const uint8_t* p) { return p[0] | (static_cast<uint16_t>(p[1]) << 8); }
uint32_t ReadLe32(const uint8_t* p) { return ReadLe16(p) | (static_cast<uint32_t>(ReadLe16(p + 2)) << 16); }

bool AppendMonoResampled(const int16_t* input, size_t interleaved_samples,
                         uint32_t sample_rate, uint8_t channels,
                         uint64_t* source_frames_total,
                         uint64_t* output_frames_total,
                         std::vector<int16_t>* output) {
    if (input == nullptr || output == nullptr || source_frames_total == nullptr ||
        output_frames_total == nullptr || sample_rate == 0 ||
        (channels != 1 && channels != 2) || interleaved_samples % channels) return false;
    const size_t frames = interleaved_samples / channels;
    if (frames == 0 || *source_frames_total > UINT64_MAX - frames) return false;
    const uint64_t block_start = *source_frames_total;
    const uint64_t block_end = block_start + frames;
    while (true) {
        const uint64_t global_source =
            (*output_frames_total * static_cast<uint64_t>(sample_rate)) / kSampleRate;
        if (global_source >= block_end) break;
        if (global_source < block_start || output->size() >= kMaxDecodedTtsSamples) return false;
        const size_t source = static_cast<size_t>(global_source - block_start);
        const int16_t left = input[source * channels];
        const int32_t mono = channels == 1 ? left : (static_cast<int32_t>(left) + input[source * channels + 1]) / 2;
        output->push_back(static_cast<int16_t>(mono));
        ++*output_frames_total;
    }
    *source_frames_total = block_end;
    return true;
}

std::string Base64Encode(const uint8_t* data, size_t size) {
    size_t needed = 0;
    if (mbedtls_base64_encode(nullptr, 0, &needed, data, size) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) return {};
    std::string output(needed, '\0');
    size_t written = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(output.data()), output.size(), &written, data, size) != 0) return {};
    output.resize(written);
    return output;
}

}  // namespace

std::string BuildWavDataUrl(const std::vector<int16_t>& pcm) {
    if (pcm.empty() || pcm.size() * sizeof(int16_t) > kMaxRecordingBytes) return {};
    const uint32_t data_size = static_cast<uint32_t>(pcm.size() * sizeof(int16_t));
    std::string wav;
    wav.reserve(44 + data_size);
    wav.append("RIFF", 4); WriteLe32(&wav, 36 + data_size); wav.append("WAVEfmt ", 8);
    WriteLe32(&wav, 16); WriteLe16(&wav, 1); WriteLe16(&wav, 1); WriteLe32(&wav, kSampleRate);
    WriteLe32(&wav, kSampleRate * sizeof(int16_t)); WriteLe16(&wav, sizeof(int16_t)); WriteLe16(&wav, 16);
    wav.append("data", 4); WriteLe32(&wav, data_size);
    wav.append(reinterpret_cast<const char*>(pcm.data()), data_size);
    const std::string encoded = Base64Encode(reinterpret_cast<const uint8_t*>(wav.data()), wav.size());
    return encoded.empty() ? std::string() : "data:audio/wav;base64," + encoded;
}

bool ParseDataUrl(std::string_view value, size_t max_bytes, DataUrlResult* out) {
    if (out == nullptr || value.size() < 14 || value.size() > max_bytes * 2) return false;
    const size_t comma = value.find(',');
    if (comma == std::string_view::npos || value.substr(0, 5) != "data:") return false;
    const std::string_view meta = value.substr(5, comma - 5);
    constexpr std::string_view suffix = ";base64";
    if (meta.size() <= suffix.size() || meta.substr(meta.size() - suffix.size()) != suffix) return false;
    const std::string_view mime = meta.substr(0, meta.size() - suffix.size());
    if (mime != "audio/wav" && mime != "audio/x-wav" && mime != "audio/mpeg") return false;
    const std::string_view encoded = value.substr(comma + 1);
    if (encoded.empty() || encoded.size() > ((max_bytes + 2) / 3) * 4) return false;
    size_t decoded_size = 0;
    if (mbedtls_base64_decode(nullptr, 0, &decoded_size, reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || decoded_size > max_bytes) return false;
    std::vector<uint8_t> decoded(decoded_size);
    if (mbedtls_base64_decode(decoded.data(), decoded.size(), &decoded_size, reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()) != 0) return false;
    decoded.resize(decoded_size); out->mime_type.assign(mime); out->bytes = std::move(decoded); return true;
}

bool ParseTtsDataUrlJson(std::string_view json, std::string* data_url) {
    if (data_url == nullptr || json.empty() || json.size() > kMaxHttpResponseBytes) return false;
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    const cJSON* value = cJSON_IsObject(root) ? cJSON_GetObjectItemCaseSensitive(root, "data_url") : root;
    if (!cJSON_IsString(value) || std::strlen(value->valuestring) > ((kMaxTtsBytes + 2) / 3) * 4 + 64) {
        cJSON_Delete(root); return false;
    }
    *data_url = value->valuestring; cJSON_Delete(root); return true;
}

bool ParseSttTranscriptJson(std::string_view json, std::string* transcript) {
    if (transcript == nullptr || json.empty() || json.size() > 32 * 1024) return false;
    transcript->clear();
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    if (root == nullptr) return false;

    const cJSON* value = root;
    if (cJSON_IsObject(root)) {
        value = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        if (!cJSON_IsString(value)) {
            value = cJSON_GetObjectItemCaseSensitive(root, "text");
        }
    }
    if (!cJSON_IsString(value)) {
        cJSON_Delete(root);
        return false;
    }
    const size_t length = std::strlen(value->valuestring);
    if (length > 8192) {
        cJSON_Delete(root);
        return false;
    }
    transcript->assign(value->valuestring, length);
    cJSON_Delete(root);
    return true;
}

bool ParsePcmWav(std::string_view wav, WavPcmResult* out) {
    if (out == nullptr || wav.size() < 44 || wav.size() > kMaxTtsBytes || std::memcmp(wav.data(), "RIFF", 4) || std::memcmp(wav.data() + 8, "WAVE", 4)) return false;
    const auto* bytes = reinterpret_cast<const uint8_t*>(wav.data());
    const uint32_t riff_size = ReadLe32(bytes + 4);
    if (riff_size + 8 != wav.size()) return false;
    size_t offset = 12; bool fmt = false; const uint8_t* fmt_data = nullptr; const uint8_t* data = nullptr; uint32_t data_size = 0;
    while (offset + 8 <= wav.size()) {
        const uint32_t size = ReadLe32(bytes + offset + 4); const size_t body = offset + 8;
        if (size > wav.size() - body) return false;
        if (!std::memcmp(bytes + offset, "fmt ", 4)) {
            if (size < 16 || ReadLe16(bytes + body) != 1 ||
                (ReadLe16(bytes + body + 2) != 1 && ReadLe16(bytes + body + 2) != 2) ||
                ReadLe32(bytes + body + 4) == 0 || ReadLe16(bytes + body + 14) != 16) return false;
            fmt = true; fmt_data = bytes + body;
        } else if (!std::memcmp(bytes + offset, "data", 4)) { data = bytes + body; data_size = size; }
        offset = body + size + (size & 1);
    }
    if (!fmt || data == nullptr || data_size == 0 || data_size % 2) return false;
    // Re-read fmt fields after their bounded validation above.
    const uint16_t channels = ReadLe16(fmt_data + 2);
    const uint32_t sample_rate = ReadLe32(fmt_data + 4);
    if (data_size % (channels * sizeof(int16_t))) return false;
    uint64_t source_frames_total = 0;
    uint64_t output_frames_total = 0;
    out->samples.clear();
    return AppendMonoResampled(reinterpret_cast<const int16_t*>(data),
        data_size / sizeof(int16_t), sample_rate, channels,
        &source_frames_total, &output_frames_total, &out->samples);
}

bool DecodeMpegToPcm16k(std::string_view mpeg, WavPcmResult* out) {
    if (out == nullptr || mpeg.empty() || mpeg.size() > kMaxTtsBytes) return false;
    esp_audio_dec_register_default();
    esp_audio_dec_cfg_t config = {.type = ESP_AUDIO_TYPE_MP3, .cfg = nullptr, .cfg_sz = 0};
    esp_audio_dec_handle_t decoder = nullptr;
    if (esp_audio_dec_open(&config, &decoder) != ESP_AUDIO_ERR_OK) return false;
    std::vector<uint8_t> output_buffer(16384);
    esp_audio_dec_in_raw_t raw = {.buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(mpeg.data())), .len = static_cast<uint32_t>(mpeg.size())};
    out->samples.clear();
    uint64_t source_frames_total = 0;
    uint64_t output_frames_total = 0;
    uint32_t stream_sample_rate = 0;
    uint8_t stream_channels = 0;
    bool decoded_any = false;
    while (raw.len > 0) {
        esp_audio_dec_out_frame_t frame = {.buffer = output_buffer.data(), .len = static_cast<uint32_t>(output_buffer.size())};
        const esp_audio_err_t result = esp_audio_dec_process(decoder, &raw, &frame);
        if (result != ESP_AUDIO_ERR_OK || raw.consumed == 0) break;
        raw.buffer += raw.consumed; raw.len -= raw.consumed;
        if (frame.decoded_size == 0) continue;
        esp_audio_dec_info_t info{};
        if (esp_audio_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK || info.bits_per_sample != 16 ||
            (stream_sample_rate != 0 && (stream_sample_rate != info.sample_rate ||
                                         stream_channels != info.channel)) ||
            !AppendMonoResampled(reinterpret_cast<const int16_t*>(frame.buffer), frame.decoded_size / 2,
                info.sample_rate, info.channel, &source_frames_total,
                &output_frames_total, &out->samples)) { out->samples.clear(); break; }
        stream_sample_rate = info.sample_rate;
        stream_channels = info.channel;
        decoded_any = true;
    }
    esp_audio_dec_close(decoder);
    return decoded_any && !out->samples.empty();
}

bool ParseDashboardProfiles(std::string_view json, std::vector<std::string>* profiles) {
    if (profiles == nullptr || json.empty() || json.size() > 32 * 1024) return false;
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    const cJSON* entries = cJSON_IsArray(root)
        ? root : cJSON_GetObjectItemCaseSensitive(root, "profiles");
    if (!cJSON_IsArray(entries)) { cJSON_Delete(root); return false; }
    profiles->clear();
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, entries) {
        if (profiles->size() >= 16) break;
        const cJSON* name = cJSON_IsString(entry)
            ? entry : cJSON_GetObjectItemCaseSensitive(entry, "name");
        if (!cJSON_IsString(name) ||
            !ai_provider_config::IsValidHermesProfile(name->valuestring)) continue;
        if (std::find(profiles->begin(), profiles->end(), name->valuestring) == profiles->end()) {
            profiles->emplace_back(name->valuestring);
        }
    }
    cJSON_Delete(root);
    return true;
}

bool ParsePasswordProvider(std::string_view json, std::string* provider) {
    if (provider == nullptr || json.empty() || json.size() > 8192) return false;
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    const cJSON* providers = cJSON_GetObjectItemCaseSensitive(root, "providers");
    std::string selected;
    size_t supported = 0;
    if (cJSON_IsArray(providers)) {
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, providers) {
            const cJSON* supports_password = cJSON_GetObjectItemCaseSensitive(
                entry, "supports_password");
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(entry, "name");
            if (!cJSON_IsTrue(supports_password) || !cJSON_IsString(name)) continue;
            const std::string_view value(name->valuestring);
            const bool valid = !value.empty() && value.size() <= 128 &&
                !ai_provider_config::HasControl(value) &&
                std::none_of(value.begin(), value.end(), [](unsigned char ch) {
                    return std::isspace(ch) != 0;
                });
            if (!valid) continue;
            selected.assign(value);
            ++supported;
        }
    }
    cJSON_Delete(root);
    if (supported != 1) return false;
    *provider = std::move(selected);
    return true;
}

bool ParseWsTicket(std::string_view json, std::string* ticket) {
    if (ticket == nullptr || json.empty() || json.size() > 4096) return false;
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    const cJSON* value = cJSON_IsString(root)
        ? root : cJSON_GetObjectItemCaseSensitive(root, "ticket");
    const bool valid = cJSON_IsString(value) && std::strlen(value->valuestring) > 0 &&
        std::strlen(value->valuestring) <= 1024 &&
        !ai_provider_config::HasControl(value->valuestring);
    if (valid) *ticket = value->valuestring;
    cJSON_Delete(root);
    return valid;
}

bool BuildCookieHeader(const std::vector<std::string>& set_cookie_headers,
                       std::string* cookie_header) {
    if (cookie_header == nullptr) return false;
    cookie_header->clear();
    for (const auto& header : set_cookie_headers) {
        const size_t end = header.find(';');
        const std::string_view pair(header.data(),
            end == std::string::npos ? header.size() : end);
        const size_t equals = pair.find('=');
        if (equals == std::string_view::npos || equals == 0 ||
            equals + 1 >= pair.size() || pair.size() > 1536 ||
            ai_provider_config::HasControl(pair)) continue;
        if (!cookie_header->empty()) cookie_header->append("; ");
        cookie_header->append(pair);
    }
    return !cookie_header->empty() && cookie_header->size() <= 2048;
}

std::string DashboardWebSocketUrl(std::string_view base_url,
                                  std::string_view ticket,
                                  std::string_view profile) {
    std::string base = ai_provider_config::NormalizeHermesBaseUrl(base_url);
    if (base.rfind("https://", 0) == 0) base.replace(0, 5, "wss");
    else if (base.rfind("http://", 0) == 0) base.replace(0, 4, "ws");
    else return {};
    return base + "/api/ws?ticket=" + ai_provider_config::UrlEncode(ticket) +
        "&profile=" + ai_provider_config::UrlEncode(profile);
}

Session::Session() {
    recorder_.reserve(kMaxRecordingBytes / sizeof(int16_t));
}
Session::~Session() {
    recording_.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(recorder_mutex_);
    if (endpoint_vad_ != nullptr) {
        vad_destroy(static_cast<vad_handle_t>(endpoint_vad_));
    }
}
uint32_t Session::StartRecording() {
    const uint32_t next = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    std::lock_guard<std::mutex> lock(recorder_mutex_);
    recorder_.clear();
    if (recorder_.capacity() < kMaxRecordingBytes / sizeof(int16_t)) {
        recorder_.reserve(kMaxRecordingBytes / sizeof(int16_t));
    }
    // Hermes needs a stricter endpointer than the shared AFE VAD used for UI
    // and XiaoZhi. Two 30 ms aggressive WebRTC VAD decisions are combined
    // into one 60 ms recorder frame; the application applies release hangover.
    if (endpoint_vad_ == nullptr) {
        endpoint_vad_ = vad_create_with_param(
            VAD_MODE_2, kSampleRate, 30, 90, 90);
    }
    if (endpoint_vad_ != nullptr) {
        vad_reset_trigger(static_cast<vad_handle_t>(endpoint_vad_));
    }
    endpoint_voice_detected_.store(false, std::memory_order_release);
    endpoint_transitions_.store(0, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
    state_.store(State::Recording, std::memory_order_release);
    return next;
}
bool Session::CopyProcessedFrame(const int16_t* samples, size_t count) {
    if (!recording_.load(std::memory_order_acquire) || samples == nullptr ||
        count != kFrameSamples) return false;
    std::lock_guard<std::mutex> lock(recorder_mutex_);
    if (!recording_.load(std::memory_order_relaxed) ||
        recorder_.size() + count > recorder_.capacity()) return false;
    recorder_.insert(recorder_.end(), samples, samples + count);
    if (endpoint_vad_ == nullptr) return false;
    const auto endpoint_vad = static_cast<vad_handle_t>(endpoint_vad_);
    const bool first_half_speech = vad_process(
        endpoint_vad, const_cast<int16_t*>(samples), kSampleRate, 30) == VAD_SPEECH;
    const bool second_half_speech = vad_process(
        endpoint_vad, const_cast<int16_t*>(samples + kFrameSamples / 2),
        kSampleRate, 30) == VAD_SPEECH;
    const bool voice_detected = first_half_speech || second_half_speech;
    const bool changed = endpoint_voice_detected_.exchange(
        voice_detected, std::memory_order_acq_rel) != voice_detected;
    if (changed) endpoint_transitions_.fetch_add(1, std::memory_order_acq_rel);
    return changed;
}
std::vector<int16_t> Session::StopRecording(uint32_t epoch) { recording_.store(false, std::memory_order_release); endpoint_voice_detected_.store(false, std::memory_order_release); std::lock_guard<std::mutex> lock(recorder_mutex_); if (!IsCurrent(epoch)) return {}; state_.store(State::Transcribing, std::memory_order_release); std::vector<int16_t> snapshot; snapshot.swap(recorder_); return snapshot; }
uint32_t Session::Cancel() { recording_.store(false, std::memory_order_release); endpoint_voice_detected_.store(false, std::memory_order_release); const uint32_t next = epoch_.fetch_add(1, std::memory_order_acq_rel) + 1; std::lock_guard<std::mutex> lock(recorder_mutex_); recorder_.clear(); state_.store(State::Idle, std::memory_order_release); return next; }
bool Session::IsCurrent(uint32_t epoch) const { return epoch_.load(std::memory_order_acquire) == epoch; }

bool HttpJson(std::string_view method, std::string_view url,
              std::string_view cookie_header, std::string&& body,
              HttpResult* result, size_t max_response_bytes) {
    if (result == nullptr || url.empty() || method.empty() ||
        body.size() > kMaxHttpRequestBytes || max_response_bytes > kMaxHttpResponseBytes) return false;
    auto network = Board::GetInstance().GetNetwork(); if (network == nullptr) return false;
    auto http = network->CreateHttp(0); if (http == nullptr) return false;
    http->SetTimeout(15000);
    http->SetHeader("Accept", "application/json");
    if (!cookie_header.empty()) http->SetHeader("Cookie", std::string(cookie_header));
    if (!body.empty()) {
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(body));
    }
    if (!http->Open(std::string(method), std::string(url))) return false;
    result->status = http->GetStatusCode();
    result->set_cookie_headers = http->GetResponseHeaders("Set-Cookie");
    const int body_length = http->GetBodyLength();
    // A chunked response reports zero in this wrapper.  Read in fixed chunks
    // to EOF, enforcing the cap before every append (0 is a valid empty body).
    if (body_length < 0 || static_cast<size_t>(body_length) > max_response_bytes) { http->Close(); return false; }
    result->body.clear();
    if (body_length > 0) result->body.reserve(static_cast<size_t>(body_length));
    // Keep the network worker's stack bounded; the task also carries cJSON and
    // C++ HTTP frames, so a 4 KiB local buffer leaves too little headroom.
    char chunk[1024];
    while (true) {
        const int n = http->Read(chunk, sizeof(chunk));
        if (n < 0 || static_cast<size_t>(n) > max_response_bytes - result->body.size()) { http->Close(); return false; }
        if (n == 0) break;
        result->body.append(chunk, static_cast<size_t>(n));
    }
    http->Close(); return true;
}

namespace {

std::string JsonObject(std::initializer_list<std::pair<const char*, std::string_view>> fields) {
    cJSON* root = cJSON_CreateObject();
    for (const auto& [key, value] : fields) {
        cJSON_AddStringToObject(root, key, std::string(value).c_str());
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string body = printed != nullptr ? printed : "";
    cJSON_free(printed);
    cJSON_Delete(root);
    return body;
}

class GatewayRpcClient {
public:
    bool Connect(std::string_view url, std::string* error) {
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) return Fail(error, "网络不可用");
        websocket_ = network->CreateWebSocket(2);
        if (websocket_ == nullptr) return Fail(error, "无法创建 WebSocket");
        websocket_->SetReceiveBufferSize(16 * 1024);
        websocket_->OnConnected([this]() {
            std::lock_guard<std::mutex> lock(mutex_); connected_ = true; cv_.notify_all();
        });
        websocket_->OnDisconnected([this]() {
            std::lock_guard<std::mutex> lock(mutex_); disconnected_ = true; cv_.notify_all();
        });
        websocket_->OnError([this](int) {
            std::lock_guard<std::mutex> lock(mutex_); failed_ = true; cv_.notify_all();
        });
        websocket_->OnData([this](const char* data, size_t size, bool binary) {
            if (!binary && data != nullptr && size <= 32 * 1024) OnFrame(data, size);
        });
        if (!websocket_->Connect(std::string(url).c_str())) return Fail(error, "WebSocket 握手失败");
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(8), [this] {
                return ready_ || failed_ || disconnected_;
            }) || !ready_) return Fail(error, "未收到 gateway.ready");
        return true;
    }

    bool Rpc(int id, const char* method, cJSON* params, std::string* response,
             std::string* error, int timeout_seconds = 15) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "jsonrpc", "2.0");
        cJSON_AddNumberToObject(root, "id", id);
        cJSON_AddStringToObject(root, "method", method);
        cJSON_AddItemToObject(root, "params", params != nullptr ? params : cJSON_CreateObject());
        char* printed = cJSON_PrintUnformatted(root);
        std::string request = printed != nullptr ? printed : "";
        cJSON_free(printed); cJSON_Delete(root);
        if (request.empty()) return Fail(error, "JSON-RPC 编码失败");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            waiting_id_ = id; response_ready_ = false; response_.clear();
        }
        if (!websocket_->Send(request)) return Fail(error, "JSON-RPC 发送失败");
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(timeout_seconds), [this] {
                return response_ready_ || failed_ || disconnected_;
            }) || !response_ready_) return Fail(error, "JSON-RPC 响应超时");
        *response = response_;
        return true;
    }

    bool WaitForTurn(std::string_view live_session_id, std::string* text,
                     std::string* error,
                     const std::function<bool()>& keep_waiting) {
        std::unique_lock<std::mutex> lock(mutex_);
        expected_session_id_.assign(live_session_id);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(180);
        while (!turn_complete_ && !failed_ && !disconnected_) {
            if (keep_waiting && !keep_waiting()) {
                lock.unlock();
                cJSON* params = cJSON_CreateObject();
                cJSON_AddStringToObject(params, "session_id",
                                        std::string(live_session_id).c_str());
                std::string ignored_response;
                std::string ignored_error;
                Rpc(99, "session.interrupt", params, &ignored_response,
                    &ignored_error, 2);
                Close();
                return Fail(error, "Hermes 请求已取消");
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return Fail(error, "Hermes 回复超时");
            cv_.wait_for(lock, std::min(std::chrono::milliseconds(250),
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)));
        }
        if (!turn_complete_) return Fail(error, "Hermes 连接已断开");
        if (interaction_required_) {
            lock.unlock();
            BestEffortInterrupt(live_session_id);
            return Fail(error, "需要在 Dashboard 完成人工确认");
        }
        if (turn_failed_) return Fail(error, "Hermes Agent 返回错误");
        *text = final_text_.empty() ? streamed_text_ : final_text_;
        if (text->empty() || text->size() > 8192) return Fail(error, "Hermes 回复为空或过长");
        return true;
    }

    void Close() {
        if (websocket_ == nullptr) return;
        websocket_->Close();
        // Destroy the transport synchronously while mutex_/cv_ and every other
        // callback target are still alive. Member destruction would otherwise
        // destroy those fields before websocket_ because it is declared first.
        websocket_.reset();
    }
    void BestEffortInterrupt(std::string_view live_session_id) {
        cJSON* params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "session_id",
                                std::string(live_session_id).c_str());
        std::string ignored_response;
        std::string ignored_error;
        Rpc(99, "session.interrupt", params, &ignored_response, &ignored_error, 2);
        Close();
    }
    ~GatewayRpcClient() { Close(); }

private:
    static bool Fail(std::string* error, const char* message) {
        if (error != nullptr) *error = message;
        return false;
    }

    void OnFrame(const char* data, size_t size) {
        cJSON* root = cJSON_ParseWithLength(data, size);
        if (!cJSON_IsObject(root)) { cJSON_Delete(root); return; }
        std::lock_guard<std::mutex> lock(mutex_);
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (cJSON_IsNumber(id) && id->valueint == waiting_id_) {
            char* printed = cJSON_PrintUnformatted(root);
            response_ = printed != nullptr ? printed : "";
            cJSON_free(printed); response_ready_ = true; cv_.notify_all();
        }
        const cJSON* method = cJSON_GetObjectItemCaseSensitive(root, "method");
        const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
        const cJSON* type = cJSON_IsObject(params)
            ? cJSON_GetObjectItemCaseSensitive(params, "type") : nullptr;
        if (cJSON_IsString(method) && std::strcmp(method->valuestring, "event") == 0 &&
            cJSON_IsString(type)) {
            if (std::strcmp(type->valuestring, "gateway.ready") == 0) {
                ready_ = true; cv_.notify_all();
            } else {
                const cJSON* session_id = cJSON_GetObjectItemCaseSensitive(params, "session_id");
                if (cJSON_IsString(session_id) &&
                    (expected_session_id_.empty() || expected_session_id_ == session_id->valuestring)) {
                    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(params, "payload");
                    const cJSON* event_text = cJSON_IsObject(payload)
                        ? cJSON_GetObjectItemCaseSensitive(payload, "text") : nullptr;
                    if (std::strcmp(type->valuestring, "message.delta") == 0 &&
                        cJSON_IsString(event_text) &&
                        streamed_text_.size() + std::strlen(event_text->valuestring) <= 8192) {
                        streamed_text_ += event_text->valuestring;
                    } else if (std::strcmp(type->valuestring, "message.complete") == 0) {
                        const cJSON* status = cJSON_IsObject(payload)
                            ? cJSON_GetObjectItemCaseSensitive(payload, "status") : nullptr;
                        turn_failed_ = cJSON_IsString(status) &&
                            std::strcmp(status->valuestring, "complete") != 0;
                        if (cJSON_IsString(event_text)) final_text_ = event_text->valuestring;
                        turn_complete_ = true; cv_.notify_all();
                    } else if (std::strcmp(type->valuestring, "approval.request") == 0 ||
                               std::strcmp(type->valuestring, "clarify.request") == 0 ||
                               std::strcmp(type->valuestring, "sudo.request") == 0 ||
                               std::strcmp(type->valuestring, "secret.request") == 0) {
                        interaction_required_ = true;
                        turn_failed_ = true;
                        turn_complete_ = true;
                        cv_.notify_all();
                    }
                }
            }
        }
        cJSON_Delete(root);
    }

    std::unique_ptr<WebSocket> websocket_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool connected_ = false;
    bool ready_ = false;
    bool failed_ = false;
    bool disconnected_ = false;
    bool response_ready_ = false;
    bool turn_complete_ = false;
    bool turn_failed_ = false;
    bool interaction_required_ = false;
    int waiting_id_ = 0;
    std::string response_;
    std::string expected_session_id_;
    std::string streamed_text_;
    std::string final_text_;
};

bool ParseRpcResult(std::string_view json, cJSON** root_out, const cJSON** result_out,
                    std::string* error) {
    cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
    const cJSON* rpc_error = cJSON_GetObjectItemCaseSensitive(root, "error");
    const cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsObject(root) || cJSON_IsObject(rpc_error) || !cJSON_IsObject(result)) {
        if (error != nullptr) *error = "Hermes JSON-RPC 返回错误";
        cJSON_Delete(root); return false;
    }
    *root_out = root; *result_out = result; return true;
}

}  // namespace

bool LoginDashboard(const AiProviderConfig& config, DashboardSession* session,
                    std::string* error) {
    if (session == nullptr || !ai_provider_config::IsCompleteHermesConfig(config)) {
        if (error != nullptr) *error = "Hermes 配置不完整";
        return false;
    }
    const std::string base_url = ai_provider_config::NormalizeHermesBaseUrl(
        config.hermes_dashboard_url);
    HttpResult providers_response;
    std::string password_provider;
    if (!HttpJson("GET", base_url + "/api/auth/providers", {}, {},
                  &providers_response, 8192) || providers_response.status != 200 ||
        !ParsePasswordProvider(providers_response.body, &password_provider)) {
        if (error != nullptr) *error = "未找到唯一的密码登录方式";
        return false;
    }
    HttpResult response;
    const std::string url = base_url + "/auth/password-login";
    if (!HttpJson("POST", url, {}, JsonObject({
            {"provider", password_provider}, {"username", config.hermes_username},
            {"password", config.hermes_password}, {"next", "/"}}),
            &response, 4096) || response.status != 200) {
        if (error != nullptr) *error = "用户名、密码或服务地址无效";
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(response.body.data(), response.body.size());
    const cJSON* ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    const bool accepted = cJSON_IsTrue(ok) != 0;
    cJSON_Delete(root);
    if (!accepted || !BuildCookieHeader(response.set_cookie_headers,
                                        &session->cookie_header)) {
        if (error != nullptr) *error = "登录成功但未收到会话 Cookie";
        return false;
    }
    return true;
}

bool CheckDashboardIdentity(const AiProviderConfig& config,
                            const DashboardSession& session, std::string* error) {
    HttpResult response;
    const std::string url = ai_provider_config::NormalizeHermesBaseUrl(
        config.hermes_dashboard_url) + "/api/auth/me";
    if (!HttpJson("GET", url, session.cookie_header, {}, &response, 8192) ||
        response.status != 200) {
        if (error != nullptr) *error = "身份验证失败";
        return false;
    }
    return true;
}

bool LoadDashboardProfiles(const AiProviderConfig& config,
                           const DashboardSession& session,
                           std::vector<std::string>* profiles, std::string* error) {
    HttpResult response;
    if (!HttpJson("GET", ai_provider_config::HermesProfilesUrl(config.hermes_dashboard_url),
                  session.cookie_header, {}, &response, 32 * 1024) ||
        response.status != 200 || !ParseDashboardProfiles(response.body, profiles)) {
        if (error != nullptr) *error = "Agent 列表获取失败";
        return false;
    }
    return true;
}

bool MintDashboardWsTicket(const AiProviderConfig& config,
                           const DashboardSession& session,
                           std::string* ticket, std::string* error) {
    HttpResult response;
    const std::string url = ai_provider_config::NormalizeHermesBaseUrl(
        config.hermes_dashboard_url) + "/api/auth/ws-ticket";
    if (!HttpJson("POST", url, session.cookie_header, "{}", &response, 4096) ||
        response.status != 200 || !ParseWsTicket(response.body, ticket)) {
        if (error != nullptr) *error = "WebSocket ticket 获取失败";
        return false;
    }
    return true;
}

bool TestDashboardGateway(const AiProviderConfig& config,
                          const DashboardSession& session, std::string* error) {
    std::string ticket;
    if (!MintDashboardWsTicket(config, session, &ticket, error)) return false;
    GatewayRpcClient client;
    return client.Connect(DashboardWebSocketUrl(config.hermes_dashboard_url,
        ticket, config.hermes_profile), error);
}

bool RunDashboardTurn(const AiProviderConfig& config,
                      const DashboardSession& session,
                      std::string_view user_text,
                      std::string_view stored_session_id,
                      GatewayTurnResult* result, std::string* error,
                      std::function<bool()> keep_waiting) {
    if (result == nullptr || user_text.empty() || user_text.size() > 8192) return false;
    const auto is_current = [&keep_waiting]() {
        return !keep_waiting || keep_waiting();
    };
    const auto cancelled = [error]() {
        if (error != nullptr) *error = "Hermes 请求已取消";
        return false;
    };
    if (!is_current()) return cancelled();
    std::string ticket;
    if (!MintDashboardWsTicket(config, session, &ticket, error)) return false;
    if (!is_current()) return cancelled();
    GatewayRpcClient client;
    if (!client.Connect(DashboardWebSocketUrl(config.hermes_dashboard_url,
            ticket, config.hermes_profile), error)) return false;
    if (!is_current()) return cancelled();

    cJSON* session_params = cJSON_CreateObject();
    cJSON_AddNumberToObject(session_params, "cols", 80);
    cJSON_AddStringToObject(session_params, "source", "metalio");
    cJSON_AddStringToObject(session_params, "profile", config.hermes_profile.c_str());
    const bool resume = !stored_session_id.empty();
    if (resume) cJSON_AddStringToObject(session_params, "session_id",
                                        std::string(stored_session_id).c_str());
    std::string session_response;
    if (!client.Rpc(1, resume ? "session.resume" : "session.create",
                    session_params, &session_response, error, 30)) return false;
    cJSON* session_root = nullptr; const cJSON* session_result = nullptr;
    if (!ParseRpcResult(session_response, &session_root, &session_result, error)) return false;
    const cJSON* live_id = cJSON_GetObjectItemCaseSensitive(session_result, "session_id");
    const cJSON* stored_id = cJSON_GetObjectItemCaseSensitive(
        session_result, resume ? "session_key" : "stored_session_id");
    const cJSON* info = cJSON_GetObjectItemCaseSensitive(session_result, "info");
    const cJSON* profile_name = cJSON_IsObject(info)
        ? cJSON_GetObjectItemCaseSensitive(info, "profile_name") : nullptr;
    if (!cJSON_IsString(live_id) ||
        (cJSON_IsString(profile_name) && config.hermes_profile != profile_name->valuestring)) {
        cJSON_Delete(session_root);
        if (error != nullptr) *error = cJSON_IsString(live_id)
            ? "Hermes 未路由到所选 Agent/Profile" : "Hermes 会话响应无效";
        return false;
    }
    const std::string live_session_id = live_id->valuestring;
    result->stored_session_id = resume ? std::string(stored_session_id)
        : (cJSON_IsString(stored_id) ? std::string(stored_id->valuestring) : std::string());
    cJSON_Delete(session_root);
    if (!is_current()) {
        client.BestEffortInterrupt(live_session_id);
        return cancelled();
    }

    cJSON* prompt_params = cJSON_CreateObject();
    cJSON_AddStringToObject(prompt_params, "session_id", live_session_id.c_str());
    cJSON_AddStringToObject(prompt_params, "text", std::string(user_text).c_str());
    cJSON_AddStringToObject(prompt_params, "profile", config.hermes_profile.c_str());
    std::string prompt_response;
    if (!client.Rpc(2, "prompt.submit", prompt_params, &prompt_response, error, 30)) return false;
    if (!is_current()) {
        client.BestEffortInterrupt(live_session_id);
        return cancelled();
    }
    cJSON* prompt_root = nullptr; const cJSON* prompt_result = nullptr;
    if (!ParseRpcResult(prompt_response, &prompt_root, &prompt_result, error)) return false;
    const cJSON* status = cJSON_GetObjectItemCaseSensitive(prompt_result, "status");
    const bool streaming = cJSON_IsString(status) &&
        std::strcmp(status->valuestring, "streaming") == 0;
    cJSON_Delete(prompt_root);
    if (!streaming) {
        if (error != nullptr) *error = "Hermes 未接受消息";
        return false;
    }
    return client.WaitForTurn(live_session_id, &result->text, error, keep_waiting);
}

}  // namespace hermes_voice
