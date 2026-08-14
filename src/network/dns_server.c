#include "network/dns_server.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_HEADER_LEN 12
#define DNS_TYPE_A 1
#define DNS_TYPE_AAAA 28
#define DNS_CLASS_IN 1

static const char *TAG = "dns_server";

static uint16_t read_u16_be(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] << 8 | data[1]);
}

static bool skip_dns_name(const uint8_t *packet, size_t length, size_t *offset)
{
    size_t pos = *offset;
    size_t jumps = 0;

    while (pos < length) {
        uint8_t label = packet[pos];

        if (label == 0) {
            *offset = pos + 1;
            return true;
        }

        if ((label & 0xC0) == 0xC0) {
            if (pos + 1 >= length) {
                return false;
            }
            if (jumps++ > 4) {
                return false;
            }
            *offset = pos + 2;
            return true;
        }

        if ((label & 0xC0) != 0 || label > 63) {
            return false;
        }

        pos++;
        if (pos + label > length) {
            return false;
        }
        pos += label;
    }

    return false;
}

bool dns_server_parse_request(const uint8_t *packet, size_t length, dns_request_info_t *info)
{
    size_t offset = DNS_HEADER_LEN;
    uint16_t qdcount;

    if (info == NULL) {
        return false;
    }

    memset(info, 0, sizeof(*info));
    if (packet == NULL || length < DNS_HEADER_LEN) {
        return false;
    }

    qdcount = read_u16_be(&packet[4]);
    if (qdcount != 1) {
        return false;
    }

    if (!skip_dns_name(packet, length, &offset)) {
        return false;
    }

    if (offset + 4 > length) {
        return false;
    }

    info->valid = true;
    info->question_end = offset + 4;
    info->qdcount = qdcount;
    info->qtype = read_u16_be(&packet[offset]);
    info->qclass = read_u16_be(&packet[offset + 2]);
    return true;
}

static void dns_task(void *parameter)
{
    const uint32_t ap_ip = inet_addr("192.168.4.1");
    uint8_t buffer[512];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (sock < 0 || bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS server failed");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int len = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&client_addr, &client_len);
        dns_request_info_t request = {0};
        int answer;

        if (len <= 0 || !dns_server_parse_request(buffer, (size_t)len, &request)) {
            continue;
        }

        if (request.qclass != DNS_CLASS_IN) {
            continue;
        }

        buffer[2] = 0x81;
        buffer[3] = 0x80;
        buffer[6] = 0x00;
        buffer[7] = 0x01;
        buffer[8] = 0x00;
        buffer[9] = 0x00;
        buffer[10] = 0x00;
        buffer[11] = 0x00;

        answer = (int)request.question_end;
        if (answer + 16 > (int)sizeof(buffer)) {
            continue;
        }

        buffer[answer++] = 0xC0;
        buffer[answer++] = 0x0C;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x01;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x01;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x3C;
        buffer[answer++] = 0x00;
        buffer[answer++] = 0x04;
        memcpy(&buffer[answer], &ap_ip, 4);
        answer += 4;

        sendto(sock, buffer, answer, 0, (struct sockaddr *)&client_addr, client_len);
    }
}

esp_err_t dns_server_init(void)
{
    return xTaskCreate(dns_task, "dns_task", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}
