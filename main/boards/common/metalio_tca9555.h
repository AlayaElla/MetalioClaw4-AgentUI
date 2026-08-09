#pragma once

#include <cstdint>

#include <driver/i2c_master.h>
#include <esp_io_expander.h>

esp_err_t metalio_tca9555_new(i2c_master_bus_handle_t i2c_bus,
                              uint32_t dev_addr,
                              uint32_t scl_speed_hz,
                              esp_io_expander_handle_t* handle_ret);
