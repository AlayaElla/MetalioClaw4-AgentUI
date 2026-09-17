#pragma once

#include <cstddef>
#include <string_view>

namespace CommonSounds {

extern const char ogg_ratchet_detent_start[]
    asm("_binary_ratchet_detent_ogg_start");
extern const char ogg_ratchet_detent_end[]
    asm("_binary_ratchet_detent_ogg_end");

inline const std::string_view OGG_RATCHET_DETENT{
    ogg_ratchet_detent_start,
    static_cast<size_t>(ogg_ratchet_detent_end - ogg_ratchet_detent_start)};

extern const char ogg_codex_attention_start[]
    asm("_binary_codex_attention_ogg_start");
extern const char ogg_codex_attention_end[]
    asm("_binary_codex_attention_ogg_end");
extern const char ogg_codex_success_start[]
    asm("_binary_codex_success_ogg_start");
extern const char ogg_codex_success_end[]
    asm("_binary_codex_success_ogg_end");

inline const std::string_view OGG_CODEX_ATTENTION{
    ogg_codex_attention_start,
    static_cast<size_t>(ogg_codex_attention_end - ogg_codex_attention_start)};
inline const std::string_view OGG_CODEX_SUCCESS{
    ogg_codex_success_start,
    static_cast<size_t>(ogg_codex_success_end - ogg_codex_success_start)};

}  // namespace CommonSounds
