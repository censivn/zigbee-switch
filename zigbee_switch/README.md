# ESP32-H2 Zigbee Color Dimmable Light

基于 ESP32-H2 的 Zigbee 彩色可调光灯，支持本地按键控制和状态同步到 Zigbee2MQTT。

## 功能特性

- **本地控制**: 短按按钮 Toggle 灯光开关，状态自动同步到网关
- **恢复出厂**: 长按按钮 3 秒恢复出厂设置并重新配网
- **配网指示**: LED 蓝色慢闪表示配网中
- **超时保护**: 配网超时 40 秒后进入深度睡眠，节省电量
- **唤醒机制**: 深度睡眠后长按 3 秒唤醒并重新配网
- **颜色支持**: 支持 RGB (X/Y) 和色温两种颜色模式

## 硬件要求

- ESP32-H2 开发板
- 内置 RGB LED (使用 `RGB_BUILTIN`)
- Boot 按钮 (使用 `BOOT_PIN`)

## 编译配置

在 Arduino IDE 中设置：
- **Board**: ESP32H2 Dev Module
- **Zigbee Mode**: Zigbee ED (end device)
- **Partition Scheme**: Zigbee 4MB with spiffs

命令行编译：
```bash
arduino-cli compile --fqbn esp32:esp32:esp32h2:ZigbeeMode=ed,PartitionScheme=zigbee zigbee_switch.ino
```

## 按键操作

| 操作 | 功能 |
|------|------|
| 短按 | Toggle 灯光开关，同步状态到网关 |
| 长按 3 秒 | 恢复出厂设置，重新配网 |
| 深度睡眠后长按 3 秒 | 唤醒并开始配网 |

## LED 状态指示

| 状态 | LED 行为 |
|------|----------|
| 配网中 | 蓝色慢闪 (500ms 间隔) |
| 配网失败 | 红灯常亮 2 秒后进入睡眠 |
| 正常运行 | 由 Zigbee 网关控制 |
| 恢复出厂 | 红灯闪烁 |

## 代码架构

### 文件结构

```
zigbee_switch/
├── zigbee_switch.ino    # 主程序
└── README.md            # 说明文档
```

### 模块划分

| 模块 | 功能 |
|------|------|
| Configuration | 配置参数定义 |
| State Management | 状态枚举和结构体 |
| LED Control | LED 颜色和闪烁控制 |
| Light Control | Zigbee 灯光回调处理 |
| Zigbee Report | 状态上报功能 |
| Button Handling | 非阻塞式按钮检测 |
| Pairing State Machine | 配网状态机 |
| Deep Sleep | 深度睡眠和唤醒处理 |

### 主要函数

#### LED 控制
- `ledSetColor(r, g, b)` - 设置 LED 颜色
- `ledOff()` / `ledBlue()` / `ledRed()` / `ledWhite()` - 预设颜色
- `ledBlink(interval, colorFunc)` - 非阻塞闪烁

#### Zigbee 回调
- `onRgbChange(on, r, g, b, level)` - RGB 模式变化回调
- `onTempChange(on, level, mireds)` - 色温模式变化回调
- `onIdentify(time)` - 识别功能回调

#### 状态上报
- `setupReporting()` - 配置属性报告 (必须在连接后调用)
- `reportOnOff()` - 报告开关状态
- `reportLevel()` - 报告亮度级别
- `reportLightState()` - 报告所有状态

#### 灯光控制
- `toggleLight()` - Toggle 灯光并上报状态

#### 按钮处理
- `checkButton()` - 非阻塞检测按钮动作
- `handleButton(action)` - 处理按钮动作

#### 状态机
- `updatePairingState()` - 更新配网状态机

#### 深度睡眠
- `enterDeepSleep()` - 进入深度睡眠
- `handleWakeup()` - 处理唤醒

## Zigbee 协议详解

### 设备类型

本设备为 **Zigbee End Device (ED)**，特点：
- 不能路由消息
- 可以主动发送消息给协调器/路由器
- 支持深度睡眠省电

### ZCL Clusters

| Cluster | ID | 功能 |
|---------|-----|------|
| On/Off | 0x0006 | 开关控制 |
| Level Control | 0x0008 | 亮度控制 |
| Color Control | 0x0300 | 颜色控制 (XY/色温) |

### 状态上报原理

End Device 主动上报状态需要：

1. **配置报告规则** - 使用 `esp_zb_zcl_update_reporting_info()`
```cpp
esp_zb_zcl_reporting_info_t reporting_info = {};
reporting_info.cluster_id = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
reporting_info.attr_id = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
reporting_info.u.send_info.min_interval = 0;
reporting_info.u.send_info.max_interval = 300;
esp_zb_zcl_update_reporting_info(&reporting_info);
```

2. **发送报告命令** - 使用 `esp_zb_zcl_report_attr_cmd_req()`
```cpp
esp_zb_zcl_report_attr_cmd_t cmd = {};
// 关键：使用显式地址模式，直接发送到协调器
cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;  // 协调器地址
cmd.zcl_basic_cmd.dst_endpoint = 1;
esp_zb_zcl_report_attr_cmd_req(&cmd);
```

### 地址模式说明

| 模式 | 说明 | 使用场景 |
|------|------|----------|
| `ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT` | 依赖绑定表 | 需要预先配置绑定 |
| `ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT` | 显式短地址 | 直接指定目标地址 |

**重要**: End Device 使用显式地址模式 (`0x0000` = 协调器) 可以确保状态上报成功，无需依赖绑定表。

## ESP-IDF Zigbee API

### 核心 API

```cpp
// 获取/释放 Zigbee 锁 (线程安全)
esp_zb_lock_acquire(portMAX_DELAY);
esp_zb_lock_release();

// 设置属性值
esp_zb_zcl_set_attribute_val(endpoint, cluster_id, role, attr_id, value, check);

// 配置报告
esp_zb_zcl_update_reporting_info(&reporting_info);

// 发送报告
esp_zb_zcl_report_attr_cmd_req(&cmd);
```

### Arduino Zigbee 库 API

```cpp
// ZigbeeColorDimmableLight 类
zbLight.setLightColorCapabilities(capabilities);
zbLight.onLightChangeRgb(callback);
zbLight.onLightChangeTemp(callback);
zbLight.onIdentify(callback);
zbLight.setLight(state, level, r, g, b);
zbLight.setLightState(state);
zbLight.getLightState();
zbLight.getLightLevel();
zbLight.getLightRed/Green/Blue();
zbLight.restoreLight();

// Zigbee 核心
Zigbee.addEndpoint(&zbLight);
Zigbee.begin();
Zigbee.connected();
Zigbee.factoryReset();
```

## 配置参数

```cpp
// 硬件
#define ZIGBEE_RGB_LIGHT_ENDPOINT 10
const uint8_t LED_PIN = RGB_BUILTIN;
const uint8_t BUTTON_PIN = BOOT_PIN;

// 时间
const unsigned long PAIRING_TIMEOUT_MS = 40000;  // 配网超时 40秒
const unsigned long LED_SLOW_BLINK_MS = 500;     // 慢闪间隔
const unsigned long LONG_PRESS_MS = 3000;        // 长按时间 3秒
const unsigned long DEBOUNCE_MS = 100;           // 消抖时间

// 默认灯光
const uint8_t DEFAULT_BRIGHTNESS = 255;
const uint8_t DEFAULT_RED = 255;
const uint8_t DEFAULT_GREEN = 255;
const uint8_t DEFAULT_BLUE = 255;
```

## 深度睡眠

ESP32-H2 使用 GPIO 唤醒：

```cpp
// 配置 GPIO 低电平唤醒
gpio_wakeup_enable((gpio_num_t)BUTTON_PIN, GPIO_INTR_LOW_LEVEL);
esp_sleep_enable_gpio_wakeup();
esp_deep_sleep_start();

// 检测唤醒原因
esp_sleep_wakeup_cause_t reason = esp_sleep_get_wakeup_cause();
if (reason == ESP_SLEEP_WAKEUP_GPIO) {
    // GPIO 唤醒
}
```

**注意**: ESP32-H2 不支持 `ext0/ext1` 唤醒，需使用 `gpio_wakeup_enable()`。

## 常见问题

### Q: 状态上报不成功？
A: 确保使用显式地址模式 `ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT`，目标地址为 `0x0000` (协调器)。

### Q: End Device 能主动通信吗？
A: 可以。End Device 可以主动发送消息，只需正确配置目标地址。

### Q: 配网超时怎么办？
A: 设备会进入深度睡眠。长按按钮 3 秒唤醒并重新配网。

## 许可证

Apache License 2.0

## 参考资料

- [ESP-IDF Zigbee Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32h2/api-reference/zigbee/index.html)
- [Arduino ESP32 Zigbee Library](https://github.com/espressif/arduino-esp32/tree/master/libraries/Zigbee)
- [Zigbee Cluster Library (ZCL) Specification](https://zigbeealliance.org/solution/zigbee/)
