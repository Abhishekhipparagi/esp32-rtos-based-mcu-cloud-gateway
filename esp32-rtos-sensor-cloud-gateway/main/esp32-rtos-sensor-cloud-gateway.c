/*
 * ESP32 WiFi Configuration Through WebPage + UART Sensor Gateway
 *
 * Data flow:
 *   UART RX --[queue]--> Parser --[mutex]--> Shared Data
 *                                                |
 *   HTTP Upload <--[semaphore]---[mutex]----------
 *
 *   UART RX --[task notify]--> Provision Monitor (AP(WiFi) + web config(Hotspot + WebPage for SSID and Pass Configuration))
 *
 * UART packet format:
 *   X  CMD  042035078012099  10110
 *   ^  ^^^  ^^^^^^^^^^^^^^^  ^^^^^
 *  [0] [1-3]    [4-18]      [19-23]
 *
 *   XWEB04203507801209910110  → start web server(for hosting webpage(SSID/PASS conifg)) + parse data
 *   XYYY04203507801209910110  → parse data only
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "lwip/err.h"
#include "lwip/sys.h"

/* --- Configuration --- */

#define UART_PORT          UART_NUM_1
#define UART_TX_PIN        17
#define UART_RX_PIN        16
#define UART_BAUD          38400
#define UART_BUF_SIZE      256

#define NVS_NAMESPACE      "wifi_cfg"
#define NVS_KEY_SSID       "ssid"
#define NVS_KEY_PASS       "password"

#define AP_SSID            "ESP32_SETUP"
#define AP_PASS            "admin123"

#define UPLOAD_URL_FMT                                    \
    "https://example.com/api/submit?id=DEVICE_001"        \
    "&data_a=%d&data_b=%d&data_c=%d&data_d=%d&data_e=%d"  \
    "&alert_a=%d&alert_b=%d&alert_c=%d&alert_d=%d&alert_e=%d"

#define HTTP_TIMEOUT_MS    10000
#define UART_QUEUE_DEPTH   5

#define WIFI_CONNECTED_BIT BIT0
#define AP_RUNNING_BIT     BIT1

static const char *TAG = "app";

/* --- Data types --- */

typedef struct {
    int data_a, data_b, data_c, data_d, data_e;
} sensor_data_t;

typedef struct {
    int alert_a, alert_b, alert_c, alert_d, alert_e;
} alert_data_t;

typedef struct {
    uint8_t payload[UART_BUF_SIZE];
    int     length;
} uart_packet_t;

/* --- FreeRTOS handles --- */

static SemaphoreHandle_t  g_data_mutex     = NULL;
static SemaphoreHandle_t  g_new_data_sem   = NULL;
static QueueHandle_t      g_uart_queue     = NULL;
static EventGroupHandle_t g_event_group    = NULL;
static TaskHandle_t       g_provision_task = NULL;

/* --- Shared state (access under g_data_mutex only) --- */

static sensor_data_t g_sensor = {0};
static alert_data_t  g_alerts = {0};

static httpd_handle_t           g_web_server  = NULL;
static esp_http_client_handle_t g_http_client = NULL;
static esp_netif_t             *g_sta_netif   = NULL;
static esp_netif_t             *g_ap_netif    = NULL;

/* --- URL / form utilities --- */

static void url_decode(char *out, const char *in)
{
    char a, b;
    while (*in) {
        if (*in == '%' && isxdigit((unsigned char)in[1]) &&
            isxdigit((unsigned char)in[2])) {
            a = in[1]; b = in[2];
            if (a >= 'a') a -= ('a' - 'A');
            a = (a >= 'A') ? (a - 'A' + 10) : (a - '0');
            if (b >= 'a') b -= ('a' - 'A');
            b = (b >= 'A') ? (b - 'A' + 10) : (b - '0');
            *out++ = (char)(16 * a + b);
            in += 3;
        } else if (*in == '+') {
            *out++ = ' '; in++;
        } else {
            *out++ = *in++;
        }
    }
    *out = '\0';
}

static void form_parse(const char *body,
                       char *ssid, size_t ssid_sz,
                       char *pass, size_t pass_sz)
{
    memset(ssid, 0, ssid_sz);
    memset(pass, 0, pass_sz);
    const char *cur = body;

    while (cur && *cur) {
        const char *eq  = strchr(cur, '=');
        if (!eq) break;
        const char *amp = strchr(eq + 1, '&');
        size_t klen = eq - cur;
        size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);

        char key[64], val[128], dec[128];
        if (klen < sizeof(key) && vlen < sizeof(val)) {
            memcpy(key, cur, klen);    key[klen] = '\0';
            memcpy(val, eq + 1, vlen); val[vlen] = '\0';
            url_decode(dec, val);
            if (strcmp(key, "ssid") == 0)
                strncpy(ssid, dec, ssid_sz - 1);
            else if (strcmp(key, "pass") == 0)
                strncpy(pass, dec, pass_sz - 1);
        }
        if (!amp) break;
        cur = amp + 1;
    }
}

/* --- NVS helpers --- */

static void nvs_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
}

static esp_err_t nvs_save_wifi(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (e == ESP_OK) e = nvs_set_str(h, NVS_KEY_PASS, pass);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

static bool nvs_load_wifi(char *ssid, size_t ssid_sz,
                          char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = 0;
    if (nvs_get_str(h, NVS_KEY_SSID, NULL, &len) != ESP_OK ||
        len == 0 || len > ssid_sz) {
        nvs_close(h);
        return false;
    }
    nvs_get_str(h, NVS_KEY_SSID, ssid, &len);

    len = 0;
    if (nvs_get_str(h, NVS_KEY_PASS, NULL, &len) != ESP_OK || len > pass_sz)
        pass[0] = '\0';
    else
        nvs_get_str(h, NVS_KEY_PASS, pass, &len);

    nvs_close(h);
    return true;
}

/* --- WiFi STA --- */

static void wifi_event_cb(void *ctx, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected - reconnecting");
        xEventGroupClearBits(g_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(g_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_sta_connect(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Connecting to '%s'", ssid);

    if (!g_sta_netif)
        g_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&icfg);

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(g_event_group, WIFI_CONNECTED_BIT,
                                           false, true, pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) ESP_LOGI(TAG, "WiFi connected");
    else                           ESP_LOGW(TAG, "WiFi timeout");
}

/* --- Provisioning (AP + web server) --- */

static const char *HTML_PAGE =
    "<!DOCTYPE html><html><head><title>Setup</title>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>"
    "body{font-family:sans-serif;max-width:420px;margin:40px auto;"
    "padding:16px;background:#f5f5f5}"
    ".box{background:#fff;padding:24px;border-radius:8px;"
    "box-shadow:0 2px 8px rgba(0,0,0,.1)}"
    "h1{text-align:center;color:#333;font-size:1.4em}"
    "input{width:100%;padding:10px;margin:6px 0;border:1px solid #ddd;"
    "border-radius:4px;box-sizing:border-box}"
    "button{width:100%;padding:12px;background:#4CAF50;color:#fff;"
    "border:none;border-radius:4px;font-size:15px;cursor:pointer}"
    "button:hover{background:#43a047}"
    "</style></head><body><div class='box'>"
    "<h1>Device WiFi Setup</h1>"
    "<form method='POST' action='/save'>"
    "<label>SSID:</label><input name='ssid' maxlength='32' required>"
    "<label>Password:</label><input type='password' name='pass' maxlength='64'>"
    "<button>Connect</button></form></div></body></html>";

static esp_err_t http_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, HTML_PAGE, HTTPD_RESP_USE_STRLEN);
}

static void deferred_connect_task(void *arg)
{
    char *blk  = (char *)arg;
    char *ssid = blk;
    char *pass = ssid + strlen(ssid) + 1;

    vTaskDelay(pdMS_TO_TICKS(1000));

    if (g_web_server) { httpd_stop(g_web_server); g_web_server = NULL; }
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);
    xEventGroupClearBits(g_event_group, AP_RUNNING_BIT);

    wifi_sta_connect(ssid, pass);
    free(blk);
    vTaskDelete(NULL);
}

static esp_err_t http_post_handler(httpd_req_t *req)
{
    int len = req->content_len;
    if (len <= 0 || len > 512) { httpd_resp_send_500(req); return ESP_FAIL; }

    char *body = malloc(len + 1);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }

    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[len] = '\0';

    char ssid[33], pass[65];
    form_parse(body, ssid, sizeof(ssid), pass, sizeof(pass));
    free(body);

    if (nvs_save_wifi(ssid, pass) != ESP_OK) {
        httpd_resp_sendstr(req, "Save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, "<h2>Saved - connecting...</h2>", HTTPD_RESP_USE_STRLEN);

    size_t sz = strlen(ssid) + 1 + strlen(pass) + 1;
    char *blk = malloc(sz);
    if (blk) {
        strcpy(blk, ssid);
        strcpy(blk + strlen(ssid) + 1, pass);
        xTaskCreate(deferred_connect_task, "def_conn", 4096, blk, 5, NULL);
    }
    return ESP_OK;
}

static const httpd_uri_t uri_root = {"/",     HTTP_GET,  http_get_handler,  NULL};
static const httpd_uri_t uri_save = {"/save", HTTP_POST, http_post_handler, NULL};

static void provision_start(void)
{
    ESP_LOGI(TAG, "Starting AP provisioning");
    esp_wifi_stop();

    if (!g_ap_netif) g_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t icfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&icfg);

    wifi_config_t ap = {
        .ap = {.channel = 1, .max_connection = 4,
               .authmode = WIFI_AUTH_WPA_WPA2_PSK}
    };
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(AP_SSID);
    strncpy((char *)ap.ap.password, AP_PASS, sizeof(ap.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.core_id = tskNO_AFFINITY;
    if (httpd_start(&g_web_server, &hcfg) == ESP_OK) {
        httpd_register_uri_handler(g_web_server, &uri_root);
        httpd_register_uri_handler(g_web_server, &uri_save);
    }
    ESP_LOGI(TAG, "AP '%s' up — http://192.168.4.1", AP_SSID);
    xEventGroupSetBits(g_event_group, AP_RUNNING_BIT);
}

static void provision_monitor_task(void *arg)
{
    uint32_t value;
    while (1) {
        if (xTaskNotifyWait(0, ULONG_MAX, &value, portMAX_DELAY) == pdTRUE) {
            if (xEventGroupGetBits(g_event_group) & AP_RUNNING_BIT)
                ESP_LOGI(TAG, "Already provisioning");
            else
                provision_start();
        }
    }
}

/* --- UART --- */

static void uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT, UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT, &cfg);
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/*
 * Packet format:
 *   X  WEB  042035078012099  10110    → start web server + parse data
 *   X  YYY  042035078012099  10110    → data only
 *   ^  ^^^  ^^^^^^^^^^^^^^^  ^^^^^
 *  [0] [1-3]    [4-18]      [19-23]
 */
static void uart_rx_task(void *arg)
{
    uart_packet_t pkt;

    while (1) {
        int n = uart_read_bytes(UART_PORT, pkt.payload,
                                UART_BUF_SIZE - 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;

        pkt.payload[n] = '\0';
        pkt.length = n;

        if (pkt.payload[0] != 'X') {
            ESP_LOGW(TAG, "Bad header — discarding %d bytes", n);
            continue;
        }

        /* "WEB" at [1-3] triggers provisioning */
        if (n >= 4 && pkt.payload[1] == 'W' &&
            pkt.payload[2] == 'E' && pkt.payload[3] == 'B') {
            if (g_provision_task)
                xTaskNotifyGive(g_provision_task);
            ESP_LOGI(TAG, "WEB command — triggering provisioning");
        }

        /* All valid X packets carry data — send to parser */
        if (xQueueSend(g_uart_queue, &pkt, 0) != pdTRUE)
            ESP_LOGW(TAG, "UART queue full — packet dropped");
    }
    vTaskDelete(NULL);
}

/* --- Parser task ---
 *
 * Data starts at [4] because [0]='X' and [1-3]=command:
 *   [4-6]   data_a
 *   [7-9]   data_b
 *   [10-12] data_c
 *   [13-15] data_d
 *   [16-18] data_e
 *   [19-23] alert_a..e
 *   Minimum: 24 bytes
 */

static inline int parse_3digit(const uint8_t *p)
{
    return (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
}

static void parser_task(void *arg)
{
    uart_packet_t pkt;

    while (1) {
        if (xQueueReceive(g_uart_queue, &pkt, portMAX_DELAY) != pdTRUE)
            continue;

        if (pkt.length < 24) {
            ESP_LOGW(TAG, "Packet too short (%d bytes)", pkt.length);
            continue;
        }

        const uint8_t *d = pkt.payload;

        sensor_data_t new_s = {
            .data_a = parse_3digit(&d[4]),
            .data_b = parse_3digit(&d[7]),
            .data_c = parse_3digit(&d[10]),
            .data_d = parse_3digit(&d[13]),
            .data_e = parse_3digit(&d[16]),
        };

        alert_data_t new_a = {
            .alert_a = (d[19] == '1'),
            .alert_b = (d[20] == '1'),
            .alert_c = (d[21] == '1'),
            .alert_d = (d[22] == '1'),
            .alert_e = (d[23] == '1'),
        };

        if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_sensor = new_s;
            g_alerts = new_a;
            xSemaphoreGive(g_data_mutex);
            xSemaphoreGive(g_new_data_sem);

            ESP_LOGI(TAG, "data: %d %d %d %d %d | alerts: %d %d %d %d %d",
                     new_s.data_a, new_s.data_b, new_s.data_c,
                     new_s.data_d, new_s.data_e,
                     new_a.alert_a, new_a.alert_b, new_a.alert_c,
                     new_a.alert_d, new_a.alert_e);
        } else {
            ESP_LOGW(TAG, "Mutex timeout — data dropped");
        }
    }
    vTaskDelete(NULL);
}

/* --- HTTP upload task --- */

static void http_upload_task(void *arg)
{
    xEventGroupWaitBits(g_event_group, WIFI_CONNECTED_BIT,
                        false, true, portMAX_DELAY);

    char url[512];

    while (1) {
        if (xSemaphoreTake(g_new_data_sem, pdMS_TO_TICKS(5000)) != pdTRUE)
            continue;

        if (!(xEventGroupGetBits(g_event_group) & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG, "WiFi down — waiting");
            xEventGroupWaitBits(g_event_group, WIFI_CONNECTED_BIT,
                                false, true, portMAX_DELAY);
            if (g_http_client) {
                esp_http_client_cleanup(g_http_client);
                g_http_client = NULL;
            }
            continue;
        }

        sensor_data_t snap_s;
        alert_data_t  snap_a;

        if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            snap_s = g_sensor;
            snap_a = g_alerts;
            xSemaphoreGive(g_data_mutex);
        } else {
            ESP_LOGW(TAG, "Mutex timeout in HTTP task");
            continue;
        }

        snprintf(url, sizeof(url), UPLOAD_URL_FMT,
                 snap_s.data_a, snap_s.data_b, snap_s.data_c,
                 snap_s.data_d, snap_s.data_e,
                 snap_a.alert_a, snap_a.alert_b, snap_a.alert_c,
                 snap_a.alert_d, snap_a.alert_e);

        if (!g_http_client) {
            esp_http_client_config_t cfg = {
                .url               = url,
                .transport_type    = HTTP_TRANSPORT_OVER_SSL,
                .timeout_ms        = HTTP_TIMEOUT_MS,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .keep_alive_enable = true,
            };
            g_http_client = esp_http_client_init(&cfg);
            if (!g_http_client) {
                ESP_LOGE(TAG, "HTTP client init failed");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            esp_http_client_set_header(g_http_client, "Connection", "keep-alive");
        }

        esp_http_client_set_url(g_http_client, url);
        esp_http_client_set_method(g_http_client, HTTP_METHOD_GET);

        esp_err_t err = esp_http_client_perform(g_http_client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTPS %d",
                     esp_http_client_get_status_code(g_http_client));
            char buf[256];
            while (esp_http_client_read(g_http_client, buf, sizeof(buf) - 1) > 0)
                ;
        } else {
            ESP_LOGE(TAG, "HTTP failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(g_http_client);
            g_http_client = NULL;
        }
    }
    vTaskDelete(NULL);
}

/* --- Entry point --- */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Application starting ===");

    nvs_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_data_mutex   = xSemaphoreCreateMutex();
    g_new_data_sem = xSemaphoreCreateBinary();
    g_uart_queue   = xQueueCreate(UART_QUEUE_DEPTH, sizeof(uart_packet_t));
    g_event_group  = xEventGroupCreate();
    configASSERT(g_data_mutex && g_new_data_sem && g_uart_queue && g_event_group);

    xTaskCreate(provision_monitor_task, "prov_mon", 4096, NULL, 5,
                &g_provision_task);
    xTaskCreate(parser_task, "parser", 4096, NULL, 6, NULL);

    char ssid[33], pass[65];
    if (nvs_load_wifi(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Stored WiFi: '%s'", ssid);
        wifi_sta_connect(ssid, pass);
    } else {
        ESP_LOGI(TAG, "No credentials — send XWEB to configure");
    }

    uart_init();
    xTaskCreate(uart_rx_task,     "uart_rx",     4096,  NULL, 7, NULL);
    xTaskCreate(http_upload_task, "http_upload",  12288, NULL, 5, NULL);

    ESP_LOGI(TAG, "All tasks running");
}