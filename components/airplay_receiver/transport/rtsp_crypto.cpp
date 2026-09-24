// airplay_receiver RTSP encrypted control channel (port of main/rtsp/rtsp_crypto.c).
#include "rtsp_crypto.h"

#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "esp_timer.h"
#include "../allocator.h"
#include "esphome/core/log.h"

namespace esphome {
namespace airplay_receiver {

static const char *const TAG = "airplay_rtsp_crypto";

static int send_all(int socket, const uint8_t *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t r = send(socket, data + sent, len - sent, 0);
    if (r <= 0) {
      return -1;
    }
    sent += (size_t)r;
  }
  return 0;
}

static int send_all_until(int socket, const uint8_t *data, size_t len, int64_t deadline_us) {
  size_t sent = 0;
  while (sent < len) {
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
      errno = EAGAIN;
      return -1;
    }
    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket, &writable);
    timeval timeout = {static_cast<time_t>(remaining_us / 1000000),
                       static_cast<suseconds_t>(remaining_us % 1000000)};
    const int selected = select(socket + 1, nullptr, &writable, nullptr, &timeout);
    if (selected == 0) {
      errno = EAGAIN;
      return -1;
    }
    if (selected < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    const ssize_t result = send(socket, data + sent, len - sent, MSG_DONTWAIT);
    if (result > 0) {
      sent += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      continue;
    }
    return -1;
  }
  return 0;
}

// deadline_us == 0 preserves the RTSP server's indefinite, interruptible
// receive semantics. AP2 events pass a deadline because an iPhone that never
// answers updateInfo must not leave reverse controls permanently unavailable.
static bool recv_exact(int socket, uint8_t *buffer, size_t size, int64_t deadline_us) {
  size_t received = 0;
  while (received < size) {
    if (deadline_us != 0) {
      const int64_t remaining_us = deadline_us - esp_timer_get_time();
      if (remaining_us <= 0) {
        errno = EAGAIN;
        return false;
      }
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(socket, &readable);
      timeval timeout = {static_cast<time_t>(remaining_us / 1000000),
                         static_cast<suseconds_t>(remaining_us % 1000000)};
      const int selected = select(socket + 1, &readable, nullptr, nullptr, &timeout);
      if (selected == 0) {
        errno = EAGAIN;
        return false;
      }
      if (selected < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
    }

    const ssize_t result = recv(socket, buffer + received, size - received, deadline_us == 0 ? 0 : MSG_DONTWAIT);
    if (result > 0) {
      received += static_cast<size_t>(result);
      continue;
    }
    if (result == 0) {
      errno = 0;  // Clean close is never a timeout.
      return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

static int rtsp_crypto_read_block_impl(int socket, RtspConn *conn, uint8_t *buffer, size_t buffer_size,
                                       int64_t deadline_us) {
  // Clear stale errno up front: recv() returns 0 on a clean close without
  // setting errno, so a caller branching on `errno != EAGAIN` could otherwise
  // keep a stale EAGAIN (left over from the timeout loop) and spin instead of
  // tearing the session down. Forcing errno to 0 on every terminal return makes
  // a clean close always look like a real error to that caller.
  errno = 0;
  if (conn == nullptr || conn->hap_session == nullptr || !conn->encrypted_mode) {
    errno = 0;
    return -1;
  }

  uint8_t len_buf[2];
  if (!recv_exact(socket, len_buf, sizeof(len_buf), deadline_us)) {
    return -1;
  }

  const uint16_t block_len = static_cast<uint16_t>(len_buf[0]) |
                             (static_cast<uint16_t>(len_buf[1]) << 8);
  if (block_len == 0 || block_len > AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX || block_len > buffer_size) {
    ESP_LOGE(TAG, "Invalid encrypted block length: %u", block_len);
    return -1;
  }

  const size_t encrypted_len = block_len + 16;  // + Poly1305 tag
  uint8_t *encrypted = static_cast<uint8_t *>(airplay_alloc(encrypted_len, false));
  if (encrypted == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
    return -1;
  }
  if (!recv_exact(socket, encrypted, encrypted_len, deadline_us)) {
    airplay_free(encrypted);
    return -1;
  }

  size_t plaintext_len = 0;
  if (conn->crypto == nullptr || conn->crypto->session_decrypt(conn->hap_session, encrypted, encrypted_len,
                                                               len_buf, sizeof(len_buf), buffer,
                                                               &plaintext_len) != 0) {
    airplay_free(encrypted);
    ESP_LOGE(TAG, "Failed to decrypt control frame");
    return -1;
  }

  airplay_free(encrypted);
  return static_cast<int>(plaintext_len);
}

int rtsp_crypto_read_block(int socket, RtspConn *conn, uint8_t *buffer, size_t buffer_size) {
  return rtsp_crypto_read_block_impl(socket, conn, buffer, buffer_size, 0);
}

int rtsp_crypto_read_block_until(int socket, RtspConn *conn, uint8_t *buffer, size_t buffer_size,
                                 int64_t deadline_us) {
  return rtsp_crypto_read_block_impl(socket, conn, buffer, buffer_size, deadline_us);
}

static int rtsp_crypto_write_frame_impl(int socket, RtspConn *conn, const uint8_t *data, size_t data_len,
                                        int64_t deadline_us) {
  if (conn == nullptr || conn->hap_session == nullptr || !conn->encrypted_mode) {
    return -1;
  }

  size_t offset = 0;
  while (offset < data_len) {
    const uint16_t block_len = (data_len - offset) > AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX
                                   ? AIRPLAY_RTSP_ENCRYPTED_BLOCK_MAX
                                   : static_cast<uint16_t>(data_len - offset);
    uint8_t len_buf[2] = {static_cast<uint8_t>(block_len & 0xFF),
                          static_cast<uint8_t>((block_len >> 8) & 0xFF)};
    const size_t encrypted_len = block_len + 16;
    uint8_t *encrypted = static_cast<uint8_t *>(airplay_alloc(encrypted_len, false));
    if (encrypted == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate encrypted buffer");
      return -1;
    }

    size_t ct_len = 0;
    if (conn->crypto == nullptr ||
        conn->crypto->session_encrypt(conn->hap_session, data + offset, block_len, len_buf,
                                      sizeof(len_buf), encrypted, &ct_len) != 0 ||
        ct_len != encrypted_len) {
      ESP_LOGE(TAG, "Failed to encrypt control frame");
      airplay_free(encrypted);
      return -1;
    }

    const int sent = deadline_us == 0
                         ? (send_all(socket, len_buf, sizeof(len_buf)) == 0 && send_all(socket, encrypted, ct_len) == 0)
                         : (send_all_until(socket, len_buf, sizeof(len_buf), deadline_us) == 0 &&
                            send_all_until(socket, encrypted, ct_len, deadline_us) == 0);
    airplay_free(encrypted);
    if (!sent) {
      return -1;
    }
    offset += block_len;
  }
  return 0;
}

int rtsp_crypto_write_frame(int socket, RtspConn *conn, const uint8_t *data, size_t data_len) {
  return rtsp_crypto_write_frame_impl(socket, conn, data, data_len, 0);
}

int rtsp_crypto_write_frame_until(int socket, RtspConn *conn, const uint8_t *data, size_t data_len,
                                  int64_t deadline_us) {
  return rtsp_crypto_write_frame_impl(socket, conn, data, data_len, deadline_us);
}

}  // namespace airplay_receiver
}  // namespace esphome
