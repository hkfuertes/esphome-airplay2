#pragma once
// airplay_receiver ESP32 -> sender control channel (DACP).
//
// Legacy fallback for a sender that puts Active-Remote on RTSP requests and
// runs DACP (Digital Audio Control Protocol) on port 3689. Modern encrypted
// iPhones use transport/ap2_events instead; do not assume these headers exist.
// When present, requests are `GET /ctrl-int/1/<command>` with Active-Remote
// echoed as a request header. Volume steps are blind; absolute volume uses
// Shairport's setproperty?dmcp.device-volume=<dB> path and converges through
// the sender's SET_PARAMETER volume event.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace airplay_receiver {

enum class DacpCommand : uint8_t {
  PLAY_PAUSE,
  VOLUME_UP,
  VOLUME_DOWN,
  SET_DEVICE_VOLUME,
};

/// Remember the live session's DACP endpoint (peer IP + Active-Remote token).
/// Spawns the sender task on first use. Safe from any task.
bool dacp_session_set(uint32_t client_ip_netorder, const char *active_remote);

/// Forget the endpoint (the sender went away). Safe from any task.
void dacp_session_clear();

/// True while a session with an Active-Remote identity is live.
bool dacp_available();

/// Queue a static command for the sender. False = no session or queue full.
/// Returns as soon as the command is queued; the HTTP round-trip happens on
/// the DACP task (never on the caller, never on the RTSP task).
bool dacp_send(DacpCommand cmd);

/// Queue an absolute AirPlay volume in dB (-30.0 = mute, 0.0 = full scale).
bool dacp_set_volume(float volume_db);

}  // namespace airplay_receiver
}  // namespace esphome
