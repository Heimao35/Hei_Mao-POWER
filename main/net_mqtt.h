/**
 * @file net_mqtt.h
 * @brief 功率计 MQTT 客户端：上报遥测、接收远程 PD 控制命令。
 *
 * 使用前请修改下方 NET_MQTT_DEVICE_ID 与 NET_MQTT_BROKER_URI。
 */
#ifndef NET_MQTT_H
#define NET_MQTT_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 设备唯一 ID，需与网关侧识别的 ID 一致（手动填写）。 */
#ifndef NET_MQTT_DEVICE_ID
#define NET_MQTT_DEVICE_ID  "meter-01"
#endif

/** MQTT Broker 地址，格式 mqtt://IP:端口（网关 ESP 的局域网 IP）。 */
#ifndef NET_MQTT_BROKER_URI
#define NET_MQTT_BROKER_URI "mqtt://这里填你的网关ip地址:1883"
#endif

/** 遥测上报间隔（毫秒）。 */
#ifndef NET_MQTT_TELEMETRY_INTERVAL_MS
#define NET_MQTT_TELEMETRY_INTERVAL_MS  2000U
#endif

/** 在 net_wifi_init() 之后调用，启动 MQTT 后台任务。 */
void net_mqtt_init(void);

/** Wi-Fi 扫描/换网期间暂停 MQTT，释放射频与 TCP（由 net_wifi 调用）。 */
void net_mqtt_suspend(void);

/** 恢复 MQTT 连接（由 net_wifi 调用）。 */
void net_mqtt_resume(void);

/** 当前是否已与 MQTT Broker 建立连接。 */
bool net_mqtt_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_MQTT_H */
