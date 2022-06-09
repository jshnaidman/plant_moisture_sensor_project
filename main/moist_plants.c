/* HTTPS GET Example using plain mbedTLS sockets
 *
 * Contacts the howsmyssl.com API via TLS v1.2 and reads a JSON
 * response.
 *
 * Adapted from the ssl_client1 example in mbedtls.
 *
 * Original Copyright (C) 2006-2016, ARM Limited, All Rights Reserved, Apache 2.0 License.
 * Additions Copyright (C) Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD, Apache 2.0 License.
 *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_system.h"

#include "soc/rtc_cntl_reg.h"

#include "driver/rtc_io.h"

#include "esp_sleep.h"
#include "esp32/ulp.h"

#include "secrets.h"
#include "certificates.h"
#include "ulp_main.h"
#define MAX_RETRY 30
/* Constants that aren't configurable in menuconfig */
#define PUSH_URL "api.pushbullet.com"
#define HOST_SERVER "https://api.pushbullet.com/v2/pushes"
#define HTTP_PATH "/v2/pushes"

#define MINIMUM_SENDING_INTERVAL 6*60*60 // 4 hours

static const char *TAG = "MOIST";

extern const uint8_t measurement_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t measurement_bin_end[] asm("_binary_ulp_main_bin_end");

// static esp_adc_cal_characteristics_t *adc_chars;

/* ADC parameters */
#define ATTENUATION ADC_ATTEN_DB_0
#define UNIT ADC_UNIT_1
#define CHANNEL_1 ADC1_CHANNEL_5             // this is GPIO 33
#define CHANNEL_2 ADC1_CHANNEL_4             // this is GPIO 32
#define CHANNEL_3 ADC1_CHANNEL_6             // this is GPIO 34
#define WIDTH ADC_WIDTH_BIT_9                // Resolution doesn't matter here
#define NO_OF_SAMPLES 64                     // we take multiple samples to reduce noise. Also stick a 0.1uF capacitor on readings.
#define DEFAULT_VREF 1100                    // The actual voltage doesn't matter, it's just a proxy for moisture. Experiment for correct moisture threshold.
#define test_sleep_interval 500000         // 0.5 seconds

void start_ulp_program()
{
    /* Start the program */
    esp_err_t err = ulp_run(&ulp_entry - RTC_SLOW_MEM);
    ESP_ERROR_CHECK(err);
}

// Init ADC
void get_ready_to_feel_moist()
{
    adc1_config_width(WIDTH);
    adc1_config_channel_atten(CHANNEL_1, ATTENUATION);
    adc1_config_channel_atten(CHANNEL_2, ATTENUATION);
    adc1_config_channel_atten(CHANNEL_3, ATTENUATION);

    adc1_ulp_enable(); 
}

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAX_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .channel = 11,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT)
    {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
    }
    else if (bits & WIFI_FAIL_BIT)
    {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
    }
    else
    {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);
}

esp_err_t _http_event_handle(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER");
        printf("%.*s", evt->data_len, (char *)evt->data);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            printf("%.*s", evt->data_len, (char *)evt->data);
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    }
    return ESP_OK;
}

void https_send_pushbullet_measurement(float reading_1, float reading_2)
{
    esp_http_client_config_t config = {
        .url = HOST_SERVER,
        .method = HTTP_METHOD_POST,
        .event_handler = _http_event_handle,
        .host = PUSH_URL,
        .path = HTTP_PATH,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = pushbullet_root_cert_pem};

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Access-Token", PUSHBULLET_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    const char *REQUEST_BODY = "{\"type\":\"note\",\"title\":\"moisture_voltage\",\"body\":\"GARDEN PLANTS NEED MOISTURE!!\\n\\nMeasurements: \\n\\t%3.1f%% dry\\n\\t%3.1f%% dry\",\"device_iden\":\"" PHONE_IDENTIFIER "\",\"guid\":\"%u\"}\r\n";

    uint32_t rand_int = esp_random();
    int16_t body_len = snprintf(NULL, 0, REQUEST_BODY, reading_1, reading_2, rand_int);
    char req[body_len];
    snprintf(req, body_len, REQUEST_BODY, reading_1, reading_2, rand_int);
    ESP_LOGI(TAG, "%s", req);

    // body_len-1 because we don't wanna include null-terminating character in post request
    esp_http_client_set_post_field(client, req, body_len - 1);

    esp_http_client_perform(client);

    ESP_LOGI(TAG, "Status = %d, content_length = %d",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));

    esp_http_client_cleanup(client);
}

void init_ulp() {
    ESP_ERROR_CHECK(ulp_load_binary(0, measurement_bin_start,
                          (measurement_bin_end - measurement_bin_start) / sizeof(uint32_t)));

    /* Set ULP wake up period to 5s */
    ulp_set_wakeup_period(0, test_sleep_interval);
    esp_deep_sleep_disable_rom_logging(); // suppress boot messages
    rtc_gpio_isolate(GPIO_NUM_12);
    rtc_gpio_isolate(GPIO_NUM_15);
    
}



void app_main(void)
{
    ESP_LOGI(TAG, "Initializing NVS");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    

    if (cause != ESP_SLEEP_WAKEUP_ULP) {

        
        ESP_LOGI(TAG, "Initializing ULP");
        init_ulp();

        // Config ADC
        ESP_LOGI(TAG, "Initializing ADC");
        get_ready_to_feel_moist();

    }
    else {
        
        float reading_1 = (ulp_channel_1_last_result & 0x0000FFFF) - 70;
        float reading_2 = (ulp_channel_2_last_result & 0x0000FFFF) - 70;
        reading_1 /= 1.3;
        reading_2 /= 1.3;
        // ulp_last_measurement_time = *RTC_CNTL_TIME1_REG & 0xFFFF;
        ESP_LOGI(TAG, "Initializing Wifi Station");
        wifi_init_sta(); 
        https_send_pushbullet_measurement(reading_1, reading_2);
    }
    ESP_LOGI(TAG, "debug_1: %X, debug_2: %X", ulp_debug_1, ulp_debug_2);
    ESP_LOGI(TAG, "Last Battery Notif Time: %X", ulp_last_battery_notification_time);
    ESP_LOGI(TAG, "Last Battery Notif Time: %X", ulp_last_battery_notification_time);
    ESP_LOGI(TAG, "Starting ULP");
    start_ulp_program();
    ESP_ERROR_CHECK( esp_sleep_enable_ulp_wakeup() );
    ESP_LOGI(TAG, "Going to deep sleep");
    esp_deep_sleep_start();
    
}
