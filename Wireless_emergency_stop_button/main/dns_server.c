#include "dns_server.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#define DNS_PORT    53
#define DNS_BUF_LEN 512

static const char *TAG = "dns";

// 把所有 DNS 查询都回应为 192.168.4.1
static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t buf[DNS_BUF_LEN];
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&src, &src_len);
        if (len < 12) continue;

        // 构造 DNS 响应：将所有查询解析到 192.168.4.1
        uint8_t resp[DNS_BUF_LEN];
        memcpy(resp, buf, len);

        resp[2] = 0x81; // QR=1 响应, Opcode=0, AA=1
        resp[3] = 0x80; // RA=1
        resp[6] = 0x00; // ANCOUNT 高位
        resp[7] = 0x01; // ANCOUNT = 1

        // 追加 Answer 段
        int pos = len;
        resp[pos++] = 0xC0; // 指针压缩，指向 Question 的域名
        resp[pos++] = 0x0C;
        resp[pos++] = 0x00; resp[pos++] = 0x01; // TYPE A
        resp[pos++] = 0x00; resp[pos++] = 0x01; // CLASS IN
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x3C; // TTL = 60s
        resp[pos++] = 0x00; resp[pos++] = 0x04; // RDLENGTH = 4
        resp[pos++] = 192;
        resp[pos++] = 168;
        resp[pos++] = 4;
        resp[pos++] = 1;

        sendto(sock, resp, pos, 0, (struct sockaddr *)&src, src_len);
    }
    close(sock);
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(void)
{
    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "DNS 劫持服务已启动");
    return ESP_OK;
}
