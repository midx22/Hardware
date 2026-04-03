#include "ina226.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "INA226";

static esp_err_t ina226_write_reg(ina226_t *dev, uint8_t reg, uint16_t value)
{
    uint8_t buf[3] = {reg, (value >> 8) & 0xFF, value & 0xFF};
    return i2c_master_transmit(dev->dev_handle, buf, sizeof(buf), 100);
}

static esp_err_t ina226_read_reg(ina226_t *dev, uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    esp_err_t ret = i2c_master_transmit_receive(dev->dev_handle, &reg, 1, data, 2, 100);
    if (ret == ESP_OK) {
        *value = ((uint16_t)data[0] << 8) | data[1];
    }
    return ret;
}

esp_err_t ina226_init(i2c_master_bus_handle_t bus, ina226_t *dev)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = INA226_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev->dev_handle),
                        TAG, "添加 I2C 设备失败");

    // 验证芯片 ID
    uint16_t mfg_id = 0;
    ESP_RETURN_ON_ERROR(ina226_read_reg(dev, INA226_REG_MFG_ID, &mfg_id),
                        TAG, "读取 MFG ID 失败");
    if (mfg_id != 0x5449) {
        ESP_LOGE(TAG, "MFG ID 不匹配: 0x%04X (期望 0x5449)", mfg_id);
        return ESP_ERR_NOT_FOUND;
    }

    // 写入校准寄存器
    ESP_RETURN_ON_ERROR(ina226_write_reg(dev, INA226_REG_CALIB, INA226_CALIB_VALUE),
                        TAG, "写入校准寄存器失败");

    // 配置：平均 16 次，总线 1.1ms，分流 1.1ms，连续测量
    ESP_RETURN_ON_ERROR(ina226_write_reg(dev, INA226_REG_CONFIG, 0x4527),
                        TAG, "写入配置寄存器失败");

    ESP_LOGI(TAG, "初始化成功，MFG ID: 0x%04X", mfg_id);
    return ESP_OK;
}

esp_err_t ina226_read_bus_voltage(ina226_t *dev, float *voltage_v)
{
    uint16_t raw;
    ESP_RETURN_ON_ERROR(ina226_read_reg(dev, INA226_REG_BUS, &raw), TAG, "读取总线电压失败");
    *voltage_v = raw * 1.25f / 1000.0f;  // LSB = 1.25mV
    return ESP_OK;
}

esp_err_t ina226_read_shunt_voltage(ina226_t *dev, float *voltage_mv)
{
    uint16_t raw;
    ESP_RETURN_ON_ERROR(ina226_read_reg(dev, INA226_REG_SHUNT, &raw), TAG, "读取分流电压失败");
    *voltage_mv = (int16_t)raw * 2.5f / 1000.0f;  // LSB = 2.5μV，转换为 mV
    return ESP_OK;
}

esp_err_t ina226_read_current(ina226_t *dev, float *current_ma)
{
    uint16_t raw;
    ESP_RETURN_ON_ERROR(ina226_read_reg(dev, INA226_REG_CURRENT, &raw), TAG, "读取电流失败");
    *current_ma = (int16_t)raw * INA226_CURRENT_LSB_MA;
    return ESP_OK;
}

esp_err_t ina226_read_power(ina226_t *dev, float *power_mw)
{
    uint16_t raw;
    ESP_RETURN_ON_ERROR(ina226_read_reg(dev, INA226_REG_POWER, &raw), TAG, "读取功率失败");
    *power_mw = raw * 25.0f * INA226_CURRENT_LSB_MA;
    return ESP_OK;
}
