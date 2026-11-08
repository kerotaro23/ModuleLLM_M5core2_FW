#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include "config.h"
#include "VoiceCore.h"
#include "TerminalUi.h"
#include "ConnectionAttempt.h"

namespace {
voice::Session session;
voice::Framer framer;
struct Request { char text[1025]; char id[64]; };
struct Result { bool ok; char text[1100]; char id[64]; };
QueueHandle_t requests, results;
uint32_t shownRevision = UINT32_MAX, requestCounter = 0, bootId;
std::string activeRequest;
unsigned page = 0;
bool configured = false, details = false, uiDirty = true;
device::ConnectionAttempt connection;

void connectWifi(uint32_t now) {
  if (!configured || !connection.start(now)) return;
  WiFi.disconnect();
  WiFi.begin(VOICE_WIFI_SSID, VOICE_WIFI_PASSWORD);
  uiDirty = true;
}
bool canInspect() {
  return session.acceptsNewConversation() && !session.microphoneMayBeOn();
}

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
int countdown(uint32_t now) {
  if (session.phase != voice::Phase::Listen && !session.confirming()) return -1;
  return static_cast<int>((session.remainingMs(now) + 999) / 1000);
}
std::string diagnostics() {
  std::string text = std::string("端末: ") + kDeviceId;
  text += "\nCore2 IP: " + std::string(WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "未接続");
  if (WiFi.status()==WL_CONNECTED) text += "\n電波: " + std::to_string(WiFi.RSSI()) + " dBm";
  text += "\nPi設定: " + std::string(strlen(VOICE_BRIDGE_IP) ? VOICE_BRIDGE_IP : "未設定") + ":" + std::to_string(kBridgePort);
  text += "\nPiの稼働状態は未確認";
  text += "\nModule FW: " + (session.version.empty() ? std::string("未取得") : session.version);
  text += "\nASR: " + session.whisperModel + "\nTTS: " + session.ttsModel;
  text += "\n空きRAM: " + std::to_string(ESP.getFreeHeap()/1024) + " KB";
  text += "\n空きPSRAM: " + std::to_string(ESP.getFreePsram()/1024) + " KB";
  return text;
}
void draw() {
  view.mood = static_cast<terminal_ui::Mood>(session.state);
  view.connected = WiFi.status()==WL_CONNECTED;
  view.mic = session.microphoneMayBeOn();
  view.configured = configured;
  view.confirm = session.confirming();
  view.details = details;
  view.canInspect = canInspect();
  view.remainingSeconds = countdown(millis());
  view.connecting = connection.connecting();
  view.reconnect = configured && !view.connected && session.acceptsNewConversation();
  const auto phase = session.phase;
  view.canStart = configured && !view.connecting &&
      (view.connected ? (view.confirm || session.acceptsNewConversation()) : view.reconnect);
  view.canCancel = phase!=voice::Phase::Http && phase!=voice::Phase::Idle &&
                   phase!=voice::Phase::FaultAck && phase!=voice::Phase::FaultDone && phase!=voice::Phase::Failed;
  // Keep failure/stop warnings visible even when credentials are not configured.
  if(session.state==voice::State::Failure) view.body=session.message;
  else if(!configured) view.body="Wi-FiとPiの接続設定を入れてね。設定方法はREADMEを見てね。";
  else if(view.confirm) view.body=session.transcript;
  else if(!session.reply.empty()) view.body=session.reply+"\n"+session.message;
  else if(view.connecting) view.body="Wi-Fiに接続中です (最大15秒)。接続後にAで会話を始めてね。";
  else if(!view.connected && phase==voice::Phase::Idle) view.body="Wi-Fiが切れています。Aで再接続できます。C長押しで接続情報を確認してね。";
  else if(phase==voice::Phase::Idle) view.body="こんにちは！予定や天気、メモのお手伝いをするよ。Aを押してね。";
  else view.body=session.message;
  if(!view.confirm && !session.transcript.empty()) view.body+="\n認識: "+session.transcript;
  if(session.state==voice::State::Failure && !session.reply.empty()) view.body+="\n返答: "+session.reply;
  if(!session.version.empty()) view.body+="\nModule FW: "+session.version;
  view.footer = view.mic ? "マイク動作中 / 停止確認中" : view.connected ? WiFi.localIP().toString().c_str() :
                !configured ? "接続設定が必要です" : view.connecting ? "Wi-Fi接続中 (最大15秒)" : "Wi-Fi未接続 / A:再接続";
  if (details) { view.body=diagnostics(); view.footer="C:次の頁 / 長押しで戻る"; }
  else if (view.canInspect && view.connected) view.footer += " / C長押し:情報";
  Surface surface{bufferedUi?static_cast<lgfx::LGFXBase&>(screen):static_cast<lgfx::LGFXBase&>(M5.Display)};
  terminal_ui::render(surface,view,page,animationFrame);
  if(bufferedUi) screen.pushSprite(0,0);
}
void animate(uint32_t now) {
  if(uint32_t(now-animationAt)<125) return;
  animationAt=now;
  const int remaining=countdown(now);
  if(!bufferedUi) {
    if(remaining!=view.remainingSeconds) draw();
    return;
  }
  view.remainingSeconds=remaining;
  ++animationFrame;
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
  M5.BtnC.setHoldThresh(700);
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
  connectWifi(millis());
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
  const auto event = connection.update(now, WiFi.status()==WL_CONNECTED);
  if (event != device::ConnectionAttempt::Event::None) uiDirty=true;
  if (event == device::ConnectionAttempt::Event::TimedOut) {
    WiFi.disconnect(); M5.Speaker.tone(300,180);
  } else if (event == device::ConnectionAttempt::Event::Disconnected) M5.Speaker.tone(300,180);
  // Deadline wins over a confirmation button pressed in the same loop.
  session.tick(now);
  if (M5.BtnA.wasPressed()) {
    if (details) { details=false; page=0; uiDirty=true; }
    else if (configured && !connection.connecting()) {
      if (WiFi.status()==WL_CONNECTED) session.pressA(now);
      else if (session.acceptsNewConversation()) connectWifi(now);
    }
  }
  if (M5.BtnB.wasPressed()) {
    details=false; page=0; uiDirty=true;
    session.cancel(now);
  }
  // A hold must not also advance a page; short clicks act on release.
  if (M5.BtnC.wasHold() && (details || canInspect())) {
    details=!details; page=0; uiDirty=true;
  } else if (M5.BtnC.wasClicked()) { ++page; uiDirty=true; }
  Result result;
  if (xQueueReceive(results, &result, 0) == pdTRUE && activeRequest == result.id)
    session.bridgeResult(result.ok, result.text, now);
  session.tick(now);
  if (shownRevision != session.revision) {
    shownRevision = session.revision; page = 0; details=false; uiDirty=true;
    if (session.phase == voice::Phase::Listen) M5.Speaker.tone(1200, 80);
    if (session.phase == voice::Phase::Failed) M5.Speaker.tone(300, 180);
    if (session.phase == voice::Phase::Spoken) M5.Speaker.tone(1800, 80);
  }
  if (uiDirty) { draw(); uiDirty=false; }
  animate(now);
  delay(2);
}
