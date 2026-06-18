/**
 * @file net_mqtt.h
 * @brief 功率计 MQTT 客户端：上报遥测、接收远程 PD 控制命令。
 *
 * 使用前请修改下方 NET_MQTT_DEVICE_ID 与 NET_MQTT_BROKER_URI。
 */
#ifndef NET_MQTT_H
#define NET_MQTT_H

#ifdef __cplusplus
extern "C" {
#endif

/** 设备唯一 ID，需与网关侧识别的 ID 一致（手动填写）。 */
#ifndef NET_MQTT_DEVICE_ID
#define NET_MQTT_DEVICE_ID  "meter-01"
#endif

/** MQTT Broker 地址，格式 mqtt://IP:端口（网关 ESP 的局域网 IP）。 */
#ifndef NET_MQTT_BROKER_URI
#define NET_MQTT_BROKER_URI "mqtt://192.168.31.11:1883"
#endif

/** 遥测上报间隔（毫秒）。 */
#ifndef NET_MQTT_TELEMETRY_INTERVAL_MS
#define NET_MQTT_TELEMETRY_INTERVAL_MS  2000U
#endif

/** 在 net_wifi_init() 之后调用，启动 MQTT 后台任务。 */
void net_mqtt_init(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_MQTT_H */
