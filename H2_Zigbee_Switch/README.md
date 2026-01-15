# ESP32-H2 Zigbee Switch (Arduino)

基于 ESP32-H2 的 Zigbee 智能舵机开关，使用 Arduino 框架实现。

## 功能特性

- Zigbee 3.0 Router 设备
- 舵机控制，模拟按键按下/释放
- 2秒自动回弹
- 本地按键控制
- RGB LED 状态指示
- 长按3秒工厂重置

## 硬件连接

| 组件 | GPIO | 说明 |
|------|------|------|
| 舵机 PWM | 5 | 信号线(黄/橙) |
| 按键 | 9 | 内部上拉，按下接地 |
| RGB LED | 8 | WS2812 |

```
ESP32-H2        舵机           按键
   5 ---------> PWM
   9 -----------------------> 一端
  GND --------> GND --------> 另一端
  3.3V -------> VCC
   8 ---------> WS2812 LED
```

## Arduino IDE 设置

### 必要设置

1. **Board**: ESP32-H2 Dev Module
2. **Zigbee mode**: Zigbee ZCZR (coordinator/router)
3. **Partition Scheme**: Zigbee 4MB with spiffs

### 版本要求

- Arduino ESP32 Core >= 3.0.0 (推荐 3.1.0+)

## 舵机控制逻辑

```
按键/Zigbee ON
     │
     ▼
┌─────────────┐     2秒后自动     ┌─────────────┐
│ TARGET 160° │ ───────────────> │  REST 20°   │
└─────────────┘                   └─────────────┘
     ▲                                   │
     │      按键/Zigbee OFF 立即返回      │
     └───────────────────────────────────┘
```

## 按键操作

| 操作 | 功能 |
|------|------|
| 短按 | 舵机动作切换 |
| 长按3秒 | 工厂重置(清除配网信息) |

## LED 状态指示

| 状态 | LED显示 |
|------|---------|
| 配对中 | 蓝色闪烁 |
| 已连接 | 绿色常亮 |
| 错误 | 红色闪烁 |
| 重置警告 | 蓝色常亮 |

## Zigbee 配置

| 参数 | 值 |
|------|-----|
| 设备类型 | Router |
| 端点 | 10 |
| 设备ID | On/Off Light |
| 制造商 | Espressif |
| 型号 | ESP32H2_ZB_Switch |

## 编译上传

1. 打开 Arduino IDE
2. 选择正确的开发板和端口
3. 配置 Zigbee mode 和 Partition Scheme (见上方)
4. 点击上传

## 配对流程

1. 上电后设备自动进入配对模式(蓝色LED闪烁)
2. 在Zigbee网关/协调器上搜索新设备
3. 配对成功后LED变为绿色常亮
4. 如需重新配对，长按按键3秒进行工厂重置

## 参数调整

在代码中修改以下定义:

```cpp
#define SERVO_TARGET_ANGLE      160     // 目标角度
#define SERVO_REST_ANGLE        20      // 休息角度
#define SERVO_AUTO_RETURN_MS    2000    // 自动返回延迟(ms)
#define BUTTON_LONG_PRESS_MS    3000    // 长按触发时间(ms)
```

## 与 ESP-IDF 版本的区别

Arduino 版本使用官方的 Zigbee Arduino 库，相比直接使用 ESP-IDF:

- API 更简洁易用
- 自动处理网络初始化和配置
- 兼容官方 Arduino IDE 工具链
- 旧的 ESP-IDF 版本代码已移至 `legacy_esp_idf/` 文件夹

## 故障排除

### Zigbee 无法启动
- 检查 Zigbee mode 是否设置为 "Zigbee ZCZR"
- 检查 Partition Scheme 是否正确

### 无法搜索到网络
- 确保有 Zigbee 协调器(网关)在运行
- 尝试长按按键重置设备

### 舵机不动
- 检查 GPIO 5 连接
- 检查舵机供电(3.3V 或外部 5V)

## License

MIT
