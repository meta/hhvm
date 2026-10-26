/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman/PDU.h"
#include <folly/Range.h>
#include <folly/String.h>
#include "watchman/Constants.h"
#include "watchman/Logging.h"
#include "watchman/bser.h"
#include "watchman/portability/WinError.h"
#include "watchman/watchman_stream.h"

namespace watchman {

PduBuffer::PduBuffer()
    : buf((char*)malloc(WATCHMAN_IO_BUF_SIZE)), allocd(WATCHMAN_IO_BUF_SIZE) {
  if (!buf) {
    throw std::bad_alloc();
  }
}

PduBuffer::~PduBuffer() {
  free(buf);
}

void PduBuffer::clear() {
  wpos = 0;
  rpos = 0;
}

// Shunt down, return available size
uint32_t PduBuffer::shuntDown() {
  if (rpos && rpos == wpos) {
    rpos = 0;
    wpos = 0;
  }
  if (rpos && rpos < wpos) {
    memmove(buf, buf + rpos, wpos - rpos);
    wpos -= rpos;
    rpos = 0;
  }
  return allocd - wpos;
}

bool PduBuffer::fillBuffer(watchman_stream* stm) {
  uint32_t avail = shuntDown();

  // Get some more space if we need it
  if (avail == 0) {
    char* newBuf = (char*)realloc(buf, allocd * 2);
    if (!newBuf) {
      return false;
    }

    buf = newBuf;
    allocd *= 2;

    avail = allocd - wpos;
  }

  errno = 0;
  int r = stm->read(buf + wpos, avail);
  if (r <= 0) {
    return false;
  }

  wpos += r;

  return true;
}

inline PduType PduBuffer::detectPdu() {
  if (wpos - rpos < 2) {
    return need_data;
  }
  if (memcmp(buf + rpos, BSER_MAGIC, 2) == 0) {
    return is_bser;
  }
  if (memcmp(buf + rpos, BSER_V2_MAGIC, 2) == 0) {
    return is_bser_v2;
  }
  return is_json_compact;
}

std::optional<json_ref> PduBuffer::readJsonPrettyPdu(
    watchman_stream* stm,
    json_error_t* jerr) {
  // Assume newline is at the end of what we have
  char* nl = buf + wpos;
  int r = (int)(nl - (buf + rpos));
  std::optional<json_ref> res = json_loadb(buf + rpos, r, 0, jerr);
  while (!res) {
    // Maybe we can fill more data into the buffer and retry?
    if (!fillBuffer(stm)) {
      // No, then error is terminal
      return std::nullopt;
    }
    // Recompute end of buffer
    nl = buf + wpos;
    r = (int)(nl - (buf + rpos));
    // And try parsing this
    res = json_loadb(buf + rpos, r, 0, jerr);
  }

  // update read pos to look beyond this point
  rpos += r + 1;

  return res;
}

std::optional<json_ref> PduBuffer::readJsonPdu(
    watchman_stream* stm,
    json_error_t* jerr) {
  /* look for a newline; that indicates the end of
   * a json packet */
  auto nl = (char*)memchr(buf + rpos, '\n', wpos - rpos);

  // If we don't have a newline, we need to fill the
  // buffer
  while (!nl) {
    if (!fillBuffer(stm)) {
      if (errno == 0 && stm == w_stm_stdin()) {
        // Ugly-ish hack to support the -j CLI option.  This allows
        // us to consume a JSON input that doesn't end with a newline.
        // We only allow this on EOF when reading from stdin
        nl = buf + wpos;
        break;
      }
      return std::nullopt;
    }
    nl = (char*)memchr(buf + rpos, '\n', wpos - rpos);
  }

  // buflen
  int r = (int)(nl - (buf + rpos));
  auto res = json_loadb(buf + rpos, r, 0, jerr);

  // update read pos to look beyond this point
  rpos += r + 1;

  return res;
}

bool PduBuffer::decodePduInfo(
    watchman_stream* stm,
    uint32_t bser_version,
    json_int_t* len,
    json_int_t* bser_capabilities,
    json_error_t* jerr) {
  if (bser_version == 2) {
    uint32_t capabilities;
    while (wpos - rpos < sizeof(capabilities)) {
      if (!fillBuffer(stm)) {
        snprintf(jerr->text, sizeof(jerr->text), "unable to fill buffer");
        return false;
      }
    }
    // json_int_t is architecture-dependent, so go through the uint32_t for
    // safety.
    memcpy(&capabilities, buf + rpos, sizeof(capabilities));
    *bser_capabilities = capabilities;
    rpos += sizeof(capabilities);
  }

  for (;;) {
    size_t needed;
    std::optional<json_int_t> pdu_length =
        bunser_int(buf + rpos, wpos - rpos, &needed);
    if (pdu_length) {
      *len = *pdu_length;
      rpos += needed;
      return true;
    }

    if (needed == kDecodeIntFailed) {
      snprintf(jerr->text, sizeof(jerr->text), "failed to read PDU size");
      return false;
    }
    if (!fillBuffer(stm)) {
      snprintf(jerr->text, sizeof(jerr->text), "unable to fill buffer");
      return false;
    }
  }
}

std::optional<json_ref> PduBuffer::readBserPdu(
    watchman_stream* stm,
    uint32_t bser_version,
    json_error_t* jerr) {
  json_int_t val;
  json_int_t bser_capabilities;
  uint32_t ideal;
  int r;

  rpos += 2;

  // We don't handle EAGAIN cleanly in here
  stm->setNonBlock(false);
  if (!decodePduInfo(stm, bser_version, &val, &bser_capabilities, jerr)) {
    return std::nullopt;
  }

  // val tells us exactly how much storage we need for this PDU
  if (val > allocd - wpos) {
    ideal = allocd;
    while ((ideal - wpos) < (uint32_t)val) {
      ideal *= 2;
    }
    if (ideal > allocd) {
      auto newBuf = (char*)realloc(buf, ideal);

      if (!newBuf) {
        snprintf(
            jerr->text,
            sizeof(jerr->text),
            "out of memory while allocating %" PRIu32 " bytes",
            ideal);
        return std::nullopt;
      }

      buf = newBuf;
      allocd = ideal;
    }
  }

  // We have enough room for the whole thing, let's read it in
  while ((wpos - rpos) < val) {
    r = stm->read(buf + wpos, allocd - wpos);
    if (r <= 0) {
      jerr->position = wpos - rpos;
      snprintf(
          jerr->text,
          sizeof(jerr->text),
          "error reading %" PRIu32 " bytes val=%" PRIu64 " wpos=%" PRIu32
          " rpos=%" PRIu32 " for PDU: %s",
          uint32_t(allocd - wpos),
          int64_t(val),
          wpos,
          rpos,
          folly::errnoStr(errno).c_str());
      return std::nullopt;
    }
    wpos += r;
  }

  std::optional<json_ref> obj;
  try {
    obj = bunser(buf + rpos, buf + wpos);
  } catch (const BserParseError& e) {
    // Deserialization failed. Log the message that failed to deserialize to
    // stderr.
    logf(
        ERR,
        "decoding BSER failed. The first KB of the hex representation of "
        "message follows:\n{:.1024}\n",
        folly::hexlify(
            folly::ByteRange{
                reinterpret_cast<const unsigned char*>(buf + rpos),
                wpos - rpos}));
    *jerr = e.detail;
  }

  // Ensure that we move the read position to the wpos; we consumed it all
  rpos = wpos;

  stm->setNonBlock(true);
  return obj;
}

bool PduBuffer::readAndDetectPdu(watchman_stream* stm, json_error_t* jerr) {
  PduFormat detected_format;

  shuntDown();
  detected_format.type = detectPdu();
  if (detected_format.type == need_data) {
    if (!fillBuffer(stm)) {
      if (errno != EAGAIN) {
        snprintf(
            jerr->text,
            sizeof(jerr->text),
            "fill_buffer: %s",
            errno ? folly::errnoStr(errno).c_str() : "EOF");
      }
      return false;
    }
    detected_format.type = detectPdu();
  }

  constexpr size_t kCapSize = 4;
  static_assert(kCapSize == sizeof(detected_format.capabilities));

  if (detected_format.type == is_bser_v2) {
    // read capabilities (since we haven't increased rpos, first two bytes are
    // still the header)
    while (wpos - rpos < 2 + kCapSize) {
      if (!fillBuffer(stm)) {
        if (errno != EAGAIN) {
          snprintf(
              jerr->text,
              sizeof(jerr->text),
              "fillBuffer: %s",
              errno ? folly::errnoStr(errno).c_str() : "EOF");
        }
        return false;
      }
    }

    // Copy the capabilities over. BSER is system-endian so this is safe.
    memcpy(&detected_format.capabilities, buf + rpos + 2, kCapSize);
  }

  if (detected_format.type == is_json_compact && stm == w_stm_stdin()) {
    // Minor hack for the `-j` option for reading pretty printed
    // json from stdin
    detected_format.type = is_json_pretty;
  }

  format = detected_format;
  return true;
}

static bool output_bytes(const char* buf, int x) {
  auto& stm = FileDescriptor::stdOut();

  while (x > 0) {
    auto res = stm.write(buf, x);
    if (res.hasError()) {
      errno = res.error().value();
#ifdef _WIN32
      // TODO: propagate Result<int, std::error_code> as return type
      errno = map_win32_err(errno);
#endif
      return false;
    }

    auto len = res.value();

    buf += len;
    x -= len;
  }
  return true;
}

bool PduBuffer::streamUntilNewLine(watchman_stream* stm) {
  bool is_done = false;

  while (true) {
    char* localBuf = buf + rpos;
    auto nl = (char*)memchr(localBuf, '\n', wpos - rpos);
    int x;
    if (nl) {
      x = 1 + (int)(nl - localBuf);
      is_done = true;
    } else {
      x = wpos - rpos;
    }

    if (!output_bytes(localBuf, x)) {
      return false;
    }
    rpos += x;

    if (is_done) {
      break;
    }

    if (!fillBuffer(stm)) {
      break;
    }
  }
  return true;
}

bool PduBuffer::streamN(
    watchman_stream* stm,
    json_int_t len,
    json_error_t* jerr) {
  if (!output_bytes(buf, rpos)) {
    snprintf(
        jerr->text,
        sizeof(jerr->text),
        "failed output headers bytes %d: %s\n",
        rpos,
        folly::errnoStr(errno).c_str());
    return false;
  }
  while (len > 0) {
    uint32_t avail = wpos - rpos;

    if (avail) {
      if (!output_bytes(buf + rpos, avail)) {
        snprintf(
            jerr->text,
            sizeof(jerr->text),
            "output_bytes: avail=%d, failed %s\n",
            avail,
            folly::errnoStr(errno).c_str());
        return false;
      }
      rpos += avail;
      len -= avail;

      if (len == 0) {
        return true;
      }
    }

    avail = std::min((uint32_t)len, shuntDown());
    int r = stm->read(buf + wpos, avail);

    if (r <= 0) {
      snprintf(
          jerr->text,
          sizeof(jerr->text),
          "read: len=%" PRIi64 " wanted %" PRIu32 " got %d %s\n",
          (int64_t)len,
          avail,
          r,
          folly::errnoStr(errno).c_str());
      return false;
    }
    wpos += r;
  }
  return true;
}

bool PduBuffer::streamPdu(watchman_stream* stm, json_error_t* jerr) {
  switch (format.type) {
    case is_json_compact:
    case is_json_pretty:
      return streamUntilNewLine(stm);
    case is_bser:
    case is_bser_v2: {
      uint32_t bser_version;
      if (format.type == is_bser_v2) {
        bser_version = 2;
      } else {
        bser_version = 1;
      }
      rpos += 2;
      json_int_t bser_capabilities;
      json_int_t len;
      if (!decodePduInfo(stm, bser_version, &len, &bser_capabilities, jerr)) {
        return false;
      }
      return streamN(stm, len, jerr);
    }
    default:
      logf(FATAL, "not streaming for pdu type {}\n", format.type);
      return false;
  }
}

std::optional<json_ref> PduBuffer::decodePdu(
    watchman_stream* stm,
    json_error_t* jerr) {
  switch (format.type) {
    case is_json_compact:
      return readJsonPdu(stm, jerr);
    case is_json_pretty:
      return readJsonPrettyPdu(stm, jerr);
    case is_bser_v2:
      return readBserPdu(stm, 2, jerr);
    default: // bser v1
      return readBserPdu(stm, 1, jerr);
  }
}

std::optional<json_ref> PduBuffer::decodeNext(
    watchman_stream* stm,
    json_error_t* jerr) {
  *jerr = json_error_t();
  if (!readAndDetectPdu(stm, jerr)) {
    return std::nullopt;
  }
  return decodePdu(stm, jerr);
}

namespace {

struct jbuffer_write_data {
  watchman_stream* stm;
  PduBuffer* jr;

  bool flush() {
    while (jr->wpos - jr->rpos) {
      int x = stm->write(jr->buf + jr->rpos, jr->wpos - jr->rpos);

      if (x <= 0) {
        return false;
      }

      jr->rpos += x;
    }

    jr->clear();
    return true;
  }

  static int write(const char* buffer, size_t size, void* ptr) {
    auto data = (jbuffer_write_data*)ptr;
    return data->write(buffer, size);
  }

  int write(const char* buffer, size_t size) {
    while (size) {
      // Accumulate in the buffer
      int room = jr->allocd - jr->wpos;

      // No room? send it over the wire
      if (!room) {
        if (!flush()) {
          return -1;
        }
        room = jr->allocd - jr->wpos;
      }

      if ((int)size < room) {
        room = (int)size;
      }

      // Stick it in the buffer
      memcpy(jr->buf + jr->wpos, buffer, room);

      buffer += room;
      size -= room;
      jr->wpos += room;
    }

    return 0;
  }
};

} // namespace

ResultErrno<folly::Unit> PduBuffer::bserEncodeToStream(
    uint32_t bser_version,
    uint32_t bser_capabilities,
    const json_ref& json,
    watchman_stream* stm) {
  jbuffer_write_data data = {stm, this};

  int res = w_bser_write_pdu(
      bser_version, bser_capabilities, jbuffer_write_data::write, json, &data);

  if (res != 0) {
    return errno;
  }

  if (!data.flush()) {
    return errno;
  }

  return folly::unit;
}

ResultErrno<folly::Unit> PduBuffer::jsonEncodeToStream(
    const json_ref& json,
    watchman_stream* stm,
    int flags) {
  jbuffer_write_data data = {stm, this};

  int res = json_dump_callback(json, jbuffer_write_data::write, &data, flags);
  if (res != 0) {
    return errno;
  }

  if (data.write("\n", 1) != 0) {
    return errno;
  }

  if (!data.flush()) {
    return errno;
  }

  return folly::unit;
}

ResultErrno<folly::Unit> PduBuffer::pduEncodeToStream(
    PduFormat format_2,
    const json_ref& json,
    watchman_stream* stm) {
  switch (format_2.type) {
    case is_json_compact:
      return jsonEncodeToStream(json, stm, JSON_COMPACT);
    case is_json_pretty:
      return jsonEncodeToStream(json, stm, JSON_INDENT(4));
    case is_bser:
      return bserEncodeToStream(1, format_2.capabilities, json, stm);
    case is_bser_v2:
      return bserEncodeToStream(2, format_2.capabilities, json, stm);
    case need_data:
    default:
      return EINVAL;
  }
}

/* vim:ts=2:sw=2:et:
 */

} // namespace watchman
