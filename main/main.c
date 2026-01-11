/**
 * @file main.c
 * @brief Hue Compatible Zigbee Router (Smart Plug / Light) with Servo Control
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "led_strip.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

static const char *TAG = "ESP_HUE";

/* ============================================================================
 * Configuration
 * ============================================================================ */

// Hardware
#define SERVO_GPIO              5
#define BUTTON_GPIO             9
#define RGB_LED_GPIO            8

// Servo Settings
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY          50
#define DUTY_AT_0_DEG           205
#define DUTY_AT_180_DEG         1024
#define SERVO_TARGET_ANGLE      180
#define SERVO_REST_ANGLE        20

// Button
#define BUTTON_LONG_PRESS_MS    3000
#define BUTTON_DEBOUNCE_MS      50

// Zigbee - Hue Compatibility
#define HA_HUE_ENDPOINT         11
#define MANUFACTURER_NAME       "Signify Netherlands B.V."
#define MODEL_IDENTIFIER        "LOM001" // Hue Smart Plug
#define ZB_HUE_CHANNEL_MASK     (1<<11 | 1<<15 | 1<<20 | 1<<25) // Hue preferred channels

/* ============================================================================
 * Globals
 * ============================================================================ */

static led_strip_handle_t g_led_strip = NULL;
static bool g_zigbee_connected = false;
static bool g_servo_state = false;

typedef enum {
    LED_OFF,
    LED_PAIRING, // Blue Blink
    LED_CONNECTED, // Green Solid
    LED_ERROR, // Red Blink
    LED_RESET_WARN // Blue Solid
} led_state_t;

static led_state_t g_led_state = LED_OFF;

/* ============================================================================
 * Hardware Control (Servo & LED)
 * ============================================================================ */

static void set_led(uint8_t r, uint8_t g, uint8_t b) {
    if (g_led_strip) {
        led_strip_set_pixel(g_led_strip, 0, r, g, b);
        led_strip_refresh(g_led_strip);
    }
}

static void led_task(void *pvParameters) {
    int tick = 0;
    while (1) {
        switch (g_led_state) {
            case LED_OFF: set_led(0, 0, 0); break;
            case LED_PAIRING: // Blue Blink
                if (tick++ % 2 == 0) set_led(0, 0, 50); else set_led(0, 0, 0);
                break;
            case LED_CONNECTED: // Green (3s timeout handled externally or keep on)
                set_led(0, 20, 0);
                break;
            case LED_ERROR: // Red Blink
                if (tick++ % 5 == 0) set_led(20, 0, 0); else set_led(0, 0, 0);
                break;
            case LED_RESET_WARN: // Blue Solid
                set_led(0, 0, 50);
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void set_servo(bool on) {
    int angle = on ? SERVO_TARGET_ANGLE : SERVO_REST_ANGLE;
    int duty = DUTY_AT_0_DEG + (angle * (DUTY_AT_180_DEG - DUTY_AT_0_DEG) / 180);
    
    ESP_LOGI(TAG, "Servo: %s (Angle: %d, Duty: %d)", on ? "ON" : "OFF", angle, duty);
    
    // Smooth movement logic could be added here, currently instant for simplicity in Router code
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
    g_servo_state = on;
}

/* ============================================================================
 * Zigbee Logic
 * ============================================================================ */

static void report_attribute(void) {
    esp_zb_zcl_report_attr_cmd_t report_cmd;
    report_cmd.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
    report_cmd.attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
    report_cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
    report_cmd.zcl_basic_cmd.src_endpoint = HA_HUE_ENDPOINT;
    
    esp_zb_zcl_report_attr_cmd_req(&report_cmd);
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message) {
    if (message->info.dst_endpoint == HA_HUE_ENDPOINT) {
        if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF) {
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
                message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL) {
                
                bool on = *(bool *)message->attribute.data.value;
                ESP_LOGI(TAG, "Zigbee On/Off: %d", on);
                set_servo(on);
                
                // If turning on, auto-off timer could be added here if needed
                // For now, just follow command
            }
        }
    }
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message) {
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
    }
    return ESP_OK;
}



static void retry_steering(uint8_t arg) {
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING | ESP_ZB_BDB_MODE_TOUCHLINK_TARGET);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee Stack Initialized");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err_status == ESP_OK) {
                if (esp_zb_bdb_is_factory_new()) {
                    ESP_LOGI(TAG, "Factory New -> Start Steering");
                    g_led_state = LED_PAIRING;
                    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING | ESP_ZB_BDB_MODE_TOUCHLINK_TARGET);
                } else {
                    ESP_LOGI(TAG, "Rejoined Network");
                    g_led_state = LED_CONNECTED;
                    g_zigbee_connected = true;
                }
            } else {
                ESP_LOGE(TAG, "Device Start Failed: %s", esp_err_to_name(err_status));
                g_led_state = LED_ERROR;
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "Joined Network");
                g_led_state = LED_CONNECTED;
                g_zigbee_connected = true;
            } else {
                ESP_LOGW(TAG, "Steering Failed, Retry in 1s...");
                esp_zb_scheduler_alarm(retry_steering, 0, 1000);
            }
            break;
            
        case ESP_ZB_ZDO_SIGNAL_LEAVE:
            ESP_LOGI(TAG, "Left Network");
            g_zigbee_connected = false;
            g_led_state = LED_OFF;
            break;

        default: break;
    }
}

static void esp_zb_task(void *pvParameters) {
    // 1. Router Config
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {
            .max_children = 10,
        },
    };
    esp_zb_init(&zb_nwk_cfg);

    // 2. On/Off Light Config
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(HA_HUE_ENDPOINT, &light_cfg);
    
    // 3. Customize Basic Cluster
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, HA_HUE_ENDPOINT);
    esp_zb_attribute_list_t *basic_cluster = esp_zb_cluster_list_get_cluster(cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, MODEL_IDENTIFIER);
    
    // 4. Register & Start
    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_nvram_erase_at_start(true); // Ensure clean start to fix stack assertion
    esp_zb_set_primary_network_channel_set(ZB_HUE_CHANNEL_MASK); // 11, 15, 20, 25
    
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

/* ============================================================================
 * Button Logic
 * ============================================================================ */

static void button_task(void *pvParameters) {
    int last_level = 1;
    int64_t press_start = 0;
    bool handled = false;

    while (1) {
        int level = gpio_get_level(BUTTON_GPIO);
        
        if (last_level == 1 && level == 0) { // Press
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                press_start = esp_timer_get_time() / 1000;
                handled = false;
            }
        }
        
        if (level == 0 && !handled) { // Holding
            if ((esp_timer_get_time() / 1000 - press_start) > BUTTON_LONG_PRESS_MS) {
                ESP_LOGW(TAG, "Long Press Detected");
                g_led_state = LED_RESET_WARN; // Blue Solid
                handled = true;
            }
        }
        
        if (last_level == 0 && level == 1) { // Release
            if (handled) {
                ESP_LOGW(TAG, "Factory Resetting...");
                esp_zb_factory_reset();
            } else if (press_start > 0) {
                // Toggle Servo
                set_servo(!g_servo_state);
                report_attribute();
            }
            press_start = 0;
        }
        
        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_erase()); // ERASE NVS to fix boot loop
    ESP_ERROR_CHECK(nvs_flash_init());
    
    // LED
    led_strip_config_t strip_config = { .strip_gpio_num = RGB_LED_GPIO, .max_leds = 1 };
    led_strip_rmt_config_t rmt_config = { .resolution_hz = 10 * 1000 * 1000 };
    led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip);
    led_strip_clear(g_led_strip);
    
    // Servo
    ledc_timer_config_t timer_cfg = { .speed_mode = LEDC_MODE, .timer_num = LEDC_TIMER, .duty_resolution = LEDC_DUTY_RES, .freq_hz = LEDC_FREQUENCY, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&timer_cfg);
    ledc_channel_config_t channel_cfg = { .speed_mode = LEDC_MODE, .channel = LEDC_CHANNEL, .timer_sel = LEDC_TIMER, .intr_type = LEDC_INTR_DISABLE, .gpio_num = SERVO_GPIO, .duty = DUTY_AT_0_DEG, .hpoint = 0 };
    ledc_channel_config(&channel_cfg);
    
    // Button
    gpio_config_t btn_cfg = { .pin_bit_mask = (1ULL << BUTTON_GPIO), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&btn_cfg);

    // Zigbee Config
    esp_zb_platform_config_t zb_platform_cfg = { .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE }, .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE } };
    esp_zb_platform_config(&zb_platform_cfg);

    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, NULL);
    xTaskCreate(esp_zb_task, "zb_task", 8192, NULL, 5, NULL);
}