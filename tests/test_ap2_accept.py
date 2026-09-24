#!/usr/bin/env python3
"""Run the production AP2 accept loop on real loopback sockets (no ESP32 required)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "components/airplay_receiver/transport/ap2_events.cpp").read_text()
start = source.index("  while (!is_stopping() && listener >= 0) {")
end = source.index("  if (listener >= 0) {", start)
# ponytail: isolate the actual loop, not a rewritten model or a FreeRTOS emulator.
loop = source[start:end]
harness = r'''
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <initializer_list>
#include <sys/socket.h>
#include <unistd.h>
#define ESP_LOGI(tag, ...) std::fprintf(stderr, __VA_ARGS__)
#define ESP_LOGW(tag, ...) std::fprintf(stderr, __VA_ARGS__)
#define pdMS_TO_TICKS(ms) (ms)
constexpr unsigned AP2_ACCEPT_POLL_MS = 1;
using Clock = std::chrono::steady_clock;
Clock::time_point stop_at;
bool is_stopping() { return Clock::now() >= stop_at; }
void vTaskDelay(unsigned ms) { usleep(ms * 1000); }
unsigned accept_calls;
int counted_accept(int fd, sockaddr *address, socklen_t *size) {
  ++accept_calls;
  return accept(fd, address, size);
}
#define accept counted_accept
int accept_peer(int listener, uint32_t peer_ip) {
  int client = -1;
  accept_calls = 0;
  stop_at = Clock::now() + std::chrono::milliseconds(200);
''' + loop + r'''
  return client;
}
#undef accept

int connect_from(const sockaddr_in &server, uint32_t ip) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = ip;
  assert(bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0);
  assert(connect(fd, reinterpret_cast<const sockaddr *>(&server), sizeof(server)) == 0);
  return fd;
}
int main() {
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  assert(listener >= 0);
  sockaddr_in server{};
  server.sin_family = AF_INET;
  server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, reinterpret_cast<sockaddr *>(&server), sizeof(server)) == 0);
  assert(listen(listener, 4) == 0);
  assert(fcntl(listener, F_SETFL, O_NONBLOCK) == 0);
  socklen_t size = sizeof(server);
  assert(getsockname(listener, reinterpret_cast<sockaddr *>(&server), &size) == 0);
  for (bool foreign_first : {false, true}) {
    int foreign = foreign_first ? connect_from(server, htonl(0x7f000002)) : -1;
    int peer = connect_from(server, htonl(INADDR_LOOPBACK));
    int client = accept_peer(listener, htonl(INADDR_LOOPBACK));
    assert(client >= 0 && "accepted peer was lost: accept loop never reached updateInfo");
    assert(accept_calls == (foreign_first ? 2u : 1u));
    assert(send(client, "U", 1, 0) == 1);
    char byte = 0;
    assert(recv(peer, &byte, 1, 0) == 1 && byte == 'U');
    if (foreign >= 0) close(foreign);
    close(client);
    close(peer);
  }
  assert(accept_peer(listener, htonl(INADDR_LOOPBACK)) == -1);  // idle stop
  close(listener);
}
'''
with tempfile.TemporaryDirectory(prefix=".ap2-accept-", dir=ROOT) as directory:
    tmp = Path(directory)
    (tmp / "test.cpp").write_text(harness)
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    str(tmp / "test.cpp"), "-o", str(tmp / "test")], check=True)
    result = subprocess.run([str(tmp / "test")], capture_output=True, text=True, timeout=5)
    assert result.returncode == 0, result.stderr
print("PASS: AP2 accepted peer reaches handshake; foreign peer rejected; idle stop returns")
