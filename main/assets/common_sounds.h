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

}  // namespace CommonSounds
