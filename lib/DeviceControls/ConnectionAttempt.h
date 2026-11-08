#pragma once
#include <cstdint>

namespace device {
// A connection attempt has its own deadline; uptime is not a retry timer.
// The caller performs WiFi.begin/disconnect only on explicit start/timeout.
class ConnectionAttempt {
 public:
  enum class State { Offline, Connecting, Online, Failed };
  enum class Event { None, Connected, Disconnected, TimedOut };
  State state = State::Offline;
  bool connecting() const { return state == State::Connecting; }
  bool start(uint32_t now) {
    if (connecting() || state == State::Online) return false;
    started = now;
    state = State::Connecting;
    return true;
  }
  Event update(uint32_t now, bool connected) {
    if (connected) {
      if (state == State::Online) return Event::None;
      state = State::Online;
      return Event::Connected;
    }
    if (state == State::Online) {
      state = State::Offline;
      return Event::Disconnected;
    }
    if (connecting() && uint32_t(now - started) >= 15000) {
      state = State::Failed;
      return Event::TimedOut;
    }
    return Event::None;
  }
 private:
  uint32_t started = 0;
};
} // namespace device
