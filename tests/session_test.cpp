#include "VoiceCore.h"
#include <cassert>
#include <iostream>
#include <vector>
using namespace voice;
struct Rig {
  Session s;
  std::vector<std::string> sent, posted;
  uint32_t time = 100;
  Rig() {
    s.uart = [&](const std::string& v) { sent.push_back(v); };
    s.submit = [&](const std::string& v) { posted.push_back(v); };
  }
  JsonDocument last() { JsonDocument d; assert(!deserializeJson(d, sent.back())); return d; }
  void ack(const char* work, int code=0) {
    JsonDocument d; d["request_id"] = last()["request_id"];
    d["work_id"] = work; d["data"] = "v1.6"; d["error"]["code"] = code;
    std::string raw; serializeJson(d, raw); s.receive(raw, ++time);
  }
  void resetDone() { s.receive(R"({"request_id":"0","work_id":"sys","error":{"code":0,"message":"reset over"}})", ++time); }
  void listen() {
    s.pressA(time); assert(last()["action"] == "reset");
    ack("sys"); resetDone(); ack("sys"); ack("audio.1"); ack("vad.2");
    assert(last()["data"]["language"] == "ja");
    assert(last()["data"]["input"][1] == "vad.2");
    ack("whisper.3"); assert(s.state == State::Listening);
  }
  void text() {
    s.receive(R"({"request_id":"core2-5","work_id":"whisper.3","object":"asr.utf-8","data":"買い物メモに牛乳","error":{"code":0}})", ++time);
  }
  void confirm() { listen(); text(); ack("whisper.3"); ack("vad.2"); assert(s.confirming()); }
};
int main() {
  unsigned cases = 0;
  { Rig r; r.s.begin(r.time); assert(r.s.microphoneMayBeOn()); r.ack("sys"); r.resetDone(); assert(r.s.state==State::Idle); assert(!r.s.microphoneMayBeOn()); ++cases; }
  {
    Framer f; std::string out; std::vector<std::string> got;
    const std::string input = "noise" R"({"data":"{escaped \" quote}"}{"data":"日本語"})";
    for (char c : input) if (f.feed(c, out)) got.push_back(out);
    assert(got.size() == 2);
    JsonDocument d; assert(!deserializeJson(d, got[0])); assert(!deserializeJson(d, got[1])); ++cases;
  }
  { Framer f; std::string out; f.feed('{',out); for(int i=0;i<8192;++i) f.feed('a',out); assert(f.overflow); ++cases; }
  {
    Rig r; assert(r.sent.empty()); r.listen(); r.text();
    assert(r.posted.empty()); assert(r.s.microphoneMayBeOn());
    assert(r.last()["action"] == "exit"); r.ack("whisper.3"); r.ack("vad.2");
    assert(!r.s.microphoneMayBeOn()); assert(r.posted.empty());
    r.s.pressA(++r.time); r.s.pressA(++r.time); assert(r.posted.size()==1);
    r.s.bridgeResult(true,"牛乳を記録しました",++r.time);
    assert(r.last()["data"]["model"] == "melotts-ja-jp"); r.ack("melotts.4");
    assert(r.s.state==State::Success); assert(r.last()["action"]=="inference"); ++cases;
  }
  { Rig r; r.confirm(); r.s.cancel(++r.time); r.ack("sys"); r.resetDone(); assert(r.posted.empty()); assert(!r.s.microphoneMayBeOn()); ++cases; }
  { Rig r; r.listen(); r.s.tick(r.time+15000); assert(r.s.state==State::Failure); assert(r.last()["action"]=="reset"); assert(r.posted.empty()); ++cases; }
  { Rig r; r.listen(); r.s.cancel(++r.time); r.s.tick(r.time+5000); assert(r.s.microphoneMayBeOn()); assert(r.s.message.find("停止未確認")!=std::string::npos); ++cases; }
  { Rig r; r.s.pressA(r.time); r.ack("sys",-1); assert(r.s.state==State::Failure); assert(r.posted.empty()); ++cases; }
  { Rig r; r.listen(); r.s.receive(R"({"request_id":"stale","work_id":"whisper.old","object":"asr.utf-8","data":"bad","error":{"code":0}})",++r.time); assert(r.s.transcript.empty()); ++cases; }
  { Rig r; r.confirm(); r.s.pressA(++r.time); r.s.tick(r.time+15000); assert(r.posted.size()==1); assert(r.s.state==State::Failure); r.s.bridgeResult(true,"late",r.time+16000); assert(r.s.reply.empty()); ++cases; }
  { Rig r; r.s.pressA(UINT32_MAX-100); r.s.tick(5000); assert(r.s.state==State::Failure); ++cases; }
  { Rig r; r.listen(); r.s.receive(R"({"request_id":"old-setup","work_id":"whisper.3","object":"asr.utf-8","data":"late","error":{"code":0}})",++r.time); assert(r.s.transcript.empty()); ++cases; }
  { Rig r; r.confirm(); r.s.tick(r.time+30000); assert(r.posted.empty()); assert(r.s.state==State::Failure); ++cases; }
  { Rig r; r.listen(); r.s.receive(R"({"request_id":"core2-5","work_id":"whisper.3","object":"asr.utf-8","data":""})",++r.time); assert(r.s.state==State::Failure); ++cases; }
  {
    std::string reply;
    assert(parseBridge(R"({"request_id":"id","ok":true,"intent":"todo.read","reply":"予定なし"})","id",reply));
    assert(!parseBridge(R"({"request_id":"other","ok":true,"intent":"todo.read","reply":"ok"})","id",reply));
    assert(!parseBridge(R"({"request_id":"id","ok":true,"intent":"shell.exec","reply":"ok"})","id",reply));
    assert(!parseBridge(R"({"request_id":"id","ok":"true","intent":"todo.read","reply":"ok"})","id",reply));
    assert(!parseBridge("not json","id",reply));
    assert(!parseBridge(std::string(5000,'a'),"id",reply)); ++cases;
  }
  { assert(privateIp(192,168)); assert(privateIp(10,0)); assert(privateIp(172,31)); assert(!privateIp(172,32)); assert(!privateIp(127,0)); assert(!privateIp(8,8)); ++cases; }
  std::cout << cases << " test groups passed\n";
}
