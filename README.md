# Zigbee Servo Switch (ESP32-H2)

基于 ESP32-H2 的 Zigbee 智能舵机开关控制器，通过 Zigbee 协议控制舵机模拟物理按键按下动作。

## 项目概述

本项目实现了一个 Zigbee 终端设备 (End Device)，可以接收来自 Zigbee 网关的开/关命令，并通过舵机执行物理按键按压动作。支持自动回弹功能，模拟真实的按键按下和释放。

### 核心功能

- **Zigbee ON/OFF 控制**: 接收 Home Automation 标准的开关命令
- **舵机精确控制**: 使用 LEDC PWM 驱动舵机，平滑移动
- **自动回弹模式**: 按下后自动回到初始位置，模拟点动开关
- **本地按键控制**: 支持物理按键本地触发
- **低功耗设计**: 支持 Light Sleep 模式，任务完成后自动休眠

## 硬件要求

| 组件 | 规格 | 引脚 |
|------|------|------|
| MCU | ESP32-H2 | - |
| 舵机 | 标准舵机 (50Hz PWM) | GPIO 5 |
| 按键 | 轻触开关 (上拉) | GPIO 9 |

### 接线图

```
ESP32-H2          舵机
---------         -----
GPIO 5   -------> PWM (黄/橙)
3.3V/5V  -------> VCC (红)
GND      -------> GND (棕/黑)

ESP32-H2          按键
---------         -----
GPIO 9   -------> 一端
GND      -------> 另一端
```

## 软件依赖

- **ESP-IDF**: >= 5.0.0 (建议 5.5.x)
- **esp-zboss-lib**: ~1.6.0
- **esp-zigbee-lib**: ~1.6.0

## 配置参数

在 `main/main.c` 中可配置以下参数:

### 功能配置

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `ENABLE_AUTO_RETURN` | `true` | 是否开启自动回弹 |
| `AUTO_RETURN_DELAY_MS` | `300` | 到达目标后停留时间 (ms) |
| `DEFAULT_MOVE_SPEED` | `5` | 移动速度延迟 (ms/步) |

### 角度配置

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `SERVO_MIN_ANGLE` | `20` | ON 状态角度 |
| `SERVO_MAX_ANGLE` | `180` | OFF/复位状态角度 |

### 硬件引脚

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `SERVO_GPIO` | `5` | 舵机 PWM 输出引脚 |
| `BUTTON_GPIO` | `9` | 本地按键输入引脚 |

## 编译与烧录

### 环境设置

```bash
# 设置 ESP-IDF 环境
source $IDF_PATH/export.sh

# 设置目标芯片
idf.py set-target esp32h2
```

### 编译

```bash
idf.py build
```

### 烧录

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

## Zigbee 网络配置

- **设备类型**: Zigbee End Device (ZED)
- **端点**: 1
- **设备配置**: On/Off Light (用于接收 ON/OFF 命令)
- **制造商**: ESPRESSIF
- **型号**: esp32h2

### 配对流程

1. 确保 Zigbee 协调器 (网关) 处于配对模式
2. 给 ESP32-H2 设备上电
3. 设备自动进入网络搜索模式
4. 配对成功后，可在 Home Assistant / 小米网关 等平台控制

## 工作原理

```
┌─────────────────────────────────────────────────────────────┐
│                    工作流程图                                │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│   Zigbee 命令 ──┐                                            │
│                 │     ┌──────────────┐    ┌──────────────┐  │
│                 ├────>│ 命令解析     │───>│ 舵机控制任务 │  │
│                 │     │ (ON/OFF)     │    │ (平滑移动)   │  │
│   本地按键 ────┘     └──────────────┘    └──────┬───────┘  │
│                                                   │          │
│                 ┌─────────────────────────────────┘          │
│                 │                                            │
│                 v                                            │
│        ┌──────────────────┐                                  │
│        │ 自动回弹 (可选)  │                                  │
│        │ 延迟后回到 OFF   │                                  │
│        └──────────────────┘                                  │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

## 项目结构

```
zigbee-servo-switch/
├── README.md                    # 项目说明文档
├── CMakeLists.txt               # 项目主 CMake 配置
├── sdkconfig.defaults           # SDK 默认配置
├── dependencies.lock            # 依赖版本锁定
│
├── main/                        # 主应用程序
│   ├── CMakeLists.txt           # 组件 CMake 配置
│   ├── idf_component.yml        # 组件依赖声明
│   └── main.c                   # 主程序源码
│
├── docs/                        # 文档目录
│   └── hardware_setup.md        # 硬件安装指南
│
├── config/                      # 配置文件目录
│   └── app_config.h             # 应用配置头文件
│
├── .vscode/                     # VS Code 配置
│   ├── settings.json            # 编辑器设置
│   ├── c_cpp_properties.json    # C/C++ 智能提示配置
│   └── launch.json              # 调试配置
│
├── .devcontainer/               # Docker 开发环境
│   ├── Dockerfile               # 容器镜像定义
│   └── devcontainer.json        # 开发容器配置
│
├── build/                       # 编译输出 (git忽略)
├── managed_components/          # 自动下载的依赖 (git忽略)
└── sdkconfig                    # 当前 SDK 配置 (git忽略)
```

## 常见问题

### Q: 舵机不转动

- 检查 GPIO 5 接线
- 检查舵机供电是否充足 (建议独立供电)
- 查看串口日志确认命令是否收到

### Q: 无法加入 Zigbee 网络

- 确认网关处于配对模式
- 尝试重新上电或恢复出厂设置
- 检查 Zigbee 信道是否兼容

### Q: 回弹太快/太慢

- 调整 `AUTO_RETURN_DELAY_MS` 参数
- 调整 `DEFAULT_MOVE_SPEED` 参数

## License

MIT License
