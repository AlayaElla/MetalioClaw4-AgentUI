#include "metalio_tca9555.h"

#include <cstdlib>

#include <esp_log.h>

namespace {

constexpr char kTag[] = "MetalioTCA9555";
constexpr int kTimeoutMs = 100;
constexpr uint8_t kInputReg = 0x00;
constexpr uint8_t kOutputReg = 0x02;
constexpr uint8_t kDirectionReg = 0x06;
constexpr uint16_t kDirectionDefault = 0xffff;
constexpr uint16_t kOutputDefault = 0xffff;

struct Tca9555 {
    esp_io_expander_t base;
    i2c_master_dev_handle_t i2c_handle;
    uint16_t direction;
    uint16_t output;
};

Tca9555* FromHandle(esp_io_expander_handle_t handle) {
    return reinterpret_cast<Tca9555*>(handle);
}

esp_err_t ReadInput(esp_io_expander_handle_t handle, uint32_t* value) {
    Tca9555* tca = FromHandle(handle);
    uint8_t data[2] = {};
    const esp_err_t err = i2c_master_transmit_receive(
        tca->i2c_handle, &kInputReg, 1, data, sizeof(data), kTimeoutMs);
    if (err != ESP_OK) {
        ESP_LOGD(kTag, "read input failed: %s", esp_err_to_name(err));
        return err;
    }
    *value = static_cast<uint32_t>(data[0]) |
             (static_cast<uint32_t>(data[1]) << 8);
    return ESP_OK;
}

esp_err_t WriteOutput(esp_io_expander_handle_t handle, uint32_t value) {
    Tca9555* tca = FromHandle(handle);
    value &= 0xffff;
    const uint8_t data[] = {
        kOutputReg,
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
    };
    const esp_err_t err =
        i2c_master_transmit(tca->i2c_handle, data, sizeof(data), kTimeoutMs);
    if (err == ESP_OK) {
        tca->output = static_cast<uint16_t>(value);
    }
    return err;
}

esp_err_t ReadOutput(esp_io_expander_handle_t handle, uint32_t* value) {
    *value = FromHandle(handle)->output;
    return ESP_OK;
}

esp_err_t WriteDirection(esp_io_expander_handle_t handle, uint32_t value) {
    Tca9555* tca = FromHandle(handle);
    value &= 0xffff;
    const uint8_t data[] = {
        kDirectionReg,
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
    };
    const esp_err_t err =
        i2c_master_transmit(tca->i2c_handle, data, sizeof(data), kTimeoutMs);
    if (err == ESP_OK) {
        tca->direction = static_cast<uint16_t>(value);
    }
    return err;
}

esp_err_t ReadDirection(esp_io_expander_handle_t handle, uint32_t* value) {
    *value = FromHandle(handle)->direction;
    return ESP_OK;
}

esp_err_t Reset(esp_io_expander_t* handle) {
    esp_err_t err = WriteDirection(handle, kDirectionDefault);
    if (err != ESP_OK) {
        return err;
    }
    return WriteOutput(handle, kOutputDefault);
}

esp_err_t Delete(esp_io_expander_t* handle) {
    Tca9555* tca = FromHandle(handle);
    const esp_err_t err = i2c_master_bus_rm_device(tca->i2c_handle);
    if (err == ESP_OK) {
        std::free(tca);
    }
    return err;
}

}  // namespace

esp_err_t metalio_tca9555_new(i2c_master_bus_handle_t i2c_bus,
                              uint32_t dev_addr,
                              uint32_t scl_speed_hz,
                              esp_io_expander_handle_t* handle_ret) {
    if (i2c_bus == nullptr || handle_ret == nullptr || scl_speed_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    Tca9555* tca = static_cast<Tca9555*>(std::calloc(1, sizeof(Tca9555)));
    if (tca == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = static_cast<uint16_t>(dev_addr),
        .scl_speed_hz = scl_speed_hz,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    esp_err_t err =
        i2c_master_bus_add_device(i2c_bus, &config, &tca->i2c_handle);
    if (err != ESP_OK) {
        std::free(tca);
        return err;
    }

    tca->base.config.io_count = 16;
    tca->base.config.flags.dir_out_bit_zero = 1;
    tca->base.read_input_reg = ReadInput;
    tca->base.write_output_reg = WriteOutput;
    tca->base.read_output_reg = ReadOutput;
    tca->base.write_direction_reg = WriteDirection;
    tca->base.read_direction_reg = ReadDirection;
    tca->base.reset = Reset;
    tca->base.del = Delete;

    err = Reset(&tca->base);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(tca->i2c_handle);
        std::free(tca);
        return err;
    }

    *handle_ret = &tca->base;
    return ESP_OK;
}
