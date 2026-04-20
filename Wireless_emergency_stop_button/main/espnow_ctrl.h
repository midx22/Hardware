#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// ─── 消息类型 ────────────────────────────────────────────────────────────────
typedef enum {
    ESPNOW_MSG_PAIR_REQUEST = 0x01, // 接收端广播：进入配对模式，我的 MAC 是…
    ESPNOW_MSG_PAIR_ACK     = 0x02, // 遥控器回复：确认配对，我的 MAC 是…
    ESPNOW_MSG_CTRL         = 0x10, // 遥控器→接收端：控制命令
    ESPNOW_MSG_STATUS       = 0x11, // 接收端→遥控器：状态回报（保留）
} espnow_msg_type_t;

// ─── 通用消息帧（11 字节）───────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint8_t type;       // espnow_msg_type_t
    uint8_t mac[6];     // 发送方 MAC（AP 接口）
    uint8_t payload[4]; // CTRL: payload[0]=IO 电平; 其余保留
} espnow_msg_t;

// ─── IO 控制回调 ─────────────────────────────────────────────────────────────
// 收到合法 CTRL 帧后由模块调用，参数为目标 IO 电平（0/1）
typedef void (*espnow_ctrl_cb_t)(uint8_t io_state);

// ─── 公共 API ────────────────────────────────────────────────────────────────
// 必须在 WiFi AP 启动之后调用
esp_err_t espnow_ctrl_init(espnow_ctrl_cb_t cb);

// 启动配对任务（广播 PAIR_REQUEST，最长 30 s）
void espnow_start_pairing(void);

bool espnow_is_paired(void);
bool espnow_is_pairing(void);
