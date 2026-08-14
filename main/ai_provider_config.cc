#include "ai_provider_config.h"

#include "settings.h"

namespace ai_provider_config {
namespace {

void WriteHermesDraft(Settings& settings, const AiProviderConfig& config) {
    settings.SetString(std::string(kHermesDashboardUrlKey),
                       NormalizeHermesBaseUrl(config.hermes_dashboard_url));
    settings.SetString(std::string(kHermesUsernameKey), config.hermes_username);
    settings.SetString(std::string(kHermesPasswordKey), config.hermes_password);
    settings.SetString(std::string(kHermesProfileKey), config.hermes_profile);
}

}  // namespace

const char* ToString(AiProvider provider) {
    return provider == AiProvider::Hermes ? "hermes" : "xiaozhi";
}

AiProvider ParseProvider(std::string_view provider) {
    return provider == "hermes" ? AiProvider::Hermes : AiProvider::Xiaozhi;
}

AiProviderConfig Load() {
    Settings settings(std::string(kNamespace), false);
    AiProviderConfig config;
    config.provider = ParseProvider(settings.GetString(std::string(kProviderKey), "xiaozhi"));
    config.hermes_dashboard_url = ResolveHermesBaseUrl(settings.GetString(
        std::string(kHermesDashboardUrlKey), std::string(kDefaultHermesDashboardUrl)));
    config.hermes_username = settings.GetString(std::string(kHermesUsernameKey));
    config.hermes_password = settings.GetString(std::string(kHermesPasswordKey));
    config.hermes_profile = settings.GetString(std::string(kHermesProfileKey), "default");
    return config;
}

void SaveHermesDraft(const AiProviderConfig& config) {
    Settings settings(std::string(kNamespace), true);
    WriteHermesDraft(settings, config);
}

bool Save(const AiProviderConfig& config) {
    if (config.provider == AiProvider::Hermes && !IsCompleteHermesConfig(config)) return false;
    Settings settings(std::string(kNamespace), true);
    settings.SetString(std::string(kProviderKey), ToString(config.provider));
    WriteHermesDraft(settings, config);
    return true;
}

}  // namespace ai_provider_config
