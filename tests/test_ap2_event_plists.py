#!/usr/bin/env python3
"""Check AP2 reverse-event encoders with Python's independent plist decoder."""
import ctypes
from pathlib import Path
import plistlib
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GROUP = b"01234567-89AB-CDEF-0123-456789ABCDEF"
COMMAND = b"FEDCBA98-7654-4321-89AB-CDEF01234567"

with tempfile.TemporaryDirectory(prefix=".event-plists-", dir=ROOT) as directory:
    tmp = Path(directory)
    wrapper = tmp / "wrapper.cpp"
    wrapper.write_text('''#include "components/airplay_receiver/transport/bplist.h"
using namespace esphome::airplay_receiver;
extern "C" size_t command(uint8_t *b, size_t n, const char *g, const char *c) {
  return bplist_build_event_command(b, n, g, c);
}
extern "C" size_t volume(uint8_t *b, size_t n, double v) { return bplist_build_event_volume(b, n, v); }
extern "C" size_t info(uint8_t *b, size_t n, const char *g) {
  const uint8_t key[32]{};
  return bplist_build_info_response(b, n, "00:11:22:33:44:55", "test", key, 32, 0x1c340405c4a00ULL, 2, g);
}
''')
    library = tmp / "test.so"
    subprocess.run([
        "c++", "-std=c++17", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
        "-I", str(ROOT), str(wrapper),
        str(ROOT / "components/airplay_receiver/transport/bplist.cpp"), "-o", str(library),
    ], check=True)
    lib = ctypes.CDLL(str(library))
    lib.command.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_char_p]
    lib.volume.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_double]
    lib.info.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p]
    for fn in [lib.command, lib.volume, lib.info]:
        fn.restype = ctypes.c_size_t

    def encode(fn, *args, capacity=2048):
        buffer = ctypes.create_string_buffer(capacity + 1)
        buffer[capacity] = b"X"
        size = fn(buffer, capacity, *args)
        assert size <= capacity and buffer[capacity] == b"X", "encoder overflow"
        return bytes(buffer[:size])

    decoded = plistlib.loads(encode(lib.command, GROUP, COMMAND))
    assert decoded["type"] == "sendMediaRemoteCommand"
    assert decoded["modernMediaRemoteCommand"] == "2"
    params = decoded["params"]
    assert params["kMRMediaRemoteOptionCommandID"] == COMMAND.decode()
    assert params["kMRMediaRemoteOptionIsRedirectingCommand"] is True
    assert params["kMRMediaRemoteOptionSendOptionsNumber"] == 0
    archive = plistlib.loads(params["kMRMediaRemoteOptionDestinationDeviceUIDs"])
    assert archive == {
        "$version": 100000, "$archiver": "NSKeyedArchiver", "$top": {"root": {"CF$UID": 1}},
        "$objects": ["$null", {"NS.objects": [{"CF$UID": 2}], "$class": {"CF$UID": 3}},
                     GROUP.decode(), {"$classname": "NSMutableArray",
                                      "$classes": ["NSMutableArray", "NSArray", "NSObject"]}],
    }
    for level in [0.0, 0.5, 1.0]:
        assert plistlib.loads(encode(lib.volume, level)) == {
            "type": "sendMediaRemoteCommand", "value": "dvlc", "volume": level,
            "params": {"volume": level},
        }
    for level in [-1.0, 1.01, float("nan"), float("inf")]:
        assert encode(lib.volume, level) == b""
    normal = plistlib.loads(encode(lib.info, None))
    update = plistlib.loads(encode(lib.info, GROUP))
    assert update == {"type": "updateInfo", "value": dict(normal, gid=GROUP.decode())}
    for size in range(2048):
        for fn, args in [(lib.command, (GROUP, COMMAND)), (lib.volume, (0.5,)), (lib.info, (GROUP,))]:
            data = encode(fn, *args, capacity=size)
            if data:
                plistlib.loads(data)
    assert not encode(lib.command, b"", COMMAND)
    assert not encode(lib.command, GROUP, b"invalid")

print("PASS: AP2 play/pause archive, dvlc volume, updateInfo, and bounded encoders")
