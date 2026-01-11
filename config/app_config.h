/**
 * @file app_config.h
 * @brief Zigbee Servo Switch 应用配置文件
 *
 * 本文件包含所有可配置的参数，方便根据实际需求进行调整
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * 功能配置
 * ============================================================================ */

/**
 * @brief 是否开启自动回弹功能
 * - true: 舵机到达目标位置后自动回到初始位置 (点动模式)
 * - false: 舵机保持在目标位置 (切换模式)
 */
#define CONFIG_ENABLE_AUTO_RETURN       true

/**
 * @brief 到达目标位置后的停留时间 (毫秒)
 * 模拟按键按下的持续时间
 */
#define CONFIG_AUTO_RETURN_DELAY_MS     300

/**
 * @brief 默认移动速度延迟 (毫秒/步)
 * 数值越小移动越快
 */
#define CONFIG_DEFAULT_MOVE_SPEED       5

/* ============================================================================
 * 舵机角度配置
 * ============================================================================ */

/**
 * @brief 舵机最小角度 (ON 状态 / 按下状态)
 * 对应 0.0f 的位置
 */
#define CONFIG_SERVO_MIN_ANGLE          20

/**
 * @brief 舵机最大角度 (OFF 状态 / 复位状态)
 * 对应 1.0f 的位置
 */
#define CONFIG_SERVO_MAX_ANGLE          180

/**
 * @brief 初始启动角度
 */
#define CONFIG_START_ANGLE              20

/* ============================================================================
 * PWM 占空比映射
 * ============================================================================ */

/**
 * @brief 0度对应的占空比
 * 基于 13-bit 分辨率 (0-8191)
 */
#define CONFIG_DUTY_AT_0_DEG            1024

/**
 * @brief 180度对应的占空比
 * 基于 13-bit 分辨率 (0-8191)
 */
#define CONFIG_DUTY_AT_180_DEG          205

/* ============================================================================
 * 硬件引脚配置
 * ============================================================================ */

/**
 * @brief 舵机 PWM 输出引脚
 */
#define CONFIG_SERVO_GPIO               5

/**
 * @brief 本地按键输入引脚
 */
#define CONFIG_BUTTON_GPIO              9

/* ============================================================================
 * LEDC (PWM) 配置
 * ============================================================================ */

/**
 * @brief LEDC 定时器编号
 */
#define CONFIG_LEDC_TIMER               LEDC_TIMER_0

/**
 * @brief LEDC 速度模式
 */
#define CONFIG_LEDC_MODE                LEDC_LOW_SPEED_MODE

/**
 * @brief LEDC 通道编号
 */
#define CONFIG_LEDC_CHANNEL             LEDC_CHANNEL_0

/**
 * @brief LEDC 占空比分辨率 (13-bit = 0-8191)
 */
#define CONFIG_LEDC_DUTY_RES            LEDC_TIMER_13_BIT

/**
 * @brief LEDC PWM 频率 (Hz)
 * 标准舵机使用 50Hz
 */
#define CONFIG_LEDC_FREQUENCY           50

/* ============================================================================
 * Zigbee 配置
 * ============================================================================ */

/**
 * @brief Zigbee HA 开关端点号
 */
#define CONFIG_HA_ONOFF_SWITCH_ENDPOINT 1

/**
 * @brief 是否启用安装码策略
 */
#define CONFIG_INSTALLCODE_POLICY       false

/**
 * @brief End Device 老化超时时间
 */
#define CONFIG_ED_AGING_TIMEOUT         ESP_ZB_ED_AGING_TIMEOUT_64MIN

/**
 * @brief End Device Keep Alive 间隔 (毫秒)
 */
#define CONFIG_ED_KEEP_ALIVE            3000

/**
 * @brief Zigbee 主信道掩码
 * ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK 表示所有信道
 */
#define CONFIG_ZB_PRIMARY_CHANNEL_MASK  ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK

/**
 * @brief 制造商名称
 */
#define CONFIG_MANUFACTURER_NAME        "\x09""ESPRESSIF"

/**
 * @brief 型号标识符
 */
#define CONFIG_MODEL_IDENTIFIER         "\x07"CONFIG_IDF_TARGET

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
