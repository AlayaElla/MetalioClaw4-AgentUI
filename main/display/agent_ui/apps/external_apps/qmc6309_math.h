#pragma once

#include <cstdint>

namespace qmc6309 {

constexpr uint8_t kAddress = 0x7C;
constexpr uint8_t kChipIdRegister = 0x00;
constexpr uint8_t kChipId = 0x90;
constexpr uint8_t kOutputStartRegister = 0x01;
constexpr uint8_t kStatusRegister = 0x09;
constexpr uint8_t kControl1Register = 0x0A;
constexpr uint8_t kControl2Register = 0x0B;
constexpr uint8_t kStatusDataReady = 1U << 0;
constexpr uint8_t kStatusOverflow = 1U << 1;

// QST QMC6309 Rev C: 0x40 selects 200 Hz, +/-32 G and set/reset on.
constexpr uint8_t kControl2Odr200HzRange32G = 0x40;
// OSR2=8, OSR1=8, normal mode. Low two bits select mode.
constexpr uint8_t kControl1Base = 0x60;
constexpr uint8_t kModeSuspend = 0x00;
constexpr uint8_t kModeNormal = 0x01;
constexpr uint8_t kControl2SoftReset = 0x80;
constexpr int32_t kNanoteslaPerLsb = 100;

constexpr bool DataReady(uint8_t status) {
    return (status & kStatusDataReady) != 0;
}

constexpr bool Overflow(uint8_t status) {
    return (status & kStatusOverflow) != 0;
}

constexpr int16_t DecodeAxis(uint8_t low, uint8_t high) {
    const uint16_t bits = static_cast<uint16_t>(low) |
                          (static_cast<uint16_t>(high) << 8);
    const int32_t signed_value =
        bits < 0x8000U ? static_cast<int32_t>(bits)
                       : static_cast<int32_t>(bits) - 0x10000;
    return static_cast<int16_t>(signed_value);
}

constexpr int32_t ToNanotesla(int16_t raw) {
    return static_cast<int32_t>(raw) * kNanoteslaPerLsb;
}

}  // namespace qmc6309
