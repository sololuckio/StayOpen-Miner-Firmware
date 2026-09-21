#include <pthread.h>
#include <fcntl.h>
#include <string.h>
#include <sys/param.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_vfs.h"

#include "dns_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "cJSON.h"
#include "global_state.h"
#include "history.h"
#include "nvs_config.h"
#include "dual_clamp.h"
#include "api_auth.h"
//#include "vcore.h"
#include "connect.h"
#include "asic.h"
#include "gpio_input_output.h"
#include "eeprom.h"
//#include "TPS546.h"
#include "theme_api.h"
#include "http_server.h"
#include "protocol_task.h"

#include "api_helper.h"
#include "network.h"
#include "ip_reporter.h"
#include "system.h"

#include "mbedtls/sha256.h"

#include "esp_heap_caps.h"
#include <time.h>
#include <sys/time.h>
#include "vcore.h"
#include "device.h"
#include "lvgl_porting.h"
#include "displays/lvgl_screen.h"
#include "main.h"

static const char * TAG = "http_server";
static const char * CORS_TAG = "CORS";

static GlobalState * GLOBAL_STATE;
static httpd_handle_t server = NULL;
QueueHandle_t log_queue = NULL;
static esp_err_t set_cors_headers(httpd_req_t * req);
static esp_err_t is_network_allowed(httpd_req_t * req);


#define HTTPD_NEW_LOG

#ifdef HTTPD_NEW_LOG
// --- 日志环形缓冲区定义 ---
#define LOG_BUFFER_SIZE (256 * 1024) // 64KB
static char *g_log_buffer = NULL;
static size_t g_log_head = 0;
static bool g_log_wrapped = false;
static SemaphoreHandle_t g_log_mutex = NULL;

// 1. 写入日志到缓冲区 (内部函数)
static void write_to_log_buffer(const char *data, size_t len) {
    if (!g_log_buffer || !g_log_mutex) return;
    // 使用 portMAX_DELAY 确保写入时不丢数据，但要注意死锁风险(日志中不要再打日志)
    if (xSemaphoreTake(g_log_mutex, portMAX_DELAY) == pdTRUE) {
        for (size_t i = 0; i < len; i++) {
            g_log_buffer[g_log_head++] = data[i];
            if (g_log_head >= LOG_BUFFER_SIZE) {
                g_log_head = 0;
                g_log_wrapped = true;
            }
        }
        xSemaphoreGive(g_log_mutex);
    }
}

// 2. 自定义日志处理函数 (核心)
int log_to_queue(const char * format, va_list args)
{
    // 格式化原始消息
    va_list args_copy;
    va_copy(args_copy, args);
    int raw_len = vsnprintf(NULL, 0, format, args_copy) + 1;
    va_end(args_copy);

    char * raw_buffer = (char *) malloc(raw_len + 1);
    if (!raw_buffer) return 0;

    va_copy(args_copy, args);
    vsnprintf(raw_buffer, raw_len, format, args_copy);
    va_end(args_copy);

    printf("%s", raw_buffer);

    // 获取时间戳
    char time_str[32] = {0};
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm timeinfo;
    localtime_r(&tv.tv_sec, &timeinfo);
    
    if (timeinfo.tm_year > (2020 - 1900)) {
        strftime(time_str, sizeof(time_str), "[%Y-%m-%d %H:%M:%S] ", &timeinfo);
    }

    // 拼接: 时间 + 内容
    size_t final_len = strlen(time_str) + strlen(raw_buffer) + 2; 
    char * final_buffer = (char *) malloc(final_len);
    if (!final_buffer) {
        free(raw_buffer);
        return 0;
    }

    strcpy(final_buffer, time_str);
    strcat(final_buffer, raw_buffer);
    
    // 确保换行符
    size_t cur_len = strlen(final_buffer);
    if (cur_len > 0 && final_buffer[cur_len - 1] != '\n') {
        final_buffer[cur_len] = '\n';
        final_buffer[cur_len + 1] = '\0';
    }
    free(raw_buffer);

    // A. 写入环形缓冲区 (历史记录)
    write_to_log_buffer(final_buffer, strlen(final_buffer));

    // B. 打印到串口 (调试用)
    // printf("%s", final_buffer);

    // C. 发送到 WebSocket 队列 (实时推送)
    if (log_queue) {
        if (xQueueSendToBack(log_queue, (void*)&final_buffer, (TickType_t) 0) != pdPASS) {
            free(final_buffer); // 队列满则丢弃
        }
    } else {
        free(final_buffer); // 队列未就绪则丢弃
    }

    return 0;
}

// 3. 初始化系统 (调整位置到 log_to_queue 之后，无需 extern)
void init_logging_system(void) {
    if (!g_log_buffer) {
        g_log_buffer = heap_caps_calloc(1, LOG_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!g_log_buffer) {
            g_log_buffer = calloc(1, LOG_BUFFER_SIZE); 
        }
    }
    if (!g_log_mutex) {
        g_log_mutex = xSemaphoreCreateMutex();
    }
    
    // 立即接管系统日志
    esp_log_set_vprintf(log_to_queue);
}

// 4. 辅助函数：获取当前日志快照
// 返回一个新的缓冲区，包含当前按顺序排列的所有日志。调用者负责 free。
// 解决重复代码问题，并确保持锁时间极短。
static char* get_log_snapshot(size_t *out_len) {
    if (!g_log_buffer || !g_log_mutex) return NULL;

    char *snapshot = heap_caps_calloc(1, LOG_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!snapshot) snapshot = malloc(LOG_BUFFER_SIZE); // Fallback
    if (!snapshot) return NULL;

    if (xSemaphoreTake(g_log_mutex, portMAX_DELAY) == pdTRUE) {
        if (g_log_wrapped) {
            // 已回绕：[Head...End] + [Start...Head]
            size_t first_part = LOG_BUFFER_SIZE - g_log_head;
            memcpy(snapshot, g_log_buffer + g_log_head, first_part);
            memcpy(snapshot + first_part, g_log_buffer, g_log_head);
            *out_len = LOG_BUFFER_SIZE;
        } else {
            // 未回绕：[Start...Head]
            memcpy(snapshot, g_log_buffer, g_log_head);
            *out_len = g_log_head;
        }
        xSemaphoreGive(g_log_mutex);
    } else {
        free(snapshot);
        return NULL;
    }
    return snapshot;
}

// 5. WebSocket 历史日志发送 (使用快照)
static void send_log_history_to_ws(httpd_req_t *req) {
    size_t data_len = 0;
    char *temp_buf = get_log_snapshot(&data_len); // 复用逻辑

    if (temp_buf) {
        size_t sent = 0;
        size_t chunk_size = 4096; 
        while (sent < data_len) {
            size_t this_chunk = (data_len - sent) > chunk_size ? chunk_size : (data_len - sent);
            httpd_ws_frame_t ws_pkt;
            memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
            ws_pkt.payload = (uint8_t *)(temp_buf + sent);
            ws_pkt.len = this_chunk;
            ws_pkt.type = HTTPD_WS_TYPE_TEXT;
            
            httpd_ws_send_frame(req, &ws_pkt);
            sent += this_chunk;
        }
        free(temp_buf);
    }
}


// 6. 文件下载 (使用快照，安全无锁发送)
static esp_err_t GET_log_download(httpd_req_t *req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"system_log.txt\"");

    // 使用快照模式：先复制，后发送。
    // 虽然多占用一份内存，但避免了在网络发送期间持有 g_log_mutex，防止卡死系统日志。
    size_t total_len = 0;
    char *snapshot = get_log_snapshot(&total_len);

    if (snapshot) {
        size_t sent = 0;
        size_t chunk_size = 4096;
        while (sent < total_len) {
            size_t this_chunk = (total_len - sent) > chunk_size ? chunk_size : (total_len - sent);
            if (httpd_resp_send_chunk(req, snapshot + sent, this_chunk) != ESP_OK) {
                free(snapshot);
                return ESP_FAIL;
            }
            sent += this_chunk;
        }
        free(snapshot);
    } else {
        httpd_resp_send_chunk(req, "Log buffer empty or not initialized.\n", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#endif

static char WWWVersion[32];

/* Handler for WiFi scan endpoint */
static esp_err_t GET_wifi_scan(httpd_req_t *req)
{
    /*
     * This had neither gate, alone among the endpoints that do something.
     *
     * Unauthenticated, it handed anyone on the network the list of nearby
     * SSIDs -- a map of the owner's RF neighbourhood, and by extension a
     * decent guess at where the miner is. Being a plain GET with no custom
     * header, a web page open in the owner's browser could also fire it
     * cross-origin: the reply is not readable there, but the scan still runs
     * on the radio the miner needs for its pool connection.
     */
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    httpd_resp_set_type(req, "application/json");
    
    // Give some time for the connected flag to take effect
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Set CORS headers for OPTIONS request
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    wifi_ap_record_simple_t ap_records[20];
    uint16_t ap_count = 0;

    esp_err_t err = wifi_scan(ap_records, &ap_count, 20);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi scan failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    for (int i = 0; i < ap_count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi);
        cJSON_AddNumberToObject(network, "authmode", ap_records[i].authmode);
        cJSON_AddItemToArray(networks, network);
    }

    cJSON_AddItemToObject(root, "networks", networks);

    const char *response = cJSON_Print(root);
    httpd_resp_sendstr(req, response);

    cJSON_Delete(root);
    if(NULL != response){
        free((void*)response);
    }

    return ESP_OK;
}

//static GlobalState * GLOBAL_STATE;
//static httpd_handle_t server = NULL;
//QueueHandle_t log_queue = NULL;

/*
 * Log viewers, by socket. This was one global fd, so opening the log page in a
 * second tab took the stream away from the first, which then simply sat there
 * showing nothing with no indication why. A phone and a desktop cannot both
 * watch a miner, and neither can a browser and a script.
 *
 * A dead client is noticed when a send to it fails, which is the only
 * notification available -- the server is not told when a socket goes away.
 */
#define MAX_WS_LOG_CLIENTS 4
static int ws_log_clients[MAX_WS_LOG_CLIENTS] = { -1, -1, -1, -1 };

static bool ws_log_clients_present(void)
{
    for (int i = 0; i < MAX_WS_LOG_CLIENTS; i++) {
        if (ws_log_clients[i] >= 0) {
            return true;
        }
    }
    return false;
}

static void ws_log_client_add(int sockfd)
{
    for (int i = 0; i < MAX_WS_LOG_CLIENTS; i++) {
        if (ws_log_clients[i] == sockfd) {
            return;                      /* already watching */
        }
    }
    for (int i = 0; i < MAX_WS_LOG_CLIENTS; i++) {
        if (ws_log_clients[i] < 0) {
            ws_log_clients[i] = sockfd;
            ESP_LOGI(TAG, "Added WebSocket log client, fd: %d, slot: %d", sockfd, i);
            return;
        }
    }
    /* Full: take the oldest slot rather than refuse. A stale entry is the
     * likely occupant, since the only way one is freed is a failed send. */
    ESP_LOGW(TAG, "WebSocket log clients full; replacing fd %d in slot 0",
             ws_log_clients[0]);
    ws_log_clients[0] = sockfd;
}

#define REST_CHECK(a, str, goto_tag, ...)                                                                                          \
    do {                                                                                                                           \
        if (!(a)) {                                                                                                                \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__);                                                  \
            goto goto_tag;                                                                                                         \
        }                                                                                                                          \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)
#define MESSAGE_QUEUE_SIZE (128)

typedef struct rest_server_context
{
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

// --- (新增) 辅助函数：将32字节的 SHA256 转换为64字节的十六进制字符串 ---
static void bytes_to_hex_string(const unsigned char *bytes, size_t len, char *hex_str) {
    for (size_t i = 0; i < len; ++i) {
        sprintf(hex_str + (i * 2), "%02x", bytes[i]);
    }
    hex_str[len * 2] = '\0';
}

static esp_err_t ip_in_private_range(uint32_t address) {
    uint32_t ip_address = ntohl(address);

    // 10.0.0.0 - 10.255.255.255 (Class A)
    if ((ip_address >= 0x0A000000) && (ip_address <= 0x0AFFFFFF)) {
        return ESP_OK;
    }

    // 172.16.0.0 - 172.31.255.255 (Class B)
    if ((ip_address >= 0xAC100000) && (ip_address <= 0xAC1FFFFF)) {
        return ESP_OK;
    }

    // 192.168.0.0 - 192.168.255.255 (Class C)
    if ((ip_address >= 0xC0A80000) && (ip_address <= 0xC0A8FFFF)) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static uint32_t extract_origin_ip_addr(char *origin)
{
    char ip_str[16];
    uint32_t origin_ip_addr = 0;

    // Find the start of the IP address in the Origin header
    const char *prefix = "http://";
    char *ip_start = strstr(origin, prefix);
    if (ip_start) {
        ip_start += strlen(prefix); // Move past "http://"

        // Extract the IP address portion (up to the next '/')
        char *ip_end = strchr(ip_start, '/');
        size_t ip_len = ip_end ? (size_t)(ip_end - ip_start) : strlen(ip_start);
        if (ip_len < sizeof(ip_str)) {
            strncpy(ip_str, ip_start, ip_len);
            ip_str[ip_len] = '\0'; // Null-terminate the string

            // Convert the IP address string to uint32_t
            origin_ip_addr = inet_addr(ip_str);
            if (origin_ip_addr == INADDR_NONE) {
                ESP_LOGW(CORS_TAG, "Invalid IP address: %s", ip_str);
            } else {
                ESP_LOGD(CORS_TAG, "Extracted IP address %lu", origin_ip_addr);
            }
        } else {
            ESP_LOGW(CORS_TAG, "IP address string is too long: %s", ip_start);
        }
    }

    return origin_ip_addr;
}

static esp_err_t is_network_allowed(httpd_req_t * req)
{
    if(NORMAL_MODE != GLOBAL_STATE->SYSTEM_MODULE.boot_mode){
        return ESP_OK;
    }

    if (GLOBAL_STATE->SYSTEM_MODULE.ap_enabled == true) {
        ESP_LOGI(CORS_TAG, "Device in AP mode. Allowing CORS.");
        return ESP_OK;
    }

    int sockfd = httpd_req_to_sockfd(req);
    char ipstr[INET6_ADDRSTRLEN];
    struct sockaddr_in6 addr;   // esp_http_server uses IPv6 addressing
    socklen_t addr_size = sizeof(addr);

    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) < 0) {
        ESP_LOGE(CORS_TAG, "Error getting client IP");
        return ESP_FAIL;
    }

    uint32_t request_ip_addr = addr.sin6_addr.un.u32_addr[3];

    // // Convert to IPv6 string
    // inet_ntop(AF_INET, &addr.sin6_addr, ipstr, sizeof(ipstr));

    // Convert to IPv4 string
    inet_ntop(AF_INET, &request_ip_addr, ipstr, sizeof(ipstr));

    // Attempt to get the Origin header.
    char origin[128];
    uint32_t origin_ip_addr;
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK) {
        ESP_LOGD(CORS_TAG, "Origin header: %s", origin);
        origin_ip_addr = extract_origin_ip_addr(origin);
    } else {
        ESP_LOGD(CORS_TAG, "No origin header found.");
        origin_ip_addr = request_ip_addr;
    }

    if (ip_in_private_range(origin_ip_addr) == ESP_OK && ip_in_private_range(request_ip_addr) == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI(CORS_TAG, "Client is NOT in the private ip ranges or same range as server.");
    return ESP_FAIL;
}

/*
 * OTA update images are obfuscated with a repeating 16-byte pad.
 *
 * This was previously written as aes_decrypt() and commented as a
 * "simplified AES-ECB" implementation, deriving the pad as SHA-256 of an
 * embedded 32-byte key and indexing it with hash_val[(pos % 16) % 32].
 * That second modulo could never do anything, so only the first 16 bytes
 * of the digest were ever used.
 *
 * It is a Vigenere cipher, not AES, and it is not a security boundary:
 * the pad is recoverable from any published image by frequency analysis,
 * with no key. It is kept solely for compatibility with existing update
 * files and with the vendor's packaging tool.
 *
 * Update integrity comes from the Secure Boot v2 signature inside the
 * payload, which esp_ota_set_boot_partition() verifies. Access control
 * comes from require_authenticated(). See docs/OTA-FORMAT.md.
 *
 * The pad is now a build-time constant rather than a derived value. The
 * key file it used to be computed from was never part of the source
 * release, so the tree could not be built without it.
 */
#define OTA_PAD_LEN 16

static const uint8_t ota_obfuscation_pad[OTA_PAD_LEN] = {
    0x69, 0xcc, 0x74, 0xae, 0xaf, 0x0c, 0xe6, 0x83,
    0x22, 0x9d, 0x42, 0x2f, 0x54, 0x42, 0x8a, 0x54,
};

/* Deobfuscate `input_len` bytes, continuing the pad from `stream_offset`
 * bytes into the update file. In-place use (input == output) is safe. */
static void ota_deobfuscate(const unsigned char *input, size_t input_len,
                            unsigned char *output, size_t stream_offset)
{
    for (size_t i = 0; i < input_len; i++) {
        output[i] = input[i] ^ ota_obfuscation_pad[(stream_offset + i) % OTA_PAD_LEN];
    }
}

static void readWWWVersion(void) {
    FILE* f = fopen("/www/version.txt", "r");
    if (f != NULL) {
        size_t n = fread(WWWVersion, 1, sizeof(WWWVersion) - 1, f);
        WWWVersion[n] = '\0';
        fclose(f);
        ESP_LOGI(TAG, "WWW version: %s", WWWVersion);
    } else {
        strcpy(WWWVersion, "unknown");
        ESP_LOGI(TAG, "Failed to open WWW version.txt");
    }
}

esp_err_t init_fs(void)
{
    //const char* www_label = "www";
    const char* www_base_path = "/www";

    esp_vfs_spiffs_conf_t conf = {
        .base_path = www_base_path,
        .partition_label = NULL,
        .max_files = 15,
        .format_if_mount_failed = false
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    readWWWVersion();

    return ESP_OK;
}

/* Function for stopping the webserver */
void stop_webserver(httpd_handle_t server)
{
    if (server) {
        /* Stop the httpd server */
        httpd_stop(server);
    }
}

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t * req, const char * filepath)
{
    const char * type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        //type = "text/xml";
        type = "image/svg+xml";
    } else if (CHECK_FILE_EXTENSION(filepath, ".pdf")) {
        type = "application/pdf";
    }
    return httpd_resp_set_type(req, type);
}

static esp_err_t set_cors_headers(httpd_req_t * req)
{
    esp_err_t err;

    if(GLOBAL_STATE->SYSTEM_MODULE.boot_mode == NORMAL_MODE){
        /*close remote debug when in normal mode.*/
        return ESP_OK;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, X-File-SHA256"); // (修改) 允许 X-File-SHA256
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Recovery handler */
static esp_err_t rest_recovery_handler(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    extern const unsigned char recovery_page_start[] asm("_binary_recovery_page_html_start");
    extern const unsigned char recovery_page_end[] asm("_binary_recovery_page_html_end");
    const size_t recovery_page_size = (recovery_page_end - recovery_page_start);
    httpd_resp_send_chunk(req, (const char*)recovery_page_start, recovery_page_size);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t * req)
{
    char filepath[FILE_PATH_MAX];
    uint8_t filePathLength = sizeof(filepath);

    rest_server_context_t * rest_context = (rest_server_context_t *) req->user_ctx;
    strlcpy(filepath, rest_context->base_path, filePathLength);
    
    if (req->uri[strlen(req->uri) - 1] == '/') {
        strlcat(filepath, "/index.html", filePathLength);
    } else {
        strlcat(filepath, req->uri, filePathLength);
    }

    // 1. 设置 Content-Type (根据原始文件名)
    set_content_type_from_file(req, filepath);

    // [重构开始] ----------------------------------------------------
    int fd = -1;
    char gz_filepath[FILE_PATH_MAX];
    
    // 尝试构建 .gz 路径
    snprintf(gz_filepath, sizeof(gz_filepath), "%s.gz", filepath);

    // 2. 优先尝试打开 .gz 文件
    fd = open(gz_filepath, O_RDONLY, 0);
    
    if (fd != -1) {
        // 如果 .gz 存在，告诉浏览器这是 gzip 压缩内容
        ESP_LOGD(TAG, "Serving gzipped file: %s", gz_filepath);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    } else {
        // 3. 如果 .gz 不存在，尝试打开原始文件 (明文)
        // 这完美解决了 timer.worker.js 以及其他小文件没有被压缩的问题
        ESP_LOGD(TAG, "Serving plain file: %s", filepath);
        fd = open(filepath, O_RDONLY, 0);
    }
    // [重构结束] ----------------------------------------------------

    if (fd == -1) {
        // Set status
        httpd_resp_set_status(req, "302 Temporary Redirect");
        // Redirect to the "/" root directory
        httpd_resp_set_hdr(req, "Location", "/");
        // iOS requires content in the response to detect a captive portal
        httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

        ESP_LOGW(TAG, "File not found: %s, Redirecting to root!", filepath);
        return ESP_OK;
    }

    /*
     * Thirty days is right for a file whose name changes when its contents
     * do, and wrong for the document that says what those names are.
     *
     * index.html is served for "/", which took no header at all, leaving the
     * browser free to guess a lifetime for it. A guessed lifetime on that one
     * file pins the whole interface: it is the only place the hashed chunk
     * names appear, so a cached copy keeps pointing an updated miner at the
     * assets of the firmware it used to be running.
     */
    if (req->uri[strlen(req->uri) - 1] != '/') {
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=2592000");
    } else {
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    }

    // [删除] 这一行必须删除，已经在上方根据实际打开的文件动态设置了
    // httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char * chunk = rest_context->scratch;
    ssize_t read_bytes;
    do {
        /* Read file in chunks into the scratch buffer */
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if (read_bytes == -1) {
            ESP_LOGE(TAG, "Failed to read file : %s", filepath);
        } else if (read_bytes > 0) {
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                close(fd);
                ESP_LOGE(TAG, "File sending failed!");
                /* Abort sending file */
                httpd_resp_sendstr_chunk(req, NULL);
                /* Respond with 500 Internal Server Error */
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
                return ESP_OK;
            }
        }
    } while (read_bytes > 0);
    /* Close file after sending complete */
    close(fd);
    ESP_LOGD(TAG, "File sending complete");
    /* Respond with an empty chunk to signal HTTP response completion */
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_options_request(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers for OPTIONS request
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    // Send a blank response for OPTIONS request
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t PATCH_update_settings(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int total_len = req->content_len;
    int cur_len = 0;
    char * buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_OK;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_OK;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON * root = cJSON_Parse(buf);
    cJSON * item;
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumURL"))) {
        // Strip protocol prefix (stratum+ssl:// or stratum+tcp://)
        const char *raw_url = item->valuestring;
        if (strncmp(raw_url, "stratum+ssl://", 14) == 0) {
            raw_url += 14;
        } else if (strncmp(raw_url, "stratum+tcp://", 14) == 0) {
            raw_url += 14;
        }
        nvs_config_set_string(NVS_CONFIG_STRATUM_URL, raw_url);
        if(NULL != GLOBAL_STATE->SYSTEM_MODULE.pool_url){
            free(GLOBAL_STATE->SYSTEM_MODULE.pool_url);
            GLOBAL_STATE->SYSTEM_MODULE.pool_url = strdup(raw_url);
        }
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumURL"))) {
        // Strip protocol prefix (stratum+ssl:// or stratum+tcp://)
        const char *raw_url = item->valuestring;
        if (strncmp(raw_url, "stratum+ssl://", 14) == 0) {
            raw_url += 14;
        } else if (strncmp(raw_url, "stratum+tcp://", 14) == 0) {
            raw_url += 14;
        }
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_URL, raw_url);
        if(NULL != GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url){
            free(GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url);
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url = strdup(raw_url);
        }
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumUser"))) {
        nvs_config_set_string(NVS_CONFIG_STRATUM_USER, item->valuestring);
        if(NULL != GLOBAL_STATE->SYSTEM_MODULE.pool_user){
            free(GLOBAL_STATE->SYSTEM_MODULE.pool_user);
            GLOBAL_STATE->SYSTEM_MODULE.pool_user = strdup(item->valuestring);
        }
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "apiPassword"))) {
        nvs_config_set_string(NVS_CONFIG_API_PASSWORD, item->valuestring);
        /* Existing tokens were issued against the old secret. */
        api_auth_revoke_all();
        ESP_LOGI(TAG, "API password changed; all sessions revoked");
    }

    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "stratumPassword"))) {
        nvs_config_set_string(NVS_CONFIG_STRATUM_PASS, item->valuestring);
        if(NULL != GLOBAL_STATE->SYSTEM_MODULE.pool_pass){
            free(GLOBAL_STATE->SYSTEM_MODULE.pool_pass);
            GLOBAL_STATE->SYSTEM_MODULE.pool_pass = strdup(item->valuestring);
        }
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumUser"))) {
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_USER, item->valuestring);
        if(NULL != GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user){
            free(GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user);
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user = strdup(item->valuestring);
        }
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "fallbackStratumPassword"))) {
        nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_PASS, item->valuestring);
        if(NULL != GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_pass){
            free(GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_pass);
            GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_pass = strdup(item->valuestring);
        }
    }
    if ((item = cJSON_GetObjectItem(root, "stratumPort")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_PORT, item->valueint);
        GLOBAL_STATE->SYSTEM_MODULE.pool_port = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "fallbackStratumPort")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, item->valueint);
        GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_port = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "stratumTLS")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_TLS, item->valueint);
        GLOBAL_STATE->SYSTEM_MODULE.pool_tls = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "stratumExtranonceSubscribe")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_STRATUM_EXTRANONCE_SUBSCRIBE, item->valueint);
        GLOBAL_STATE->SYSTEM_MODULE.pool_extranonce_subscribe = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "fallbackStratumTLS")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_TLS, item->valueint);
        GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_tls = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "fallbackStratumExtranonceSubscribe")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_EXTRANONCE_SUBSCRIBE, item->valueint);
        GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_extranonce_subscribe = item->valueint;
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "ssid"))) {
        nvs_config_set_string(NVS_CONFIG_WIFI_SSID, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "wifiPass"))) {
        nvs_config_set_string(NVS_CONFIG_WIFI_PASS, item->valuestring);
        #ifdef SHOW_WIFI_PASSWORD_FEATURE
        if(NULL != GLOBAL_STATE->SYSTEM_MODULE.wifiPass){
            free(GLOBAL_STATE->SYSTEM_MODULE.wifiPass);
            GLOBAL_STATE->SYSTEM_MODULE.wifiPass = strdup(item->valuestring);
        }
        #endif
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "hostname"))) {
        nvs_config_set_string(NVS_CONFIG_HOSTNAME, item->valuestring);
    }

    if ((item = cJSON_GetObjectItem(root, "boot_mode")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_BOOT_MODE, item->valueint);
    }

    if ((item = cJSON_GetObjectItem(root, "asicovervdef")) != NULL) 
    {
        /*
         * Against the model's ceiling, not against TPS546_VOUT_MAX.
         *
         * On a BC04 those differ -- 4.80 V against 5.20 -- and all four of the
         * voltage fields here checked the wrong one, so a single request could
         * command four BM1370s in series 8% over the cap the vendor put on
         * them, and store it as the value the miner comes back up on. The cap
         * was only ever applied to the startup default, in nvs_device.c.
         */
        if((item->valueint >= GLOBAL_STATE->asic_vol_min) &&
           (item->valueint <= device_core_voltage_ceiling(GLOBAL_STATE)))
        {
            nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE_DEF, item->valueint);
        }
        else
        {
            ESP_LOGW(TAG, "v0l def %d is not valid.", item->valueint);
        }
    }

    if ((item = cJSON_GetObjectItem(root, "coreVoltage")) != NULL && item->valueint > 0) {
        
        if((item->valueint >= GLOBAL_STATE->asic_vol_min) &&
           (item->valueint <= device_core_voltage_ceiling(GLOBAL_STATE)))
        {
            nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, item->valueint);

            /*
             * Also store it where the running boot mode will look for it after
             * a restart. SYSTEM_init reads asicnormalvol in normal mode and
             * asicovervol in over-frequency mode; asicvoltage is read by
             * neither. Writing only that key changed the voltage until the next
             * restart and then silently reverted -- so a client that set a
             * voltage, restarted to apply it, and read the value back saw its
             * own setting rejected, with nothing reported as having failed.
             *
             * A benchmark hits this on its second measurement and stops. The
             * API says coreVoltage; it should mean the voltage the miner comes
             * back up on.
             */
            switch (GLOBAL_STATE->SYSTEM_MODULE.boot_mode) {
                case NORMAL_MODE:
                    nvs_config_set_u16(NVS_CONFIG_ASIC_NORMAL_VOLTAGE, item->valueint);
                    break;
                case OVER_FREQ_MODE:
                    nvs_config_set_u16(NVS_CONFIG_ASIC_OVER_VOLTAGE, item->valueint);
                    break;
                default:
                    /* user-customised reads asicvoltage directly */
                    break;
            }

            while(GLOBAL_STATE->HEALTH_MODULE.voltage != item->valueint)
            {

                float voltage = item->valueint - GLOBAL_STATE->HEALTH_MODULE.voltage;
                if(voltage > 3)
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage += 3;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                }
                else if(voltage < -3)
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage -= 3;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                }
                else
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage = item->valueint;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                    GLOBAL_STATE->asic_vol_default = item->valueint;
                    break;
                }
            }
        }
        else
        {
            ESP_LOGW(TAG, "v0l %d is not valid.", item->valueint);
        }
    }
    if ((item = cJSON_GetObjectItem(root, "coreNormalVoltage")) != NULL && item->valueint > 0) {

        if((item->valueint >= GLOBAL_STATE->asic_vol_min) &&
           (item->valueint <= device_core_voltage_ceiling(GLOBAL_STATE)))
        {
            nvs_config_set_u16(NVS_CONFIG_ASIC_NORMAL_VOLTAGE, item->valueint);

            while(GLOBAL_STATE->HEALTH_MODULE.voltage != item->valueint)
            {

                float voltage = item->valueint - GLOBAL_STATE->HEALTH_MODULE.voltage;
                if(voltage > 3)
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage += 3;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                }
                else if(voltage < -3)
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage -= 3;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                }
                else
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage = item->valueint;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                    GLOBAL_STATE->asic_vol_default = item->valueint;
                    break;
                }
            }
        }
        else
        {
            ESP_LOGW(TAG, "normal v0l %d is not valid.", item->valueint);
        }
    }
    if ((item = cJSON_GetObjectItem(root, "coreOverVoltage")) != NULL && item->valueint > 0) {

        if((item->valueint >= GLOBAL_STATE->asic_vol_min) &&
           (item->valueint <= device_core_voltage_ceiling(GLOBAL_STATE)))
        {
            nvs_config_set_u16(NVS_CONFIG_ASIC_OVER_VOLTAGE, item->valueint);

            while(GLOBAL_STATE->HEALTH_MODULE.voltage != item->valueint)
            {

                float voltage = item->valueint - GLOBAL_STATE->HEALTH_MODULE.voltage;
                if(voltage > 3)
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage += 3;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                }
                else if(voltage < -3)
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage -= 3;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                }
                else
                {
                    GLOBAL_STATE->HEALTH_MODULE.voltage = item->valueint;
                    voltage = GLOBAL_STATE->HEALTH_MODULE.voltage / 100.0;
                    VCORE_set_voltage(voltage);
                    GLOBAL_STATE->asic_vol_default = item->valueint;
                    break;
                }
            }
        }
        else
        {
            ESP_LOGW(TAG, "Over v0l %d is not valid.", item->valueint);
        }
    }
    if ((item = cJSON_GetObjectItem(root, "frequency")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_ASIC_FREQ, item->valueint);

        /* Same as coreVoltage above: the mode that will run after the restart
         * reads its own key, and asicfrequency is not it. */
        switch (GLOBAL_STATE->SYSTEM_MODULE.boot_mode) {
            case NORMAL_MODE:
                nvs_config_set_u16(NVS_CONFIG_ASIC_NORMAL_FREQ, item->valueint);
                break;
            case OVER_FREQ_MODE:
                nvs_config_set_u16(NVS_CONFIG_ASIC_OVER_FREQ, item->valueint);
                break;
            default:
                break;
        }
    }
#ifdef CONFIG_BC04_INDIVIDUAL_FREQ
    if (GLOBAL_STATE->device_model == DEVICE_BC04) {
        if ((item = cJSON_GetObjectItem(root, "frequency_0")) != NULL && item->valueint > 0) {
            nvs_config_set_u16("asicfreq_c0", item->valueint);
        }
        if ((item = cJSON_GetObjectItem(root, "frequency_1")) != NULL && item->valueint > 0) {
            nvs_config_set_u16("asicfreq_c1", item->valueint);
        }
        if ((item = cJSON_GetObjectItem(root, "frequency_2")) != NULL && item->valueint > 0) {
            nvs_config_set_u16("asicfreq_c2", item->valueint);
        }
        if ((item = cJSON_GetObjectItem(root, "frequency_3")) != NULL && item->valueint > 0) {
            nvs_config_set_u16("asicfreq_c3", item->valueint);
        }
    }
#endif
    if ((item = cJSON_GetObjectItem(root, "Normalfrequency")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_ASIC_NORMAL_FREQ, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "Overfrequency")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_ASIC_OVER_FREQ, item->valueint);
    }

    /* Ethernet could only be turned back on by a factory restore, which also
     * throws away the WiFi credentials -- so a board whose Ethernet misbehaves
     * had no way back except reflashing NVS over USB. Takes effect at the next
     * restart, like the other network settings. */
    if ((item = cJSON_GetObjectItem(root, "eth_on")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_ETH_ON, item->valueint ? 1 : 0);
    }
    if ((item = cJSON_GetObjectItem(root, "wifi_on")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_WIFI_ON, item->valueint ? 1 : 0);
    }

    if ((item = cJSON_GetObjectItem(root, "flipscreen")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FLIP_SCREEN, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "backlight")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_BACKLIGHT, item->valueint);
        if (item->valueint) {
            display_backlight_on();
        } else {
            display_backlight_off();
        }
    }
    if ((item = cJSON_GetObjectItem(root, "overheat_mode")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
    }
    //if ((item = cJSON_GetObjectItem(root, "invertscreen")) != NULL) {
    //    nvs_config_set_u16(NVS_CONFIG_INVERT_SCREEN, item->valueint);
    //}
    if ((item = cJSON_GetObjectItem(root, "invertfanpolarity")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_INVERT_FAN_POLARITY, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "autofanspeed")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "fanspeed")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_FAN_SPEED, item->valueint);
    }
    //if ((item = cJSON_GetObjectItem(root, "overclockEnabled")) != NULL) {
    //    nvs_config_set_u16(NVS_CONFIG_OVERCLOCK_ENABLED, item->valueint);
    //}
    if ((item = cJSON_GetObjectItem(root, "ntpServer")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_NTP_SERVER, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "ntpServerBackup")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_NTP_SERVER_BACKUP, item->valuestring);
    }
    /* The NTP servers were settable but the zone was not, so the clock ran on
     * whatever the firmware was compiled with -- UTC+8 by default, which is
     * wrong nearly everywhere. Takes a POSIX TZ string, e.g.
     * "EST5EDT,M3.2.0,M11.1.0". Applied at the next restart, when rtc_sync()
     * calls setenv("TZ", ...). */
    if ((item = cJSON_GetObjectItem(root, "timezone")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_TIME_ZONE, item->valuestring);
    }

    /* ---- dual mining, pool B ----
     * Takes effect on restart: the pool B session and the slice scheduler read
     * these at start-up. Ratio and slice length are also re-read live by
     * create_jobs_task, so those two can be tuned without a reboot. */
    if ((item = cJSON_GetObjectItem(root, "poolBUrl")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_POOLB_URL, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBPort")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_POOLB_PORT, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBUser")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_POOLB_USER, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBPass")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_POOLB_PASS, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBTLS")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_POOLB_TLS, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBExtranonceSubscribe")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_POOLB_XNSUB, item->valueint);
        GLOBAL_STATE->SYSTEM_MODULE.poolB_extranonce_subscribe = item->valueint;
    }
    if ((item = cJSON_GetObjectItem(root, "poolBFbUrl")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_POOLB_FB_URL, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBFbPort")) != NULL && item->valueint > 0) {
        nvs_config_set_u16(NVS_CONFIG_POOLB_FB_PORT, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBFbUser")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_POOLB_FB_USER, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBFbPass")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_POOLB_FB_PASS, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "poolBFbTLS")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_POOLB_FB_TLS, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "dualEnable")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_DUAL_ENABLE, item->valueint);
    }
    if ((item = cJSON_GetObjectItem(root, "dualRatioA")) != NULL) {
        uint8_t ratio = dual_clamp_ratio(item->valueint);
        nvs_config_set_u16(NVS_CONFIG_DUAL_RATIO, ratio);
        GLOBAL_STATE->dual_ratio_a = ratio;   /* live, no restart needed */
    }
    if ((item = cJSON_GetObjectItem(root, "dualSliceMs")) != NULL) {
        uint16_t slice = dual_clamp_interval(item->valueint);
        nvs_config_set_u16(NVS_CONFIG_DUAL_SLICE, slice);
        GLOBAL_STATE->dual_interval_ms = slice;
    }

    if ((item = cJSON_GetObjectItem(root, "snStr")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_SN, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "boardVer")) != NULL) {
        nvs_config_set_string(NVS_CONFIG_BOARD_VERSION, item->valuestring);
    }
    int restore = 0;
    if ((item = cJSON_GetObjectItem(root, "restore")) != NULL) {
        if (cJSON_IsBool(item))
        {
            if (cJSON_IsTrue(item))
            {
                nvs_config_set_string(NVS_CONFIG_STRATUM_URL, CONFIG_STRATUM_URL);
                nvs_config_set_u16(NVS_CONFIG_STRATUM_PORT, CONFIG_STRATUM_PORT);
                nvs_config_set_string(NVS_CONFIG_STRATUM_USER, CONFIG_STRATUM_USER);
                nvs_config_set_string(NVS_CONFIG_STRATUM_PASS, CONFIG_STRATUM_PW);

                nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_URL, CONFIG_FALLBACK_STRATUM_URL);
                nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, CONFIG_FALLBACK_STRATUM_PORT);
                nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_USER, CONFIG_FALLBACK_STRATUM_USER);
                nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_PASS, CONFIG_FALLBACK_STRATUM_PW);

                nvs_config_set_u64(NVS_CONFIG_BEST_DIFF, 0);
                nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1);
                //nvs_config_set_u16(NVS_CONFIG_OVERCLOCK_ENABLED, 0);
                nvs_config_set_u16(NVS_CONFIG_BOOT_MODE, OVER_FREQ_MODE);

                nvs_config_set_u16(NVS_CONFIG_IS_STATIC_IP, 0);
                nvs_config_set_u16(NVS_CONFIG_ETH_IS_STATIC_IP, 0);

                nvs_config_set_u16(NVS_CONFIG_WIFI_ON, 1);
                nvs_config_set_u16(NVS_CONFIG_ETH_ON, 1);
                restore = 1;
            }
        }
    }

    if ((item = cJSON_GetObjectItem(root, "displayFlash")) != NULL) {
        if (cJSON_IsTrue(item))
        {
            displayFlashSet(1);
            GLOBAL_STATE->screen_flash = true;
        }
        else
        {
            displayFlashSet(0);
            GLOBAL_STATE->screen_flash = false;
        }
    }
    /*
    if ((item = cJSON_GetObjectItem(root, "staticIP")) != NULL){
        if(is_valid_ip(item->valuestring)){
            nvs_config_set_string(NVS_CONFIG_STATIC_IP, item->valuestring);
        }else{
            ESP_LOGW(TAG, "staticIP %s is not a valid IP", item->valuestring);
        }
    }

    if ((item = cJSON_GetObjectItem(root, "subnetMask")) != NULL){
        if(is_valid_ip(item->valuestring)){
            nvs_config_set_string(NVS_CONFIG_SUBNET_MASK, item->valuestring);
        }else{
            ESP_LOGW(TAG, "subnetMask %s is not valid.", item->valuestring);
        }
    }

    if ((item = cJSON_GetObjectItem(root, "gateway")) != NULL){
        if(is_valid_ip(item->valuestring)){
            nvs_config_set_string(NVS_CONFIG_GATEWAY, item->valuestring);
        }else{
            ESP_LOGW(TAG, "gateway %s is not valid.", item->valuestring);
        }
    }

    if ((item = cJSON_GetObjectItem(root, "dns")) != NULL){
        if(is_valid_ip(item->valuestring)){
            nvs_config_set_string(NVS_CONFIG_DNS, item->valuestring);
        }else{
            ESP_LOGW(TAG, "DNS %s is not valid.", item->valuestring);
        }
    }
    */
    cJSON_Delete(root);
    httpd_resp_send_chunk(req, NULL, 0);

    if(restore)
    {
        ESP_LOGI(TAG, "Restarting System because of restore api");
        // Delay to ensure the response is sent
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        // Restart the system
        restart_with_reason("System restore initiated");
    }
    return ESP_OK;
}

static esp_err_t POST_restart(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    ESP_LOGI(TAG, "Restarting System because of API Request");

    // Send HTTP response before restarting
    const char* resp_str = "System will restart shortly.";
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // Delay to ensure the response is sent
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    // Restart the system
    restart_with_reason("API restart request");

    // This return statement will never be reached, but it's good practice to include it
    return ESP_OK;
}
/*
// URL解码函数
static void urldecode(char *dst, const char *src, size_t dst_size) {
    char a, b;
    size_t i = 0, j = 0;
    
    while (src[i] && j < dst_size - 1) {
        if (src[i] == '%') {
            if (src[i+1] && src[i+2]) {
                a = src[i+1];
                b = src[i+2];
                if (a >= 'A' && a <= 'Z') a = a - 'A' + 10;
                else if (a >= 'a' && a <= 'z') a = a - 'a' + 10;
                else if (a >= '0' && a <= '9') a = a - '0';
                
                if (b >= 'A' && b <= 'Z') b = b - 'A' + 10;
                else if (b >= 'a' && b <= 'z') b = b - 'a' + 10;
                else if (b >= '0' && b <= '9') b = b - '0';
                
                dst[j++] = 16 * a + b;
                i += 3;
            } else {
                dst[j++] = src[i++];
            }
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

// 解析键值对
static void parse_key_value(const char *str, char *key, char *value, size_t buf_size) {
    const char *eq = strchr(str, '=');
    if (eq) {
        size_t key_len = eq - str;
        strncpy(key, str, key_len < buf_size ? key_len : buf_size - 1);
        key[key_len < buf_size ? key_len : buf_size - 1] = '\0';
        
        urldecode(value, eq + 1, buf_size);
    } else {
        strncpy(key, str, buf_size - 1);
        key[buf_size - 1] = '\0';
        value[0] = '\0';
    }
}
*/
// 处理 application/x-www-form-urlencoded (暂时未使用)
/*
static esp_err_t handle_urlencoded_form(httpd_req_t *req, const char *data, size_t len) {
    char buffer[512];
    char key[256], value[256];
    
    strncpy(buffer, data, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    
    char *token = strtok(buffer, "&");
    while (token != NULL) {
        parse_key_value(token, key, value, sizeof(value));
        ESP_LOGI(TAG, "Form field: %s = %s", key, value);
        
        if(strcmp(key, "_bb_pool1url") == 0) {
            char url[40] = "\0";
            int port = 0;
            int ret_sscanf = sscanf(value, "%39[^:]:%d", url, &port);

            if(2 == ret_sscanf){
                nvs_config_set_string(NVS_CONFIG_STRATUM_URL, url);
                nvs_config_set_u16(NVS_CONFIG_STRATUM_PORT, port);
            }else{
                ESP_LOGI(TAG, "Get %d param, Error _bb_pool1url %s", ret_sscanf, value);
            }
        }else if(strcmp(key, "_bb_pool1user") == 0){
           nvs_config_set_string(NVS_CONFIG_STRATUM_USER, value);             
        }else if(strcmp(key, "_bb_pool1pw") == 0){
            nvs_config_set_string(NVS_CONFIG_STRATUM_PASS, value);
        }else if(strcmp(key, "_bb_pool2url") == 0){
            char url[40] = "\0";
            int port = 0;
            int ret_sscanf = sscanf(value, "%39[^:]:%d", url, &port);

            if(2 == ret_sscanf){
                nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_URL, url);
                nvs_config_set_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, port);
            }else{
                ESP_LOGI(TAG, "Get %d param, Error _bb_pool2url %s", ret_sscanf, value);
            }
        }else if(strcmp(key, "_bb_pool2user") == 0){
            nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_USER, value);
        }else if(strcmp(key, "_bb_pool2pw") == 0){
            nvs_config_set_string(NVS_CONFIG_FALLBACK_STRATUM_PASS, value);
        }else if(strcmp(key, "_bb_fan_customize_value") == 0){
            uint16_t fan_value = atoi(value);
            nvs_config_set_u16(NVS_CONFIG_FAN_SPEED, fan_value);
        }else if(strcmp(key, "_bb_fan_customize_switch") == 0){
            if(0 == strcmp(value, "true")){
                nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, 0);
            }else if(0 == strcmp(value, "false")){
                nvs_config_set_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1);
            }
        }else if(strcmp(key, "_bb_freq") == 0){
            uint16_t freq = atoi(value);
            nvs_config_set_u16(NVS_CONFIG_ASIC_FREQ, freq);
        }else if(strcmp(key, "_bb_voltage") == 0){   
            uint16_t coreVoltage = atoi(value);
            nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, coreVoltage);
        }else if(strcmp(key, "_bb_runmode") == 0){
            uint16_t run_mode = atoi(value);
            nvs_config_set_u16(NVS_CONFIG_BOOT_MODE, run_mode);
        }else if(strcmp(key, "_bb_ema") == 0){
            ESP_LOGI(TAG, "bb_ema not supported.");
        }else if(strcmp(key, "_bb_debug") == 0){
            ESP_LOGI(TAG, "bb_debug not supported.");
        }
        
        token = strtok(NULL, "&");
    }
    
    return ESP_OK;
}
*/

static cJSON *generate_miner_conf()
{
    cJSON *root = cJSON_CreateObject();
    cJSON *pools_array = cJSON_CreateArray();
    char tmp_str[100] = "\0";
    SystemModule *system_module = &(GLOBAL_STATE->SYSTEM_MODULE); 
    
    cJSON *pool1 = cJSON_CreateObject();
    const char *pool1_prefix = (system_module->pool_tls != DISABLED) ? "stratum+ssl" : "stratum+tcp";
    snprintf(tmp_str, 100, "%s://%s:%d", pool1_prefix, system_module->pool_url, system_module->pool_port);
    cJSON_AddStringToObject(pool1, "url", tmp_str);
    cJSON_AddStringToObject(pool1, "user", system_module->pool_user);
    cJSON_AddStringToObject(pool1, "pass", system_module->pool_pass);
    cJSON_AddItemToArray(pools_array, pool1);
    
    cJSON *pool2 = cJSON_CreateObject();
    const char *pool2_prefix = (system_module->fallback_pool_tls != DISABLED) ? "stratum+ssl" : "stratum+tcp";
    snprintf(tmp_str, 100, "%s://%s:%d", pool2_prefix, system_module->fallback_pool_url, system_module->fallback_pool_port);
    cJSON_AddStringToObject(pool2, "url", tmp_str);
    cJSON_AddStringToObject(pool2, "user", system_module->fallback_pool_user);
    cJSON_AddStringToObject(pool2, "pass", system_module->fallback_pool_pass);
    cJSON_AddItemToArray(pools_array, pool2);
    
    cJSON *pool3 = cJSON_CreateObject();
    cJSON_AddStringToObject(pool3, "url", "");
    cJSON_AddStringToObject(pool3, "user", "");
    cJSON_AddStringToObject(pool3, "pass", "");
    cJSON_AddItemToArray(pools_array, pool3);
    
    cJSON_AddItemToObject(root, "pools", pools_array);
    
    cJSON_AddBoolToObject(root, "api-listen", true);
    cJSON_AddBoolToObject(root, "api-network", true);
    cJSON_AddStringToObject(root, "api-groups", "A:stats:pools:devs:summary:version");
    cJSON_AddStringToObject(root, "api-allow", "A:0/0,W:*");
    cJSON_AddBoolToObject(root, "use-vil", true);
    snprintf(tmp_str, 100, "%"PRIu32"", GLOBAL_STATE->asic_freqency);
    cJSON_AddStringToObject(root, "freq", tmp_str);

    cJSON_AddStringToObject(root, "sram-voltage", "3");
    cJSON_AddStringToObject(root, "coin-type", "ltc");

    return root;
}

static esp_err_t get_miner_conf_handler(httpd_req_t * req)
{
    esp_err_t result = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    cJSON *miner_conf = generate_miner_conf();
    httpd_resp_set_type(req, "application/json");
    char* resp_str = cJSON_Print(miner_conf);
    httpd_resp_send(
        req, resp_str, strlen(resp_str)
    );
    
    if(NULL != miner_conf){
        cJSON_Delete(miner_conf);
    }
    if(NULL != resp_str){
        free(resp_str);
    }

    return result;
}

float roundToPrecision(float value, int precision) {
    float factor = pow(10, precision);
    return round(value * factor) / factor;
}

/* Simple handler for getting system handler */

/*
 * Since-boot history for the dashboard chart.
 *
 * GET /api/system/history?window=<seconds>&points=<n>
 *
 * The caller says how far back it wants and how many points it can draw; the
 * device averages its stored samples into that many buckets. Timestamps are
 * deliberately absent. The miner's clock depends on NTP having worked, and a
 * chart that mixes a device clock with a browser clock draws its live points
 * at an offset from its history. `age` -- how long ago the newest returned
 * point was taken -- lets the browser place the whole series against its own
 * clock, which is the only one it can plot live points against anyway.
 */
static esp_err_t GET_system_history(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    uint32_t window = HISTORY_RETENTION_SECONDS;
    uint32_t points = 180;

    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "window", value, sizeof(value)) == ESP_OK) {
            uint32_t asked = (uint32_t)strtoul(value, NULL, 10);
            if (asked > 0 && asked < window) {
                window = asked;
            }
        }
        if (httpd_query_key_value(query, "points", value, sizeof(value)) == ESP_OK) {
            uint32_t asked = (uint32_t)strtoul(value, NULL, 10);
            if (asked > 0) {
                points = (asked > 720) ? 720 : asked;
            }
        }
    }

    history_sample_t * samples = calloc(points, sizeof(history_sample_t));
    if (samples == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    uint32_t interval = HISTORY_INTERVAL_SECONDS;
    uint32_t count = history_query(window, points, samples, &interval);

    cJSON * root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "interval", interval);
    cJSON_AddNumberToObject(root, "sampleInterval", HISTORY_INTERVAL_SECONDS);
    cJSON_AddNumberToObject(root, "retention", HISTORY_RETENTION_SECONDS);
    cJSON_AddNumberToObject(root, "span", history_span_seconds());
    cJSON_AddNumberToObject(root, "age", history_age_seconds());
    cJSON_AddNumberToObject(root, "count", count);

    cJSON * hashrate = cJSON_AddArrayToObject(root, "hashrate");
    cJSON * temp     = cJSON_AddArrayToObject(root, "temp");
    cJSON * vr       = cJSON_AddArrayToObject(root, "vrTemp");
    cJSON * power    = cJSON_AddArrayToObject(root, "power");

    for (uint32_t i = 0; i < count; i++) {
        /* An empty bucket is a gap, not a zero: the chart should break the
         * line there rather than draw a dive to the floor that never
         * happened. null is how it says so. */
        if (samples[i].valid) {
            cJSON_AddItemToArray(hashrate, cJSON_CreateNumber(samples[i].hashrate_gh));
            cJSON_AddItemToArray(temp,     cJSON_CreateNumber(samples[i].board_c));
            cJSON_AddItemToArray(vr,       cJSON_CreateNumber(samples[i].vr_c));
            cJSON_AddItemToArray(power,    cJSON_CreateNumber(samples[i].power_dw / 10.0));
        } else {
            cJSON_AddItemToArray(hashrate, cJSON_CreateNull());
            cJSON_AddItemToArray(temp,     cJSON_CreateNull());
            cJSON_AddItemToArray(vr,       cJSON_CreateNull());
            cJSON_AddItemToArray(power,    cJSON_CreateNull());
        }
    }

    free(samples);

    httpd_resp_set_type(req, "application/json");
    const char * body = cJSON_PrintUnformatted(root);
    httpd_resp_sendstr(req, body);
    free((void *)body);
    cJSON_Delete(root);

    return ESP_OK;
}

static esp_err_t GET_system_info(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    char * ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, CONFIG_ESP_WIFI_SSID);
    char * hostname = nvs_config_get_string(NVS_CONFIG_HOSTNAME, CONFIG_LWIP_LOCAL_HOSTNAME);
    uint8_t mac[6];
    char formattedMac[18];
    char * stratumURL = GLOBAL_STATE->SYSTEM_MODULE.pool_url;
    char * fallbackStratumURL = GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_url;
    char * stratumUser = GLOBAL_STATE->SYSTEM_MODULE.pool_user;
    char * fallbackStratumUser = GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_user;
    char * board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION, "unknown");
    char * ntp_server = nvs_config_get_string(NVS_CONFIG_NTP_SERVER, "pool.ntp.org");
    char * ntp_server_backup = nvs_config_get_string(NVS_CONFIG_NTP_SERVER_BACKUP, "ntp.aliyun.com");
    char * time_zone = nvs_config_get_string(NVS_CONFIG_TIME_ZONE, "CST-8");
    
    struct tm timeinfo;
    time_t now;
    char time_string[50];
    
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(time_string, 50, "%Y-%m-%d %H:%M:%S", &timeinfo);

    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(formattedMac, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    int8_t wifi_rssi = -90;
    get_wifi_current_rssi(&wifi_rssi);

    cJSON * root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "currentTime", time_string);
    cJSON_AddNumberToObject(root, "hashRate", GLOBAL_STATE->SYSTEM_MODULE.current_hashrate);
    cJSON_AddStringToObject(root, "bestDiff", GLOBAL_STATE->SYSTEM_MODULE.best_diff_string);
    cJSON_AddStringToObject(root, "bestSessionDiff", GLOBAL_STATE->SYSTEM_MODULE.best_session_diff_string);
    cJSON_AddNumberToObject(root, "stratumDiff", GLOBAL_STATE->stratum_difficulty);

    cJSON_AddNumberToObject(root, "isUsingFallbackStratum", GLOBAL_STATE->SYSTEM_MODULE.is_using_fallback);

    cJSON_AddNumberToObject(root, "current", roundToPrecision(GLOBAL_STATE->HEALTH_MODULE.out_current, 3));
    cJSON_AddNumberToObject(root, "voltage", GLOBAL_STATE->HEALTH_MODULE.input_voltage*1000);
    cJSON_AddNumberToObject(root, "nominalVoltage", GLOBAL_STATE->HEALTH_MODULE.nominal_input_voltage);
    cJSON_AddNumberToObject(root, "power", roundToPrecision(GLOBAL_STATE->HEALTH_MODULE.power, 3));
    cJSON_AddNumberToObject(root, "temp", GLOBAL_STATE->HEALTH_MODULE.board_temperature[0]);

    cJSON_AddNumberToObject(root, "maxPower", GLOBAL_STATE->HEALTH_MODULE.max_power);
    cJSON_AddNumberToObject(root, "coreVoltageActual", GLOBAL_STATE->HEALTH_MODULE.out_voltage*1000);

    cJSON_AddNumberToObject(root, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "coreVoltage", GLOBAL_STATE->HEALTH_MODULE.voltage);
    cJSON_AddNumberToObject(root, "frequency", GLOBAL_STATE->asic_freqency);
#ifdef CONFIG_BC04_INDIVIDUAL_FREQ
    if (GLOBAL_STATE->device_model == DEVICE_BC04) {
        cJSON_AddBoolToObject(root, "bc04_individual_freq", true);
        cJSON_AddNumberToObject(root, "frequency_0", nvs_config_get_u16("asicfreq_c0", GLOBAL_STATE->asic_freqency));
        cJSON_AddNumberToObject(root, "frequency_1", nvs_config_get_u16("asicfreq_c1", GLOBAL_STATE->asic_freqency));
        cJSON_AddNumberToObject(root, "frequency_2", nvs_config_get_u16("asicfreq_c2", GLOBAL_STATE->asic_freqency));
        cJSON_AddNumberToObject(root, "frequency_3", nvs_config_get_u16("asicfreq_c3", GLOBAL_STATE->asic_freqency));
    } else {
        cJSON_AddBoolToObject(root, "bc04_individual_freq", false);
    }
#else
    cJSON_AddBoolToObject(root, "bc04_individual_freq", false);
#endif
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "macAddr", formattedMac);
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON_AddBoolToObject(root, "authEnabled", !api_auth_is_disabled());
    cJSON_AddStringToObject(root, "wifiStatus", GLOBAL_STATE->SYSTEM_MODULE.wifi_status);
    cJSON_AddNumberToObject(root, "wifiRSSI", wifi_rssi);

    // Ethernet status
    NetWorkInfo ethInfo = network_get_info();
    cJSON_AddStringToObject(root, "ethStatus", ethInfo.eth_status);
    cJSON_AddNumberToObject(root, "ethConnected", (ethInfo.eth_get_ip || strstr(ethInfo.eth_status, "Link Up") != NULL) ? 1 : 0);
    cJSON_AddNumberToObject(root, "eth_on", nvs_config_get_u16(NVS_CONFIG_ETH_ON, 1));
    /* The address the miner is actually reachable at over the wire.
     * Captured all along in eth_ip_str and never reported, so the web
     * interface could only ever show a static "plug in a cable" hint. */
    cJSON_AddStringToObject(root, "ethIP", ethInfo.eth_ip_str);
    cJSON_AddNumberToObject(root, "wifi_on", nvs_config_get_u16(NVS_CONFIG_WIFI_ON, 1));

    cJSON_AddNumberToObject(root, "apEnabled", GLOBAL_STATE->SYSTEM_MODULE.ap_enabled);
    cJSON_AddNumberToObject(root, "sharesAccepted", GLOBAL_STATE->SYSTEM_MODULE.shares_accepted);
    cJSON_AddNumberToObject(root, "sharesRejected", GLOBAL_STATE->SYSTEM_MODULE.shares_rejected);

    if(GLOBAL_STATE->SYSTEM_MODULE.shares_rejected && GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count)
    {
        cJSON *error_array = cJSON_CreateArray();
        cJSON_AddItemToObject(root, "sharesRejectedReasons", error_array);

        for (int i = 0; i < GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats_count; i++) {
            cJSON *error_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(error_obj, "message", GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].message);
            cJSON_AddNumberToObject(error_obj, "count", GLOBAL_STATE->SYSTEM_MODULE.rejected_reason_stats[i].count);
            cJSON_AddItemToArray(error_array, error_obj);
        }
    }

    cJSON_AddNumberToObject(root, "uptimeSeconds", (esp_timer_get_time() - GLOBAL_STATE->SYSTEM_MODULE.start_time) / 1000000);
    cJSON_AddNumberToObject(root, "asicCount", ASIC_get_asic_count(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "smallCoreCount", ASIC_get_small_core_count(GLOBAL_STATE));
    /*
     * Tuning telemetry. A benchmark needs to know how hard the chip is failing
     * at a given voltage and frequency, and the counters that answer it were
     * only reachable per core.
     *
     * These are lifetime totals, deliberately. A rolling average computed here
     * would have to guess at a window; a caller taking the difference between
     * two reads gets the rate over exactly the interval it cares about, which
     * is what a per-setting measurement needs. /api/system/cores breaks the
     * same totals down per core.
     */
    cJSON_AddNumberToObject(root, "hwErrorCount", GLOBAL_STATE->SYSTEM_MODULE.recveived_hw);
    cJSON_AddNumberToObject(root, "noncesFound", GLOBAL_STATE->SYSTEM_MODULE.recveived_nonce);

    /* Regulator temperature. Read all along for the thermal cut-out, never
     * reported, so anything tuning voltage was blind to the limit that bites
     * first when voltage goes up. */
    cJSON_AddNumberToObject(root, "vrTemp", read_power_temp());

    /* What the chip should manage at this frequency, so a caller does not have
     * to hard-code a core count that a firmware change would silently break. */
    cJSON_AddNumberToObject(root, "expectedHashrate",
        (double) GLOBAL_STATE->asic_freqency *
        ASIC_get_small_core_count(GLOBAL_STATE) * ASIC_get_asic_count(GLOBAL_STATE) / 1000.0);

    cJSON_AddStringToObject(root, "ASICModel", GLOBAL_STATE->asic_model_str);
    cJSON_AddStringToObject(root, "DeviceModel", GLOBAL_STATE->device_model_str);

    cJSON_AddStringToObject(root, "stratumURL", stratumURL);
    cJSON_AddStringToObject(root, "fallbackStratumURL", fallbackStratumURL);
    cJSON_AddNumberToObject(root, "stratumPort", GLOBAL_STATE->SYSTEM_MODULE.pool_port);
    cJSON_AddNumberToObject(root, "fallbackStratumPort", GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_port);
    cJSON_AddStringToObject(root, "stratumUser", stratumUser);
    cJSON_AddStringToObject(root, "fallbackStratumUser", fallbackStratumUser);
    cJSON_AddNumberToObject(root, "stratumTLS", GLOBAL_STATE->SYSTEM_MODULE.pool_tls);
    cJSON_AddNumberToObject(root, "stratumExtranonceSubscribe", GLOBAL_STATE->SYSTEM_MODULE.pool_extranonce_subscribe);
    cJSON_AddNumberToObject(root, "fallbackStratumTLS", GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_tls);
    cJSON_AddNumberToObject(root, "fallbackStratumExtranonceSubscribe", GLOBAL_STATE->SYSTEM_MODULE.fallback_pool_extranonce_subscribe);
    cJSON_AddStringToObject(root, "ntpServer", ntp_server);
    cJSON_AddStringToObject(root, "ntpServerBackup", ntp_server_backup);
    cJSON_AddStringToObject(root, "timezone", time_zone);

    cJSON_AddNumberToObject(root, "dualEnable", GLOBAL_STATE->dual_enable);
    cJSON_AddNumberToObject(root, "dualRatioA", GLOBAL_STATE->dual_ratio_a);
    cJSON_AddNumberToObject(root, "dualSliceMs", GLOBAL_STATE->dual_interval_ms);
    cJSON_AddStringToObject(root, "poolBUrl",
        GLOBAL_STATE->SYSTEM_MODULE.poolB_url ? GLOBAL_STATE->SYSTEM_MODULE.poolB_url : "");
    cJSON_AddNumberToObject(root, "poolBPort", GLOBAL_STATE->SYSTEM_MODULE.poolB_port);
    cJSON_AddStringToObject(root, "poolBUser",
        GLOBAL_STATE->SYSTEM_MODULE.poolB_user ? GLOBAL_STATE->SYSTEM_MODULE.poolB_user : "");
    cJSON_AddNumberToObject(root, "poolBConnected", GLOBAL_STATE->SYSTEM_MODULE.poolB_connected);
    /*
     * poolBTLS was accepted by the settings handler and stored, but never
     * reported back. The pool page reads it as `st.poolBTLS ?? 0`, so it came
     * up off on every load regardless of what the miner was doing, and saving
     * the page then wrote that back -- turning pool B's TLS off for anyone who
     * opened the tab and pressed Save. The failover switch beside it was
     * reported correctly all along, which is what made it look deliberate.
     */
    cJSON_AddNumberToObject(root, "poolBTLS", GLOBAL_STATE->SYSTEM_MODULE.poolB_tls);
    cJSON_AddNumberToObject(root, "poolBExtranonceSubscribe",
                            GLOBAL_STATE->SYSTEM_MODULE.poolB_extranonce_subscribe);
    cJSON_AddStringToObject(root, "poolBFbUrl",
        GLOBAL_STATE->SYSTEM_MODULE.poolB_fb_url ? GLOBAL_STATE->SYSTEM_MODULE.poolB_fb_url : "");
    cJSON_AddNumberToObject(root, "poolBFbPort", GLOBAL_STATE->SYSTEM_MODULE.poolB_fb_port);
    cJSON_AddStringToObject(root, "poolBFbUser",
        GLOBAL_STATE->SYSTEM_MODULE.poolB_fb_user ? GLOBAL_STATE->SYSTEM_MODULE.poolB_fb_user : "");
    cJSON_AddNumberToObject(root, "poolBFbTLS", GLOBAL_STATE->SYSTEM_MODULE.poolB_fb_tls);
    cJSON_AddBoolToObject(root, "boardMismatch", GLOBAL_STATE->SYSTEM_MODULE.board_mismatch);
    cJSON_AddNumberToObject(root, "poolBUsingFailover", GLOBAL_STATE->SYSTEM_MODULE.poolB_is_using_failover);
    cJSON_AddNumberToObject(root, "poolBSharesAccepted", GLOBAL_STATE->SYSTEM_MODULE.poolB_shares_accepted);
    cJSON_AddNumberToObject(root, "poolBSharesRejected", GLOBAL_STATE->SYSTEM_MODULE.poolB_shares_rejected);
    /* Pool B's own vardiff. Without it the two pools' share counts cannot be
     * compared: work delivered is shares x difficulty, and the pools rarely
     * settle on the same one. */
    cJSON_AddNumberToObject(root, "poolBDiff", GLOBAL_STATE->stratum_difficultyB);
    cJSON_AddNumberToObject(root, "poolAJobsSelected", GLOBAL_STATE->jobs_selected[0]);
    cJSON_AddNumberToObject(root, "poolAJobsServed", GLOBAL_STATE->jobs_served[0]);
    cJSON_AddNumberToObject(root, "poolBJobsSelected", GLOBAL_STATE->jobs_selected[1]);
    cJSON_AddNumberToObject(root, "poolBJobsServed", GLOBAL_STATE->jobs_served[1]);

    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "WWWVersion", WWWVersion);
    cJSON_AddStringToObject(root, "idfVersion", esp_get_idf_version());
    cJSON_AddStringToObject(root, "boardVersion", board_version);
    cJSON_AddStringToObject(root, "runningPartition", esp_ota_get_running_partition()->label);

    cJSON_AddNumberToObject(root, "overheat_mode", GLOBAL_STATE->SYSTEM_MODULE.overheat_mode);
    //cJSON_AddNumberToObject(root, "overclockEnabled", nvs_config_get_u16(NVS_CONFIG_OVERCLOCK_ENABLED, 0));

    cJSON_AddNumberToObject(root, "flipscreen", nvs_config_get_u16(NVS_CONFIG_FLIP_SCREEN, 1));
    cJSON_AddNumberToObject(root, "backlight", nvs_config_get_u16(NVS_CONFIG_BACKLIGHT, 1));
    //cJSON_AddNumberToObject(root, "invertscreen", nvs_config_get_u16(NVS_CONFIG_INVERT_SCREEN, 0));
    cJSON_AddNumberToObject(root, "invertfanpolarity", nvs_config_get_u16(NVS_CONFIG_INVERT_FAN_POLARITY, 0));

    cJSON_AddNumberToObject(root, "autofanspeed", nvs_config_get_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1));
    cJSON_AddNumberToObject(root, "fanspeed", GLOBAL_STATE->HEALTH_MODULE.fan_percent[0]);
    cJSON_AddNumberToObject(root, "fanrpm", GLOBAL_STATE->HEALTH_MODULE.fan_rpm[0]);

    /*   
    if (GLOBAL_STATE->SYSTEM_MODULE.power_fault > 0) {
        cJSON_AddStringToObject(root, "power_fault", VCORE_get_fault_string(GLOBAL_STATE));
    }
    */
#ifdef HW_STATISTIC_FEATURE
    cJSON_AddNumberToObject(root, "nonceNumber", GLOBAL_STATE->SYSTEM_MODULE.recveived_nonce);
    cJSON_AddNumberToObject(root, "hwNumber", GLOBAL_STATE->SYSTEM_MODULE.recveived_hw);
    cJSON_AddNumberToObject(root, "hwRate", 100.0*(float)GLOBAL_STATE->SYSTEM_MODULE.recveived_hw/(float)(GLOBAL_STATE->SYSTEM_MODULE.recveived_nonce + GLOBAL_STATE->SYSTEM_MODULE.recveived_hw));
#endif
    cJSON_AddNumberToObject(root, "boot_mode", GLOBAL_STATE->SYSTEM_MODULE.boot_mode);
    cJSON_AddStringToObject(root, "sn_str", GLOBAL_STATE->SYSTEM_MODULE.sn[0]);
    //cJSON_AddNumberToObject(root, "blockFound", GLOBAL_STATE->SYSTEM_MODULE.FOUND_BLOCK);
    //cJSON_AddNumberToObject(root, "blocknum", GLOBAL_STATE->SYSTEM_MODULE.BLOCK_NUM);
    cJSON_AddBoolToObject(root,"displayFlash",GLOBAL_STATE->screen_flash);

    free(ssid);
    free(hostname);
    free(board_version);
    free(time_zone);
    free(ntp_server);
    free(ntp_server_backup);

    const char * sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((char *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

#if 0

#define CONFIG_INFLUX_ENABLE 0
/* Empty by default. These carried the vendor's own telemetry endpoint and
 * a plaintext token, so any device that enabled reporting without setting
 * its own destination would have reported to them. Telemetry is opt-in and
 * has nowhere to go until the owner supplies a server. */
#define CONFIG_INFLUX_URL ""
#define CONFIG_INFLUX_TOKEN ""
#define CONFIG_INFLUX_PORT 8086
#define CONFIG_INFLUX_BUCKET ""
#define CONFIG_INFLUX_ORG ""
#define CONFIG_INFLUX_PREFIX "mainnet_stats"

static esp_err_t GET_influx_info(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    int16_t influx_enable = nvs_config_get_u16(NVS_CONFIG_INFLUX_ENABLE, CONFIG_INFLUX_ENABLE) ? 1 : 0;
    char * influx_url = nvs_config_get_string(NVS_CONFIG_INFLUX_URL, CONFIG_INFLUX_URL);
    // char * influx_token = nvs_config_get_string(NVS_CONFIG_INFLUX_TOKEN, CONFIG_INFLUX_TOKEN);
    int16_t influx_port = nvs_config_get_u16(NVS_CONFIG_INFLUX_PORT, CONFIG_INFLUX_PORT);
    char * influx_bucket = nvs_config_get_string(NVS_CONFIG_INFLUX_BUCKET, CONFIG_INFLUX_BUCKET);
    char * influx_org = nvs_config_get_string(NVS_CONFIG_INFLUX_ORG, CONFIG_INFLUX_ORG);
    char * influx_prefix = nvs_config_get_string(NVS_CONFIG_INFLUX_PREFIX, CONFIG_INFLUX_PREFIX);

    cJSON * root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "influxURL", influx_url);
    cJSON_AddNumberToObject(root, "influxPort", influx_port);
    cJSON_AddStringToObject(root, "influxBucket", influx_bucket);
    cJSON_AddStringToObject(root, "influxOrg", influx_org);
    cJSON_AddStringToObject(root, "influxPrefix", influx_prefix);
    cJSON_AddNumberToObject(root, "influxEnable", influx_enable);

    free(influx_url);
    free(influx_bucket);
    free(influx_org);
    free(influx_prefix);

    const char * sys_info = cJSON_Print(root);
    httpd_resp_sendstr(req, sys_info);
    free((char *)sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t PATCH_update_influx(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    int total_len = req->content_len;
    int cur_len = 0;
    char * buf = ((rest_server_context_t *) (req->user_ctx))->scratch;
    int received = 0;
    if (total_len >= SCRATCH_BUFSIZE) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "content too long");
        return ESP_OK;
    }
    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len);
        if (received <= 0) {
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to post control value");
            return ESP_OK;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    cJSON * root = cJSON_Parse(buf);
    cJSON * item;
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "influxURL"))) {
        nvs_config_set_string(NVS_CONFIG_INFLUX_URL, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "influxToken"))) {
        nvs_config_set_string(NVS_CONFIG_INFLUX_TOKEN, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "influxPort")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_INFLUX_PORT, item->valueint);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "influxBucket"))) {
        nvs_config_set_string(NVS_CONFIG_INFLUX_BUCKET, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "influxOrg"))) {
        nvs_config_set_string(NVS_CONFIG_INFLUX_ORG, item->valuestring);
    }
    if (cJSON_IsString(item = cJSON_GetObjectItem(root, "influxPrefix"))) {
        nvs_config_set_string(NVS_CONFIG_INFLUX_PREFIX, item->valuestring);
    }
    if ((item = cJSON_GetObjectItem(root, "influxEnable")) != NULL) {
        nvs_config_set_u16(NVS_CONFIG_INFLUX_ENABLE, item->valueint);
    }

    cJSON_Delete(root);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#endif
/* Selects the obfuscated OTA container (docs/OTA-FORMAT.md) over the
 * plain one. This was keyed off CONFIG_SECURE_FLASH_ENC_ENABLED, which
 * conflated the update format with flash encryption: you could not build
 * a device that speaks the shipping update format without also turning on
 * flash encryption, whose RELEASE mode is irreversible once booted. The
 * two are unrelated and are now configured independently. */
#ifdef CONFIG_STAYOPEN_OTA_OBFUSCATED
esp_err_t POST_WWW_update(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP mode");
        return ESP_OK;
    }

    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = true;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_filename, 20, "www.bin");
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Starting...");

    char buf[1000];
    int remaining = req->content_len;

    if (remaining < 1) {
        ESP_LOGW(TAG, "WWW update: File too short");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    unsigned char type_flag;
    int recv_len = httpd_req_recv(req, (char*)&type_flag, 1);
    if (recv_len != 1) {
        ESP_LOGW(TAG, "WWW update: Failed to receive file type");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    if (type_flag != 0x55) {
        ESP_LOGW(TAG, "WWW update: Invalid file type");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }
    remaining -= 1;

    size_t encrypted_header_size = 48;
    unsigned char encrypted_header[48];
    unsigned char decrypted_header[48];
    size_t header_received = 0;
    
    while (header_received < encrypted_header_size && remaining > 0) {
        size_t recv_size = MIN(encrypted_header_size - header_received, (size_t)remaining);
        recv_len = httpd_req_recv(req, (char*)(encrypted_header + header_received), recv_size);
        if (recv_len <= 0) {
            ESP_LOGW(TAG, "WWW update: Failed to receive header");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }
        header_received += recv_len;
        remaining -= recv_len;
    }
    
    ota_deobfuscate(encrypted_header, encrypted_header_size, decrypted_header, 0);
    
    char project_id[11];
    memcpy(project_id, decrypted_header + 32, 10);
    project_id[10] = '\0';
    if (strcmp(project_id, "GULLPOWER") != 0) {
        ESP_LOGW(TAG, "WWW update: Invalid firmware", project_id);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }
    
    unsigned char embedded_sha256[32];
    memcpy(embedded_sha256, decrypted_header, 32);
    ESP_LOGI(TAG, "WWW update: start...");

    if (remaining == 0) {
        ESP_LOGW(TAG, "WWW update: No file content");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    const esp_partition_t * www_partition =
    esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
    if (www_partition == NULL) {
        ESP_LOGW(TAG, "WWW update: WWW partition not found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WWW partition not found");
        return ESP_OK;
    }

    // Don't attempt to write more than what can be stored in the partition
    if (remaining > www_partition->size) {
        ESP_LOGW(TAG, "WWW update: File too large for partition");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File provided is too large for device");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    // Erase the entire www partition before writing
    ESP_ERROR_CHECK(esp_partition_erase_range(www_partition, 0, www_partition->size));

    // 使用嵌入的SHA256哈希值进行文件完整性校验
    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0); // 0 表示 SHA-256

    // 边接收边写入文件内容并计算SHA256
    size_t written = 0;
    size_t total_file_size = remaining;
    
    while (remaining > 0) {
        size_t recv_size = MIN((size_t)remaining, sizeof(buf));
        recv_len = httpd_req_recv(req, buf, recv_size);
        if (recv_len <= 0) {
            ESP_LOGW(TAG, "WWW update: Failed to receive file content");
            mbedtls_sha256_free(&sha256_ctx);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }
        
        // 更新 SHA256 计算
        mbedtls_sha256_update(&sha256_ctx, (unsigned char *)buf, recv_len);
        
        if (esp_partition_write(www_partition, written, (const void *)buf, recv_len) != ESP_OK) {
            ESP_LOGW(TAG, "WWW update: Failed to write to partition");
            mbedtls_sha256_free(&sha256_ctx);
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Write Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }
        
        written += recv_len;
        remaining -= recv_len;
        
        // 更新进度百分比
        uint8_t percentage = (written * 100) / total_file_size;
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Working (%d%%)", percentage);
    }

    // 完成 SHA256 计算并校验
    unsigned char calculated_sha256[32];
    mbedtls_sha256_finish(&sha256_ctx, calculated_sha256);
    mbedtls_sha256_free(&sha256_ctx);

    char embedded_sha256_str[65];
    char calculated_sha256_str[65];
    bytes_to_hex_string(embedded_sha256, 32, embedded_sha256_str);
    bytes_to_hex_string(calculated_sha256, 32, calculated_sha256_str);
    //ESP_LOGI(TAG, "Embedded SHA256: %s", embedded_sha256_str);
    //ESP_LOGI(TAG, "Calculated SHA256: %s", calculated_sha256_str);

    if (memcmp(embedded_sha256, calculated_sha256, 32) != 0) {
        ESP_LOGE(TAG, "SHA256 Mismatch! Embedded: %s, Calc: %s", embedded_sha256_str, calculated_sha256_str);
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Checksum Fail");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SHA256 Mismatch - File Corrupted");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }
    ESP_LOGI(TAG, "WWW Update SHA256 OK. Update Successful.");

    httpd_resp_sendstr(req, "WWW update complete, rebooting now!\n");

    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Finished...");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;

    /*
     * Restart, the way the application update above already does.
     *
     * The partition is written by this point but the running server is still
     * serving the old mount, so without this the answer was "WWW update
     * complete" followed by every page staying exactly as it was -- or, if
     * the previous contents were damaged, by the built-in recovery page. Both
     * look like an update that silently failed, and the only way out was to
     * know that a restart was owed.
     *
     * Only reached once the SHA256 above matched, so a partial or corrupt
     * upload does not get a reboot; it gets the error and leaves the running
     * interface alone.
     */
    restart_with_reason("Web interface update complete");

    return ESP_OK;
}

/*
 * Handle OTA file upload
 */
esp_err_t POST_OTA_update(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        ESP_LOGW(TAG, "OTA update: Unauthorized access");
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        ESP_LOGW(TAG, "OTA update: Failed to set CORS headers");
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        ESP_LOGW(TAG, "OTA update: Not allowed in AP mode");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP mode");
        return ESP_OK;
    }

    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = true;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_filename, 20, "app.bin");
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Starting...");

    char buf[640];
    unsigned char decrypt_buf[656];
    /* Zero-initialised: the error paths below run before esp_ota_begin()
     * assigns this, and must never abort an indeterminate handle. */
    esp_ota_handle_t ota_handle = 0;
    int remaining = req->content_len;

    if (remaining < 1) {
        ESP_LOGW(TAG, "OTA update: File too short");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    unsigned char type_flag;
    int recv_len = httpd_req_recv(req, (char*)&type_flag, 1);
    if (recv_len != 1) {
        ESP_LOGW(TAG, "OTA update: Failed to receive file type");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    if (type_flag != 0xAA) {
        ESP_LOGW(TAG, "OTA update: Invalid file type");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }
    remaining -= 1;

    mbedtls_sha256_context sha256_ctx;
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);

    size_t written = 0;
    size_t total_file_size = remaining;
    
    size_t encrypted_header_size = 48;
    unsigned char encrypted_header[48];
    unsigned char decrypted_header[48];
    size_t header_received = 0;
    
    while (header_received < encrypted_header_size && remaining > 0) {
        size_t recv_size = MIN(encrypted_header_size - header_received, (size_t)remaining);
        recv_len = httpd_req_recv(req, (char*)(encrypted_header + header_received), recv_size);
        if (recv_len <= 0) {
            ESP_LOGW(TAG, "OTA update: Failed to receive header");
            mbedtls_sha256_free(&sha256_ctx);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }
        header_received += recv_len;
        remaining -= recv_len;
    }
    
    ota_deobfuscate(encrypted_header, encrypted_header_size, decrypted_header, 0);
    
    char project_id[11];
    memcpy(project_id, decrypted_header + 32, 10);
    project_id[10] = '\0';
    if (strcmp(project_id, "GULLPOWER") != 0) {
        ESP_LOGW(TAG, "OTA update: Invalid project");
        mbedtls_sha256_free(&sha256_ctx);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }
    
    unsigned char embedded_sha256[32];
    memcpy(embedded_sha256, decrypted_header, 32);
    
    uint32_t original_firmware_length;
    memcpy(&original_firmware_length, decrypted_header + 42, 4);
    ESP_LOGI(TAG, "OTA update:firmware length: %u", original_firmware_length);
    
    // 初始化OTA更新
    const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_partition == NULL) {
        ESP_LOGW(TAG, "OTA update: Failed to get next update partition");
        mbedtls_sha256_free(&sha256_ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Partition error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    // Don't attempt to write more than what can be stored in the partition
    if (original_firmware_length > ota_partition->size) {
        ESP_LOGW(TAG, "OTA update: File too large for partition");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File provided is too large for device");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    if (esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle) != ESP_OK) {
        ESP_LOGW(TAG, "OTA update: Failed to begin OTA");
        mbedtls_sha256_free(&sha256_ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    written += encrypted_header_size;
    
    size_t processed_firmware_bytes = 0;
    size_t content_start_offset = encrypted_header_size; 
    size_t current_content_offset = 0;
    
    while (remaining > 0) {
        size_t recv_size = MIN((size_t)remaining, sizeof(buf));
        recv_len = httpd_req_recv(req, buf, recv_size);
        if (recv_len <= 0) {
            ESP_LOGW(TAG, "OTA update: Failed to receive data");
            mbedtls_sha256_free(&sha256_ctx);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }

        size_t input_size = recv_len;
        
        if (input_size > sizeof(decrypt_buf)) {
            ESP_LOGW(TAG, "OTA update: Buffer size error");
            mbedtls_sha256_free(&sha256_ctx);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Buffer error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }
        
        memset(decrypt_buf, 0, sizeof(decrypt_buf));
        memcpy(decrypt_buf, buf, input_size);
        
        size_t decrypt_block_size = 16;
        size_t decrypt_size = ((input_size + decrypt_block_size - 1) / decrypt_block_size) * decrypt_block_size;
        if (decrypt_size > sizeof(decrypt_buf)) {
            ESP_LOGW(TAG, "OTA update: Buffer size error");
            mbedtls_sha256_free(&sha256_ctx);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Buffer error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }
        
        ota_deobfuscate(decrypt_buf, decrypt_size, decrypt_buf,
                        content_start_offset + current_content_offset);
        
        current_content_offset += input_size;
        
        size_t actual_write_size = recv_len;
        if (processed_firmware_bytes + actual_write_size > original_firmware_length) {
            actual_write_size = original_firmware_length - processed_firmware_bytes;
        }
        
        if (actual_write_size > 0) {

            mbedtls_sha256_update(&sha256_ctx, decrypt_buf, actual_write_size);
            
            if (esp_ota_write(ota_handle, decrypt_buf, actual_write_size) != ESP_OK) {
                ESP_LOGW(TAG, "OTA update: Failed to write to OTA partition");
                mbedtls_sha256_free(&sha256_ctx);
                esp_ota_abort(ota_handle);
                snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Write Error");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write Error");
                GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
                return ESP_OK;
            }
            
            processed_firmware_bytes += actual_write_size;
            //ESP_LOGI(TAG, "OTA update: Processed %u bytes", processed_firmware_bytes);
        }
        
        written += input_size;
        remaining -= recv_len;
        
        if (total_file_size > 0) {
            uint8_t percentage = (written * 100) / total_file_size;
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Working (%d%%)", percentage);
        }
    }

    unsigned char calculated_sha256[32];
    mbedtls_sha256_finish(&sha256_ctx, calculated_sha256);
    mbedtls_sha256_free(&sha256_ctx);

    char embedded_sha256_str[65];
    char calculated_sha256_str[65];
    bytes_to_hex_string(embedded_sha256, 32, embedded_sha256_str);
    bytes_to_hex_string(calculated_sha256, 32, calculated_sha256_str);
    //ESP_LOGI(TAG, "Embedded SHA256: %s", embedded_sha256_str);
    //ESP_LOGI(TAG, "Calculated SHA256: %s", calculated_sha256_str);

    if (memcmp(embedded_sha256, calculated_sha256, 32) != 0) {
        ESP_LOGE(TAG, "SHA256 Mismatch! Embedded: %s, Calc: %s", embedded_sha256_str, calculated_sha256_str);
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Checksum Fail");
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SHA256 Mismatch - File Corrupted");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }
    ESP_LOGI(TAG, "OTA Update SHA256 OK. Update Successful.");

    ESP_LOGI(TAG, "OTA Update complete. Validating...");
    // Validate and switch to new OTA image and reboot
    if (esp_ota_end(ota_handle) != ESP_OK || esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
        ESP_LOGW(TAG, "OTA update: Validation error");
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Validation Error");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation / Activation Error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
        return ESP_OK;
    }

    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Rebooting...");

    httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
    ESP_LOGI(TAG, "Firmware update complete, rebooting now!");
    // 重启设备
    vTaskDelay(pdMS_TO_TICKS(1000));
    restart_with_reason("Firmware update complete");

    return ESP_OK;
}

#else

esp_err_t POST_WWW_update(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP mode");
        return ESP_OK;
    }

    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = true;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_filename, 20, "www.bin");
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Starting...");

    char buf[1000];
    int remaining = req->content_len;

    const esp_partition_t * www_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
    if (www_partition == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WWW partition not found");
        return ESP_OK;
    }

    // Don't attempt to write more than what can be stored in the partition
    if (remaining > www_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File provided is too large for device");
        return ESP_OK;
    }

    // Erase the entire www partition before writing
    ESP_ERROR_CHECK(esp_partition_erase_range(www_partition, 0, www_partition->size));

    static char client_sha256[65] = {0};
    client_sha256[64] = '\0';
    // --- 1. 初始化 SHA256 上下文 ---
    mbedtls_sha256_context sha256_ctx;
    int sha256_ok = 0;

    if (httpd_req_get_hdr_value_str(req, "X-File-SHA256", client_sha256, sizeof(client_sha256)) == ESP_OK) 
    {
        sha256_ok = 1;
        mbedtls_sha256_init(&sha256_ctx);
        mbedtls_sha256_starts(&sha256_ctx, 0); // 0 表示 SHA-256
        ESP_LOGI(TAG, "WWW Update: Client SHA256: %s", client_sha256);
    }

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        } else if (recv_len <= 0) {
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Protocol Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            return ESP_OK;
        }

        // 更新 SHA256 计算
        if(sha256_ok)
        {
            mbedtls_sha256_update(&sha256_ctx, (unsigned char *)buf, recv_len);
        }
        
        if (esp_partition_write(www_partition, www_partition->size - remaining, (const void *) buf, recv_len) != ESP_OK) {
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Write Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write Error");
            if(sha256_ok)
            {
                mbedtls_sha256_free(&sha256_ctx);
            }
            return ESP_OK;
        }

        uint8_t percentage = 100 - ((remaining * 100 / req->content_len));
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Working (%d%%)", percentage);

        remaining -= recv_len;
    }

    if(sha256_ok)
    {
        // --- 5. 完成 SHA256 计算并校验 ---
        unsigned char output_sha256[32];
        mbedtls_sha256_finish(&sha256_ctx, output_sha256);
        mbedtls_sha256_free(&sha256_ctx);

        char calc_sha256_str[65];
        bytes_to_hex_string(output_sha256, 32, calc_sha256_str);
        ESP_LOGI(TAG, "Calculated SHA256: %s", calc_sha256_str);

        if (strcasecmp(client_sha256, calc_sha256_str) != 0) {
            ESP_LOGE(TAG, "SHA256 Mismatch! Client: %s, Calc: %s", client_sha256, calc_sha256_str);
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Checksum Fail");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "SHA256 Mismatch - File Corrupted");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }
        ESP_LOGI(TAG, "WWW Update SHA256 OK. Update Successful.");
    }
    else
    {
        ESP_LOGI(TAG, "WWW update complete\n");
    }

    httpd_resp_sendstr(req, "WWW update complete, rebooting now!\n");

    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Finished...");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;

    /* Same reasoning as the obfuscated handler above: the partition is
     * written but the running server still serves the old mount, so without a
     * restart a successful update is indistinguishable from one that did
     * nothing. */
    restart_with_reason("Web interface update complete");

    return ESP_OK;
}

/*
 * Handle OTA file upload
 */
esp_err_t POST_OTA_update(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP mode");
        return ESP_OK;
    }

    GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = true;
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_filename, 20, "volc-miner.bin");
    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Starting...");

    char buf[1000];
    esp_ota_handle_t ota_handle;
    int remaining = req->content_len;

    const esp_partition_t * ota_partition = esp_ota_get_next_update_partition(NULL);
    ESP_ERROR_CHECK(esp_ota_begin(ota_partition, OTA_SIZE_UNKNOWN, &ota_handle));

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));

        // Timeout Error: Just retry
        if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;

            // Serious Error: Abort OTA
        } else if (recv_len <= 0) {
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Protocol Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Protocol Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }

        // Successful Upload: Flash firmware chunk
        if (esp_ota_write(ota_handle, (const void *) buf, recv_len) != ESP_OK) {
            esp_ota_abort(ota_handle);
            snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Write Error");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write Error");
            GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false;
            return ESP_OK;
        }

        uint8_t percentage = 100 - ((remaining * 100 / req->content_len));

        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Working (%d%%)", percentage);

        remaining -= recv_len;
    }

    ESP_LOGI(TAG, "OTA Update complete. Validating...");

    // Validate and switch to new OTA image and reboot
    if (esp_ota_end(ota_handle) != ESP_OK || esp_ota_set_boot_partition(ota_partition) != ESP_OK) {
        snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Validation Error");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Validation / Activation Error");
        GLOBAL_STATE->SYSTEM_MODULE.is_firmware_update = false; // (修改)
        return ESP_OK;
    }

    snprintf(GLOBAL_STATE->SYSTEM_MODULE.firmware_update_status, 20, "Rebooting...");

    httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
    ESP_LOGI(TAG, "Restarting System because of Firmware update complete");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    restart_with_reason("Firmware update complete");

    return ESP_OK;
}
#endif


#ifndef HTTPD_NEW_LOG
int log_to_queue(const char * format, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    // Calculate the required buffer size
    int needed_size = vsnprintf(NULL, 0, format, args_copy) + 1;
    va_end(args_copy);

    // Allocate the buffer dynamically
    char * log_buffer = (char *) calloc(needed_size + 2, sizeof(char));  // +2 for potential \n and \0
    if (log_buffer == NULL) {
        return 0;
    }

    // Format the string into the allocated buffer
    va_copy(args_copy, args);
    vsnprintf(log_buffer, needed_size, format, args_copy);
    va_end(args_copy);

    // Ensure the log message ends with a newline
    size_t len = strlen(log_buffer);
    if (len > 0 && log_buffer[len - 1] != '\n') {
        log_buffer[len] = '\n';
        log_buffer[len + 1] = '\0';
        len++;
    }

    // Print to standard output
    printf("%s", log_buffer);

    if (xQueueSendToBack(log_queue, (void*)&log_buffer, (TickType_t) 0) != pdPASS) {
        if (log_buffer != NULL) {
            free((void*)log_buffer);
        }
    }

    return 0;
}
#endif

void send_log_to_websocket(char *message)
{
    // Prepare the WebSocket frame
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)message;
    ws_pkt.len = strlen(message);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    if (server != NULL) {
        for (int i = 0; i < MAX_WS_LOG_CLIENTS; i++) {
            if (ws_log_clients[i] < 0) {
                continue;
            }
            if (httpd_ws_send_frame_async(server, ws_log_clients[i], &ws_pkt) != ESP_OK) {
                ESP_LOGD(TAG, "log client fd %d went away, freeing slot %d",
                         ws_log_clients[i], i);
                ws_log_clients[i] = -1;
            }
        }
    }

    // Free the allocated buffer
    free((void*)message);
}

/*
 * This handler echos back the received ws data
 * and triggers an async send if certain message received
 */
esp_err_t echo_handler(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    /*
     * The websocket gate, not the plain one. A browser WebSocket cannot send an
     * Authorization header, so this endpoint was rejecting every connection the
     * log viewer made -- the page showed "connection successful" then an error,
     * because the socket is upgraded before this handler runs and the 401 had
     * nowhere to go. The token arrives as ?token=... instead; returning
     * ESP_FAIL closes the connection.
     */
    if (api_auth_require_ws(req) != ESP_OK) {
        return ESP_FAIL;
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        ws_log_client_add(httpd_req_to_sockfd(req));
        
        #ifdef HTTPD_NEW_LOG
        // 【新增】：发送历史日志
        send_log_history_to_ws(req);
        #else
        // 【移除】：移除此处的 esp_log_set_vprintf，改为在 start_rest_server 中全局设置
        // esp_log_set_vprintf(log_to_queue); 
        #endif
        return ESP_OK;
    }
    return ESP_OK;
}

/*
 * Per-core hardware error counts.
 *
 * Reported on its own endpoint rather than folded into /api/system/info:
 * this is a hundred-odd entries that a dashboard polls every few seconds
 * has no use for, and it is read when someone is diagnosing a chip.
 *
 * Only cores that have produced something are listed. A core absent from
 * the list has returned nothing at all, which on a healthy chip simply
 * means it has not got there yet, and on a sick one is itself the finding.
 */
static esp_err_t GET_core_stats(httpd_req_t * req)
{
    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");

    cJSON * root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

#ifdef HW_STATISTIC_FEATURE
    SystemModule * module = &GLOBAL_STATE->SYSTEM_MODULE;
    cJSON * cores = cJSON_AddArrayToObject(root, "cores");
    uint64_t total_nonces = 0, total_errors = 0;
    int active = 0, faulty = 0;

    for (int i = 0; i < CORE_STATS_CORES; i++) {
        uint32_t good = module->core_nonces[i];
        uint32_t bad = module->core_errors[i];

        total_nonces += good;
        total_errors += bad;
        if (good || bad) {
            active++;
        }
        if (bad) {
            faulty++;
        }

        if (cores != NULL && (good || bad)) {
            cJSON * entry = cJSON_CreateObject();
            if (entry != NULL) {
                cJSON_AddNumberToObject(entry, "core", i);
                cJSON_AddNumberToObject(entry, "nonces", good);
                cJSON_AddNumberToObject(entry, "errors", bad);
                cJSON_AddItemToArray(cores, entry);
            }
        }
    }

    cJSON_AddNumberToObject(root, "coresSeen", active);
    cJSON_AddNumberToObject(root, "coresWithErrors", faulty);
    cJSON_AddNumberToObject(root, "totalNonces", total_nonces);
    cJSON_AddNumberToObject(root, "totalErrors", total_errors);
    cJSON_AddNumberToObject(root, "coreCount", CORE_STATS_CORES);
#else
    cJSON_AddArrayToObject(root, "cores");
    cJSON_AddNumberToObject(root, "coresSeen", 0);
#endif

    const char * out = cJSON_Print(root);
    httpd_resp_sendstr(req, out);
    free((void *)out);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t sync_time_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    xSemaphoreGive(xSyncTimeSemaphore);
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);

    return ret;
}

esp_err_t get_fw_type_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    ESP_LOGD(TAG, "Get fw type.");
    httpd_resp_set_type(req, "text/plain");
    char resp[100] = "app:0047;www:0048;";
    ret = httpd_resp_send(req, resp, strlen(resp));

    return ret;
}

esp_err_t get_errLog_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    ESP_LOGI(TAG, "Get error log.");
    httpd_resp_set_type(req, "text/plain");
    if(GLOBAL_STATE->SYSTEM_MODULE.system_error)
    httpd_resp_send(
        req, GLOBAL_STATE->SYSTEM_MODULE.system_error,
        strlen(GLOBAL_STATE->SYSTEM_MODULE.system_error));
    else
    httpd_resp_send(
        req, "NULL", 4);
    return ret;
}

esp_err_t get_network_info_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    //ESP_LOGI(TAG, "Get network info");
    httpd_resp_set_type(req, "application/json");
    cJSON *net_info = get_network_info_json();
    char *net_info_str = cJSON_PrintUnformatted(net_info);
    httpd_resp_send(
        req, net_info_str, strlen(net_info_str)
    );

    if(NULL != net_info_str){
        free(net_info_str);
    }
    if(NULL != net_info){
        cJSON_Delete(net_info);
    }

    return ret;
}

esp_err_t set_network_conf_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;
    char buf[500];

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Set network config.");
    ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_OK;
    }

    buf[ret] = '\0';
    //ESP_LOGI(TAG, "recv: %s", buf);

    cJSON * root = cJSON_Parse(buf);
    ret = set_network_conf_json(root);
    if(ESP_OK == ret){
        const char *resp = "{\"code\": \"200\", \"msg\": \"\", \"data\":\"\"}";
        httpd_resp_send(req, resp, strlen(resp));
    }

    if(NULL != root){
        cJSON_Delete(root);
    }

    return ret;
}

cJSON *get_system_info_cgi_json(GlobalState *global_state)
{
    cJSON *root = cJSON_CreateObject();
    SystemModule *system_module = &(global_state->SYSTEM_MODULE);

    cJSON_AddStringToObject(root, "minertype", GLOBAL_STATE->device_model_str);

    uint16_t isStatic = nvs_config_get_u16(NVS_CONFIG_IS_STATIC_IP, 0);
    if(!isStatic){
        cJSON_AddStringToObject(root, "nettype", "DHCP");
    }else{
        cJSON_AddStringToObject(root, "nettype", "Static");
    }
    cJSON_AddStringToObject(root, "netdevice", "wifi0");

    char str_mac[20];
    get_mac(str_mac);
    cJSON_AddStringToObject(root, "macaddr", str_mac);

    char str_ip[16], str_netmask[16], str_gateway[16];
    get_ip_netmask_gw(str_ip, str_netmask, str_gateway);
    cJSON_AddStringToObject(root, "ipaddress", str_ip);
    cJSON_AddStringToObject(root, "netmask", str_netmask);
    cJSON_AddStringToObject(root, "gateway", str_gateway);

    char str_hostname[30];
    get_hostname(str_hostname);
    cJSON_AddStringToObject(root, "hostname", str_hostname);

    char str_dns_server[20];
    cJSON_AddStringToObject(root, "dnsservers", str_dns_server);

    struct tm timeinfo;
    time_t now;
    char time_string[50];
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(time_string, 50, "%H:%M:%S", &timeinfo);
    cJSON_AddStringToObject(root, "curtime", time_string);

    char str_uptime[20];
    snprintf(str_uptime, 20, "%lld", (esp_timer_get_time() - system_module->start_time)/1000000);
    cJSON_AddStringToObject(root, "uptime", str_uptime);
    cJSON_AddStringToObject(root, "loadaverage", "0.37, 0.33, 0.31");

    cJSON_AddStringToObject(root, "mem_total", "");
    cJSON_AddStringToObject(root, "mem_used", "");
    cJSON_AddStringToObject(root, "mem_free", "");
    cJSON_AddStringToObject(root, "mem_buffers", "");
    cJSON_AddStringToObject(root, "mem_cached", "");
    cJSON_AddStringToObject(root, "system_mode", "freertos");
    cJSON_AddStringToObject(root, "bb_hwv", "1.0.0");

    cJSON_AddStringToObject(root, "system_kernel_version", tskKERNEL_VERSION_NUMBER);
    cJSON_AddStringToObject(root, "system_filesystem_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "cgminer_version", "1.0.0");

    return root;
}

esp_err_t get_system_info_cgi_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    ESP_LOGI(TAG, "get_system_info_cgi_handler");
    cJSON *system_info = get_system_info_cgi_json(GLOBAL_STATE);

    const char *resp = cJSON_Print(system_info);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    if(NULL != system_info){
        cJSON_Delete(system_info);
    }
    if(NULL != resp){
        free((void *)resp);
    }

    return ret;
}

esp_err_t get_voltage_cgi_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_OK;

    if (is_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (api_auth_require(req) != ESP_OK) {
        return ESP_OK; /* 401 already sent */
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }


    ESP_LOGI(TAG, "get_voltage_cgi_handler");
    cJSON *root = cJSON_CreateObject();
    char tmp_str[20];
    snprintf(tmp_str, 20, "%f", GLOBAL_STATE->HEALTH_MODULE.voltage);
    cJSON_AddStringToObject(root, "enable", "true");
    cJSON_AddStringToObject(root, "voltage", tmp_str);

    char *resp = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));

    if(NULL != root){
        cJSON_Delete(root);
    }
    if(NULL != resp){
        free((void*)resp);
    }

    return ret;
}


// HTTP Error (404) Handler - Redirects all requests to the root page
esp_err_t http_404_error_handler(httpd_req_t * req, httpd_err_code_t err)
{
    // Set status
    httpd_resp_set_status(req, "302 Temporary Redirect");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    ESP_LOGW(TAG, "Redirecting to root");
    return ESP_OK;
}

void websocket_log_handler()
{
    while (true)
    {
        char *message;
        if (xQueueReceive(log_queue, &message, (TickType_t) portMAX_DELAY) != pdPASS) {
            if (message != NULL) {
                free((void*)message);
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        if (!ws_log_clients_present()) {
            free((void*)message);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }

        send_log_to_websocket(message);
    }
}

esp_err_t start_rest_server(void * pvParameters)
{
    GLOBAL_STATE = (GlobalState *) pvParameters;
    const char * base_path = "/www";
    // esp_err_t ret = ESP_OK; // (修改) 移除未使用的变量

    bool enter_recovery = false;
    if (init_fs() != ESP_OK) {
        // Unable to initialize the web app filesystem.
        // Enter recovery mode
        enter_recovery = true;
    }

    REST_CHECK(base_path, "wrong base path", err);
    //rest_server_context_t * rest_context = calloc(1, sizeof(rest_server_context_t));
    rest_server_context_t * rest_context = heap_caps_calloc(1, sizeof(rest_server_context_t), MALLOC_CAP_SPIRAM);
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    log_queue = xQueueCreate(MESSAGE_QUEUE_SIZE, sizeof(char*));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.max_open_sockets = 8;

    /*
     * Reclaim the oldest connection instead of refusing the newest.
     *
     * Without this the server stops answering once eight sockets are held,
     * and it holds them for a long time: a browser tab killed without a
     * close, a laptop that slept mid-request, a phone that walked out of
     * range. None of those send a FIN, so the slot stays occupied until the
     * receive timeout expires, and with several of them the interface simply
     * stops responding.
     *
     * Mining is unaffected while this happens, because the stratum tasks own
     * their own sockets -- which is what makes the symptom so confusing to
     * diagnose from outside. The miner is plainly working, hashing away, and
     * the web interface is simply gone. Reported by an owner running a BC04
     * for twenty hours.
     */
    config.lru_purge_enable = true;
    config.max_uri_handlers = 48;

    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

    ESP_LOGI(TAG, "Starting HTTP Server");
    api_auth_init();

    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_start);

	/* URI handler for recovery */
    httpd_uri_t recovery_explicit_get_uri = {
        .uri = "/recovery",
        .method = HTTP_GET,
        .handler = rest_recovery_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &recovery_explicit_get_uri);

    // Register theme API endpoints
    ESP_ERROR_CHECK(register_theme_api_endpoints(server, rest_context));

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
        .uri = "/api/system/info",
        .method = HTTP_GET,
        .handler = GET_system_info,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* URI handler for per-core hardware error statistics */
    httpd_uri_t core_stats_get_uri = {
        .uri = "/api/system/cores",
        .method = HTTP_GET,
        .handler = GET_core_stats,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &core_stats_get_uri);

    #if 0
    httpd_uri_t influx_info_get_uri = {
        .uri = "/api/influx/info",
        .method = HTTP_GET,
        .handler = GET_influx_info,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &influx_info_get_uri);

    httpd_uri_t update_influx_settings_uri = {
        .uri = "/api/influx",
        .method = HTTP_PATCH,
        .handler = PATCH_update_influx,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &update_influx_settings_uri);
    #endif

    /* URI handler for WiFi scan */
    httpd_uri_t wifi_scan_get_uri = {
        .uri = "/api/system/wifi/scan",
        .method = HTTP_GET,
        .handler = GET_wifi_scan,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &wifi_scan_get_uri);

	/* URI handler for restart */
    httpd_uri_t system_restart_uri = {
        .uri = "/api/system/restart",
        .method = HTTP_POST,
        .handler = POST_restart,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_restart_uri);

    /* The vendor web UI already posted here; the route never existed.
     * See docs/SECURITY.md finding 1. */
    httpd_uri_t system_login_uri = {
        .uri = "/api/system/login",
        .method = HTTP_POST,
        .handler = POST_api_login,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_login_uri);

    httpd_uri_t system_login_options_uri = {
        .uri = "/api/system/login",
        .method = HTTP_OPTIONS,
        .handler = handle_options_request,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_login_options_uri);

    httpd_uri_t system_restart_options_uri = {
        .uri = "/api/system/restart",
        .method = HTTP_OPTIONS,
        .handler = handle_options_request,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_restart_options_uri);

	/* URI handler for system setting */
    httpd_uri_t update_system_settings_uri = {
        .uri = "/api/system",
        .method = HTTP_PATCH,
        .handler = PATCH_update_settings,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &update_system_settings_uri);

    httpd_uri_t system_options_uri = {
        .uri = "/api/system",
        .method = HTTP_OPTIONS,
        .handler = handle_options_request,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &system_options_uri);

    httpd_uri_t system_history_uri = {
        .uri = "/api/system/history",
        .method = HTTP_GET,
        .handler = GET_system_history,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &system_history_uri);

	/* URI handler for OTA */
    httpd_uri_t update_post_ota_firmware = {
        .uri = "/api/system/OTA",
        .method = HTTP_POST,
        .handler = POST_OTA_update,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &update_post_ota_firmware);

	/* URI handler for OTAWWW */
    httpd_uri_t update_post_ota_www = {
        .uri = "/api/system/OTAWWW",
        .method = HTTP_POST,
        .handler = POST_WWW_update,
        .user_ctx = rest_context // (修改) 确保 user_ctx 被传递
    };
    httpd_register_uri_handler(server, &update_post_ota_www);

    httpd_uri_t ws = {
        .uri = "/api/ws",
        .method = HTTP_GET,
        .handler = echo_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &ws);

    /*sync time api*/
    httpd_uri_t sync_time = {
        .uri = "/api/sync_time",
        .method = HTTP_POST,
        .handler = sync_time_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &sync_time);


    httpd_uri_t get_fw_type = {
        .uri = "/api/get_fw_type",
        .method = HTTP_GET,
        .handler = get_fw_type_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &get_fw_type);

    httpd_uri_t get_errLog = {
        .uri = "/api/get_err",
        .method = HTTP_GET,
        .handler = get_errLog_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &get_errLog);

    /*get_network_infoV1.cgi, set_network_conf.cgi*/
    httpd_uri_t get_network_info = {
        .uri = "/api/get_network",
        .method = HTTP_POST,
        .handler = get_network_info_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &get_network_info);

    httpd_uri_t set_network_conf = {
        .uri = "/api/set_network",
        .method = HTTP_POST,
        .handler = set_network_conf_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &set_network_conf);

    #ifdef HTTPD_NEW_LOG
    // 【新增】：注册下载接口
    httpd_uri_t log_download_uri = {
        .uri = "/api/system/log/download",
        .method = HTTP_GET,
        .handler = GET_log_download,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &log_download_uri);
    #endif

    httpd_uri_t get_miner_conf = {
        .uri = "/api/get_miner_conf",
        .method = HTTP_GET,
        .handler = get_miner_conf_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &get_miner_conf);

    httpd_uri_t get_system_info_cgi = {
        .uri = "/api/get_system_info",
        .method = HTTP_GET,
        .handler = get_system_info_cgi_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &get_system_info_cgi);

    httpd_uri_t get_voltage_cgi = {
        .uri = "/api/get_voltage",
        .method = HTTP_GET,
        .handler = get_voltage_cgi_handler,
        .user_ctx = NULL,
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &get_voltage_cgi);

    if (enter_recovery) {
        /* Make default route serve Recovery */
        httpd_uri_t recovery_implicit_get_uri = {
            .uri = "/*", 
            .method = HTTP_GET,
            .handler = rest_recovery_handler,
            .user_ctx = rest_context
        };
        httpd_register_uri_handler(server, &recovery_implicit_get_uri);
    } else {
        /* URI handler for getting web server files */
        httpd_uri_t common_get_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = rest_common_get_handler,
            .user_ctx = rest_context
        };
        httpd_register_uri_handler(server, &common_get_uri);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    // Start websocket log handler thread
    xTaskCreate(&websocket_log_handler, "websocket_log", 4096, NULL, 2, NULL);

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    start_dns_server(&dns_config);

    return ESP_OK;
err_start:
    free(rest_context);
err:
    return ESP_FAIL;
}
