#include "espnow_ctrl.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "espnow";

#define NVS_NAMESPACE           "espnow_ctrl"
#define NVS_KEY_PEER_MAC        "peer_mac"

#define PAIRING_TIMEOUT_MS      30000
#define PAIRING_BEACON_MS       500

static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

static uint8_t          s_peer_mac[6] = {0};
static bool             s_paired      = false;
static volatile bool    s_pairing     = false;
static espnow_ctrl_cb_t s_ctrl_cb     = NULL;

// ─── NVS ─────────────────────────────────────────────────────────────────────

static void nvs_save_mac(const uint8_t *mac)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_PEER_MAC, mac, 6);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NVS 已保存配对 MAC %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
}

static bool nvs_load_mac(uint8_t *mac)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 6;
    bool ok = (nvs_get_blob(h, NVS_KEY_PEER_MAC, mac, &len) == ESP_OK && len == 6);
    nvs_close(h);
    return ok;
}

// ─── peer 管理 ───────────────────────────────────────────────────────────────

static void add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac)) return;
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;       // 跟随当前信道
    p.encrypt = false;
    esp_now_add_peer(&p);
}

// ─── ESP-NOW 接收回调 ────────────────────────────────────────────────────────

static void on_recv(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len)
{
    if (len < (int)sizeof(espnow_msg_t)) return;
    const espnow_msg_t *msg = (const espnow_msg_t *)data;

    if (s_pairing && msg->type == ESPNOW_MSG_PAIR_ACK) {
        // 遥控器回应配对确认，msg->mac 为遥控器 MAC
        ESP_LOGI(TAG, "收到 PAIR_ACK，遥控器 %02X:%02X:%02X:%02X:%02X:%02X",
                 msg->mac[0],msg->mac[1],msg->mac[2],msg->mac[3],msg->mac[4],msg->mac[5]);
        memcpy(s_peer_mac, msg->mac, 6);
        nvs_save_mac(s_peer_mac);
        add_peer(s_peer_mac);
        s_paired  = true;
        s_pairing = false;  // 通知配对任务退出循环
        return;
    }

    // 正常控制帧：只接受已配对遥控器
    if (!s_pairing && s_paired && msg->type == ESPNOW_MSG_CTRL) {
        if (memcmp(info->src_addr, s_peer_mac, 6) != 0) return; // 非配对来源
        if (s_ctrl_cb) s_ctrl_cb(msg->payload[0]);
    }
}

static void on_send(const wifi_tx_info_t *tx_info, esp_now_send_status_t st) { (void)tx_info; (void)st; }

// ─── 配对任务 ────────────────────────────────────────────────────────────────

static void pairing_task(void *arg)
{
    uint8_t my_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, my_mac);

    espnow_msg_t beacon = {
        .type    = ESPNOW_MSG_PAIR_REQUEST,
        .payload = {0},
    };
    memcpy(beacon.mac, my_mac, 6);

    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(PAIRING_TIMEOUT_MS);

    while (s_pairing && xTaskGetTickCount() < deadline) {
        esp_now_send(BROADCAST, (uint8_t *)&beacon, sizeof(beacon));
        ESP_LOGI(TAG, "广播配对请求 (%02X:%02X:%02X:%02X:%02X:%02X)...",
                 my_mac[0],my_mac[1],my_mac[2],my_mac[3],my_mac[4],my_mac[5]);
        vTaskDelay(pdMS_TO_TICKS(PAIRING_BEACON_MS));
    }

    if (!s_paired) {
        ESP_LOGW(TAG, "配对超时，退出配对模式");
        s_pairing = false;
    } else {
        ESP_LOGI(TAG, "配对成功！");
    }
    vTaskDelete(NULL);
}

// ─── 公共 API ────────────────────────────────────────────────────────────────

esp_err_t espnow_ctrl_init(espnow_ctrl_cb_t cb)
{
    s_ctrl_cb = cb;

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_send));

    // 广播地址始终加入 peer 列表（配对阶段需要发 beacon）
    add_peer(BROADCAST);

    // 从 NVS 恢复上次配对记录
    uint8_t zero[6] = {0};
    if (nvs_load_mac(s_peer_mac) && memcmp(s_peer_mac, zero, 6) != 0) {
        add_peer(s_peer_mac);
        s_paired = true;
        ESP_LOGI(TAG, "已恢复配对 MAC %02X:%02X:%02X:%02X:%02X:%02X",
                 s_peer_mac[0],s_peer_mac[1],s_peer_mac[2],
                 s_peer_mac[3],s_peer_mac[4],s_peer_mac[5]);
    } else {
        ESP_LOGI(TAG, "无历史配对记录，请长按 BOOT 键进入配对模式");
    }

    return ESP_OK;
}

void espnow_start_pairing(void)
{
    if (s_pairing) return;  // 已在配对中
    s_pairing = true;
    s_paired  = false;
    ESP_LOGI(TAG, "进入配对模式（%d s 超时）", PAIRING_TIMEOUT_MS / 1000);
    xTaskCreate(pairing_task, "espnow_pair", 3072, NULL, 4, NULL);
}

bool espnow_is_paired(void)  { return s_paired; }
bool espnow_is_pairing(void) { return s_pairing; }
