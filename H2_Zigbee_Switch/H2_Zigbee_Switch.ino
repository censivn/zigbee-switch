/**
 * @file H2_Zigbee_Switch.ino
 * @brief ESP32-H2 Zigbee Router - 使用底层API实现完全控制
 *
 * 使用ESP Zigbee SDK底层API，不使用Arduino Zigbee封装
 * 这样可以完全控制网络加入过程和获得详细调试信息
 */

#ifndef ZIGBEE_MODE_ZCZR
#error "请在 Arduino IDE 中选择: Tools -> Zigbee mode -> Zigbee ZCZR (coordinator/router)"
#endif

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ========== 调试选项 ==========
#define AUTO_FACTORY_RESET    true   // 启动时自动重置

/* ============================================================================
 * 硬件配置
 * ============================================================================ */
#define SERVO_GPIO              5
#define BUTTON_GPIO             9
#define RGB_LED_GPIO            8

/* ============================================================================
 * 舵机配置
 * ============================================================================ */
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY          50
#define DUTY_AT_0_DEG           205
#define DUTY_AT_180_DEG         1024
#define SERVO_TARGET_ANGLE      160
#define SERVO_REST_ANGLE        20
#define SERVO_AUTO_RETURN_MS    2000

/* ============================================================================
 * Zigbee 配置
 * ============================================================================ */
#define ENDPOINT_ID             1
// ZCL字符串格式: 第一个字节是长度
#define MANUFACTURER_NAME       "\x09""Espressif"
#define MODEL_IDENTIFIER        "\x09""ZB_Switch"

// 目标信道 - 你的Z2M在信道11
#define TARGET_CHANNEL          11
#define CHANNEL_MASK            (1 << TARGET_CHANNEL)

/* ============================================================================
 * 全局变量
 * ============================================================================ */
typedef enum {
  LED_OFF,
  LED_PAIRING,
  LED_CONNECTED,
  LED_ERROR
} LedState_t;

static LedState_t g_ledState = LED_OFF;
static bool g_zigbeeConnected = false;
static bool g_servoAtTarget = false;
static esp_timer_handle_t g_servoTimer = NULL;

/* ============================================================================
 * LED 控制 (WS2812)
 * ============================================================================ */
void setLED(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_LED_GPIO, r, g, b);
}

void ledTask(void *pvParameters) {
  int tick = 0;
  while (1) {
    switch (g_ledState) {
      case LED_OFF: setLED(0, 0, 0); break;
      case LED_PAIRING:
        setLED(0, 0, (tick % 2) ? 50 : 0);
        break;
      case LED_CONNECTED:
        setLED(0, 20, 0);
        break;
      case LED_ERROR:
        setLED((tick % 2) ? 50 : 0, 0, 0);
        break;
    }
    tick++;
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

/* ============================================================================
 * 舵机控制
 * ============================================================================ */
void servoSetAngle(int angle) {
  int duty = DUTY_AT_0_DEG + (angle * (DUTY_AT_180_DEG - DUTY_AT_0_DEG) / 180);
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

void servoReturnCallback(void *arg) {
  Serial.println("[Servo] Auto return to REST");
  servoSetAngle(SERVO_REST_ANGLE);
  g_servoAtTarget = false;
}

void servoPlay() {
  Serial.printf("[Servo] PLAY -> %d°\n", SERVO_TARGET_ANGLE);
  servoSetAngle(SERVO_TARGET_ANGLE);
  g_servoAtTarget = true;
  if (g_servoTimer) {
    esp_timer_stop(g_servoTimer);
    esp_timer_start_once(g_servoTimer, SERVO_AUTO_RETURN_MS * 1000);
  }
}

void servoRest() {
  Serial.printf("[Servo] REST -> %d°\n", SERVO_REST_ANGLE);
  if (g_servoTimer) esp_timer_stop(g_servoTimer);
  servoSetAngle(SERVO_REST_ANGLE);
  g_servoAtTarget = false;
}

/* ============================================================================
 * Zigbee 属性处理
 * ============================================================================ */
static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message) {
  if (message->info.dst_endpoint != ENDPOINT_ID) return ESP_OK;

  if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
      message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
    bool on = *(bool *)message->attribute.data.value;
    Serial.printf("[Zigbee] ON/OFF command received: %s\n", on ? "ON" : "OFF");
    if (on) servoPlay(); else servoRest();
  }
  return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message) {
  if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
    return zb_attribute_handler((esp_zb_zcl_set_attr_value_message_t *)message);
  }
  return ESP_OK;
}

/* ============================================================================
 * Zigbee 信号处理 - 关键的网络状态回调
 * ============================================================================ */
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask) {
  esp_zb_bdb_start_top_level_commissioning(mode_mask);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
  uint32_t *p_sg_p = signal_struct->p_app_signal;
  esp_err_t err_status = signal_struct->esp_err_status;
  esp_zb_app_signal_type_t sig_type = (esp_zb_app_signal_type_t)*p_sg_p;

  switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
      Serial.println("[ZB-SIG] SKIP_STARTUP -> Starting initialization");
      esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
      break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
      Serial.printf("[ZB-SIG] DEVICE_FIRST_START (status: %s)\n", esp_err_to_name(err_status));
      if (err_status == ESP_OK) {
        Serial.println("[ZB-SIG] -> Starting NETWORK STEERING...");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
      } else {
        Serial.println("[ZB-SIG] -> ERROR! Stack init failed");
        g_ledState = LED_ERROR;
      }
      break;

    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
      Serial.printf("[ZB-SIG] DEVICE_REBOOT (status: %s)\n", esp_err_to_name(err_status));
      if (err_status == ESP_OK) {
        Serial.println("[ZB-SIG] -> Starting NETWORK STEERING...");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
      } else {
        Serial.println("[ZB-SIG] -> ERROR!");
        g_ledState = LED_ERROR;
      }
      break;

    case ESP_ZB_BDB_SIGNAL_STEERING:
      Serial.printf("[ZB-SIG] STEERING result (status: 0x%X = %s)\n", err_status, esp_err_to_name(err_status));
      if (err_status == ESP_OK) {
        // 成功加入网络!
        Serial.println("\n========================================");
        Serial.println("  SUCCESS! JOINED ZIGBEE NETWORK!");
        Serial.println("========================================");
        Serial.printf("  PAN ID:     0x%04X\n", esp_zb_get_pan_id());
        Serial.printf("  Short Addr: 0x%04X\n", esp_zb_get_short_address());
        Serial.printf("  Channel:    %d\n", esp_zb_get_current_channel());
        esp_zb_ieee_addr_t ext_pan;
        esp_zb_get_extended_pan_id(ext_pan);
        Serial.printf("  Ext PAN:    %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n",
          ext_pan[7], ext_pan[6], ext_pan[5], ext_pan[4],
          ext_pan[3], ext_pan[2], ext_pan[1], ext_pan[0]);
        Serial.println("========================================\n");
        g_ledState = LED_CONNECTED;
        g_zigbeeConnected = true;
      } else {
        Serial.println("[ZB-SIG] -> Steering FAILED! Retrying in 1 second...");
        // 1秒后重试
        esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                               ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
      }
      break;

    case ESP_ZB_COMMON_SIGNAL_CAN_SLEEP:
      // 忽略睡眠信号
      break;

    case ESP_ZB_ZDO_SIGNAL_LEAVE:
      Serial.println("[ZB-SIG] LEAVE - Left network");
      g_zigbeeConnected = false;
      g_ledState = LED_PAIRING;
      break;

    default:
      Serial.printf("[ZB-SIG] Signal %d (status: %s)\n", sig_type, esp_err_to_name(err_status));
      break;
  }
}

/* ============================================================================
 * Zigbee 主任务
 * ============================================================================ */
static void zigbee_task(void *pvParameters) {
  Serial.println("[ZB-TASK] Starting Zigbee task...");

  // 1. 初始化Zigbee栈配置
  esp_zb_cfg_t zb_nwk_cfg = {
    .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
    .install_code_policy = false,
    .nwk_cfg = {
      .zczr_cfg = {
        .max_children = 10
      }
    }
  };
  esp_zb_init(&zb_nwk_cfg);
  Serial.println("[ZB-TASK] Stack initialized (ROUTER mode)");

  // 2. 设置信道掩码 - 必须在init之后, start之前
  Serial.printf("[ZB-TASK] Setting channel mask to 0x%08X (channel %d)\n", CHANNEL_MASK, TARGET_CHANNEL);
  esp_zb_set_primary_network_channel_set(CHANNEL_MASK);

  // 3. 创建On/Off Light设备端点
  esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
  esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(ENDPOINT_ID, &light_cfg);

  // 4. 修改Basic Cluster属性
  esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, ENDPOINT_ID);
  esp_zb_attribute_list_t *basic_cluster = esp_zb_cluster_list_get_cluster(
    cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

  esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void*)MANUFACTURER_NAME);
  esp_zb_basic_cluster_add_attr(basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void*)MODEL_IDENTIFIER);
  Serial.printf("[ZB-TASK] Endpoint %d configured (Manufacturer: Espressif, Model: ZB_Switch)\n", ENDPOINT_ID);

  // 5. 注册设备
  esp_zb_device_register(ep_list);
  Serial.println("[ZB-TASK] Device registered");

  // 6. 注册回调
  esp_zb_core_action_handler_register(zb_action_handler);

  // 7. 启动Zigbee栈
  Serial.println("[ZB-TASK] Starting Zigbee stack...");
  ESP_ERROR_CHECK(esp_zb_start(false));

  Serial.println("[ZB-TASK] Entering main loop...");
  esp_zb_stack_main_loop();
}

/* ============================================================================
 * Arduino Setup
 * ============================================================================ */
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n");
  Serial.println("================================================");
  Serial.println("  ESP32-H2 Zigbee Switch - LOW LEVEL API MODE");
  Serial.println("================================================");

#if AUTO_FACTORY_RESET
  Serial.println("\n[NVS] Erasing NVS for fresh start...");
  nvs_flash_erase();
#endif
  ESP_ERROR_CHECK(nvs_flash_init());
  Serial.println("[NVS] Initialized");

  // LED初始化
  setLED(50, 0, 0);  // 红色 = 初始化中

  // 舵机初始化
  ledc_timer_config_t timer_cfg = {
    .speed_mode = LEDC_MODE,
    .duty_resolution = LEDC_DUTY_RES,
    .timer_num = LEDC_TIMER,
    .freq_hz = LEDC_FREQUENCY,
    .clk_cfg = LEDC_AUTO_CLK
  };
  ledc_timer_config(&timer_cfg);

  ledc_channel_config_t channel_cfg = {
    .gpio_num = SERVO_GPIO,
    .speed_mode = LEDC_MODE,
    .channel = LEDC_CHANNEL,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = LEDC_TIMER,
    .duty/Users/censivn/workspace/ai/hardware/arduino/TEST/H2_Zigbee_Switch/H2_Zigbee_Switch.ino = DUTY_AT_0_DEG,
    .hpoint = 0
  };
  ledc_channel_config(&channel_cfg);
  servoSetAngle(SERVO_REST_ANGLE);
  Serial.println("[Servo] Initialized");

  // 舵机定时器
  esp_timer_create_args_t timer_args = {
    .callback = servoReturnCallback,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "servo_timer"
  };
  esp_timer_create(&timer_args, &g_servoTimer);

  // Zigbee平台配置
  esp_zb_platform_config_t config = {
    .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
    .host_config = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE }
  };
  ESP_ERROR_CHECK(esp_zb_platform_config(&config));
  Serial.println("[Zigbee] Platform configured");

  // 启动LED任务
  xTaskCreate(ledTask, "led", 2048, NULL, 5, NULL);
  g_ledState = LED_PAIRING;

  // 启动Zigbee任务 (高优先级)
  xTaskCreate(zigbee_task, "zigbee", 8192, NULL, 10, NULL);

  Serial.println("\n[System] Ready! Waiting for Zigbee connection...");
  Serial.println("[System] Commands: 's'=status, 'r'=reset, 'p'=servo test\n");
}

/* ============================================================================
 * Arduino Loop
 * ============================================================================ */
void loop() {
  if (Serial.available()) {
    char cmd = Serial.read();
    switch (cmd) {
      case 'r':
        Serial.println("[CMD] Factory Reset!");
        esp_zb_factory_reset();
        break;
      case 's':
        Serial.println("[CMD] Status:");
        Serial.printf("  Connected: %s\n", g_zigbeeConnected ? "YES" : "NO");
        Serial.printf("  Channel: %d\n", esp_zb_get_current_channel());
        Serial.printf("  PAN ID: 0x%04X\n", esp_zb_get_pan_id());
        Serial.printf("  Short Addr: 0x%04X\n", esp_zb_get_short_address());
        break;
      case 'p':
        Serial.println("[CMD] Servo Play!");
        servoPlay();
        break;
    }
  }
  delay(100);
}
