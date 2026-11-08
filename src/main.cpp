#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include "config.h"
#include "VoiceCore.h"
#include <vector>

namespace {
voice::Session session;
voice::Framer framer;
struct Request { char text[1025]; char id[64]; };
struct Result { bool ok; char text[1100]; char id[64]; };
QueueHandle_t requests, results;
uint32_t shownRevision = UINT32_MAX, requestCounter = 0, bootId;
std::string activeRequest;
unsigned page = 0;
bool configured = false, wifiExpired = false;
uint32_t wifiStarted;
wl_status_t previousWifi = WL_IDLE_STATUS;

// All network I/O is isolated from UART, button handling and microphone deadlines.
void bridgeTask(void*) {
  Request request;
  for (;;) {
    if (xQueueReceive(requests, &request, portMAX_DELAY) != pdTRUE) continue;
    Result result{};
    strlcpy(result.id, request.id, sizeof(result.id));
    std::string explanation = "Wi-Fi未接続。送信していません";
    if (WiFi.status() == WL_CONNECTED) {
      WiFiClient client;
      HTTPClient http;
      http.setConnectTimeout(3000);
      http.setTimeout(5000);
      http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
      const String url = String("http://") + VOICE_BRIDGE_IP + ":" + String(kBridgePort) + "/v1/voice/commands";
      if (http.begin(client, url)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", String("Bearer ") + VOICE_DEVICE_TOKEN);
        http.addHeader("Idempotency-Key", request.id);
        JsonDocument d;
        d["request_id"] = request.id; d["device_id"] = kDeviceId;
        d["text"] = request.text; d["confirmed"] = true;
        String payload; serializeJson(d, payload);
        int code = http.POST(payload);
        explanation = "Pi応答エラー / 実行結果をPiで確認してください";
        if (code == 401 || code == 403) explanation = "Piが認証・命令を拒否しました";
        else if (code >= 300 && code < 400) explanation = "Piの転送応答を拒否しました";
        else if (code == 200) {
          const int size = http.getSize();
          // Contract requires Content-Length, limiting both memory and read duration.
          if (size > 0 && size <= 4096) {
            std::string body; body.reserve(size);
            uint32_t start = millis();
            auto* stream = http.getStreamPtr();
            while (body.size() < static_cast<size_t>(size) && millis() - start < 5000) {
              if (stream->available()) body += static_cast<char>(stream->read());
              else if (!client.connected()) break;
              else vTaskDelay(pdMS_TO_TICKS(2));
            }
            result.ok = body.size() == static_cast<size_t>(size) && voice::parseBridge(body, request.id, explanation);
            if (!result.ok) explanation = "Pi応答の形式・命令が不正。実行結果を確認してください";
          } else explanation = "Pi応答サイズ不正 / 実行結果を確認してください";
        }
        http.end();
      } else explanation = "Pi接続の初期化に失敗しました";
    }
    strlcpy(result.text, explanation.c_str(), sizeof(result.text));
    xQueueOverwrite(results, &result);
  }
}

std::vector<String> wrap(const std::string& text) {
  std::vector<String> lines;
  String line;
  for (size_t i = 0; i < text.size();) {
    unsigned char c = text[i];
    size_t len = c < 0x80 ? 1 : (c < 0xe0 ? 2 : (c < 0xf0 ? 3 : 4));
    len = std::min(len, text.size() - i);
    String glyph(text.substr(i, len).c_str()); i += len;
    if (c == '\n') { lines.push_back(line); line = ""; continue; }
    if (M5.Display.textWidth(line + glyph) > 300) { lines.push_back(line); line = ""; }
    line += glyph;
  }
  if (line.length()) lines.push_back(line);
  return lines;
}
void draw() {
  const char* labels[] = {"待機", "聞き取り中", "処理中", "成功", "失敗"};
  const uint16_t colors[] = {TFT_CYAN, TFT_YELLOW, TFT_ORANGE, TFT_GREEN, TFT_RED};
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(&fonts::efontJA_16);
  M5.Display.setTextColor(colors[static_cast<int>(session.state)]);
  M5.Display.setCursor(8, 4);
  M5.Display.printf("%s  MIC:%s", labels[static_cast<int>(session.state)], session.microphoneMayBeOn() ? "ON/確認中" : "OFF");
  M5.Display.setFont(&fonts::efontJA_12);
  M5.Display.setTextColor(TFT_LIGHTGREY);
  M5.Display.setCursor(8, 28);
  if (WiFi.status() == WL_CONNECTED) M5.Display.printf("Wi-Fi: %s", WiFi.localIP().toString().c_str());
  else M5.Display.print(wifiExpired ? "Wi-Fi接続失敗 (設定確認後に再起動)" : "Wi-Fi接続中");
  std::string body = configured ? session.message : "未設定: include/secrets.h を設定し書き込んでください";
  if (!session.version.empty()) body += "\nModule FW: " + session.version;
  if (!session.transcript.empty()) body += "\n認識: " + session.transcript;
  if (!session.reply.empty()) body += "\n返答: " + session.reply;
  auto lines = wrap(body);
  const unsigned pages = std::max(1U, (static_cast<unsigned>(lines.size()) + 8) / 9);
  page %= pages;
  M5.Display.setTextColor(TFT_WHITE);
  for (unsigned i = 0; i < 9 && page * 9 + i < lines.size(); ++i) {
    M5.Display.setCursor(8, 50 + i * 17);
    M5.Display.print(lines[page * 9 + i]);
  }
  M5.Display.setCursor(8, 215);
  M5.Display.printf("A:開始/確認  B:中止  C:頁 %u/%u", page + 1, pages);
}
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = false; // Avoid Core2 I2S/UART pin contention; use Module LLM's microphone.
  cfg.internal_spk = true;
  M5.begin(cfg);
  Serial.begin(115200);
  Serial2.setRxBufferSize(16384);
  Serial2.begin(115200, SERIAL_8N1, kModuleRx, kModuleTx);
  bootId = esp_random();
  session.whisperModel = VOICE_WHISPER_MODEL;
  session.ttsModel = VOICE_TTS_MODEL;
  session.uart = [](const std::string& raw) { Serial2.print(raw.c_str()); };
  requests = xQueueCreate(1, sizeof(Request)); results = xQueueCreate(1, sizeof(Result));
  if (!requests || !results || xTaskCreate(bridgeTask, "voice-http", 12288, nullptr, 1, nullptr) != pdPASS) {
    M5.Display.print("メモリ不足: 再起動してください");
    while (true) delay(1000);
  }
  session.submit = [](const std::string& text) {
    Request req{};
    snprintf(req.id, sizeof(req.id), "%s-%08lx-%lu", kDeviceId, static_cast<unsigned long>(bootId), static_cast<unsigned long>(++requestCounter));
    activeRequest = req.id;
    strlcpy(req.text, text.c_str(), sizeof(req.text));
    if (xQueueSend(requests, &req, 0) != pdTRUE)
      session.bridgeResult(false, "通信処理中です。送信していません", millis());
  };
  IPAddress address;
  configured = strlen(VOICE_WIFI_SSID) && strlen(VOICE_DEVICE_TOKEN) >= 24 &&
               address.fromString(VOICE_BRIDGE_IP) && voice::privateIp(address[0], address[1]);
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(false);
  wifiStarted = millis();
  if (configured) WiFi.begin(VOICE_WIFI_SSID, VOICE_WIFI_PASSWORD);
  session.begin(millis()); // Confirm any previous Module tasks have stopped.
  // No recording task is created until the user presses A.
  draw();
}
void loop() {
  M5.update();
  uint32_t now = millis();
  std::string frame;
  // Bound work per loop so a noisy UART cannot starve cancellation/timeouts.
  for (int count = 0; count < 1024 && Serial2.available(); ++count) {
    if (framer.feed(static_cast<char>(Serial2.read()), frame)) session.receive(frame, now);
  }
  if (framer.overflow) { framer.overflow = false; session.fail("UART応答が長すぎます", now); }
  const auto wifiStatus = WiFi.status();
  if (wifiStatus != previousWifi) {
    if (previousWifi == WL_CONNECTED) M5.Speaker.tone(300, 180);
    previousWifi = wifiStatus; shownRevision = UINT32_MAX;
  }
  if (!wifiExpired && WiFi.status() != WL_CONNECTED && now - wifiStarted >= 15000) {
    wifiExpired = true; WiFi.disconnect(); M5.Speaker.tone(300, 180); shownRevision = UINT32_MAX;
  }
  if (M5.BtnA.wasPressed() && configured) {
    if (WiFi.status() == WL_CONNECTED) session.pressA(now);
    else { session.message = "Wi-Fi未接続: 設定を確認して再起動してください"; ++session.revision; }
  }
  if (M5.BtnB.wasPressed()) session.cancel(now);
  if (M5.BtnC.wasPressed()) { ++page; draw(); }
  Result result;
  if (xQueueReceive(results, &result, 0) == pdTRUE && activeRequest == result.id)
    session.bridgeResult(result.ok, result.text, now);
  session.tick(now);
  if (shownRevision != session.revision) {
    shownRevision = session.revision; page = 0; draw();
    if (session.phase == voice::Phase::Listen) M5.Speaker.tone(1200, 80);
    if (session.phase == voice::Phase::Failed) M5.Speaker.tone(300, 180);
    if (session.phase == voice::Phase::Spoken) M5.Speaker.tone(1800, 80);
  }
  delay(2);
}
