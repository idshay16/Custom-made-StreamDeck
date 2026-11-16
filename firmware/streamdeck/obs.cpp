// obs.cpp — obs-websocket v5 client (built into OBS 28+, Tools -> WebSocket
// Server Settings). Runs inside the net task; other tasks hand it requests
// through a queue.
//
// Protocol: server sends Hello (op 0) with an optional auth challenge ->
// we send Identify (op 1) -> Identified (op 2). Then Request (op 6) /
// RequestResponse (op 7), and Events (op 5) for the subscriptions we asked for.
// Auth: base64(sha256( base64(sha256(password + salt)) + challenge )).
#include "app.h"
#include <WebSocketsClient.h>
#include <atomic>
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"

static WebSocketsClient g_ws;
static QueueHandle_t req_q() {   // char* request JSON, malloc'd; thread-safe lazy init
  static QueueHandle_t q = xQueueCreate(16, sizeof(char *));
  return q;
}
static std::atomic<bool> g_reconf{true};
static bool g_begun = false, g_identified = false;
static uint32_t g_req_id = 0;
static ConfigPtr g_seen_cfg;

static constexpr int SUB_SCENES = 1 << 2, SUB_INPUTS = 1 << 3, SUB_OUTPUTS = 1 << 6;

static String b64_sha256(const String &in) {
  uint8_t hash[32];
  mbedtls_sha256((const uint8_t *)in.c_str(), in.length(), hash, 0);
  uint8_t out[64];
  size_t olen = 0;
  mbedtls_base64_encode(out, sizeof out, &olen, hash, sizeof hash);
  return String((const char *)out, olen);
}

static void send_doc(JsonDocument &d) {
  String s;
  serializeJson(d, s);
  g_ws.sendTXT(s);
}

static void send_request(const char *type, const String &id, JsonObjectConst data = JsonObjectConst()) {
  JsonDocument d;
  d["op"] = 6;
  d["d"]["requestType"] = type;
  d["d"]["requestId"] = id;
  if (!data.isNull()) d["d"]["requestData"] = data;
  send_doc(d);
}

// Ask for the mute state of every input that a key on any page shows.
static void query_mutes(const Config &cfg) {
  for (JsonObjectConst pg : cfg.doc["pages"].as<JsonArrayConst>())
    for (JsonObjectConst key : pg["keys"].as<JsonArrayConst>()) {
      const char *st = key["state"] | "";
      if (strncmp(st, "obs.mute:", 9)) continue;
      JsonDocument q;
      q["inputName"] = st + 9;
      send_request("GetInputMute", String("mute:") + (st + 9), q.as<JsonObjectConst>());
    }
}

static void on_identified() {
  g_identified = true;
  state::set("obs.connected", true);
  LOG("obs: identified");
  send_request("GetCurrentProgramScene", "init:scene");
  send_request("GetRecordStatus", "init:record");
  send_request("GetStreamStatus", "init:stream");
  g_seen_cfg = config::get();
  query_mutes(*g_seen_cfg);
}

static void on_text(const uint8_t *payload, size_t len) {
  JsonDocument d;
  if (deserializeJson(d, payload, len)) return;
  int op = d["op"] | -1;
  JsonObjectConst m = d["d"];

  if (op == 0) {   // Hello
    JsonDocument id;
    id["op"] = 1;
    id["d"]["rpcVersion"] = 1;
    id["d"]["eventSubscriptions"] = SUB_SCENES | SUB_INPUTS | SUB_OUTPUTS;
    if (!m["authentication"].isNull()) {
      Settings s = settings::get();
      String secret = b64_sha256(s.obs_pass + (m["authentication"]["salt"] | ""));
      id["d"]["authentication"] = b64_sha256(secret + (m["authentication"]["challenge"] | ""));
    }
    send_doc(id);
  } else if (op == 2) {
    on_identified();
  } else if (op == 5) {   // Event
    const char *t = m["eventType"] | "";
    JsonObjectConst e = m["eventData"];
    if (!strcmp(t, "CurrentProgramSceneChanged")) state::set_str("obs.scene", e["sceneName"] | "");
    else if (!strcmp(t, "RecordStateChanged")) state::set("obs.record", e["outputActive"] | false);
    else if (!strcmp(t, "StreamStateChanged")) state::set("obs.stream", e["outputActive"] | false);
    else if (!strcmp(t, "InputMuteStateChanged"))
      state::set(String("obs.mute:") + (e["inputName"] | ""), e["inputMuted"] | false);
  } else if (op == 7) {   // RequestResponse
    const char *id = m["requestId"] | "";
    JsonObjectConst r = m["responseData"];
    if (!(m["requestStatus"]["result"] | false)) {
      LOG("obs: request %s failed: %s", m["requestType"] | "?", m["requestStatus"]["comment"] | "");
      return;
    }
    if (!strcmp(id, "init:scene"))
      state::set_str("obs.scene", r["currentProgramSceneName"] | (r["sceneName"] | ""));
    else if (!strcmp(id, "init:record")) state::set("obs.record", r["outputActive"] | false);
    else if (!strcmp(id, "init:stream")) state::set("obs.stream", r["outputActive"] | false);
    else if (!strncmp(id, "mute:", 5)) state::set(String("obs.mute:") + (id + 5), r["inputMuted"] | false);
  }
}

static void on_ws(WStype_t type, uint8_t *payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED:
      LOG("obs: socket connected");
      break;
    case WStype_DISCONNECTED:
      if (g_identified) LOG("obs: disconnected");
      g_identified = false;
      state::set("obs.connected", false);
      break;
    case WStype_TEXT:
      on_text(payload, len);
      break;
    default:
      break;
  }
}

void obs::reconfigure() { g_reconf = true; }

void obs::loop() {
  if (g_reconf.exchange(false)) {
    Settings s = settings::get();
    if (g_begun) g_ws.disconnect();
    g_begun = false;
    g_identified = false;
    state::set("obs.connected", false);
    if (s.obs_host.length()) {
      LOG("obs: connecting to ws://%s:%u", s.obs_host.c_str(), s.obs_port);
      g_ws.begin(s.obs_host.c_str(), s.obs_port, "/");
      g_ws.onEvent(on_ws);
      g_ws.setReconnectInterval(5000);
      g_begun = true;
    }
  }
  if (!g_begun) return;
  g_ws.loop();

  char *msg;
  while (xQueueReceive(req_q(), &msg, 0) == pdTRUE) {
    if (g_identified) g_ws.sendTXT(msg);
    else LOG("obs: not connected, dropped request");
    free(msg);
  }
  if (g_identified) {
    ConfigPtr c = config::get();
    if (c != g_seen_cfg) {   // config changed: there may be new inputs to watch
      g_seen_cfg = c;
      query_mutes(*c);
    }
  }
}

bool obs::request(const char *requestType, JsonObjectConst data) {
  JsonDocument d;
  d["op"] = 6;
  d["d"]["requestType"] = requestType;
  d["d"]["requestId"] = String("a:") + (++g_req_id);
  if (!data.isNull()) d["d"]["requestData"] = data;
  String s;
  serializeJson(d, s);
  char *p = strdup(s.c_str());
  if (!p || xQueueSend(req_q(), &p, 0) != pdTRUE) {
    free(p);
    return false;
  }
  return true;
}

void obs::action(JsonObjectConst a) {
  const char *op = a["op"] | "";
  JsonDocument q;
  if (!strcmp(op, "scene")) {
    q["sceneName"] = a["scene"];
    request("SetCurrentProgramScene", q.as<JsonObjectConst>());
  } else if (!strcmp(op, "record")) {
    request("ToggleRecord", JsonObjectConst());
  } else if (!strcmp(op, "stream")) {
    request("ToggleStream", JsonObjectConst());
  } else if (!strcmp(op, "mute")) {
    q["inputName"] = a["input"];
    request("ToggleInputMute", q.as<JsonObjectConst>());
  } else if (!strcmp(op, "raw")) {
    request(a["request"] | "", a["data"].as<JsonObjectConst>());
  }
}
