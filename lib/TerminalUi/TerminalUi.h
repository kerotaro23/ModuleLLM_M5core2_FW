#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace terminal_ui {
using Color = uint32_t;
constexpr Color bg=0x101D29, panel=0x1C2D3C, muted=0x9FB6C5, white=0xF1F7F5;
constexpr Color green=0x83E5B0, ink=0x15352E, yellow=0xF5D38A, red=0xFF9F9F;
enum class Mood { Idle, Listening, Processing, Success, Failure };
struct View {
  Mood mood = Mood::Idle;
  bool connected = false, mic = false, configured = false, confirm = false;
  bool canStart = false, canCancel = false;
  bool reconnect = false, connecting = false, details = false, canInspect = false;
  int remainingSeconds = -1;
  std::string body, footer;
};
inline Color accent(Mood mood) {
  return mood == Mood::Failure ? red : mood == Mood::Processing ? yellow : green;
}
// Surface supplies integer-positioned primitives and Japanese text metrics.
// Both the device and the SVG design preview use this same layout/drawing code.
template<class Surface>
void hero(Surface& s, const View& v, uint32_t frame, int y=28) {
  const Color a = accent(v.mood);
  s.rect(0,y,320,78,bg);
  const bool blink = frame % 40 >= 38;
  const int bob = (frame / 5) % 2;
  const int cy = y+43+bob;
  s.ellipse(51,y+71,31,4,0x223A45);
  s.ellipse(51,cy+13,24,17,0x50A985);
  s.ellipse(51,cy,36,25,green);
  s.circle(31,cy-21,15,green); s.circle(71,cy-21,15,green);
  s.circle(31,cy-22,10,white); s.circle(71,cy-22,10,white);
  if (blink) {
    s.line(25,cy-21,37,cy-21,ink); s.line(65,cy-21,77,cy-21,ink);
  } else if (v.mood == Mood::Success) {
    for (int x : {31,71}) { s.line(x-5,cy-20,x,cy-25,ink); s.line(x,cy-25,x+5,cy-20,ink); }
  } else {
    const int dx = v.mood == Mood::Processing ? 2 : 0;
    s.circle(31+dx,cy-21,4,ink); s.circle(71+dx,cy-21,4,ink);
    s.circle(32+dx,cy-23,1,white); s.circle(72+dx,cy-23,1,white);
  }
  s.ellipse(27,cy+3,6,3,0xF2B7AC); s.ellipse(75,cy+3,6,3,0xF2B7AC);
  if (v.mood == Mood::Listening) s.ellipse(51,cy+5,5,7,ink);
  else if (v.mood == Mood::Failure) {
    s.line(44,cy+9,51,cy+5,ink); s.line(51,cy+5,58,cy+9,ink);
    s.ellipse(88,cy-5,3,5,0x77C9ED);
  } else {
    s.line(41,cy+4,45,cy+8,ink); s.line(45,cy+8,57,cy+8,ink); s.line(57,cy+8,61,cy+4,ink);
  }
  const char* titles[] = {"待機", "聞き取り中", "処理中", "成功", "失敗"};
  const char* captions[] = {"Aを押して話しかけてね", "ゆっくり話してね", "ちょっと待ってね", "返答が届いたよ", "下の案内を確認してね"};
  const int index = static_cast<int>(v.mood);
  const bool network = v.connecting && v.mood == Mood::Idle;
  s.text(v.details ? "ステータス" : network ? "Wi-Fi接続中" : v.confirm ? "送信していい？" : titles[index],108,y+9,16,a);
  s.text(v.details ? "接続情報を確認できるよ" : network ? "最大15秒待ってね" :
         v.reconnect && v.mood == Mood::Idle ? "AでWi-Fiを再接続" :
         v.confirm ? "認識文を確認してね" : captions[index],108,y+33,12,muted);
  if (v.remainingSeconds >= 0 && !v.details) {
    s.text("あと " + std::to_string(v.remainingSeconds) + " 秒",109,y+56,12,a);
    const int total = v.confirm ? 30 : 15;
    s.round(109,y+73,180,3,1,panel);
    const int width = 180 * std::min(total, v.remainingSeconds) / total;
    if (width) s.round(109,y+73,width,3,1,a);
    if (v.mood == Mood::Listening) {
      for (int i=0;i<8;++i) {
        const int h=4+((frame*3+i*7)%11);
        s.round(212+i*10,y+62-h/2,4,h,2,green);
      }
    }
  } else if (v.mood == Mood::Listening) {
    // Decorative listening activity, not a measured microphone waveform.
    for (int i=0;i<15;++i) {
      int h=4+((frame*3+i*7)%15);
      s.round(109+i*11,y+66-h/2,5,h,2,green);
    }
  } else if (v.mood == Mood::Processing && !v.confirm) {
    for (int i=0;i<3;++i) s.circle(116+i*16,y+64,3,(frame/3)%3==static_cast<unsigned>(i)?yellow:0x394D59);
  } else s.text(v.mood==Mood::Failure ? "あわてず確認しよう" : "KERO VOICE TERMINAL",109,y+59,12,muted);
}

template<class Surface>
std::vector<std::string> wrap(Surface& s, const std::string& text) {
  std::vector<std::string> lines;
  std::string line;
  for (size_t i=0;i<text.size();) {
    unsigned char c=text[i];
    size_t n=std::min<size_t>(c<0x80?1:c<0xE0?2:c<0xF0?3:4,text.size()-i);
    std::string glyph=text.substr(i,n); i+=n;
    if(c=='\n') { lines.push_back(line); line.clear(); continue; }
    if(s.width(line+glyph,12)>276) { lines.push_back(line); line.clear(); }
    line+=glyph;
  }
  if(!line.empty()) lines.push_back(line);
  return lines;
}

template<class Surface>
void button(Surface& s,int x,const std::string& text,bool enabled,Color fill) {
  s.round(x,211,96,25,8,enabled?fill:panel);
  s.text(text,x+(96-s.width(text,12))/2,217,12,enabled?ink:muted);
}

template<class Surface>
unsigned render(Surface& s,const View& v,unsigned& page,uint32_t frame) {
  s.rect(0,0,320,240,bg);
  s.text("けろくろー",10,7,12,white);
  s.circle(159,12,3,v.connected?green:muted);
  s.text(v.connected?"Wi-Fi OK":"OFFLINE",168,6,12,muted);
  s.round(242,3,68,19,7,v.mic?yellow:panel);
  s.text(v.mic?"MIC ON?":"MIC OFF",250,6,12,v.mic?ink:green);
  hero(s,v,frame);
  s.round(10,109,300,84,10,panel);
  auto lines=wrap(s,v.body);
  const unsigned pages=std::max(1U,(static_cast<unsigned>(lines.size())+3)/4);
  page%=pages;
  s.text(v.details?"DEVICE INFO":v.confirm?"認識したことば":!v.configured?"セットアップ":"MESSAGE",20,114,12,accent(v.mood));
  std::string counter=std::to_string(page+1)+" / "+std::to_string(pages);
  s.text(counter,300-s.width(counter,12),114,12,muted);
  for(unsigned i=0;i<4&&page*4+i<lines.size();++i) s.text(lines[page*4+i],20,131+i*14,12,white);
  s.text(v.footer,12,196,12,muted);
  button(s,10,v.details?"A 戻る":v.reconnect?"A 再接続":v.confirm?"A 送信する":"A 話す",v.details||v.canStart,green);
  button(s,112,v.details?"B 戻る/停止":"B 中止",v.details||v.canCancel,yellow);
  button(s,214,pages>1?"C 次の頁":"C 長押:情報",pages>1||v.canInspect,0xB9D9EB);
  return pages;
}
} // namespace terminal_ui
