#pragma once
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef VOICE_WIFI_SSID
#define VOICE_WIFI_SSID ""
#define VOICE_WIFI_PASSWORD ""
#define VOICE_BRIDGE_IP ""
#define VOICE_DEVICE_TOKEN ""
#endif
#ifndef VOICE_WHISPER_MODEL
#define VOICE_WHISPER_MODEL "whisper-tiny"
#endif
#ifndef VOICE_TTS_MODEL
#define VOICE_TTS_MODEL "melotts-ja-jp"
#endif
constexpr int kModuleRx = 13;
constexpr int kModuleTx = 14;
constexpr unsigned kBridgePort = 8765;
constexpr const char* kDeviceId = "core2-01";
