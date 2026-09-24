#!/usr/bin/env python3
"""Check the Onju volume indicator: .venv/bin/python tests/test_onju_leds.py."""
from pathlib import Path
import subprocess
import tempfile

import yaml  # Already provided by ESPHome.

ROOT = Path(__file__).resolve().parents[1]
config = yaml.load((ROOT / "examples/onju-airplay2.yaml").read_text(), Loader=yaml.BaseLoader)
trigger = config["airplay_receiver"]["on_state"][0]["if"]
assert trigger["then"] == [{"script.execute": "show_volume"}]
show = next(s for s in config["script"] if s["id"] == "show_volume")
assert show["mode"] == "restart"
assert show["then"] == [
    {"light.turn_on": {"id": "top_led", "effect": "volume", "brightness": "60%"}},
    {"delay": "2s"},
    {"light.turn_off": "top_led"},
]
top = next(light for light in config["light"] if light["id"] == "top_led")
assert top["default_transition_length"] == "0s"
assert top["segments"] == [{"id": "leds", "from": "1", "to": "4"}]
assert config["light"][0]["restore_mode"] == "ALWAYS_OFF"

def light_actions(node):
    if isinstance(node, dict):
        return [key for key in node if key.startswith("light.")] + [
            action for value in node.values() for action in light_actions(value)]
    if isinstance(node, list):
        return [action for value in node for action in light_actions(value)]
    return []

assert light_actions(config) == ["light.turn_on", "light.turn_off"]
# Run the actual YAML lambdas, not a Python reimplementation of their logic.
harness = r'''
#include <cassert>
#include <cstdint>
struct { float volume; } airplay;
#define id(name) (name)
struct Color {
  uint8_t r, g, b;
  static const Color WHITE, BLACK;
  Color(uint8_t r=0, uint8_t g=0, uint8_t b=0) : r(r), g(g), b(b) {}
};
const Color Color::WHITE{255,255,255}, Color::BLACK{0,0,0};
struct { Color pixels[4]; int size() { return 4; }
  Color &operator[](int i) { return pixels[i]; } } it;
bool changed() {
''' + trigger["condition"]["lambda"] + r'''
}
void render() {
''' + top["effects"][0]["addressable_lambda"]["lambda"] + r'''
}
int main() {
  airplay.volume = 0.4f;
  assert(!changed());  // First state establishes a baseline, no boot flash.
  const float volumes[] = {0, .125f, .5f, .875f, 1};
  const uint8_t expected[][4] = {
    {0,0,0,0}, {127,0,0,0}, {255,255,0,0}, {255,255,255,127}, {255,255,255,255}
  };
  for (int n = 0; n < 5; ++n) {
    airplay.volume = volumes[n];
    assert(changed());
    assert(!changed());  // Same volume: playback/state updates cannot light LEDs.
    render();
    for (int i = 0; i < 4; ++i) {
      assert(it[i].r == expected[n][i]);
      assert(it[i].r == it[i].g && it[i].g == it[i].b);  // Only white/off.
    }
  }
  airplay.volume = .5f;
  assert(changed());  // Decreases must also trigger the indicator.
}
'''
with tempfile.TemporaryDirectory(prefix=".onju-leds-", dir=ROOT) as directory:
    tmp = Path(directory)
    (tmp / "test.cpp").write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(tmp / "test.cpp"), "-o", str(tmp / "test")], check=True)
    subprocess.run([str(tmp / "test")], check=True, timeout=5)
print("PASS: white volume bar, boot/state suppression, restartable two-second timeout, no other lighting")
