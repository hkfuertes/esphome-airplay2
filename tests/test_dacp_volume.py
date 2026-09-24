# /// script
# requires-python = ">=3.11"
# ///
"""Check reverse-DACP absolute-volume compatibility with AirPlay's UI scale."""

from pathlib import Path

ROOT = Path(__file__).parents[1]


def volume_to_dacp_db(level: float) -> float:
    return -30.0 + 30.0 * level


def rtsp_gain(db: float) -> float:
    normalized = (db + 30.0) / 30.0
    return normalized * normalized


for level in (0.0, 0.01, 0.25, 0.5, 1.0):
    db = volume_to_dacp_db(level)
    assert -30.0 <= db <= 0.0
    assert abs(rtsp_gain(db) - level * level) < 1e-6

# The firmware is ESP-IDF-linked, so keep this host check at the public wire
# contract: the Shairport-proven endpoint and the shared UI/gain conversion.
dacp = (ROOT / "components/airplay_receiver/transport/dacp.cpp").read_text()
receiver = (ROOT / "components/airplay_receiver/airplay_receiver.cpp").read_text()
assert "setproperty?dmcp.device-volume=%.6f" in dacp
assert "media_player_volume_to_audio_percent" in receiver
assert "transport_volume_db()" in receiver

print("ok: reverse DACP volume uses AirPlay's -30..0 dB slider")
