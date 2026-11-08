// Build: c++ -std=c++17 -Ilib/TerminalUi tools/ui_preview.cpp -o .build/ui_preview
// Run: .build/ui_preview > docs/ui-preview.svg
// Uses firmware drawing code; host text metrics approximate the device bitmap fonts.
#include "TerminalUi.h"
#include <iostream>
#include <iomanip>
#include <sstream>
std::string xml(const std::string& text) {
  std::string out;
  for(char c:text) {
    if(c=='&') out+="&amp;"; else if(c=='<') out+="&lt;";
    else if(c=='>') out+="&gt;"; else out+=c;
  }
  return out;
}
struct Svg {
  std::string color(uint32_t c) { std::ostringstream s; s<<'#'<<std::hex<<std::setw(6)<<std::setfill('0')<<c; return s.str(); }
  void round(int x,int y,int w,int h,int r,uint32_t c) {
    std::cout<<"<rect x='"<<x<<"' y='"<<y<<"' width='"<<w<<"' height='"<<h<<"' rx='"<<r<<"' fill='"<<color(c)<<"'/>\n";
  }
  void rect(int x,int y,int w,int h,uint32_t c) { round(x,y,w,h,0,c); }
  void circle(int x,int y,int r,uint32_t c) { ellipse(x,y,r,r,c); }
  void ellipse(int x,int y,int rx,int ry,uint32_t c) {
    std::cout<<"<ellipse cx='"<<x<<"' cy='"<<y<<"' rx='"<<rx<<"' ry='"<<ry<<"' fill='"<<color(c)<<"'/>\n";
  }
  void line(int x,int y,int x2,int y2,uint32_t c) {
    std::cout<<"<path d='M"<<x<<' '<<y<<" L"<<x2<<' '<<y2<<"' stroke='"<<color(c)<<"' stroke-width='1.5' fill='none'/>\n";
  }
  int width(const std::string& text,int size) {
    int w=0;
    for(unsigned char c:text) if(c<0x80) w+=size/2; else if(c>=0xC0) w+=size;
    return w;
  }
  void text(const std::string& text,int x,int y,int size,uint32_t c) {
    std::cout<<"<text x='"<<x<<"' y='"<<y+size-2<<"' font-family='Noto Sans CJK JP, sans-serif' font-size='"<<size
      <<"' fill='"<<color(c)<<"'>"<<xml(text)<<"</text>\n";
  }
};
int main() {
  std::cout<<"<svg xmlns='http://www.w3.org/2000/svg' width='1000' height='780' viewBox='0 0 1000 780'>\n";
  Svg s; s.rect(0,0,1000,780,0x09131C);
  for(int i=0;i<9;++i) {
    terminal_ui::View v;
    v.connected=v.configured=true; v.footer="192.168.1.42";
    v.canStart=(i==0||i==3||i==4||i==5); v.canCancel=(i==1||i==3||i==4);
    v.mood=static_cast<terminal_ui::Mood>(i>=6?0:i<3?i:i==3?2:i-1);
    const char* texts[]={"こんにちは！予定や天気、メモのお手伝いをするよ。Aを押してね。",
      "話してください (最大15秒) / B: 中止", "Piへ送信中", "買い物メモに牛乳を追加して",
      "牛乳を買い物メモに追加しました。次のお買い物で忘れずにね！", "Pi応答なし: 実行結果不明。自動再送しません"};
    v.body=texts[std::min(i,5)]; v.confirm=i==3; v.mic=i==1;
    v.canInspect=(i==0||i==4||i==5||i>=6);
    if(i==1) v.remainingSeconds=12;
    if(i==3) v.remainingSeconds=24;
    if(i==6) {
      v.details=true; v.footer="C:次の頁 / 長押しで戻る";
      v.body="端末: core2-01\nCore2 IP: 192.168.1.42\n電波: -52 dBm\nPi設定: 192.168.1.20:8765\nPiの稼働状態は未確認\nModule FW: v1.6\nASR: whisper-tiny\nTTS: melotts-ja-jp";
    }
    if(i==7||i==8) {
      v.connected=false; v.reconnect=true; v.canStart=i==7;
      v.connecting=i==8;
      v.footer=i==7?"Wi-Fi未接続 / A:再接続":"Wi-Fi接続中 (最大15秒)";
      v.body=i==7?"Wi-Fiが切れています。Aで再接続できます。C長押しで接続情報を確認してね。":"Wi-Fiに接続中です (最大15秒)。接続後にAで会話を始めてね。";
    }
    if(v.mic) v.footer="マイク動作中 / 停止確認中";
    std::cout<<"<g transform='translate("<<10+(i%3)*330<<' '<<10+(i/3)*260<<")'>\n";
    unsigned page=0; terminal_ui::render(s,v,page,8);
    std::cout<<"</g>\n";
  }
  std::cout<<"</svg>\n";
}
