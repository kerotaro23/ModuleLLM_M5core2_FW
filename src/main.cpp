#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include "config.h"
#include "VoiceCore.h"
#include "TerminalUi.h"

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

M5Canvas screen(&M5.Display), character(&M5.Display);
bool bufferedUi = false;
uint32_t animationAt = 0, animationFrame = 0;
terminal_ui::View view;

struct Surface {
  lgfx::LGFXBase& canvas;
  void rect(int x,int y,int w,int h,uint32_t c) { canvas.fillRect(x,y,w,h,c); }
  void round(int x,int y,int w,int h,int r,uint32_t c) { canvas.fillRoundRect(x,y,w,h,r,c); }
  void circle(int x,int y,int r,uint32_t c) { canvas.fillCircle(x,y,r,c); }
  void ellipse(int x,int y,int rx,int ry,uint32_t c) { canvas.fillEllipse(x,y,rx,ry,c); }
  void line(int x,int y,int x2,int y2,uint32_t c) { canvas.drawLine(x,y,x2,y2,c); }
  void font(int size) { canvas.setFont(size==16?&fonts::efontJA_16:&fonts::efontJA_12); }
  int width(const std::string& str,int size) { font(size); return canvas.textWidth(str.c_str()); }
  void text(const std::string& str,int x,int y,int size,uint32_t c) {
    font(size); canvas.setTextColor(c); canvas.setCursor(x,y); canvas.print(str.c_str());
  }
};
void draw() {
  view.mood = static_cast<terminal_ui::Mood>(session.state);
  view.connected = WiFi.status()==WL_CONNECTED;
  view.mic = session.microphoneMayBeOn();
  view.configured = configured;
  view.confirm = session.confirming();
  const auto phase = session.phase;
  view.canStart = configured && view.connected && (view.confirm || phase==voice::Phase::Idle ||
                  phase==voice::Phase::Spoken || phase==voice::Phase::Failed);
  view.canCancel = phase!=voice::Phase::Http && phase!=voice::Phase::Idle &&
                   phase!=voice::Phase::FaultAck && phase!=voice::Phase::FaultDone && phase!=voice::Phase::Failed;
  // Keep failure/stop warnings visible even when credentials are not configured.
  if(session.state==voice::State::Failure) view.body=session.message;
  else if(!configured) view.body="Wi-FiとPiの接続設定を入れてね。設定方法はREADMEを見てね。";
  else if(view.confirm) view.body=session.transcript;
  else if(!session.reply.empty()) view.body=session.reply+"\n"+session.message;
  else if(phase==voice::Phase::Idle) view.body="こんにちは！予定や天気、メモのお手伝いをするよ。Aを押してね。";
  else view.body=session.message;
  if(!view.confirm && !session.transcript.empty()) view.body+="\n認識: "+session.transcript;
  if(session.state==voice::State::Failure && !session.reply.empty()) view.body+="\n返答: "+session.reply;
  if(!session.version.empty()) view.body+="\nModule FW: "+session.version;
  view.footer = view.mic ? "マイク動作中 / 停止確認中" : view.connected ? WiFi.localIP().toString().c_str() :
                !configured ? "接続設定が必要です" : wifiExpired ? "Wi-Fi接続失敗 / 再起動してね" : "Wi-Fiに接続しています";
  Surface surface{bufferedUi?static_cast<lgfx::LGFXBase&>(screen):static_cast<lgfx::LGFXBase&>(M5.Display)};
  terminal_ui::render(surface,view,page,animationFrame);
  if(bufferedUi) screen.pushSprite(0,0);
}
void animate(uint32_t now) {
  if(!bufferedUi || uint32_t(now-animationAt)<125) return;
  animationAt=now; ++animationFrame;
  Surface surface{character};
  terminal_ui::hero(surface,view,animationFrame,0);
  character.pushSprite(0,28);
}
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = false; // Avoid Core2 I2S/UART pin contention; use Module LLM's microphone.
  cfg.internal_spk = true;
  M5.begin(cfg);
  screen.setColorDepth(16); character.setColorDepth(16);
  // Reserve internal RAM for Wi-Fi, UART and the HTTP worker.
  screen.setPsram(true); character.setPsram(true);
  bufferedUi = psramFound() && screen.createSprite(320,240) && character.createSprite(320,78);
  if(!bufferedUi) { screen.deleteSprite(); character.deleteSprite(); }
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
  animate(now);
  delay(2);
}
