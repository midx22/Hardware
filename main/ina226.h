#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

// INA226 I2C 地址（A0=GND, A1=GND）
#define INA226_I2C_ADDR     0x40

// 寄存器地址
#define INA226_REG_CONFIG   0x00
#define INA226_REG_SHUNT    0x01  // 分流电压
#define INA226_REG_BUS      0x02  // 总线电压
#define INA226_REG_POWER    0x03  // 功率
#define INA226_REG_CURRENT  0x04  // 电流
#define INA226_REG_CALIB    0x05  // 校准
#define INA226_REG_MASK     0x06
#define INA226_REG_ALERT    0x07
#define INA226_REG_MFG_ID   0xFE
#define INA226_REG_DIE_ID   0xFF

// 校准参数：两个 1.5mΩ 合金电阻并联 = 0.75mΩ，24V 大电流场景
// 电流 LSB = 5mA，最大测量电流 ≈ 163A（INA226 分流端限制 ≈ 109A）
// CAL = 0.00512 / (0.005 * 0.00075) = 1365
#define INA226_SHUNT_RESISTOR_OHM   0.00075f
#define INA226_CURRENT_LSB_MA       5.0f    // 单位 mA
#define INA226_CALIB_VALUE          1365    // = 0.00512 / (0.005 * 0.00075)

typedef struct {
    i2c_master_dev_handle_t dev_handle;
} ina226_t;

esp_err_t ina226_init(i2c_master_bus_handle_t bus, ina226_t *dev);
esp_err_t ina226_read_bus_voltage(ina226_t *dev, float *voltage_v);
esp_err_t ina226_read_shunt_voltage(ina226_t *dev, float *voltage_mv);
esp_err_t ina226_read_current(ina226_t *dev, float *current_ma);
esp_err_t ina226_read_power(ina226_t *dev, float *power_mw);
