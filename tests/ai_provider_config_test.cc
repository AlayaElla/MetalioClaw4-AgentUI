#include <cassert>

#include "ai_provider_config.h"

int main() {
    using namespace ai_provider_config;

    assert(IsValidHermesBaseUrl("http://192.168.1.2:9119"));
    assert(IsValidHermesBaseUrl("https://hermes.example.com/gateway"));
    assert(!IsValidHermesBaseUrl(""));
    assert(!IsValidHermesBaseUrl("ws://hermes.example.com"));
    assert(!IsValidHermesBaseUrl("https://user@example.com"));
    assert(!IsValidHermesBaseUrl("https://hermes.example.com/api?token=x"));
    assert(NormalizeHermesBaseUrl("https://hermes.example.com///") ==
           "https://hermes.example.com");
    assert(ResolveHermesBaseUrl("") == kDefaultHermesDashboardUrl);
    assert(ResolveHermesBaseUrl("http://host:9119/") == "http://host:9119");
    assert(HermesProfilesUrl("http://host:9119/") == "http://host:9119/api/profiles");

    assert(IsValidHermesProfile("default"));
    assert(IsValidHermesProfile("persona-01:cn"));
    assert(!IsValidHermesProfile(""));
    assert(!IsValidHermesProfile("profile name"));
    assert(!IsValidHermesProfile("../../escape"));
    assert(IsValidHermesUsername("alaya"));
    assert(!IsValidHermesUsername(""));
    assert(IsValidHermesPassword("a password with spaces"));
    assert(!IsValidHermesPassword("bad\npassword"));
    assert(UrlEncode("a:b c") == "a%3Ab%20c");
    assert(ParseProvider("hermes") == AiProvider::Hermes);
    assert(ParseProvider("anything-else") == AiProvider::Xiaozhi);
    return 0;
}
