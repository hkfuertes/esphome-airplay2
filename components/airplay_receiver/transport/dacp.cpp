// airplay_receiver ESP32 -> sender DACP client. Fresh code for this port; the
// protocol shape follows the observed behaviour of iOS/macOS DACP servers on
// port 3689 (playpause/pause/play/stop/volumeup/volumedown under /ctrl-int/1,
// Active-Remote echoed as a plain request header, no auth).

#include "dacp.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_dacp";

namespace {

constexpr uint16_t DACP_PORT = 3689;
constexpr int DACP_CONNECT_TIMEOUT_MS = 1500;
constexpr int DACP_RESPONSE_TIMEOUT_MS = 1000;
constexpr size_t ACTIVE_REMOTE_MAX = 48;
constexpr UBaseType_t DACP_QUEUE_LEN = 4;
constexpr int DACP_TASK_STACK = 4096;
constexpr UBaseType_t DACP_TASK_PRIORITY = 3;  // below the RTSP tasks (5): best-effort UI control

struct DacpRequest {
  uint32_t ip;  // network byte order
  char active_remote[ACTIVE_REMOTE_MAX];
  DacpCommand cmd;
};

struct Session {
  uint32_t ip = 0;  // network byte order
  char active_remote[ACTIVE_REMOTE_MAX] = {};
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Session s_session;
QueueHandle_t s_queue = nullptr;

const char *command_path(DacpCommand cmd) {
  switch (cmd) {
    case DacpCommand::PLAY_PAUSE: return "playpause";
    case DacpCommand::PLAY: return "play";
    case DacpCommand::PAUSE: return "pause";
    case DacpCommand::STOP: return "stop";
    case DacpCommand::VOLUME_UP: return "volumeup";
    case DacpCommand::VOLUME_DOWN: return "volumedown";
  }
  return "playpause";
}

bool send_all(int fd, const char *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t r = send(fd, data + sent, len - sent, 0);
    if (r <= 0) {
      if (r < 0 && errno == EINTR) {
        continue;
      }
      return false;
    }
    sent += (size_t) r;
  }
  return true;
}

/// One GET against the sender's DACP server. Returns the HTTP status (0 on
/// transport failure). The socket is always closed before returning.
int dacp_http_get(uint32_t ip, const char *active_remote, const char *path) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }

  int status = 0;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DACP_PORT);
  addr.sin_addr.s_addr = ip;

  // Non-blocking connect with a bounded timeout: the sender may be a phone
  // that just left the network; the DACP task must never wedge on it.
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int rc = connect(fd, (struct sockaddr *) &addr, sizeof(addr));
  bool connected = (rc == 0);
  if (!connected && errno == EINPROGRESS) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {DACP_CONNECT_TIMEOUT_MS / 1000, (DACP_CONNECT_TIMEOUT_MS % 1000) * 1000};
    connected = select(fd + 1, nullptr, &wfds, nullptr, &tv) > 0;
  }

  if (connected) {
    char req[256];
    int len = snprintf(req, sizeof(req),
                       "GET /ctrl-int/1/%s HTTP/1.1\r\n"
                       "Host: %u.%u.%u.%u:%u\r\n"
                       "Active-Remote: %s\r\n"
                       "Connection: close\r\n"
                       "User-Agent: esp32-airplay\r\n"
                       "\r\n",
                       path, (unsigned) (ip & 0xFF), (unsigned) ((ip >> 8) & 0xFF), (unsigned) ((ip >> 16) & 0xFF),
                       (unsigned) ((ip >> 24) & 0xFF), (unsigned) DACP_PORT, active_remote);
    if (len > 0 && send_all(fd, req, (size_t) len)) {
      // Read the status line (best effort) so the close is a clean one and we
      // can log the sender's answer.
      struct timeval tv = {DACP_RESPONSE_TIMEOUT_MS / 1000, (DACP_RESPONSE_TIMEOUT_MS % 1000) * 1000};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      char response[128] = {};
      ssize_t n = recv(fd, response, sizeof(response) - 1, 0);
      if (n > 0) {
        response[n] = '\0';
        if (sscanf(response, "HTTP/%*d.%*d %d", &status) != 1) {
          status = 0;
        }
      }
    }
  }

  close(fd);
  return status;
}

void dacp_task(void *pv) {
  DacpRequest item;
  for (;;) {
    if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) {
      continue;
    }
    const char *path = command_path(item.cmd);
    int status = dacp_http_get(item.ip, item.active_remote, path);
    if (status >= 200 && status < 300) {
      ESP_LOGD(TAG, "%s -> %u.%u.%u.%u (HTTP %d)", path, (unsigned) (item.ip & 0xFF),
               (unsigned) ((item.ip >> 8) & 0xFF), (unsigned) ((item.ip >> 16) & 0xFF),
               (unsigned) ((item.ip >> 24) & 0xFF), status);
    } else {
      // W, not E: the sender may have vanished mid-session; the RTSP layer
      // notices that on its own and clears the session.
      ESP_LOGW(TAG, "%s -> %u.%u.%u.%u failed (status %d)", path, (unsigned) (item.ip & 0xFF),
               (unsigned) ((item.ip >> 8) & 0xFF), (unsigned) ((item.ip >> 16) & 0xFF),
               (unsigned) ((item.ip >> 24) & 0xFF), status);
    }
  }
}

bool ensure_task_started() {
  if (s_queue != nullptr) {
    return true;
  }
  s_queue = xQueueCreate(DACP_QUEUE_LEN, sizeof(DacpRequest));
  if (s_queue == nullptr) {
    return false;
  }
  if (xTaskCreate(dacp_task, "airplay_dacp", DACP_TASK_STACK, nullptr, DACP_TASK_PRIORITY, nullptr) != pdPASS) {
    vQueueDelete(s_queue);
    s_queue = nullptr;
    return false;
  }
  return true;
}

}  // namespace

bool dacp_session_set(uint32_t client_ip_netorder, const char *active_remote) {
  if (active_remote == nullptr || active_remote[0] == '\0') {
    return false;
  }
  portENTER_CRITICAL(&s_mux);
  s_session.ip = client_ip_netorder;
  strncpy(s_session.active_remote, active_remote, sizeof(s_session.active_remote) - 1);
  s_session.active_remote[sizeof(s_session.active_remote) - 1] = '\0';
  portEXIT_CRITICAL(&s_mux);
  return ensure_task_started();
}

void dacp_session_clear() {
  portENTER_CRITICAL(&s_mux);
  s_session.ip = 0;
  s_session.active_remote[0] = '\0';
  portEXIT_CRITICAL(&s_mux);
}

bool dacp_available() {
  portENTER_CRITICAL(&s_mux);
  bool available = s_session.active_remote[0] != '\0';
  portEXIT_CRITICAL(&s_mux);
  return available;
}

bool dacp_send(DacpCommand cmd) {
  DacpRequest item = {};
  portENTER_CRITICAL(&s_mux);
  item.ip = s_session.ip;
  strncpy(item.active_remote, s_session.active_remote, sizeof(item.active_remote) - 1);
  item.active_remote[sizeof(item.active_remote) - 1] = '\0';
  portEXIT_CRITICAL(&s_mux);
  if (item.active_remote[0] == '\0' || s_queue == nullptr) {
    return false;
  }
  item.cmd = cmd;
  return xQueueSend(s_queue, &item, 0) == pdTRUE;
}

}  // namespace airplay_receiver
}  // namespace esphome
