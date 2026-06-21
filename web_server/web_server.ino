/**
 * ESP32-S3 功率计网关
 *
 * - 内置 MQTT Broker（端口 1883），功率计作为客户端连接
 * - HTTP 服务（端口 80）：查看设备状态、下发 PD 控制命令
 *
 * 依赖库（Arduino 库管理器安装）：
 *   - ArduinoJson by Benoit Blanchon
 *   - sMQTTBroker by terrorsl
 *
 * 使用前请修改下方 USER CONFIG 区域。
 */

// ======================== USER CONFIG ========================
#define WIFI_SSID       "A2-606"
#define WIFI_PASS       "1742137034"

#define MQTT_PORT       1883
#define HTTP_PORT       80

/** 超过该时间（毫秒）未收到遥测则视为离线 */
#define DEVICE_OFFLINE_MS  15000UL

/** 控制命令等待设备确认的超时（毫秒） */
#define CTRL_PENDING_MS    20000UL

/** 最多缓存的设备数量 */
#define MAX_DEVICES     8
// =============================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <sMQTTBroker.h>

typedef struct {
    char          id[32];
    float         voltage_v;
    float         current_a;
    float         power_w;
    bool          pd_enabled;
    int           pd_voltage;
    bool          pd_pg_ok;
    unsigned long last_seen_ms;
    bool          valid;
    bool          ctrl_pending;
    bool          ctrl_pending_en;
    int           ctrl_pending_v;
    unsigned long ctrl_pending_until_ms;
} device_state_t;

static device_state_t s_devices[MAX_DEVICES];
static WebServer      httpServer(HTTP_PORT);
static String         s_root_page;

static int find_device_index(const char *id)
{
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (s_devices[i].valid && strcmp(s_devices[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

static int alloc_device_index(const char *id)
{
    const int existing = find_device_index(id);
    if (existing >= 0) {
        return existing;
    }
    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (!s_devices[i].valid) {
            memset(&s_devices[i], 0, sizeof(s_devices[i]));
            strncpy(s_devices[i].id, id, sizeof(s_devices[i].id) - 1U);
            s_devices[i].valid = true;
            return i;
        }
    }
    return -1;
}

static bool extract_device_id_from_topic(const char *topic, char *out, size_t out_len)
{
    const char *prefix = "power/";
    const char *suffix = "/telemetry";
    const size_t prefix_len = strlen(prefix);
    const size_t suffix_len = strlen(suffix);
    const size_t topic_len  = strlen(topic);

    if (topic_len <= prefix_len + suffix_len) {
        return false;
    }
    if (strncmp(topic, prefix, prefix_len) != 0) {
        return false;
    }
    if (strcmp(topic + topic_len - suffix_len, suffix) != 0) {
        return false;
    }

    const size_t id_len = topic_len - prefix_len - suffix_len;
    if (id_len == 0 || id_len >= out_len) {
        return false;
    }
    memcpy(out, topic + prefix_len, id_len);
    out[id_len] = '\0';
    return true;
}

static void apply_telemetry_pd(device_state_t *dev, bool en, int volt)
{
    const unsigned long now_ms = millis();

    if (dev->ctrl_pending) {
        if (now_ms >= dev->ctrl_pending_until_ms) {
            dev->ctrl_pending = false;
        } else if (en == dev->ctrl_pending_en && volt == dev->ctrl_pending_v) {
            dev->ctrl_pending = false;
        } else {
            dev->pd_enabled = dev->ctrl_pending_en;
            dev->pd_voltage = dev->ctrl_pending_v;
            return;
        }
    }

    dev->pd_enabled = en;
    dev->pd_voltage = volt;
}

static void update_device_from_json(const char *device_id, const char *json, size_t len)
{
    StaticJsonDocument<384> doc;
    const DeserializationError err = deserializeJson(doc, json, len);
    if (err) {
        Serial.printf("[GW] JSON parse error: %s\n", err.c_str());
        return;
    }

    const int idx = alloc_device_index(device_id);
    if (idx < 0) {
        Serial.println("[GW] device table full");
        return;
    }

    device_state_t *dev = &s_devices[idx];
    strncpy(dev->id, device_id, sizeof(dev->id) - 1U);

    dev->voltage_v    = doc["voltage_v"]  | 0.0f;
    dev->current_a    = doc["current_a"]  | 0.0f;
    dev->power_w      = doc["power_w"]    | 0.0f;
    dev->pd_pg_ok     = doc["pd_pg_ok"]   | false;
    dev->last_seen_ms = millis();
    dev->valid        = true;

    apply_telemetry_pd(dev, doc["pd_enabled"] | false, doc["pd_voltage"] | 5);
}

class GatewayBroker : public sMQTTBroker {
public:
    bool onEvent(sMQTTEvent *event) override
    {
        if (!event) {
            return true;
        }

        switch (event->Type()) {
        case NewClient_sMQTTEventType:
            Serial.println("[GW] MQTT client connected");
            break;
        case RemoveClient_sMQTTEventType:
            Serial.println("[GW] MQTT client disconnected");
            break;
        case Public_sMQTTEventType: {
            auto *e = (sMQTTPublicClientEvent *)event;
            const std::string &topic   = e->Topic();
            const std::string &payload = e->Payload();
            char device_id[32];
            if (extract_device_id_from_topic(topic.c_str(), device_id, sizeof(device_id))) {
                update_device_from_json(device_id, payload.c_str(), payload.length());
                Serial.printf("[GW] telemetry from %s\n", device_id);
            }
            break;
        }
        case LostConnect_sMQTTEventType:
            Serial.println("[GW] WiFi lost");
            break;
        default:
            break;
        }
        return true;
    }
};

static GatewayBroker mqttBroker;

static void mqtt_publish_command(const char *topic, const char *payload)
{
    mqttBroker.publish(std::string(topic), std::string(payload));
}

static void connect_wifi(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(WIFI_PS_NONE);

    if (!mqttBroker.init(MQTT_PORT)) {
        Serial.println("[GW] MQTT broker init failed");
    } else {
        Serial.printf("[GW] MQTT broker started on port %d\n", MQTT_PORT);
    }

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[GW] Connecting WiFi %s", WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED) {
        mqttBroker.update();
        delay(50);
        Serial.print('.');
    }
    Serial.println();
    Serial.print("[GW] WiFi IP: ");
    Serial.println(WiFi.localIP());
}

static void append_device_json(const device_state_t *dev, JsonObject obj, unsigned long now_ms)
{
    obj["device_id"]    = dev->id;
    obj["online"]       = (now_ms - dev->last_seen_ms) <= DEVICE_OFFLINE_MS;
    obj["voltage_v"]    = dev->voltage_v;
    obj["current_a"]    = dev->current_a;
    obj["power_w"]      = dev->power_w;
    obj["pd_enabled"]   = dev->pd_enabled;
    obj["pd_voltage"]   = dev->pd_voltage;
    obj["pd_pg_ok"]     = dev->pd_pg_ok;
    obj["last_seen_ms"] = dev->last_seen_ms;
}

static void handle_api_devices(void)
{
    StaticJsonDocument<2048> doc;
    JsonArray arr = doc.to<JsonArray>();
    const unsigned long now_ms = millis();

    for (int i = 0; i < MAX_DEVICES; ++i) {
        if (!s_devices[i].valid) {
            continue;
        }
        JsonObject obj = arr.add<JsonObject>();
        append_device_json(&s_devices[i], obj, now_ms);
    }

    String out;
    serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

static bool parse_path_device_id(const String &uri, char *out, size_t out_len)
{
    const String prefix = "/api/devices/";
    if (!uri.startsWith(prefix)) {
        return false;
    }
    String rest = uri.substring(prefix.length());
    const int slash = rest.indexOf('/');
    const String id = (slash >= 0) ? rest.substring(0, slash) : rest;
    if (id.length() == 0 || id.length() >= out_len) {
        return false;
    }
    id.toCharArray(out, out_len);
    return true;
}

static void handle_api_device_detail(void)
{
    char device_id[32];
    if (!parse_path_device_id(httpServer.uri(), device_id, sizeof(device_id))) {
        httpServer.send(400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }

    const int idx = find_device_index(device_id);
    if (idx < 0) {
        httpServer.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }

    StaticJsonDocument<384> doc;
    JsonObject obj = doc.to<JsonObject>();
    append_device_json(&s_devices[idx], obj, millis());

    String out;
    serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
}

static void handle_api_device_control(void)
{
    char device_id[32];
    if (!parse_path_device_id(httpServer.uri(), device_id, sizeof(device_id))) {
        httpServer.send(400, "application/json", "{\"error\":\"bad path\"}");
        return;
    }

    if (!httpServer.hasArg("plain")) {
        httpServer.send(400, "application/json", "{\"error\":\"missing body\"}");
        return;
    }

    StaticJsonDocument<256> req;
    const DeserializationError err = deserializeJson(req, httpServer.arg("plain"));
    if (err) {
        httpServer.send(400, "application/json", "{\"error\":\"invalid json\"}");
        return;
    }

    StaticJsonDocument<128> cmd;
    bool has_field = false;

    if (req.containsKey("pd_enabled")) {
        cmd["pd_enabled"] = req["pd_enabled"].as<bool>();
        has_field = true;
    }
    if (req.containsKey("pd_voltage")) {
        cmd["pd_voltage"] = req["pd_voltage"].as<int>();
        has_field = true;
    }

    if (!has_field) {
        httpServer.send(400, "application/json", "{\"error\":\"no control fields\"}");
        return;
    }

    const int idx = find_device_index(device_id);
    if (idx >= 0) {
        if (req.containsKey("pd_enabled")) {
            s_devices[idx].pd_enabled = req["pd_enabled"].as<bool>();
            s_devices[idx].ctrl_pending_en = s_devices[idx].pd_enabled;
        }
        if (req.containsKey("pd_voltage")) {
            s_devices[idx].pd_voltage = req["pd_voltage"].as<int>();
            s_devices[idx].ctrl_pending_v = s_devices[idx].pd_voltage;
        }
        s_devices[idx].ctrl_pending = true;
        s_devices[idx].ctrl_pending_until_ms = millis() + CTRL_PENDING_MS;
    }

    String payload;
    serializeJson(cmd, payload);

    const String topic = String("power/") + device_id + "/command";
    mqtt_publish_command(topic.c_str(), payload.c_str());
    Serial.printf("[GW] command -> %s : %s\n", topic.c_str(), payload.c_str());

    httpServer.send(200, "application/json", "{\"ok\":true}");
}

static void build_root_page(void)
{
    s_root_page = R"HTML(<!DOCTYPE html>
<html lang="zh-CN"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>功率计网关</title>
<style>
body{font-family:sans-serif;margin:16px;background:#111;color:#eee}
.card{border:1px solid #333;border-radius:8px;padding:12px;margin:12px 0;background:#1b1b1b}
.online{color:#4caf50}.offline{color:#888}
button,input,select{margin:4px 0;padding:6px}
</style></head><body>
<h1>功率计网关</h1>
<p>MQTT Broker 端口 )HTML";
    s_root_page += String(MQTT_PORT);
    s_root_page += R"HTML( | HTTP 端口 )HTML";
    s_root_page += String(HTTP_PORT);
    s_root_page += R"HTML(</p>
<div id="list">加载中...</div>
<script>
function ensureCard(d){
  let card=document.getElementById('card_'+d.device_id);
  if(card)return card;
  card=document.createElement('div');
  card.className='card';
  card.id='card_'+d.device_id;
  card.innerHTML=`<h3>${d.device_id} <span id="ol_${d.device_id}" class="online">在线</span></h3>
    <div id="st_${d.device_id}"></div>
    <label>PD 开关 <select id="en_${d.device_id}"><option value="true">开</option><option value="false">关</option></select></label><br>
    <label>电压档位 <select id="v_${d.device_id}">
      <option value="5">5V</option><option value="9">9V</option><option value="12">12V</option>
      <option value="15">15V</option><option value="20">20V</option><option value="28">28V</option>
    </select></label><br>
    <button onclick="send('${d.device_id}')">下发控制</button>
    <span id="msg_${d.device_id}" style="margin-left:8px;color:#888"></span>`;
  document.getElementById('list').appendChild(card);
  document.getElementById('en_'+d.device_id).value=String(d.pd_enabled);
  document.getElementById('v_'+d.device_id).value=String(d.pd_voltage);
  return card;
}
function updateStatus(d){
  const ol=document.getElementById('ol_'+d.device_id);
  const st=document.getElementById('st_'+d.device_id);
  if(ol)ol.className=d.online?'online':'offline',ol.textContent=d.online?'在线':'离线';
  if(st)st.textContent=`电压: ${d.voltage_v.toFixed(3)} V | 电流: ${d.current_a.toFixed(3)} A | 功率: ${d.power_w.toFixed(3)} W | PD: ${d.pd_enabled?'开':'关'} | 档位: ${d.pd_voltage} V | PG: ${d.pd_pg_ok?'OK':'--'}`;
}
async function load(){
  const r=await fetch('/api/devices');
  const arr=await r.json();
  const root=document.getElementById('list');
  if(!arr.length){root.innerHTML='<p>暂无设备上报，请确认功率计已联网且 DEVICE_ID / Broker 地址正确。</p>';return;}
  if(!root.querySelector('.card')&&arr.length)root.innerHTML='';
  for(const d of arr){ensureCard(d);updateStatus(d);}
}
async function send(id){
  const en=document.getElementById('en_'+id).value;
  const v=document.getElementById('v_'+id).value;
  const msg=document.getElementById('msg_'+id);
  if(msg)msg.textContent='下发中...';
  const body={pd_enabled:en==='true',pd_voltage:parseInt(v,10)};
  try{
    const r=await fetch('/api/devices/'+encodeURIComponent(id)+'/control',{
      method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)
    });
    const text=await r.text();
    if(msg)msg.textContent=r.ok?'已下发，请看上方状态行是否更新':('失败: '+text);
  }catch(e){
    if(msg)msg.textContent='请求失败';
  }
}
load(); setInterval(load,5000);
</script></body></html>)HTML";
}

static void handle_root_page(void)
{
    httpServer.send(200, "text/html; charset=utf-8", s_root_page);
}

static void handle_not_found(void)
{
    httpServer.send(404, "application/json", "{\"error\":\"not found\"}");
}

void setup()
{
    Serial.begin(115200);
    delay(200);

    memset(s_devices, 0, sizeof(s_devices));
    connect_wifi();

    build_root_page();

    httpServer.on("/", HTTP_GET, handle_root_page);
    httpServer.on("/api/devices", HTTP_GET, handle_api_devices);
    httpServer.onNotFound([]() {
        const String uri = httpServer.uri();
        if (httpServer.method() == HTTP_GET && uri.startsWith("/api/devices/") && !uri.endsWith("/control")) {
            handle_api_device_detail();
            return;
        }
        if (httpServer.method() == HTTP_POST && uri.startsWith("/api/devices/") && uri.endsWith("/control")) {
            handle_api_device_control();
            return;
        }
        handle_not_found();
    });
    httpServer.begin();
    Serial.printf("[GW] HTTP server started on port %d\n", HTTP_PORT);
}

void loop()
{
    /* HTTP 优先：内网穿透依赖 80 端口快速响应，不可被 MQTT 阻塞 */
    for (int i = 0; i < 4; ++i) {
        httpServer.handleClient();
    }
    mqttBroker.update();
}
