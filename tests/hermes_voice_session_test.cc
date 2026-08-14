#include <cassert>
#include <string>
#include <vector>

#include "hermes_voice_session.h"

// Candidate host-test entry point. This repository does not currently provide
// the cJSON/mbedTLS/ESP-audio host shims or a CMake target that invokes it.
void RunHermesVoiceSessionTests() {
    std::vector<int16_t> pcm(hermes_voice::kFrameSamples, 17);
    const std::string data_url = hermes_voice::BuildWavDataUrl(pcm);
    assert(!data_url.empty());
    hermes_voice::DataUrlResult encoded;
    assert(hermes_voice::ParseDataUrl(data_url, 4096, &encoded));
    hermes_voice::WavPcmResult decoded;
    assert(hermes_voice::ParsePcmWav(std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()), encoded.bytes.size()), &decoded));
    assert(decoded.samples == pcm);
    assert(hermes_voice::ParseDataUrl("data:audio/mpeg;base64,AA==", 16, &encoded));
    assert(encoded.mime_type == "audio/mpeg");
    assert(encoded.bytes.size() == 1 && encoded.bytes[0] == 0);
    assert(!hermes_voice::ParseDataUrl("data:audio/wav;base64,AAAA", 1, &encoded));
    std::string tts_url;
    assert(hermes_voice::ParseTtsDataUrlJson(R"("data:audio/mpeg;base64,AA==")", &tts_url));
    assert(hermes_voice::ParseTtsDataUrlJson(R"({"ok":true,"data_url":"data:audio/mpeg;base64,AA=="})", &tts_url));
    assert(!hermes_voice::ParseTtsDataUrlJson(R"({"ok":true})", &tts_url));

    std::string transcript;
    assert(hermes_voice::ParseSttTranscriptJson(
        R"({"ok":true,"transcript":"turn on the light","provider":"local"})",
        &transcript));
    assert(transcript == "turn on the light");
    assert(hermes_voice::ParseSttTranscriptJson(
        R"({"text":"legacy response"})", &transcript));
    assert(transcript == "legacy response");
    assert(hermes_voice::ParseSttTranscriptJson(
        R"("raw response")", &transcript));
    assert(transcript == "raw response");
    assert(hermes_voice::ParseSttTranscriptJson(
        R"({"ok":true,"transcript":""})", &transcript));
    assert(transcript.empty());
    assert(!hermes_voice::ParseSttTranscriptJson(
        R"({"ok":true,"provider":"local"})", &transcript));

    std::vector<std::string> profiles;
    assert(hermes_voice::ParseDashboardProfiles(
        R"({"profiles":["default",{"name":"xingmeng","is_default":true}]})",
        &profiles));
    assert(profiles.size() == 2 && profiles[1] == "xingmeng");
    std::string provider;
    assert(hermes_voice::ParsePasswordProvider(
        R"({"providers":[{"name":"oidc","supports_password":false},{"name":"password","supports_password":true}]})",
        &provider));
    assert(provider == "password");
    assert(!hermes_voice::ParsePasswordProvider(
        R"({"providers":[{"name":"one","supports_password":true},{"name":"two","supports_password":true}]})",
        &provider));
    std::string ticket;
    assert(hermes_voice::ParseWsTicket(R"({"ticket":"one-shot","ttl_seconds":30})", &ticket));
    assert(ticket == "one-shot");
    std::string cookie;
    assert(hermes_voice::BuildCookieHeader({
        "hermes_session_at=access; HttpOnly; Path=/",
        "hermes_session_rt=refresh; HttpOnly; Path=/"}, &cookie));
    assert(cookie == "hermes_session_at=access; hermes_session_rt=refresh");
    assert(hermes_voice::DashboardWebSocketUrl(
        "https://host:9119/", "a+b", "xingmeng") ==
        "wss://host:9119/api/ws?ticket=a%2Bb&profile=xingmeng");
}
