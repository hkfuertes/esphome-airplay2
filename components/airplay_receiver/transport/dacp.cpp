// airplay_receiver ESP32 -> sender DACP client.
//
// The iPhone DACP surface proven by the native receiver and Shairport Sync is
// deliberately small: playpause, volumeup/down, and dmcp.device-volume on
// TCP port 3689.

#include "dacp.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

namespace {

constexpr char TAG[] = "airplay_dacp";
constexpr uint16_t DACP_PORT = 3689;
constexpr int DACP_CONNECT_TIMEOUT_MS = 1500;
constexpr int DACP_RESPONSE_TIMEOUT_MS = 1000;
constexpr size_t ACTIVE_REMOTE_MAX = 48;
constexpr UBaseType_t DACP_QUEUE_LEN = 8;
constexpr int DACP_TASK_STACK = 4096;
constexpr UBaseType_t DACP_TASK_PRIORITY = 3;  // below the RTSP tasks: best-effort UI control
constexpr float DACP_MIN_VOLUME_DB = -30.0f;
constexpr float DACP_MAX_VOLUME_DB = 0.0f;
constexpr size_t DACP_PATH_MAX = 80;

struct DacpRequest {
  uint32_t ip;
  uint32_t generation;
  char active_remote[ACTIVE_REMOTE_MAX];
  DacpCommand command;
  float volume_db;
};

struct DacpSession {
  uint32_t ip = 0;
  uint32_t generation = 0;
  char active_remote[ACTIVE_REMOTE_MAX] = {};
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
DacpSession s_session;
QueueHandle_t s_queue = nullptr;
bool s_task_starting = false;

const char *command_path(DacpCommand command) {
  switch (command) {
    case DacpCommand::PLAY_PAUSE:
      return "playpause";
    case DacpCommand::VOLUME_UP:
      return "volumeup";
    case DacpCommand::VOLUME_DOWN:
      return "volumedown";
    case DacpCommand::SET_DEVICE_VOLUME:
      return nullptr;
  }
  return nullptr;
}

bool request_path(const DacpRequest &request, char *out, size_t out_size) {
  if (out == nullptr || out_size == 0) {
    return false;
  }
  if (request.command == DacpCommand::SET_DEVICE_VOLUME) {
    const int written = std::snprintf(out, out_size, "setproperty?dmcp.device-volume=%.6f", request.volume_db);
    return written > 0 && static_cast<size_t>(written) < out_size;
  }
  const char *command = command_path(request.command);
  const int written = command == nullptr ? 0 : std::snprintf(out, out_size, "%s", command);
  return written > 0 && static_cast<size_t>(written) < out_size;
}

bool format_ip(uint32_t ip, char *out, size_t out_size) {
  in_addr address{};
  address.s_addr = ip;
  return inet_ntop(AF_INET, &address, out, out_size) != nullptr;
}

bool make_request(uint32_t ip, const char *active_remote, const char *path, char *out, size_t out_size) {
  if (active_remote == nullptr || active_remote[0] == '\0' || path == nullptr || path[0] == '\0' || out == nullptr ||
      out_size == 0) {
    return false;
  }
  char host[INET_ADDRSTRLEN]{};
  if (!format_ip(ip, host, sizeof(host))) {
    return false;
  }
  const int written = std::snprintf(out, out_size,
                                    "GET /ctrl-int/1/%s HTTP/1.1\r\n"
                                    "Host: %s:%u\r\n"
                                    "Active-Remote: %s\r\n"
                                    "Connection: close\r\n"
                                    "User-Agent: esp32-airplay\r\n"
                                    "\r\n",
                                    path, host, static_cast<unsigned int>(DACP_PORT), active_remote);
  return written > 0 && static_cast<size_t>(written) < out_size;
}

bool send_all(int fd, const char *data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    const ssize_t result = send(fd, data + sent, length - sent, 0);
    if (result > 0) {
      sent += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

int http_get(uint32_t ip, const char *active_remote, const char *path) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }

  sockaddr_in destination{};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(DACP_PORT);
  destination.sin_addr.s_addr = ip;

  const int original_flags = fcntl(fd, F_GETFL, 0);
  bool connected = false;
  if (original_flags >= 0 && fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) == 0) {
    const int result = connect(fd, reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
    if (result == 0) {
      connected = true;
    } else if (errno == EINPROGRESS) {
      fd_set writable{};
      FD_SET(fd, &writable);
      timeval timeout = {DACP_CONNECT_TIMEOUT_MS / 1000, (DACP_CONNECT_TIMEOUT_MS % 1000) * 1000};
      if (select(fd + 1, nullptr, &writable, nullptr, &timeout) > 0) {
        int socket_error = 0;
        socklen_t socket_error_size = sizeof(socket_error);
        connected = getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 && socket_error == 0;
      }
    }
  }

  int status = 0;
  if (connected) {
    fcntl(fd, F_SETFL, original_flags);
    const timeval timeout = {DACP_RESPONSE_TIMEOUT_MS / 1000, (DACP_RESPONSE_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char request[256]{};
    if (make_request(ip, active_remote, path, request, sizeof(request)) && send_all(fd, request, std::strlen(request))) {
      char response[128]{};
      const ssize_t received = recv(fd, response, sizeof(response) - 1, 0);
      if (received > 0) {
        response[received] = '\0';
        if (std::sscanf(response, "HTTP/%*d.%*d %d", &status) != 1) {
          status = 0;
        }
      }
    }
  }
  close(fd);
  return status;
}

bool request_is_current(const DacpRequest &request) {
  portENTER_CRITICAL(&s_mux);
  const bool current = request.generation == s_session.generation && request.ip == s_session.ip &&
                       std::strcmp(request.active_remote, s_session.active_remote) == 0;
  portEXIT_CRITICAL(&s_mux);
  return current;
}

void dacp_task(void *argument) {
  const QueueHandle_t queue = static_cast<QueueHandle_t>(argument);
  for (;;) {
    DacpRequest request{};
    if (xQueueReceive(queue, &request, portMAX_DELAY) != pdTRUE || !request_is_current(request)) {
      continue;
    }
    char path[DACP_PATH_MAX]{};
    const int status = request_path(request, path, sizeof(path)) ? http_get(request.ip, request.active_remote, path) : 0;
    char host[INET_ADDRSTRLEN]{};
    format_ip(request.ip, host, sizeof(host));
    if (status >= 200 && status < 300) {
      ESP_LOGD(TAG, "%s -> %s (HTTP %d)", path, host, status);
    } else {
      ESP_LOGW(TAG, "%s -> %s failed (HTTP %d)", path[0] != '\0' ? path : "unknown", host, status);
    }
  }
}

bool ensure_task_started() {
  portENTER_CRITICAL(&s_mux);
  if (s_queue != nullptr) {
    portEXIT_CRITICAL(&s_mux);
    return true;
  }
  if (s_task_starting) {
    portEXIT_CRITICAL(&s_mux);
    return false;
  }
  s_task_starting = true;
  portEXIT_CRITICAL(&s_mux);

  QueueHandle_t queue = xQueueCreate(DACP_QUEUE_LEN, sizeof(DacpRequest));
  if (queue != nullptr && xTaskCreate(dacp_task, "airplay_dacp", DACP_TASK_STACK, queue, DACP_TASK_PRIORITY, nullptr) == pdPASS) {
    portENTER_CRITICAL(&s_mux);
    s_queue = queue;
    s_task_starting = false;
    portEXIT_CRITICAL(&s_mux);
    return true;
  }
  if (queue != nullptr) {
    vQueueDelete(queue);
  }
  portENTER_CRITICAL(&s_mux);
  s_task_starting = false;
  portEXIT_CRITICAL(&s_mux);
  return false;
}

bool queue_request(DacpRequest *request) {
  QueueHandle_t queue = nullptr;
  portENTER_CRITICAL(&s_mux);
  queue = s_queue;
  request->ip = s_session.ip;
  request->generation = s_session.generation;
  std::strncpy(request->active_remote, s_session.active_remote, sizeof(request->active_remote) - 1);
  request->active_remote[sizeof(request->active_remote) - 1] = '\0';
  portEXIT_CRITICAL(&s_mux);
  return queue != nullptr && request->ip != 0 && request->active_remote[0] != '\0' &&
         xQueueSend(queue, request, 0) == pdTRUE;
}

}  // namespace

bool dacp_session_set(uint32_t client_ip_netorder, const char *active_remote) {
  if (client_ip_netorder == 0 || active_remote == nullptr || active_remote[0] == '\0' ||
      std::strlen(active_remote) >= ACTIVE_REMOTE_MAX || !ensure_task_started()) {
    return false;
  }
  portENTER_CRITICAL(&s_mux);
  s_session.ip = client_ip_netorder;
  ++s_session.generation;
  std::strncpy(s_session.active_remote, active_remote, sizeof(s_session.active_remote) - 1);
  s_session.active_remote[sizeof(s_session.active_remote) - 1] = '\0';
  portEXIT_CRITICAL(&s_mux);
  return true;
}

void dacp_session_clear() {
  portENTER_CRITICAL(&s_mux);
  s_session.ip = 0;
  ++s_session.generation;
  s_session.active_remote[0] = '\0';
  portEXIT_CRITICAL(&s_mux);
}

bool dacp_available() {
  portENTER_CRITICAL(&s_mux);
  const bool available = s_queue != nullptr && s_session.ip != 0 && s_session.active_remote[0] != '\0';
  portEXIT_CRITICAL(&s_mux);
  return available;
}

bool dacp_send(DacpCommand command) {
  if (command_path(command) == nullptr) {
    return false;
  }
  DacpRequest request{};
  request.command = command;
  return queue_request(&request);
}

bool dacp_set_volume(float volume_db) {
  // Reject NaN too: both comparisons are false for it.
  if (!(volume_db >= DACP_MIN_VOLUME_DB && volume_db <= DACP_MAX_VOLUME_DB)) {
    return false;
  }
  DacpRequest request{};
  request.command = DacpCommand::SET_DEVICE_VOLUME;
  request.volume_db = volume_db;
  return queue_request(&request);
}

}  // namespace airplay_receiver
}  // namespace esphome
