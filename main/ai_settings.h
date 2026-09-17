#pragma once

namespace ai_settings {

inline constexpr char kNamespace[] = "agent_ai";

// Removes obsolete provider credentials while preserving voice preferences.
// Safe to retry at startup; commits only when an old key was present.
bool MigrateLegacySettings();

}  // namespace ai_settings
