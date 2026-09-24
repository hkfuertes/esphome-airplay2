// Encrypted AirPlay 2 reverse events follow Shairport Sync upstream commit
// 441988ce9763062b3fe95c9fd9971ddd457e2466 via shairport-echo patch 0008.
// Copyright (c) Mike Brady 2014--2026; permission notice is in
// licenses/shairport-events.txt.
#include "ap2_events.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "esp_mac.h"
#include "esp_random.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "../allocator.h"
#include "bplist.h"
#include "rtsp_crypto.h"
#include "rtsp_events.h"
#include "transport_module.h"

namespace esphome {
namespace airplay_receiver {
namespace {

constexpr char TAG[] = "airplay_ap2evt";
constexpr size_t AP2_INFO_MAX = 2048;
constexpr size_t AP2_COMMAND_MAX = 2048;
constexpr size_t AP2_REPLY_HEADER_MAX = 2048;
constexpr UBaseType_t AP2_QUEUE_LEN = 8;
constexpr uint32_t AP2_ACCEPT_POLL_MS = 50;
constexpr uint32_t AP2_IDLE_POLL_MS = 20;
constexpr uint32_t AP2_IO_TIMEOUT_S = 2;
constexpr uint32_t AP2_STOP_TIMEOUT_MS = 2500;
constexpr uint32_t AP2_TASK_STACK = 8192;

struct Command {
  bool volume;
  float level;
};

struct EventState {
  int listener = -1;
  int client = -1;
  TaskHandle_t task = nullptr;
  CryptoModule *crypto = nullptr;
  HAPSession *session = nullptr;
  uint32_t peer_ip = 0;
  char group[65] = {};
  uint8_t info[AP2_INFO_MAX] = {};
  size_t info_len = 0;
  bool stopping = true;
  bool starting = false;
  bool ready = false;
  bool have_group = false;
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
EventState s_state;
QueueHandle_t s_queue = nullptr;

bool is_stopping() {
  portENTER_CRITICAL(&s_mux);
  const bool stopping = s_state.stopping;
  portEXIT_CRITICAL(&s_mux);
  return stopping;
}

bool ensure_queue() {
  portENTER_CRITICAL(&s_mux);
  if (s_queue != nullptr) {
    portEXIT_CRITICAL(&s_mux);
    return true;
  }
  portEXIT_CRITICAL(&s_mux);

  QueueHandle_t queue = xQueueCreate(AP2_QUEUE_LEN, sizeof(Command));
  if (queue == nullptr) {
    return false;
  }

  portENTER_CRITICAL(&s_mux);
  const bool installed = s_queue == nullptr;
  if (installed) {
    s_queue = queue;
  }
  portEXIT_CRITICAL(&s_mux);
  if (!installed) {
    vQueueDelete(queue);
  }
  return true;
}

bool parse_reply_header(char *header, int *status, size_t *body_len) {
  if (header == nullptr || status == nullptr || body_len == nullptr ||
      (std::sscanf(header, "RTSP/1.0 %d", status) != 1 &&
       std::sscanf(header, "HTTP/1.1 %d", status) != 1)) {
    return false;
  }

  char *line = std::strstr(header, "\r\n");
  char *end = std::strstr(header, "\r\n\r\n");
  if (line == nullptr || end == nullptr) {
    return false;
  }
  line += 2;
  if (line > end) {
    return false;
  }
  bool have_length = false;
  *body_len = 0;
  while (line < end) {
    char *next = std::strstr(line, "\r\n");
    if (next == nullptr || next > end) {
      return false;
    }
    *next = '\0';
    if (strncasecmp(line, "Content-Length:", 15) == 0) {
      if (have_length) {
        return false;
      }
      char *value = line + 15;
      while (*value == ' ' || *value == '\t') {
        ++value;
      }
      if (*value < '0' || *value > '9') {
        return false;
      }
      char *tail = nullptr;
      errno = 0;
      const unsigned long length = std::strtoul(value, &tail, 10);
      while (*tail == ' ' || *tail == '\t') {
        ++tail;
      }
      if (errno != 0 || *tail != '\0' || length > 65536) {
        return false;
      }
      *body_len = static_cast<size_t>(length);
      have_length = true;
    }
    line = next + 2;
  }
  return true;
}

// Read and fully drain one encrypted reply. A later command must never consume
// a response that belonged to an earlier command, or the nonce stream desyncs.
bool read_reply(int socket, RtspConn *cipher, int *status) {
  char header[AP2_REPLY_HEADER_MAX] = {};
  size_t header_len = 0;
  size_t body_remaining = 0;
  bool have_header = false;

  for (;;) {
    uint8_t block[AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX];
    const int received = rtsp_crypto_read_block(socket, cipher, block, sizeof(block));
    if (received <= 0) {
      return false;
    }

    size_t pos = 0;
    while (pos < static_cast<size_t>(received)) {
      if (!have_header) {
        if (header_len + 1 >= sizeof(header)) {
          return false;
        }
        header[header_len++] = static_cast<char>(block[pos++]);
        if (header_len < 4 || std::memcmp(header + header_len - 4, "\r\n\r\n", 4) != 0) {
          continue;
        }
        header[header_len] = '\0';
        if (!parse_reply_header(header, status, &body_remaining)) {
          return false;
        }
        have_header = true;
        if (body_remaining == 0) {
          return pos == static_cast<size_t>(received);
        }
      } else {
        const size_t available = static_cast<size_t>(received) - pos;
        if (available > body_remaining) {
          return false;
        }
        body_remaining -= available;
        pos += available;
        if (body_remaining == 0) {
          return true;
        }
      }
    }
  }
}

bool post_command(int socket, RtspConn *cipher, const uint8_t *body, size_t body_len, const char *kind) {
  if (body == nullptr || body_len == 0) {
    return false;
  }
  char header[160];
  const int header_len = std::snprintf(header, sizeof(header),
                                       "POST /command RTSP/1.0\r\n"
                                       "Content-Length: %zu\r\n"
                                       "Content-Type: application/x-apple-binary-plist\r\n"
                                       "\r\n",
                                       body_len);
  if (header_len <= 0 || static_cast<size_t>(header_len) >= sizeof(header)) {
    return false;
  }

  const size_t message_len = static_cast<size_t>(header_len) + body_len;
  uint8_t *message = static_cast<uint8_t *>(airplay_alloc(message_len, false));
  if (message == nullptr) {
    return false;
  }
  std::memcpy(message, header, static_cast<size_t>(header_len));
  std::memcpy(message + header_len, body, body_len);
  const bool wrote = rtsp_crypto_write_frame(socket, cipher, message, message_len) == 0;
  airplay_free(message);
  if (!wrote) {
    ESP_LOGW(TAG, "AP2 %s send failed", kind);
    return false;
  }

  int status = 0;
  if (!read_reply(socket, cipher, &status)) {
    ESP_LOGW(TAG, "AP2 %s failed (closed, invalid reply or timeout)", kind);
    return false;
  }
  if (status >= 200 && status < 300) {
    ESP_LOGI(TAG, "AP2 %s: status=%d", kind, status);
  } else {
    ESP_LOGW(TAG, "AP2 %s rejected: status=%d", kind, status);
  }
  // A non-2xx reply was still authenticated and fully consumed. Keep the
  // channel alive so the next command uses the correct event nonce.
  return true;
}

void random_uuid(char (&text)[37]) {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);
  std::snprintf(text, sizeof(text),
                "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool connection_is_idle(int socket) {
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(socket, &readable);
  timeval timeout{};
  const int result = select(socket + 1, &readable, nullptr, nullptr, &timeout);
  return result == 0 || (result < 0 && errno == EINTR);
}

bool enqueue(Command command) {
  QueueHandle_t queue = nullptr;
  bool allowed = false;
  portENTER_CRITICAL(&s_mux);
  queue = s_queue;
  allowed = queue != nullptr && s_state.ready && !s_state.stopping && (command.volume || s_state.have_group);
  portEXIT_CRITICAL(&s_mux);
  return allowed && xQueueSend(queue, &command, 0) == pdTRUE;
}

void ap2_events_task(void *) {
  int listener = -1;
  uint32_t peer_ip = 0;
  CryptoModule *crypto = nullptr;
  HAPSession *session = nullptr;
  QueueHandle_t queue = nullptr;
  char group[sizeof(s_state.group)] = {};
  const uint8_t *info = nullptr;
  size_t info_len = 0;

  portENTER_CRITICAL(&s_mux);
  listener = s_state.listener;
  peer_ip = s_state.peer_ip;
  crypto = s_state.crypto;
  session = s_state.session;
  queue = s_queue;
  std::strncpy(group, s_state.group, sizeof(group) - 1);
  info = s_state.info;
  info_len = s_state.info_len;
  portEXIT_CRITICAL(&s_mux);

  RtspConn cipher{};
  cipher.crypto = crypto;
  cipher.hap_session = session;
  cipher.encrypted_mode = true;
  int client = -1;

  while (!is_stopping() && listener >= 0) {
    sockaddr_in address{};
    socklen_t address_len = sizeof(address);
    client = accept(listener, reinterpret_cast<sockaddr *>(&address), &address_len);
    if (client < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        vTaskDelay(pdMS_TO_TICKS(AP2_ACCEPT_POLL_MS));
        continue;
      }
      ESP_LOGW(TAG, "Event accept failed: %d", errno);
      break;
    }
    if (address.sin_addr.s_addr != peer_ip) {
      close(client);
      client = -1;
    }
  }
  if (listener >= 0) {
    close(listener);
  }

  bool active = client >= 0 && !is_stopping() && crypto != nullptr && session != nullptr && info_len > 0;
  if (active) {
    timeval timeout = {AP2_IO_TIMEOUT_S, 0};
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    portENTER_CRITICAL(&s_mux);
    s_state.client = client;
    portEXIT_CRITICAL(&s_mux);
    transport_events_emit(TRANSPORT_EVENT_CLIENT_CONNECTED, nullptr);
    active = post_command(client, &cipher, info, info_len, "updateInfo");

    portENTER_CRITICAL(&s_mux);
    s_state.ready = active && !s_state.stopping;
    s_state.have_group = group[0] != '\0';
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "AP2 reverse control %s (group=%s)", active ? "ready" : "unavailable",
             group[0] == '\0' ? "missing" : "present");
  }

  while (active && !is_stopping()) {
    Command command{};
    if (xQueueReceive(queue, &command, pdMS_TO_TICKS(AP2_IDLE_POLL_MS)) != pdTRUE) {
      active = connection_is_idle(client);
      continue;
    }

    uint8_t body[AP2_COMMAND_MAX];
    size_t body_len = 0;
    if (command.volume) {
      body_len = bplist_build_event_volume(body, sizeof(body), command.level);
    } else {
      char command_id[37];
      random_uuid(command_id);
      body_len = bplist_build_event_command(body, sizeof(body), group, command_id);
    }
    if (body_len == 0) {
      active = false;
      break;
    }
    active = post_command(client, &cipher, body, body_len, command.volume ? "volume" : "playpause");
  }

  portENTER_CRITICAL(&s_mux);
  if (s_state.client == client) {
    s_state.client = -1;
  }
  portEXIT_CRITICAL(&s_mux);
  if (client >= 0) {
    close(client);
  }
  if (crypto != nullptr && session != nullptr) {
    crypto->free_session(session);
  }

  portENTER_CRITICAL(&s_mux);
  s_state.listener = -1;
  s_state.crypto = nullptr;
  s_state.session = nullptr;
  s_state.info_len = 0;
  s_state.group[0] = '\0';
  s_state.ready = false;
  s_state.have_group = false;
  s_state.stopping = true;
  s_state.starting = false;
  s_state.task = nullptr;
  portEXIT_CRITICAL(&s_mux);
  if (queue != nullptr) {
    xQueueReset(queue);
  }
  vTaskDelete(nullptr);
}

}  // namespace

bool ap2_events_start(int listener, const RtspConn &conn, const char *group_id, const char *device_name) {
  if (listener < 0 || conn.crypto == nullptr || conn.hap_session == nullptr || group_id == nullptr ||
      std::strlen(group_id) >= sizeof(s_state.group) || device_name == nullptr || !ensure_queue()) {
    return false;
  }

  ap2_events_stop();
  portENTER_CRITICAL(&s_mux);
  const bool old_task_finished = s_state.task == nullptr && !s_state.starting;
  portEXIT_CRITICAL(&s_mux);
  if (!old_task_finished) {
    return false;
  }

  HAPSession *session = conn.crypto->create_event_session(conn.hap_session);
  if (session == nullptr) {
    return false;
  }

  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char device_id[18];
  std::snprintf(device_id, sizeof(device_id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
  const uint8_t *public_key = conn.crypto->device_public_key();
  const size_t info_len = bplist_build_info_response(
      s_state.info, sizeof(s_state.info), device_id, device_name, public_key, 32,
      (static_cast<uint64_t>(AIRPLAY_FEATURES_HI) << 32) | AIRPLAY_FEATURES_LO, 2, group_id);
  if (info_len == 0) {
    conn.crypto->free_session(session);
    return false;
  }

  portENTER_CRITICAL(&s_mux);
  s_state.listener = listener;
  s_state.client = -1;
  s_state.crypto = conn.crypto;
  s_state.session = session;
  s_state.peer_ip = conn.client_ip;
  std::strncpy(s_state.group, group_id, sizeof(s_state.group) - 1);
  s_state.group[sizeof(s_state.group) - 1] = '\0';
  s_state.info_len = info_len;
  s_state.stopping = false;
  s_state.starting = true;
  s_state.ready = false;
  s_state.have_group = false;
  portEXIT_CRITICAL(&s_mux);

  xQueueReset(s_queue);
  const BaseType_t created = xTaskCreate(ap2_events_task, "airplay_ap2evt", AP2_TASK_STACK, nullptr, 5, &s_state.task);
  if (created != pdPASS) {
    portENTER_CRITICAL(&s_mux);
    s_state.listener = -1;
    s_state.crypto = nullptr;
    s_state.session = nullptr;
    s_state.info_len = 0;
    s_state.group[0] = '\0';
    s_state.stopping = true;
    s_state.starting = false;
    s_state.task = nullptr;
    portEXIT_CRITICAL(&s_mux);
    conn.crypto->free_session(session);
    ESP_LOGW(TAG, "Failed to create AP2 event task");
    return false;
  }

  portENTER_CRITICAL(&s_mux);
  s_state.starting = false;
  portEXIT_CRITICAL(&s_mux);
  return true;
}

void ap2_events_stop() {
  int client = -1;
  QueueHandle_t queue = nullptr;
  portENTER_CRITICAL(&s_mux);
  s_state.stopping = true;
  s_state.ready = false;
  s_state.have_group = false;
  client = s_state.client;
  queue = s_queue;
  const bool idle = s_state.task == nullptr && !s_state.starting;
  portEXIT_CRITICAL(&s_mux);
  if (idle) {
    if (queue != nullptr) {
      xQueueReset(queue);
    }
    return;
  }
  if (client >= 0) {
    shutdown(client, SHUT_RDWR);
  }

  const uint32_t wait_count = AP2_STOP_TIMEOUT_MS / AP2_ACCEPT_POLL_MS;
  for (uint32_t i = 0; i < wait_count; ++i) {
    portENTER_CRITICAL(&s_mux);
    const bool stopped = s_state.task == nullptr && !s_state.starting;
    portEXIT_CRITICAL(&s_mux);
    if (stopped) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(AP2_ACCEPT_POLL_MS));
  }
  portENTER_CRITICAL(&s_mux);
  const bool stopped = s_state.task == nullptr && !s_state.starting;
  portEXIT_CRITICAL(&s_mux);
  if (!stopped) {
    ESP_LOGW(TAG, "AP2 event task did not exit within timeout");
  }
}

bool ap2_events_available() {
  portENTER_CRITICAL(&s_mux);
  const bool available = s_state.ready && !s_state.stopping;
  portEXIT_CRITICAL(&s_mux);
  return available;
}

bool ap2_events_play_pause() { return enqueue({false, 0.0f}); }

bool ap2_events_volume(float volume) {
  if (!std::isfinite(volume) || volume < 0.0f || volume > 1.0f) {
    return false;
  }
  return enqueue({true, volume});
}

}  // namespace airplay_receiver
}  // namespace esphome
