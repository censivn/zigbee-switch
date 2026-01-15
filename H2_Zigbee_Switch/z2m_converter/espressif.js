/**
 * Zigbee2MQTT External Converter for ESP32-H2 Zigbee Switch
 *
 * 使用方法:
 * 1. 将此文件复制到 Z2M 配置目录的 external_converters 文件夹
 *    - Docker: /config/zigbee2mqtt/external_converters/espressif.js
 *    - HA Add-on: /config/zigbee2mqtt/external_converters/espressif.js
 * 2. 在 configuration.yaml 中添加:
 *    external_converters:
 *      - espressif.js
 * 3. 重启 Z2M
 * 4. 重新配对设备
 */

const {onOff} = require('zigbee-herdsman-converters/lib/modernExtend');

const definition = {
    zigbeeModel: ['ZB_Switch'],
    model: 'ESP32H2_ZB_Switch',
    vendor: 'Espressif',
    description: 'ESP32-H2 Zigbee Smart Switch with Servo Control',
    extend: [onOff()],
};

module.exports = definition;
