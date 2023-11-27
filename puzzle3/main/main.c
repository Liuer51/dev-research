#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_tls.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "esp_spiffs.h"
#include <driver/uart.h>

#define EX_UART_RX_BUF_SIZE 1024
#define EX_UART_NUM UART_NUM_0
#define EX_UART_TXD_PIN 1
#define EX_UART_RXD_PIN 3
QueueHandle_t ex_uart_queue = NULL;
static const char *TAG = "https";
static const char *SPIFFS = "文件系统";

void SPIFFS_Init()
{
    ESP_LOGI(SPIFFS, "正在初始化SPIFFS");

    esp_vfs_spiffs_conf_t conf;
    memset(&conf, 0, sizeof(conf));

    conf.base_path = "/spiffs";
    conf.partition_label = NULL;
    conf.max_files = 5;
    conf.format_if_mount_failed = true;

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(SPIFFS, "无法装载或格式化文件系统");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(SPIFFS, "找不到SPIFFS分区");
        }
        else
        {
            ESP_LOGE(SPIFFS, "未能初始化SPIFF (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(SPIFFS, "无法获取SPIFFS分区信息 (%s)", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(SPIFFS, "分区大小：总计: %d, used: %d", total, used);
    }
}

int FLASH_Write(void *data, int len)
{
    FILE *fd = fopen("/spiffs/hello_this_is_long_name_products.json", "w");
    if (!fd)
    {
        return -1;
    }
    else
    {
        fwrite(data, len, 1, fd);
    }
    fclose(fd);
    return 0;
};

void uart_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };
    uart_driver_install(EX_UART_NUM, EX_UART_RX_BUF_SIZE * 2, EX_UART_RX_BUF_SIZE * 2, 20, &ex_uart_queue, 0);
    uart_param_config(EX_UART_NUM, &uart_config);
    uart_set_pin(EX_UART_NUM, EX_UART_TXD_PIN, EX_UART_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    char *dtmp = (char *)malloc(EX_UART_RX_BUF_SIZE);
    for (;;)
    {
        if (xQueueReceive(ex_uart_queue, (void *)&event, portMAX_DELAY))
        {
            bzero(dtmp, EX_UART_RX_BUF_SIZE);
            switch (event.type)
            {
            case UART_DATA:
            {
                uart_read_bytes(EX_UART_NUM, dtmp, event.size, portMAX_DELAY);
                printf("uart data%s\n", dtmp);

                if (!strcmp(dtmp,"fetch",5))
                {
                    ESP_LOGE("UART", "执行步骤一\n");
                    esp_wifi_start();
                }
            }
            break;
            default:
                printf("uart事件类型： %d\n", event.type);
                break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}


esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;
    static int output_len;
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);

        if (!esp_http_client_is_chunked_response(evt->client))
        {
            if (evt->user_data)
            {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
            }
            else
            {
                if (output_buffer == NULL)
                {
                    output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client));
                    output_len = 0;
                    if (output_buffer == NULL)
                    {
                        ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
            }
            output_len += evt->data_len;
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL)
        {
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        if (output_buffer != NULL)
        {
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    default:
        break;
    }
    return ESP_OK;
}

void https(void *param)
{
    char local_response_buffer[1024] = {0};
    esp_http_client_config_t config = {
        .url = "https://dummyjson.com/products/1",
        .method = HTTP_METHOD_GET,
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI("https", "Status = %d, content_length = %lld", esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "local_response_buffer：%s", local_response_buffer);

    cJSON *pJsonRoot = cJSON_Parse(local_response_buffer);

    if (pJsonRoot != NULL)
    {
        cJSON *pMacAdress = cJSON_GetObjectItem(pJsonRoot, "brand");
        char *brand = pMacAdress->valuestring;
        char *buffer = (char *)malloc(strlen(brand) + 1);
        memmove(buffer, brand, strlen(brand) + 1);

        ESP_LOGI(TAG, "C ESP_LOGE 输出-->：brand %s", buffer);
        printf("C printf 输出-->：brand %s\n", buffer);

        FLASH_Write(buffer, strlen(buffer));

        free(buffer);
        buffer = NULL;
    }

    vTaskDelay(3 * 1000 / portTICK_PERIOD_MS);
    esp_http_client_cleanup(client);
    esp_wifi_stop();
    vTaskDelete(NULL);
}

void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI("ESP32", "ESP32电台与AP断开连接");
        esp_wifi_connect();
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI("ESP32", "IP地址: " IPSTR, IP2STR(&event->ip_info.ip));
        xTaskCreate(&https, "https_get_task", 8192, NULL, 5, NULL);
    }
}
void app_main()
{

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);

    uart_init();
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 0, NULL);
    
    esp_netif_init();
    esp_event_loop_create_default();
    SPIFFS_Init();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_netif_create_default_wifi_sta();

    wifi_sta_config_t cfg_sta = {
        .ssid = "UPGRADE_AP",
        .password = "TEST1234",
    };
    esp_wifi_set_config(WIFI_IF_STA, (wifi_config_t *)&cfg_sta);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
    esp_wifi_start();
}
