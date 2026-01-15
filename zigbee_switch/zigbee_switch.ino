// Copyright 2024 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @brief Zigbee Color Dimmable Light with Servo Control
 *
 * Features:
 * - Short press: Toggle light on/off with servo action
 * - Long press (3s): Factory reset and re-pair
 * - Pairing timeout with deep sleep
 * - LED status indication during pairing
 * - Servo auto-return after timeout
 */

#ifndef ZIGBEE_MODE_ED
#error "Zigbee end device mode is not selected in Tools->Zigbee mode"
#endif

#include "Zigbee.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"

/********************* Configuration **************************/
#define ZIGBEE_RGB_LIGHT_ENDPOINT 10

// Hardware pins
const uint8_t LED_PIN = RGB_BUILTIN;
const uint8_t BUTTON_PIN = BOOT_PIN;
const uint8_t SERVO_PIN = 5;

// Timing configuration
const unsigned long PAIRING_TIMEOUT_MS = 40000;      // 配网超时时间 (40秒)
const unsigned long LED_FAST_BLINK_MS = 100;         // 快速闪烁间隔
const unsigned long LED_SLOW_BLINK_MS = 500;         // 慢速闪烁间隔
const unsigned long LONG_PRESS_MS = 3000;            // 长按时间 (3秒)
const unsigned long DEBOUNCE_MS = 100;               // 按键消抖时间

// Servo configuration
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT
#define LEDC_FREQUENCY          50                   // 50Hz for servo
const int SERVO_DUTY_MIN = 205;                      // 0度对应的duty
const int SERVO_DUTY_MAX = 1024;                     // 180度对应的duty
const int SERVO_TARGET_ANGLE = 160;                  // 目标角度
const int SERVO_REST_ANGLE = 20;                     // 休息角度
const unsigned long SERVO_AUTO_RETURN_MS = 2000;     // 自动回位时间 (2秒)

// Default light settings
const uint8_t DEFAULT_BRIGHTNESS = 255;
const uint8_t DEFAULT_RED = 255;
const uint8_t DEFAULT_GREEN = 255;
const uint8_t DEFAULT_BLUE = 255;

/********************* State Management **************************/
enum PairingState {
  PAIRING_IDLE,           // 已配网或等待中
  PAIRING_IN_PROGRESS,    // 正在配网
  PAIRING_FAILED          // 配网失败
};

enum ButtonAction {
  BUTTON_NONE,
  BUTTON_SHORT_PRESS,
  BUTTON_LONG_PRESS
};

struct DeviceState {
  PairingState pairing;
  unsigned long pairingStartTime;
  unsigned long lastLedToggle;
  bool ledBlinkOn;
} state = {
  .pairing = PAIRING_IDLE,
  .pairingStartTime = 0,
  .lastLedToggle = 0,
  .ledBlinkOn = false
};

// Servo timer handle
static esp_timer_handle_t servoTimer = NULL;
static volatile bool servoAutoReturnPending = false;  // 定时器触发标志
static bool internalStateChange = false;              // 内部状态变更标志，防止回调干扰

ZigbeeColorDimmableLight zbLight(ZIGBEE_RGB_LIGHT_ENDPOINT);

/********************* Forward Declarations **************************/
void turnLightOn();
void turnLightOff();
void reportLightState();

/********************* Servo Control Functions **************************/
void servoSetAngle(int angle) {
  int duty = SERVO_DUTY_MIN + (angle * (SERVO_DUTY_MAX - SERVO_DUTY_MIN) / 180);
  ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
  ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

// 定时器回调：设置标志位 (在esp_timer上下文，不能直接调用Zigbee API)
void servoReturnCallback(void *arg) {
  Serial.println("[Servo] Auto return timeout");
  servoAutoReturnPending = true;  // 在loop()中处理
}

// 舵机播放动作 (开灯时调用)
void servoPlay() {
  Serial.printf("[Servo] PLAY -> %d deg\n", SERVO_TARGET_ANGLE);
  servoSetAngle(SERVO_TARGET_ANGLE);

  // 启动/重启自动回位定时器
  if (servoTimer) {
    esp_timer_stop(servoTimer);
    esp_timer_start_once(servoTimer, SERVO_AUTO_RETURN_MS * 1000);
  }
}

// 舵机休息位置 (关灯时调用)
void servoRest() {
  Serial.printf("[Servo] REST -> %d deg\n", SERVO_REST_ANGLE);

  // 取消定时器
  if (servoTimer) {
    esp_timer_stop(servoTimer);
  }

  servoSetAngle(SERVO_REST_ANGLE);
}

// 初始化舵机
void servoInit() {
  // 配置LEDC定时器
  ledc_timer_config_t timer_cfg = {
    .speed_mode = LEDC_MODE,
    .duty_resolution = LEDC_DUTY_RES,
    .timer_num = LEDC_TIMER,
    .freq_hz = LEDC_FREQUENCY,
    .clk_cfg = LEDC_AUTO_CLK
  };
  ledc_timer_config(&timer_cfg);

  // 配置LEDC通道
  ledc_channel_config_t channel_cfg = {
    .gpio_num = SERVO_PIN,
    .speed_mode = LEDC_MODE,
    .channel = LEDC_CHANNEL,
    .intr_type = LEDC_INTR_DISABLE,
    .timer_sel = LEDC_TIMER,
    .duty = SERVO_DUTY_MIN,
    .hpoint = 0
  };
  ledc_channel_config(&channel_cfg);

  // 初始位置
  servoSetAngle(SERVO_REST_ANGLE);

  // 创建自动回位定时器
  esp_timer_create_args_t timer_args = {
    .callback = servoReturnCallback,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "servo_timer"
  };
  esp_timer_create(&timer_args, &servoTimer);

  Serial.println("[Servo] Initialized");
}

/********************* LED Control Functions **************************/
void ledSetColor(uint8_t r, uint8_t g, uint8_t b) {
  rgbLedWrite(LED_PIN, r, g, b);
}

void ledOff() {
  ledSetColor(0, 0, 0);
}

void ledBlue() {
  ledSetColor(0, 0, 255);
}

void ledRed() {
  ledSetColor(255, 0, 0);
}

void ledWhite() {
  ledSetColor(255, 255, 255);
}

// 闪烁控制 (非阻塞)
void ledBlink(unsigned long interval, void (*colorFunc)()) {
  unsigned long now = millis();
  if (now - state.lastLedToggle >= interval) {
    state.ledBlinkOn = !state.ledBlinkOn;
    if (state.ledBlinkOn) {
      colorFunc();
    } else {
      ledOff();
    }
    state.lastLedToggle = now;
  }
}

/********************* Light Control Functions **************************/
uint16_t kelvinToMireds(uint16_t kelvin) {
  return 1000000 / kelvin;
}

uint16_t miredsToKelvin(uint16_t mireds) {
  return 1000000 / mireds;
}

// 开灯 (统一入口)
void turnLightOn() {
  Serial.println("[Light] >>> turnLightOn()");

  uint8_t level = zbLight.getLightLevel();
  uint8_t r = zbLight.getLightRed();
  uint8_t g = zbLight.getLightGreen();
  uint8_t b = zbLight.getLightBlue();

  // 如果亮度为0，设置默认值
  if (level == 0) level = DEFAULT_BRIGHTNESS;
  if (r == 0 && g == 0 && b == 0) {
    r = DEFAULT_RED;
    g = DEFAULT_GREEN;
    b = DEFAULT_BLUE;
  }

  Serial.printf("[Light] setLight(true, %d, %d, %d, %d)\n", level, r, g, b);
  zbLight.setLight(true, level, r, g, b);
  servoPlay();

  // 等待属性更新后再上报
  delay(50);
  reportLightState();

  Serial.println("[Light] <<< turnLightOn() done");
}

// 关灯 (统一入口)
void turnLightOff() {
  Serial.println("[Light] >>> turnLightOff()");

  zbLight.setLightState(false);
  ledOff();
  servoRest();

  // 等待属性更新后再上报
  delay(50);
  reportLightState();

  Serial.println("[Light] <<< turnLightOff() done");
}

// Toggle灯光状态
void toggleLight() {
  bool currentState = zbLight.getLightState();
  Serial.printf("Toggle light: %s -> %s\n",
                currentState ? "ON" : "OFF",
                !currentState ? "ON" : "OFF");

  if (currentState) {
    turnLightOff();
  } else {
    turnLightOn();
  }
}

// Zigbee RGB模式回调
void onRgbChange(bool on, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
  Serial.printf("[Zigbee] RGB change: on=%d, r=%d, g=%d, b=%d, level=%d\n", on, r, g, b, level);

  if (!on) {
    ledOff();
    servoRest();
    return;
  }

  float brightness = (float)level / 255.0f;
  ledSetColor(r * brightness, g * brightness, b * brightness);
  servoPlay();
}

// Zigbee色温模式回调
void onTempChange(bool on, uint8_t level, uint16_t mireds) {
  Serial.printf("[Zigbee] Temp change: on=%d, level=%d, mireds=%d\n", on, level, mireds);

  if (!on) {
    ledOff();
    servoRest();
    return;
  }

  float brightness = (float)level / 255.0f;
  uint16_t kelvin = miredsToKelvin(mireds);
  uint8_t warm = constrain(map(kelvin, 2000, 6500, 255, 0), 0, 255);
  uint8_t cold = constrain(map(kelvin, 2000, 6500, 0, 255), 0, 255);
  ledSetColor(warm * brightness, warm * brightness, cold * brightness);
  servoPlay();
}

// Identify回调
void onIdentify(uint16_t time) {
  static bool blinkState = true;
  log_d("Identify called for %d seconds", time);
  if (time == 0) {
    zbLight.restoreLight();
    return;
  }
  blinkState = !blinkState;
  if (blinkState) {
    ledWhite();
  } else {
    ledOff();
  }
}

/********************* Zigbee Report Functions **************************/
bool setupOnOffReporting() {
  esp_zb_zcl_reporting_info_t reporting_info = {};
  reporting_info.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
  reporting_info.ep = ZIGBEE_RGB_LIGHT_ENDPOINT;
  reporting_info.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
  reporting_info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
  reporting_info.attr_id = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
  reporting_info.u.send_info.min_interval = 0;
  reporting_info.u.send_info.max_interval = 300;
  reporting_info.u.send_info.def_min_interval = 0;
  reporting_info.u.send_info.def_max_interval = 300;
  reporting_info.u.send_info.delta.u8 = 1;
  reporting_info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  reporting_info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_err_t ret = esp_zb_zcl_update_reporting_info(&reporting_info);
  esp_zb_lock_release();

  if (ret != ESP_OK) {
    Serial.printf("Failed to setup On/Off reporting: 0x%x\n", ret);
    return false;
  }
  Serial.println("On/Off reporting configured");
  return true;
}

bool setupLevelReporting() {
  esp_zb_zcl_reporting_info_t reporting_info = {};
  reporting_info.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
  reporting_info.ep = ZIGBEE_RGB_LIGHT_ENDPOINT;
  reporting_info.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL;
  reporting_info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
  reporting_info.attr_id = ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID;
  reporting_info.u.send_info.min_interval = 0;
  reporting_info.u.send_info.max_interval = 300;
  reporting_info.u.send_info.def_min_interval = 0;
  reporting_info.u.send_info.def_max_interval = 300;
  reporting_info.u.send_info.delta.u8 = 1;
  reporting_info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
  reporting_info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_err_t ret = esp_zb_zcl_update_reporting_info(&reporting_info);
  esp_zb_lock_release();

  if (ret != ESP_OK) {
    Serial.printf("Failed to setup Level reporting: 0x%x\n", ret);
    return false;
  }
  Serial.println("Level reporting configured");
  return true;
}

void setupReporting() {
  setupOnOffReporting();
  setupLevelReporting();
}

bool reportOnOff() {
  Serial.println("[Report] >>> reportOnOff()");

  esp_zb_zcl_report_attr_cmd_t cmd = {};
  cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
  cmd.zcl_basic_cmd.dst_endpoint = 1;
  cmd.zcl_basic_cmd.src_endpoint = ZIGBEE_RGB_LIGHT_ENDPOINT;
  cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
  cmd.attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
  cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  cmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;

  Serial.println("[Report] Acquiring Zigbee lock...");
  esp_zb_lock_acquire(portMAX_DELAY);
  Serial.println("[Report] Sending report command...");
  esp_err_t ret = esp_zb_zcl_report_attr_cmd_req(&cmd);
  esp_zb_lock_release();
  Serial.println("[Report] Lock released");

  if (ret != ESP_OK) {
    Serial.printf("[Report] FAILED to report On/Off: 0x%x\n", ret);
    return false;
  }
  Serial.println("[Report] On/Off state reported successfully");
  return true;
}

bool reportLevel() {
  esp_zb_zcl_report_attr_cmd_t cmd = {};
  cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
  cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
  cmd.zcl_basic_cmd.dst_endpoint = 1;
  cmd.zcl_basic_cmd.src_endpoint = ZIGBEE_RGB_LIGHT_ENDPOINT;
  cmd.clusterID = ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL;
  cmd.attributeID = ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID;
  cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  cmd.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;

  esp_zb_lock_acquire(portMAX_DELAY);
  esp_err_t ret = esp_zb_zcl_report_attr_cmd_req(&cmd);
  esp_zb_lock_release();

  if (ret != ESP_OK) {
    Serial.printf("Failed to report Level: 0x%x\n", ret);
    return false;
  }
  Serial.println("Level reported");
  return true;
}

void reportLightState() {
  if (!Zigbee.connected()) {
    Serial.println("[Report] Not connected, skip report");
    return;
  }

  bool currentState = zbLight.getLightState();
  uint8_t currentLevel = zbLight.getLightLevel();
  Serial.printf("[Report] Reporting state: on=%d, level=%d\n", currentState, currentLevel);

  reportOnOff();
  reportLevel();
}

/********************* Button Handling **************************/
ButtonAction checkButton() {
  static bool wasPressed = false;
  static unsigned long pressStartTime = 0;
  static bool longPressHandled = false;

  bool isPressed = (digitalRead(BUTTON_PIN) == LOW);

  if (isPressed && !wasPressed) {
    pressStartTime = millis();
    longPressHandled = false;
    wasPressed = true;
  } else if (isPressed && wasPressed) {
    if (!longPressHandled && (millis() - pressStartTime > LONG_PRESS_MS)) {
      longPressHandled = true;
      return BUTTON_LONG_PRESS;
    }
  } else if (!isPressed && wasPressed) {
    wasPressed = false;
    if (!longPressHandled && (millis() - pressStartTime > DEBOUNCE_MS)) {
      return BUTTON_SHORT_PRESS;
    }
  }

  return BUTTON_NONE;
}

void handleButton(ButtonAction action) {
  switch (action) {
    case BUTTON_SHORT_PRESS:
      Serial.println("Short press: Toggle light");
      toggleLight();
      break;

    case BUTTON_LONG_PRESS:
      Serial.println("Long press: Factory reset");
      ledRed();
      delay(500);
      Zigbee.factoryReset();
      break;

    default:
      break;
  }
}

/********************* Pairing State Machine **************************/
void updatePairingState() {
  bool connected = Zigbee.connected();
  unsigned long elapsed = millis() - state.pairingStartTime;

  switch (state.pairing) {
    case PAIRING_IDLE:
      if (!connected) {
        state.pairing = PAIRING_IN_PROGRESS;
        state.pairingStartTime = millis();
        Serial.println("Starting pairing...");
      }
      break;

    case PAIRING_IN_PROGRESS:
      if (connected) {
        state.pairing = PAIRING_IDLE;
        Serial.println("Pairing successful!");
        setupReporting();
        zbLight.restoreLight();
        delay(500);
        reportLightState();
      } else if (elapsed > PAIRING_TIMEOUT_MS) {
        state.pairing = PAIRING_FAILED;
        Serial.println("Pairing timeout!");
      } else {
        ledBlink(LED_SLOW_BLINK_MS, ledBlue);

        static unsigned long lastPrint = 0;
        if (millis() - lastPrint > 1000) {
          Serial.printf("Pairing... %lus / %lus\n", elapsed / 1000, PAIRING_TIMEOUT_MS / 1000);
          lastPrint = millis();
        }
      }
      break;

    case PAIRING_FAILED:
      ledRed();
      delay(2000);
      enterDeepSleep();
      break;
  }
}

/********************* Deep Sleep **************************/
void enterDeepSleep() {
  Serial.println("Entering deep sleep...");
  Serial.println("Long press button (3s) to wake and re-pair.");

  ledOff();
  servoRest();
  delay(100);

  gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  Serial.flush();
  esp_deep_sleep_start();
}

bool handleWakeup() {
  esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();

  if (reason == ESP_SLEEP_WAKEUP_GPIO) {
    Serial.println("Woke up from deep sleep!");

    unsigned long pressStart = millis();
    while (digitalRead(BUTTON_PIN) == LOW) {
      delay(50);
      if (millis() - pressStart > LONG_PRESS_MS) {
        Serial.println("Long press detected, starting pairing...");
        return true;
      }
    }

    Serial.println("Short press, going back to sleep...");
    enterDeepSleep();
  }

  return true;
}

/********************* Arduino Entry Points **************************/
void setup() {
  Serial.begin(115200);

  // 初始化硬件
  ledOff();
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // 初始化舵机
  servoInit();

  // 处理唤醒
  if (!handleWakeup()) {
    return;
  }

  // 配置Zigbee灯
  uint16_t capabilities = ZIGBEE_COLOR_CAPABILITY_X_Y | ZIGBEE_COLOR_CAPABILITY_COLOR_TEMP;
  zbLight.setLightColorCapabilities(capabilities);
  zbLight.onLightChangeRgb(onRgbChange);
  zbLight.onLightChangeTemp(onTempChange);
  zbLight.onIdentify(onIdentify);
  zbLight.setManufacturerAndModel("Espressif", "ZBColorLightBulb");
  zbLight.setLightColorTemperatureRange(kelvinToMireds(6500), kelvinToMireds(2000));

  // 启动Zigbee
  Serial.println("Starting Zigbee...");
  Zigbee.addEndpoint(&zbLight);

  if (!Zigbee.begin()) {
    Serial.println("Zigbee failed! Rebooting...");
    ESP.restart();
  }

  Serial.println("Zigbee started, entering main loop...");

  // 初始化状态
  state.pairingStartTime = millis();
  if (Zigbee.connected()) {
    state.pairing = PAIRING_IDLE;
    setupReporting();
    delay(500);
    reportLightState();
  } else {
    state.pairing = PAIRING_IN_PROGRESS;
  }
}

void loop() {
  // 1. 处理舵机自动回位 (从定时器回调触发)
  if (servoAutoReturnPending) {
    servoAutoReturnPending = false;
    Serial.println("[Loop] Processing servo auto return");
    turnLightOff();
  }

  // 2. 处理按钮
  ButtonAction action = checkButton();
  if (action != BUTTON_NONE) {
    handleButton(action);
  }

  // 3. 处理配网状态
  updatePairingState();

  // 4. 短延迟
  delay(10);
}
