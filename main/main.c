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
#define SERVO_TARGET_ANGLE      160
#define SERVO_REST_ANGLE        20
#define SERVO_AUTO_RETURN_MS    2000    // 自动返回延迟时间

// Button
#define BUTTON_LONG_PRESS_MS    3000
#define BUTTON_DEBOUNCE_MS      50

// Zigbee - Hue Compatibility
#define HA_HUE_ENDPOINT         10      /* Endpoint 10 is standard for Hue */
// Zigbee ZCL 字符串必须使用 Pascal 格式: 第一个字节是长度
// 使用真实制造商名称,Hue网关会验证设备身份
#define MANUFACTURER_NAME       "\x09""ESPRESSIF"                   // 0x09 = 9 字符
#define MODEL_IDENTIFIER        "\x10""ESP32H2_ZB_SWITCH"           // 0x10 = 16 字符
#define DATE_CODE               "\x08""20240101"                    // 0x08 = 8 字符
// Hue网关常用信道: 11, 15, 20, 25
#define ZB_HUE_CHANNEL_MASK     ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

// Basic Cluster 必需属性
#define ZCL_VERSION             0x03    // ZCL Version 3
#define APPLICATION_VERSION     0x01
#define STACK_VERSION           0x02
#define HW_VERSION              0x01
#define POWER_SOURCE            0x01    // 0x01 = Mains (single phase)

/* ============================================================================
 * Globals
 * ============================================================================ */

static led_strip_handle_t g_led_strip = NULL;
static bool g_zigbee_connected = false;
static bool g_servo_at_target = false;  // true = 在目标位置, false = 在rest位置
static esp_timer_handle_t g_servo_timer = NULL;  // 自动返回定时器

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
            case LED_CONNECTED: 
                set_led(0, 20, 0); // Green Solid
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

// 设置舵机角度
static void servo_set_angle(int angle) {
    int duty = DUTY_AT_0_DEG + (angle * (DUTY_AT_180_DEG - DUTY_AT_0_DEG) / 180);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

// 舵机返回rest位置 (定时器回调)
static void servo_return_callback(void *arg) {
    ESP_LOGI(TAG, "Servo: Auto return to REST (Angle: %d)", SERVO_REST_ANGLE);
    servo_set_angle(SERVO_REST_ANGLE);
    g_servo_at_target = false;
}

// 取消自动返回定时器
static void servo_cancel_timer(void) {
    if (g_servo_timer != NULL) {
        esp_timer_stop(g_servo_timer);
    }
}

// 启动自动返回定时器
static void servo_start_timer(void) {
    if (g_servo_timer != NULL) {
        esp_timer_stop(g_servo_timer);  // 先停止之前的定时器
        esp_timer_start_once(g_servo_timer, SERVO_AUTO_RETURN_MS * 1000);  // 微秒
    }
}

// 播放动作: 移动到目标位置,2000ms后自动返回
static void servo_play(void) {
    ESP_LOGI(TAG, "Servo: PLAY -> Target (Angle: %d)", SERVO_TARGET_ANGLE);
    servo_set_angle(SERVO_TARGET_ANGLE);
    g_servo_at_target = true;
    servo_start_timer();
}

// 立即返回rest位置,取消定时器
static void servo_rest(void) {
    ESP_LOGI(TAG, "Servo: REST immediately (Angle: %d)", SERVO_REST_ANGLE);
    servo_cancel_timer();
    servo_set_angle(SERVO_REST_ANGLE);
    g_servo_at_target = false;
}

// 按钮触发的舵机动作
// - 如果在rest位置: 执行play (移动到目标,2000ms后自动返回)
// - 如果在目标位置: 立即返回rest,取消定时器
static void servo_toggle(void) {
    if (g_servo_at_target) {
        // 电机在目标位置,立即返回
        servo_rest();
    } else {
        // 电机在rest位置,执行play
        servo_play();
    }
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
            if (message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
                bool on = *(bool *)message->attribute.data.value;
                ESP_LOGI(TAG, "Zigbee Set On/Off: %d", on);
                if (on) {
                    servo_play();  // ON -> 执行play动作
                } else {
                    servo_rest();  // OFF -> 立即返回rest
                }
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
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Signal: SKIP_STARTUP");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;
        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
            ESP_LOGI(TAG, "Signal: DEVICE_FIRST_START (status: %s)", esp_err_to_name(err_status));
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "Device first start - starting network steering...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                g_led_state = LED_PAIRING;
            } else {
                ESP_LOGE(TAG, "Stack Start Failed: %s", esp_err_to_name(err_status));
                g_led_state = LED_ERROR;
            }
            break;
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            ESP_LOGI(TAG, "Signal: DEVICE_REBOOT (status: %s)", esp_err_to_name(err_status));
            if (err_status == ESP_OK) {
                ESP_LOGI(TAG, "Device reboot - starting network steering...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
                g_led_state = LED_PAIRING;
            } else {
                ESP_LOGE(TAG, "Stack Start Failed: %s", esp_err_to_name(err_status));
                g_led_state = LED_ERROR;
            }
            break;
        case ESP_ZB_BDB_SIGNAL_STEERING:
            ESP_LOGI(TAG, "Signal: STEERING (status: %s)", esp_err_to_name(err_status));
            if (err_status == ESP_OK) {
                esp_zb_ieee_addr_t ext_pan_id;
                esp_zb_get_extended_pan_id(ext_pan_id);
                ESP_LOGI(TAG, "SUCCESS! Joined network:");
                ESP_LOGI(TAG, "  PAN ID: 0x%04x", esp_zb_get_pan_id());
                ESP_LOGI(TAG, "  Short Addr: 0x%04x", esp_zb_get_short_address());
                ESP_LOGI(TAG, "  Channel: %d", esp_zb_get_current_channel());
                g_led_state = LED_CONNECTED;
                g_zigbee_connected = true;
            } else {
                ESP_LOGW(TAG, "Steering Failed (err: 0x%x), retrying in 1s...", err_status);
                esp_zb_scheduler_alarm(retry_steering, 0, 1000);
            }
            break;
        case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
            // Ignore sleep signal
            break;
        default:
            ESP_LOGI(TAG, "Signal: %d (status: %s)", sig_type, esp_err_to_name(err_status));
            break;
    }
}

static void esp_zb_task(void *pvParameters) {
    ESP_LOGI(TAG, "Zigbee Task Started");
    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = { .max_children = 10 },
    };
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(HA_HUE_ENDPOINT, &light_cfg);
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, HA_HUE_ENDPOINT);
    esp_zb_attribute_list_t *basic_cluster = esp_zb_cluster_list_get_cluster(cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    
    // 修改 Basic Cluster 属性以确保 Hue 兼容性
    // 注意: ZCL_VERSION (0x0) 和 POWER_SOURCE (0x7) 已经存在,使用 update 而非 add
    uint8_t zcl_version = ZCL_VERSION;
    uint8_t app_version = APPLICATION_VERSION;
    uint8_t stack_version = STACK_VERSION;
    uint8_t hw_version = HW_VERSION;
    uint8_t power_source = POWER_SOURCE;

    // 更新已存在的属性
    esp_zb_cluster_update_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_ZCL_VERSION_ID, &zcl_version);
    esp_zb_cluster_update_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_POWER_SOURCE_ID, &power_source);

    // 添加新属性
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_APPLICATION_VERSION_ID, &app_version);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_STACK_VERSION_ID, &stack_version);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_HW_VERSION_ID, &hw_version);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, MODEL_IDENTIFIER);
    esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_DATE_CODE_ID, DATE_CODE);
    
    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
    // 不要在启动时擦除NVRAM,否则每次重启都会丢失网络配置
    // esp_zb_nvram_erase_at_start(true);
    esp_zb_set_primary_network_channel_set(ZB_HUE_CHANNEL_MASK);
    
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

/* ============================================================================
 * Button Task
 * ============================================================================ */

static void button_task(void *pvParameters) {
    bool btn_pressed = false;          // 按钮当前是否处于按下状态
    int64_t press_start_ms = 0;        // 按下开始时间
    bool long_press_handled = false;   // 长按是否已处理

    while (1) {
        int level = gpio_get_level(BUTTON_GPIO);  // 0=按下, 1=松开

        if (level == 0 && !btn_pressed) {
            // 按钮刚按下 - 防抖
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(BUTTON_GPIO) == 0) {
                // 确认按下
                btn_pressed = true;
                press_start_ms = esp_timer_get_time() / 1000;
                long_press_handled = false;
                ESP_LOGI(TAG, "BTN: pressed");
            }
        }
        else if (level == 0 && btn_pressed && !long_press_handled) {
            // 按钮持续按下 - 检查长按
            int64_t hold_time = esp_timer_get_time() / 1000 - press_start_ms;
            if (hold_time > BUTTON_LONG_PRESS_MS) {
                ESP_LOGW(TAG, "BTN: LONG PRESS - Factory Reset");
                g_led_state = LED_RESET_WARN;
                long_press_handled = true;
            }
        }
        else if (level == 1 && btn_pressed) {
            // 按钮松开 - 防抖
            vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(BUTTON_GPIO) == 1) {
                // 确认松开
                int64_t hold_time = esp_timer_get_time() / 1000 - press_start_ms;
                ESP_LOGI(TAG, "BTN: released (held %lldms, long=%d)", hold_time, long_press_handled);

                if (long_press_handled) {
                    // 长按已触发,执行factory reset
                    esp_zb_factory_reset();
                } else {
                    // 短按,执行servo toggle
                    servo_toggle();
                    if (g_zigbee_connected) {
                        report_attribute();
                    }
                }

                btn_pressed = false;
                press_start_ms = 0;
                long_press_handled = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));  // 更快的轮询
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\n=== HUE ROUTER BOOT ===\n");
    
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    
    led_strip_config_t strip_config = { .strip_gpio_num = RGB_LED_GPIO, .max_leds = 1 };
    led_strip_rmt_config_t rmt_config = { .resolution_hz = 10 * 1000 * 1000 };
    led_strip_new_rmt_device(&strip_config, &rmt_config, &g_led_strip);
    
    ledc_timer_config_t timer_cfg = { .speed_mode = LEDC_MODE, .timer_num = LEDC_TIMER, .duty_resolution = LEDC_DUTY_RES, .freq_hz = LEDC_FREQUENCY, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&timer_cfg);
    ledc_channel_config_t channel_cfg = { .speed_mode = LEDC_MODE, .channel = LEDC_CHANNEL, .timer_sel = LEDC_TIMER, .intr_type = LEDC_INTR_DISABLE, .gpio_num = SERVO_GPIO, .duty = DUTY_AT_0_DEG, .hpoint = 0 };
    ledc_channel_config(&channel_cfg);
    
    gpio_config_t btn_cfg = { .pin_bit_mask = (1ULL << BUTTON_GPIO), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&btn_cfg);

    // 初始化舵机自动返回定时器
    esp_timer_create_args_t timer_args = {
        .callback = servo_return_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "servo_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &g_servo_timer));

    esp_zb_platform_config_t zb_platform_cfg = { .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE }, .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE } };
    esp_zb_platform_config(&zb_platform_cfg);

    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);
    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, NULL);
    xTaskCreate(esp_zb_task, "zb_task", 8192, NULL, 10, NULL);
}
