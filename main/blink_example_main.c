#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "ina226.h"
#include "dns_server.h"
#include "espnow_ctrl.h"

static const char *TAG = "main";

#define I2C_SCL_IO      GPIO_NUM_4
#define I2C_SDA_IO      GPIO_NUM_5
#define WIFI_AP_SSID    "StopButton1"
#define WIFI_AP_PASS    "12345678"
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_STA 4
#define CTRL_GPIO       GPIO_NUM_10
#define BOOT_GPIO       GPIO_NUM_9   // BOOT 键（低电平有效）
#define PAIR_HOLD_MS    1000         // 长按超过此时长触发配对

// 全局 INA226 数据（任务间共享）
static float g_bus_v = 0, g_current_ma = 0, g_power_mw = 0;
static ina226_t g_ina;
static int g_gpio10_state = 0;

// ─── ESP-NOW 控制回调（遥控器发来控制帧时调用）────────────────────────────
static void on_espnow_ctrl(uint8_t io_state)
{
    g_gpio10_state = io_state;
    gpio_set_level(CTRL_GPIO, io_state);
    ESP_LOGI(TAG, "ESP-NOW 控制 IO10 → %d", io_state);
}

// ─── BOOT 键监听任务（长按 1 s 进入配对模式）────────────────────────────────
static void boot_button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOOT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    bool     last_level  = true;     // 上一次采样电平（高 = 未按）
    uint32_t press_start = 0;        // 按下时的 tick（ms）

    while (1) {
        bool cur = gpio_get_level(BOOT_GPIO);

        if (!cur && last_level) {
            // 下降沿：记录按下时刻
            press_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
        } else if (!cur && !last_level) {
            // 持续按住：检查是否超过阈值
            uint32_t held = xTaskGetTickCount() * portTICK_PERIOD_MS - press_start;
            if (held >= PAIR_HOLD_MS && !espnow_is_pairing()) {
                ESP_LOGI(TAG, "BOOT 键长按 %"PRIu32" ms，触发配对", held);
                espnow_start_pairing();
                // 等待配对任务结束，避免重复触发
                while (espnow_is_pairing()) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
        }

        last_level = cur;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─── INA226 采集任务 ────────────────────────────────────────────────────────
static void ina226_task(void *arg)
{
    float shunt_mv;
    while (1) {
        ina226_read_bus_voltage(&g_ina, &g_bus_v);
        ina226_read_shunt_voltage(&g_ina, &shunt_mv);
        ina226_read_current(&g_ina, &g_current_ma);
        ina226_read_power(&g_ina, &g_power_mw);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ─── 网页 HTML ──────────────────────────────────────────────────────────────
static const char *HTML_PAGE =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>电源监测</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:'Segoe UI',sans-serif;background:#0d0d0d;color:#eee;min-height:100vh;padding:24px 16px}"
    "h1{text-align:center;font-size:20px;font-weight:600;letter-spacing:3px;color:#888;text-transform:uppercase;margin-bottom:28px}"
    ".grid{display:flex;flex-direction:column;gap:16px;max-width:400px;margin:0 auto}"
    ".card{border-radius:16px;padding:22px 24px;position:relative;overflow:hidden}"
    ".card::before{content:'';position:absolute;top:-30px;right:-30px;width:100px;height:100px;border-radius:50%;opacity:0.15}"
    ".card-v{background:#0a1628;border:1px solid #1a3a6e}"
    ".card-v::before{background:#4a9eff}"
    ".card-a{background:#0f1a0f;border:1px solid #1a5c1a}"
    ".card-a::before{background:#4aff7a}"
    ".card-w{background:#1a0f0a;border:1px solid #6e2a0a}"
    ".card-w::before{background:#ff7a4a}"
    ".card-label{font-size:11px;font-weight:700;letter-spacing:2px;text-transform:uppercase;margin-bottom:10px}"
    ".lv{color:#4a9eff}.la{color:#4aff7a}.lw{color:#ff7a4a}"
    ".card-row{display:flex;align-items:baseline;gap:8px}"
    ".card-value{font-size:52px;font-weight:700;line-height:1;font-variant-numeric:tabular-nums}"
    ".vv{color:#7dc4ff}.av{color:#7affa0}.wv{color:#ffaa7a}"
    ".card-unit{font-size:18px;font-weight:400;color:#666}"
    ".card-sub{font-size:12px;color:#555;margin-top:8px}"
    ".dot{display:inline-block;width:6px;height:6px;border-radius:50%;margin-right:6px;animation:pulse 2s infinite}"
    ".dot-v{background:#4a9eff}.dot-a{background:#4aff7a}.dot-w{background:#ff7a4a}"
    "@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.3}}"
    ".sw-card{background:#141414;border:1px solid #2a2a2a;border-radius:16px;padding:22px 24px;max-width:400px;margin:16px auto;display:flex;align-items:center;justify-content:space-between}"
    ".sw-label{font-size:14px;font-weight:600;color:#aaa;letter-spacing:1px}"
    ".sw-status{font-size:11px;margin-top:4px}"
    ".sw-on{color:#4aff7a}.sw-off{color:#555}"
    ".toggle{position:relative;width:64px;height:34px;flex-shrink:0}"
    ".toggle input{opacity:0;width:0;height:0}"
    ".slider{position:absolute;inset:0;background:#2a2a2a;border-radius:34px;cursor:pointer;transition:.3s}"
    ".slider:before{content:'';position:absolute;width:26px;height:26px;left:4px;bottom:4px;background:#555;border-radius:50%;transition:.3s}"
    "input:checked+.slider{background:#1a4a2a;box-shadow:0 0 12px #4aff7a55}"
    "input:checked+.slider:before{transform:translateX(30px);background:#4aff7a}"
    "</style></head><body>"
    "<h1>⚡ 电源监测</h1>"
    "<div class='grid'>"
    "<div class='card card-v'>"
    "<div class='card-label lv'><span class='dot dot-v'></span>总线电压</div>"
    "<div class='card-row'><span class='card-value vv' id='v'>--</span><span class='card-unit'>V</span></div>"
    "<div class='card-sub' id='v-sub'>&nbsp;</div></div>"
    "<div class='card card-a'>"
    "<div class='card-label la'><span class='dot dot-a'></span>电流</div>"
    "<div class='card-row'><span class='card-value av' id='a'>--</span><span class='card-unit'>A</span></div>"
    "<div class='card-sub' id='a-sub'>&nbsp;</div></div>"
    "<div class='card card-w'>"
    "<div class='card-label lw'><span class='dot dot-w'></span>功率</div>"
    "<div class='card-row'><span class='card-value wv' id='w'>--</span><span class='card-unit'>W</span></div>"
    "<div class='card-sub' id='w-sub'>&nbsp;</div></div>"
    "</div>"
    "<div class='sw-card'>"
    "<div><div class='sw-label'>IO10 控制</div>"
    "<div class='sw-status sw-off' id='sw-status'>已关闭</div></div>"
    "<label class='toggle'>"
    "<input type='checkbox' id='sw' onchange='toggle(this)'>"
    "<span class='slider'></span></label></div>"
    "<script>"
    "function toggle(el){"
    "fetch('/gpio?v='+(el.checked?1:0)).then(r=>r.json()).then(d=>{"
    "var s=document.getElementById('sw-status');"
    "if(d.state){s.innerText='已开启';s.className='sw-status sw-on';}else{s.innerText='已关闭';s.className='sw-status sw-off';}"
    "});}"
    "function update(){"
    "fetch('/data').then(r=>r.json()).then(d=>{"
    "var v=d.v,a=d.i/1000,w=d.p/1000;"
    "document.getElementById('v').innerText=v.toFixed(2);"
    "document.getElementById('a').innerText=a.toFixed(3);"
    "document.getElementById('w').innerText=w.toFixed(2);"
    "document.getElementById('v-sub').innerText=v>20?'正常范围':'⚠ 电压偏低';"
    "document.getElementById('a-sub').innerText='分流: '+(d.i).toFixed(0)+' mA';"
    "document.getElementById('w-sub').innerText='效率监测中';"
    "}).catch(()=>{});}"
    "update();setInterval(update,200);"
    "</script></body></html>";

// ─── HTTP 处理 ──────────────────────────────────────────────────────────────
static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_data(httpd_req_t *req)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"v\":%.3f,\"i\":%.2f,\"p\":%.2f}",
             g_bus_v, g_current_ma, g_power_mw);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_gpio(httpd_req_t *req)
{
    char query[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[4] = {0};
        if (httpd_query_key_value(query, "v", val, sizeof(val)) == ESP_OK) {
            g_gpio10_state = (val[0] == '1') ? 1 : 0;
            gpio_set_level(CTRL_GPIO, g_gpio10_state);
        }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "{\"state\":%d}", g_gpio10_state);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// Captive Portal 重定向（各系统检测 URL）
static esp_err_t handler_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static void start_webserver(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    httpd_uri_t uri_root     = { .uri = "/",                    .method = HTTP_GET, .handler = handler_root };
    httpd_uri_t uri_data     = { .uri = "/data",                .method = HTTP_GET, .handler = handler_data };
    httpd_uri_t uri_gpio     = { .uri = "/gpio",               .method = HTTP_GET, .handler = handler_gpio };
    // Android / iOS / Windows Captive Portal 检测
    httpd_uri_t uri_204      = { .uri = "/generate_204",        .method = HTTP_GET, .handler = handler_redirect };
    httpd_uri_t uri_hotspot  = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handler_redirect };
    httpd_uri_t uri_ncsi     = { .uri = "/ncsi.txt",            .method = HTTP_GET, .handler = handler_redirect };
    httpd_uri_t uri_redirect = { .uri = "/redirect",            .method = HTTP_GET, .handler = handler_redirect };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_data);
    httpd_register_uri_handler(server, &uri_gpio);
    httpd_register_uri_handler(server, &uri_204);
    httpd_register_uri_handler(server, &uri_hotspot);
    httpd_register_uri_handler(server, &uri_ncsi);
    httpd_register_uri_handler(server, &uri_redirect);
    ESP_LOGI(TAG, "HTTP 服务器已启动");
}

// ─── WiFi AP 初始化 ─────────────────────────────────────────────────────────
static void wifi_init_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .password       = WIFI_AP_PASS,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .max_connection = WIFI_AP_MAX_STA,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi AP 已启动  SSID: %s  密码: %s", WIFI_AP_SSID, WIFI_AP_PASS);
}

// ─── 主函数 ─────────────────────────────────────────────────────────────────
void app_main(void)
{
    // NVS（WiFi 驱动依赖）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // GPIO 10 初始化
    gpio_reset_pin(CTRL_GPIO);
    gpio_set_direction(CTRL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(CTRL_GPIO, 0);

    // I2C
    i2c_master_bus_config_t bus_cfg = {
        .clk_source             = I2C_CLK_SRC_DEFAULT,
        .i2c_port               = I2C_NUM_0,
        .scl_io_num             = I2C_SCL_IO,
        .sda_io_num             = I2C_SDA_IO,
        .glitch_ignore_cnt      = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    ESP_ERROR_CHECK(ina226_init(bus, &g_ina));

    // WiFi AP + DNS 劫持 + Web 服务器
    wifi_init_ap();
    dns_server_start();
    start_webserver();

    // INA226 采集任务
    xTaskCreate(ina226_task, "ina226", 2048, NULL, 5, NULL);

    // ESP-NOW 初始化（WiFi 启动后才能调用）
    ESP_ERROR_CHECK(espnow_ctrl_init(on_espnow_ctrl));

    // BOOT 键监听任务
    xTaskCreate(boot_button_task, "boot_btn", 2048, NULL, 3, NULL);

    ESP_LOGI(TAG, "就绪！手机连接 WiFi \"%s\" 后访问 http://192.168.4.1", WIFI_AP_SSID);
    ESP_LOGI(TAG, "长按 BOOT 键（IO9）1 秒进入 ESP-NOW 配对模式");
}
