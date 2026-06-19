

# 功率计 MQTT 网关（ESP32-S3 Arduino）

网关开发板负责：

1. 连接 Wi-Fi（SSID/密码在 `web_server.ino` 顶部手动填写）
2. 运行 MQTT Broker（默认端口 **1883**）
3. 提供 HTTP 服务（默认端口 **80**），用于局域网或内网穿透后远程查看/控制

## 依赖库

在 Arduino IDE **库管理器**中安装：

| 库名 | 作者 |
|------|------|
| ArduinoJson | Benoit Blanchon |
| sMQTTBroker | terrorsl |

开发板选择：**ESP32S3 Dev Module**（与 N16R8 匹配的分区/Flash 配置按你现有习惯即可）。

## 使用前配置

编辑 `web_server.ino` 顶部 **USER CONFIG**：

```cpp
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASS       "你的WiFi密码"
```

烧录后打开串口（115200），记录打印的 **WiFi IP**，例如 `192.168.1.50`。

## 功率计侧配置

编辑 `main/net_mqtt.h`：

```c
#define NET_MQTT_DEVICE_ID  "meter-01"              // 手动填写，每台功率计唯一
#define NET_MQTT_BROKER_URI "mqtt://192.168.1.50:1883"  // 改为网关 IP
```

功率计 Wi-Fi 仍通过设备 UI 连接**同一路由器**。

## MQTT 主题约定

| 方向 | 主题 | 说明 |
|------|------|------|
| 功率计 → 网关 | `power/<device_id>/telemetry` | 遥测 JSON |
| 网关 → 功率计 | `power/<device_id>/command` | 控制 JSON |

遥测示例：

```json
{"device_id":"meter-01","voltage_v":11.98,"current_a":0.452,"power_w":5.42,"pd_enabled":true,"pd_voltage":12,"pd_pg_ok":true}
```

控制示例：

```json
{"pd_enabled":true,"pd_voltage":15}
```

## HTTP API（供内网穿透）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 简易 Web 控制台 |
| GET | `/api/devices` | 所有设备状态 |
| GET | `/api/devices/<id>` | 单设备状态 |
| POST | `/api/devices/<id>/control` | 下发 PD 控制 |

控制请求 Body 示例：

```json
{"pd_enabled": false, "pd_voltage": 12}
```

字段可只传其中一个。

## 联调步骤

1. 烧录网关，确认串口出现 `MQTT broker started` 与 `HTTP server started`
2. 浏览器访问 `http://<网关IP>/`
3. 修改功率计 `net_mqtt.h` 后编译烧录功率计固件
4. 功率计连上 Wi-Fi 后，网关页面应出现设备且数据刷新
5. 在页面或通过 API 下发控制，确认 PD 开关/电压档位变化

## 内网穿透

在电脑上部署 frp/ngrok 等，将公网端口转发到 **网关 IP:80** 即可远程访问 HTTP API 与 Web 页。

MQTT 端口 1883 一般**不需要**穿透（功率计与网关在同一局域网）。

## 多设备

每台功率计使用不同的 `NET_MQTT_DEVICE_ID`（如 `meter-01`、`meter-02`），Broker URI 指向同一网关 IP。
