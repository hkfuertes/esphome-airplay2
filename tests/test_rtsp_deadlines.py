#!/usr/bin/env python3
"""Check the production socket helpers' deadlines, without ESP32/crypto stubs."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "components/airplay_receiver/transport/rtsp_crypto.cpp").read_text()
start = source.index("static int send_all_until(")
end = source.index("static int rtsp_crypto_read_block_impl(", start)
helpers = source[start:end]
harness = r'''
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>
int64_t esp_timer_get_time() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
''' + helpers + r'''
int main() {
  int sockets[2];
  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  uint8_t byte = 42, received = 0;
  assert(send_all_until(sockets[0], &byte, 1, esp_timer_get_time() + 200000) == 0);
  assert(recv_exact(sockets[1], &received, 1, 0) && received == byte);

  // No reply, then a partial two-byte frame prefix: both must actually expire.
  for (bool partial : {false, true}) {
    if (partial) assert(send(sockets[0], &byte, 1, 0) == 1);
    uint8_t prefix[2];
    auto started = esp_timer_get_time();
    assert(!recv_exact(sockets[1], prefix, sizeof(prefix), started + 20000));
    assert(errno == EAGAIN && esp_timer_get_time() - started < 500000);
  }

  // A sender must time out if the peer is connected but not reading.
  int capacity = 4096;
  assert(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &capacity, sizeof(capacity)) == 0);
  std::vector<uint8_t> payload(1024 * 1024);
  auto started = esp_timer_get_time();
  assert(send_all_until(sockets[0], payload.data(), payload.size(), started + 20000) == -1);
  assert(errno == EAGAIN && esp_timer_get_time() - started < 500000);
  close(sockets[0]);
  close(sockets[1]);

  assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  close(sockets[0]);
  errno = EAGAIN;
  assert(!recv_exact(sockets[1], &received, 1, esp_timer_get_time() + 200000));
  assert(errno == 0);  // EOF must not be mistaken for a retryable timeout.
  close(sockets[1]);
}
'''
with tempfile.TemporaryDirectory(prefix=".rtsp-deadlines-", dir=ROOT) as directory:
    tmp = Path(directory)
    (tmp / "test.cpp").write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(tmp / "test.cpp"), "-o", str(tmp / "test")], check=True)
    subprocess.run([str(tmp / "test")], check=True, timeout=5)
print("PASS: read/write deadlines, partial prefix, stalled peer, and EOF handling")
