#pragma once
#include <ArduinoJson.h>
#include <cstdint>
#include <functional>
#include <string>

namespace voice {
// UART JSON can be fragmented or concatenated; braces inside strings are not delimiters.
class Framer {
 public:
  std::string buffer;
  bool feed(char c, std::string& out) {
    if (buffer.empty()) { if (c != '{') return false; depth = 0; quoted = escaped = false; }
    buffer += c;
    if (buffer.size() > 8192) { clear(); overflow = true; return false; }
    if (quoted) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quoted = false;
    } else {
      if (c == '"') quoted = true;
      else if (c == '{') ++depth;
      else if (c == '}' && --depth == 0) { out.swap(buffer); clear(); return true; }
    }
    return false;
  }
  bool overflow = false;
  void clear() { buffer.clear(); depth = 0; quoted = escaped = false; }
 private:
  int depth = 0;
  bool quoted = false, escaped = false;
};

enum class State { Idle, Listening, Processing, Success, Failure };
enum class Phase { Idle, ResetAck, ResetDone, Version, Audio, Vad, Whisper, Listen,
                   StopWhisper, StopVad, Confirm, Http, Tts, Spoken, FaultAck, FaultDone, Failed };

class Session {
 public:
  State state = State::Idle;
  Phase phase = Phase::Idle;
  std::string message = "A: 話す", transcript, reply, version;
  std::string whisperModel = "whisper-tiny", ttsModel = "melotts-ja-jp";
  std::function<void(const std::string&)> uart;
  std::function<void(const std::string&)> submit;
  uint32_t revision = 0;
  bool microphoneMayBeOn() const {
    return phase == Phase::ResetAck || phase == Phase::ResetDone || phase == Phase::Vad || phase == Phase::Whisper || phase == Phase::Listen ||
           phase == Phase::StopWhisper || phase == Phase::StopVad ||
           phase == Phase::FaultAck || phase == Phase::FaultDone || unsafeStop;
  }
  bool confirming() const { return phase == Phase::Confirm; }
  bool httpPending() const { return phase == Phase::Http; }
  void begin(uint32_t now) {
    bootReset = true;
    fault = "起動時のModule停止確認に失敗";
    set(State::Processing, "起動時のマイク停止確認中", Phase::FaultAck, now, 5000);
    command("sys", "reset");
  }
  void pressA(uint32_t now) {
    if (phase == Phase::Confirm) {
      set(State::Processing, "Piへ送信中", Phase::Http, now, 15000);
      submit(transcript); return;
    }
    if (phase != Phase::Idle && phase != Phase::Spoken && phase != Phase::Failed) return;
    transcript.clear(); reply.clear(); fault.clear(); unsafeStop = false;
    set(State::Processing, "Module LLMを準備中", Phase::ResetAck, now, 5000);
    command("sys", "reset");
  }
  void cancel(uint32_t now) {
    if (phase == Phase::Http) return; // A sent command cannot be recalled.
    if (phase == Phase::FaultAck || phase == Phase::FaultDone || phase == Phase::Failed || phase == Phase::Idle) return;
    fail("中止しました", now);
  }
  void tick(uint32_t now) {
    if (!timeout || uint32_t(now - started) < timeout) return;
    if (phase == Phase::FaultAck || phase == Phase::FaultDone) {
      unsafeStop = true;
      set(State::Failure, fault + " / 停止未確認: Moduleの電源を切ってください", Phase::Failed, now, 0);
    } else if (phase == Phase::Http) {
      // Never retry an operation with an unknown outcome.
      set(State::Failure, "Pi応答なし: 実行結果不明。自動再送しません", Phase::Failed, now, 0);
    } else fail(phase == Phase::Listen ? "音声を認識できませんでした" : "Module/確認待ちタイムアウト", now);
  }
  void fail(const std::string& why, uint32_t now) {
    if (phase == Phase::FaultAck || phase == Phase::FaultDone) return;
    bootReset = false;
    fault = why;
    set(State::Failure, why + " / マイク停止確認中", Phase::FaultAck, now, 5000);
    command("sys", "reset");
  }
  void receive(const std::string& raw, uint32_t now) {
    JsonDocument d;
    if (deserializeJson(d, raw) || !d.is<JsonObject>()) return;
    const std::string id = d["request_id"] | "";
    const std::string work = d["work_id"] | "";
    const std::string object = d["object"] | "";
    const bool ok = d["error"]["code"].is<int>() && d["error"]["code"].as<int>() == 0;
    if ((phase == Phase::ResetDone || phase == Phase::FaultDone) && id == "0" && work == "sys" &&
        d["error"]["message"].as<std::string>() == "reset over") {
      if (!ok) return;
      if (phase == Phase::FaultDone) {
        unsafeStop = false;
        set(bootReset ? State::Idle : State::Failure, bootReset ? "A: 話す" : fault, bootReset ? Phase::Idle : Phase::Failed, now, 0);
        bootReset = false;
      } else {
        set(State::Processing, "FWバージョン確認中", Phase::Version, now, 5000);
        command("sys", "version");
      }
      return;
    }
    if (phase == Phase::Listen && work == whisperId && id == asrRequest && object == "asr.utf-8") {
      if (!ok || !d["data"].is<const char*>()) { fail("認識応答エラー", now); return; }
      transcript = d["data"].as<std::string>();
      if (transcript.empty() || transcript.size() > 1024 || transcript.find_first_not_of(" \r\n\t") == std::string::npos) {
        fail("認識文が空または長すぎます", now); return;
      }
      set(State::Processing, "マイク停止中", Phase::StopWhisper, now, 5000);
      command(whisperId, "exit"); return;
    }
    if (phase == Phase::Listen && work == whisperId && !ok) { fail("Whisperエラー", now); return; }
    if (phase == Phase::Spoken && work == ttsId && !ok) { fail("Pi処理済み / 音声合成エラー", now); return; }
    if (id != pending) return; // Late responses from previous requests must not advance state.
    if (!ok) {
      if (phase == Phase::FaultAck || phase == Phase::FaultDone) return;
      fail("Moduleエラー code=" + std::to_string(d["error"]["code"] | -999), now); return;
    }
    switch (phase) {
      case Phase::ResetAck: set(State::Processing, message, Phase::ResetDone, now, 20000); break;
      case Phase::FaultAck: set(State::Failure, message, Phase::FaultDone, now, 20000); break;
      case Phase::Version: {
        if (!d["data"].is<const char*>()) { fail("FWバージョン応答不正", now); break; }
        version = d["data"].as<std::string>();
        JsonDocument c;
        c["data"]["capcard"] = 0; c["data"]["capdevice"] = 0; c["data"]["capVolume"] = 0.5;
        c["data"]["playcard"] = 0; c["data"]["playdevice"] = 1; c["data"]["playVolume"] = 0.15;
        set(State::Processing, "音声入出力を準備中", Phase::Audio, now, 5000);
        command("audio", "setup", "audio.setup", &c);
        break;
      }
      case Phase::Audio: {
        if (work.find("audio.") != 0) { fail("audio ID不正", now); break; }
        JsonDocument c;
        c["data"]["model"] = "silero-vad"; c["data"]["response_format"] = "vad.bool";
        c["data"]["enoutput"] = false; c["data"]["input"].add("sys.pcm");
        set(State::Processing, "マイクON / VAD準備中", Phase::Vad, now, 30000);
        command("vad", "setup", "vad.setup", &c); break;
      }
      case Phase::Vad: {
        if (work.find("vad.") != 0) { fail("VAD ID不正", now); break; }
        vadId = work;
        JsonDocument c;
        c["data"]["model"] = whisperModel; c["data"]["language"] = "ja";
        c["data"]["response_format"] = "asr.utf-8"; c["data"]["enoutput"] = true;
        c["data"]["input"].add("sys.pcm"); c["data"]["input"].add(vadId);
        set(State::Processing, "マイクON / Whisper準備中", Phase::Whisper, now, 30000);
        command("whisper", "setup", "whisper.setup", &c); break;
      }
      case Phase::Whisper:
        if (work.find("whisper.") != 0) { fail("Whisper ID不正", now); break; }
        whisperId = work; asrRequest = pending;
        set(State::Listening, "話してください (最大15秒) / B: 中止", Phase::Listen, now, 15000); break;
      case Phase::StopWhisper:
        set(State::Processing, "マイク停止中", Phase::StopVad, now, 5000);
        command(vadId, "exit"); break;
      case Phase::StopVad:
        set(State::Processing, "この内容を送信? A: 確認 / B: 中止", Phase::Confirm, now, 30000); break;
      case Phase::Tts: {
        if (work.find("melotts.") != 0) { fail("Pi処理済み / TTS ID不正", now); break; }
        ttsId = work;
        JsonDocument c;
        c["data"]["delta"] = reply; c["data"]["index"] = 0; c["data"]["finish"] = true;
        command(ttsId, "inference", "tts.utf-8.stream", &c);
        set(State::Success, "Pi処理完了 / 音声再生要求済み\nA: 次の会話 / B: 停止", Phase::Spoken, now, 0); break;
      }
      default: break;
    }
  }
  void bridgeResult(bool ok, const std::string& response, uint32_t now) {
    if (phase != Phase::Http) return;
    if (!ok) { set(State::Failure, response, Phase::Failed, now, 0); return; }
    reply = response;
    JsonDocument c;
    c["data"]["model"] = ttsModel; c["data"]["response_format"] = "sys.pcm";
    c["data"]["input"].add("tts.utf-8.stream"); c["data"]["enoutput"] = false; c["data"]["enaudio"] = true;
    set(State::Processing, "Pi処理完了 / 音声合成準備中", Phase::Tts, now, 30000);
    command("melotts", "setup", "melotts.setup", &c);
  }
 private:
  uint32_t started = 0, timeout = 0, sequence = 0;
  bool unsafeStop = false, bootReset = false;
  std::string pending, asrRequest, whisperId, vadId, ttsId, fault;
  void set(State s, const std::string& text, Phase p, uint32_t now, uint32_t limit) {
    state = s; message = text; phase = p; started = now; timeout = limit; ++revision;
  }
  void command(const std::string& work, const char* action, const char* object = "None", JsonDocument* data = nullptr) {
    JsonDocument d;
    pending = "core2-" + std::to_string(++sequence);
    d["request_id"] = pending; d["work_id"] = work; d["action"] = action; d["object"] = object;
    if (data) d["data"] = (*data)["data"]; else d["data"] = "None";
    std::string raw; serializeJson(d, raw); uart(raw);
  }
};

inline bool allowedIntent(const std::string& s) {
  return s == "todo.read" || s == "weather.read" || s == "shopping.append" ||
         s == "memo.append" || s == "news.read" || s == "aeon.thanks_day.read";
}
inline bool privateIp(unsigned a, unsigned b) {
  return a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168);
}
inline bool parseBridge(const std::string& raw, const std::string& expectedId, std::string& reply) {
  JsonDocument d;
  if (raw.size() > 4096 || deserializeJson(d, raw)) return false;
  if (!d["request_id"].is<const char*>() || d["request_id"].as<std::string>() != expectedId ||
      !d["ok"].is<bool>() || !d["ok"].as<bool>() || !d["intent"].is<const char*>() ||
      !allowedIntent(d["intent"].as<std::string>()) || !d["reply"].is<const char*>()) return false;
  reply = d["reply"].as<std::string>();
  return !reply.empty() && reply.size() <= 1024;
}
} // namespace voice
