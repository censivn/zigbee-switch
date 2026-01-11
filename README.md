# ESP32-H2 Zigbee Servo Switch

基于 ESP32-H2 的 Zigbee 智能舵机开关，通过舵机模拟物理按键按压动作。

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

### 动作说明

| 触发条件 | 动作 |
|----------|------|
| 按键(舵机在REST) | 移动到160°，启动2秒定时器 |
| 按键(舵机在TARGET) | 立即返回20°，取消定时器 |
| 2秒定时器触发 | 自动返回20° |
| Zigbee ON命令 | 移动到160°，启动2秒定时器 |
| Zigbee OFF命令 | 立即返回20° |

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
| 制造商 | ESPRESSIF |
| 型号 | ESP32H2_ZB_SWITCH |

## 编译烧录

```bash
# 设置环境
source $IDF_PATH/export.sh
idf.py set-target esp32h2

# 编译
idf.py build

# 烧录
idf.py -p /dev/ttyUSB0 flash

# 监控日志
idf.py -p /dev/ttyUSB0 monitor
```

## 配对流程

1. 上电后设备自动进入配对模式(蓝色LED闪烁)
2. 在Zigbee网关/协调器上搜索新设备
3. 配对成功后LED变为绿色常亮
4. 如需重新配对，长按按键3秒进行工厂重置

## 参数配置

在 `main/main.c` 中修改:

```c
#define SERVO_TARGET_ANGLE      160     // 目标角度
#define SERVO_REST_ANGLE        20      // 休息角度
#define SERVO_AUTO_RETURN_MS    2000    // 自动返回延迟(ms)
#define BUTTON_LONG_PRESS_MS    3000    // 长按触发时间(ms)
```

## 项目结构

```
├── main/
│   └── main.c              # 主程序
├── CMakeLists.txt          # 构建配置
├── partitions.csv          # 分区表
├── sdkconfig.defaults      # SDK默认配置
└── README.md               # 本文档
```

## 依赖

- ESP-IDF >= 5.0
- esp-zigbee-lib ~1.6.0
- esp-zboss-lib ~1.6.0
- led_strip ^2.0.0

## License

MIT
