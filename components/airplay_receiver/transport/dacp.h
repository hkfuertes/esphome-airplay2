#pragma once
// airplay_receiver ESP32 -> sender control channel (DACP).
//
// A sender that puts an Active-Remote header on its RTSP requests is running a
// DACP (Digital Audio Control Protocol) server on port 3689. The receiver can
// drive it back: `GET /ctrl-int/1/<command>` with the Active-Remote value
// echoed as a request header. This is what makes a button on the box move the
// phone's own lock-screen state and volume slider, instead of only touching
// the local output.
//
// ponytail: sender address = the RTSP peer IP + fixed port 3689 (what iOS and
// macOS listen on); an mDNS browse of _dacp._tcp only matters for exotic
// relays. Volume is blind-stepped (volumeup/volumedown) -- the sender echoes
// the resulting level back over SET_PARAMETER volume, so the local entity
// converges without a round-trip query.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

enum class DacpCommand : uint8_t {
  PLAY_PAUSE,
  PLAY,
  PAUSE,
  STOP,
  VOLUME_UP,
  VOLUME_DOWN,
};

/// Remember the live session's DACP endpoint (peer IP + Active-Remote token).
/// Spawns the sender task on first use. Safe from any task.
bool dacp_session_set(uint32_t client_ip_netorder, const char *active_remote);

/// Forget the endpoint (the sender went away). Safe from any task.
void dacp_session_clear();

/// True while a session with an Active-Remote identity is live.
bool dacp_available();

/// Queue a command for the sender. False = no session or queue full.
/// Returns as soon as the command is queued; the HTTP round-trip happens on
/// the DACP task (never on the caller, never on the RTSP task).
bool dacp_send(DacpCommand cmd);

}  // namespace airplay_receiver
}  // namespace esphome
