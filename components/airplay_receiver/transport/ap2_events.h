#pragma once
// Encrypted AirPlay 2 reverse-event channel.
//
// Modern iOS senders connect back to the eventPort advertised in SETUP. The
// receiver sends authenticated POST /command requests over that socket; this is
// distinct from legacy DACP and uses independent Events-{Write,Read} keys.

#include "rtsp_conn.h"

namespace esphome {
namespace airplay_receiver {

/// Takes ownership of listener on success and creates an independent event cipher.
bool ap2_events_start(int listener, const RtspConn &conn, const char *group_id, const char *device_name);

/// Stop the listener/client task and discard queued commands.
void ap2_events_stop();

/// True once updateInfo has completed on an authenticated event connection.
bool ap2_events_available();

/// Queue modernMediaRemoteCommand=2 (play/pause).
bool ap2_events_play_pause();

/// Queue a dvlc unit-volume notification (0.0 = mute, 1.0 = full scale).
bool ap2_events_volume(float volume);

}  // namespace airplay_receiver
}  // namespace esphome
