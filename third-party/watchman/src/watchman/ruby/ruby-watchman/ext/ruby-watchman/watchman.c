/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "watchman.h"

#if defined(HAVE_RUBY_ST_H)
#include <ruby/st.h>
#elif defined(HAVE_ST_H)
#include <st.h>
#else
#error no st.h header found
#endif

#include <fcntl.h> /* for fcntl() */
#include <sys/errno.h> /* for errno */
#include <sys/socket.h> /* for recv(), MSG_PEEK */

typedef struct {
  uint8_t* data; // payload
  size_t cap; // total capacity
  size_t len; // current length
} watchman_t;

// Forward declarations:
VALUE watchman_load(char** ptr, char* end);
void watchman_dump(watchman_t* w, VALUE serializable);

#define WATCHMAN_DEFAULT_STORAGE 4096

#define WATCHMAN_BINARY_MARKER "\x00\x01"
#define WATCHMAN_ARRAY_MARKER 0x00
#define WATCHMAN_HASH_MARKER 0x01
#define WATCHMAN_STRING_MARKER 0x02
#define WATCHMAN_INT8_MARKER 0x03
#define WATCHMAN_INT16_MARKER 0x04
#define WATCHMAN_INT32_MARKER 0x05
#define WATCHMAN_INT64_MARKER 0x06
#define WATCHMAN_FLOAT_MARKER 0x07
#define WATCHMAN_TRUE 0x08
#define WATCHMAN_FALSE 0x09
#define WATCHMAN_NIL 0x0a
#define WATCHMAN_TEMPLATE_MARKER 0x0b
#define WATCHMAN_SKIP_MARKER 0x0c

#define WATCHMAN_HEADER  \
  WATCHMAN_BINARY_MARKER \
  "\x06"                 \
  "\x00\x00\x00\x00\x00\x00\x00\x00"

static const char watchman_array_marker = WATCHMAN_ARRAY_MARKER;
static const char watchman_hash_marker = WATCHMAN_HASH_MARKER;
static const char watchman_string_marker = WATCHMAN_STRING_MARKER;
static const char watchman_true = WATCHMAN_TRUE;
static const char watchman_false = WATCHMAN_FALSE;
static const char watchman_nil = WATCHMAN_NIL;

/**
 * Appends `len` bytes, starting at `data`, to the watchman_t struct `w`
 *
 * Will attempt to reallocate the underlying storage if it is not sufficient.
 */
void watchman_append(watchman_t* w, const char* data, size_t len) {
  if (w->len + len > w->cap) {
    w->cap += w->len + WATCHMAN_DEFAULT_STORAGE;
    REALLOC_N(w->data, uint8_t, w->cap);
  }
  memcpy(w->data + w->len, data, len);
  w->len += len;
}

/**
 * Allocate a new watchman_t struct
 *
 * The struct has a small amount of extra capacity preallocated, and a blank
 * header that can be filled in later to describe the PDU.
 */
watchman_t* watchman_init() {
  watchman_t* w = ALLOC(watchman_t);
  w->cap = WATCHMAN_DEFAULT_STORAGE;
  w->len = 0;
  w->data = ALLOC_N(uint8_t, WATCHMAN_DEFAULT_STORAGE);

  watchman_append(w, WATCHMAN_HEADER, sizeof(WATCHMAN_HEADER) - 1);
  return w;
}

/**
 * Free a watchman_t struct `w` that was previously allocated with
 * `watchman_init`
 */
void watchman_free(watchman_t* w) {
  xfree(w->data);
  xfree(w);
}

/**
 * Encodes and appends the integer `num` to `w`
 */
void watchman_dump_int(watchman_t* w, int64_t num) {
  char encoded[1 + sizeof(int64_t)];

  if (num == (int8_t)num) {
    encoded[0] = WATCHMAN_INT8_MARKER;
    encoded[1] = (int8_t)num;
    watchman_append(w, encoded, 1 + sizeof(int8_t));
  } else if (num == (int16_t)num) {
    encoded[0] = WATCHMAN_INT16_MARKER;
    *(int16_t*)(encoded + 1) = (int16_t)num;
    watchman_append(w, encoded, 1 + sizeof(int16_t));
  } else if (num == (int32_t)num) {
    encoded[0] = WATCHMAN_INT32_MARKER;
    *(int32_t*)(encoded + 1) = (int32_t)num;
    watchman_append(w, encoded, 1 + sizeof(int32_t));
  } else {
    encoded[0] = WATCHMAN_INT64_MARKER;
    *(int64_t*)(encoded + 1) = (int64_t)num;
    watchman_append(w, encoded, 1 + sizeof(int64_t));
  }
}

/**
 * Encodes and appends the string `string` to `w`
 */
void watchman_dump_string(watchman_t* w, VALUE string) {
  watchman_append(w, &watchman_string_marker, sizeof(watchman_string_marker));
  watchman_dump_int(w, RSTRING_LEN(string));
  watchman_append(w, RSTRING_PTR(string), RSTRING_LEN(string));
}

/**
 * Encodes and appends the double `num` to `w`
 */
void watchman_dump_double(watchman_t* w, double num) {
  char encoded[1 + sizeof(double)];
  encoded[0] = WATCHMAN_FLOAT_MARKER;
  *(double*)(encoded + 1) = num;
  watchman_append(w, encoded, sizeof(encoded));
}

/**
 * Encodes and appends the array `array` to `w`
 */
void watchman_dump_array(watchman_t* w, VALUE array) {
  long i;
  watchman_append(w, &watchman_array_marker, sizeof(watchman_array_marker));
  watchman_dump_int(w, RARRAY_LEN(array));
  for (i = 0; i < RARRAY_LEN(array); i++) {
    watchman_dump(w, rb_ary_entry(array, i));
  }
}

/**
 * Helper method that encodes and appends a key/value pair (`key`, `value`) from
 * a hash to the watchman_t struct passed in via `data`
 */
int watchman_dump_hash_iterator(VALUE key, VALUE value, VALUE data) {
  watchman_t* w = (watchman_t*)data;
  watchman_dump_string(w, StringValue(key));
  watchman_dump(w, value);
  return ST_CONTINUE;
}

/**
 * Encodes and appends the hash `hash` to `w`
 */
void watchman_dump_hash(watchman_t* w, VALUE hash) {
  watchman_append(w, &watchman_hash_marker, sizeof(watchman_hash_marker));
  watchman_dump_int(w, RHASH_SIZE(hash));
  rb_hash_foreach(hash, watchman_dump_hash_iterator, (VALUE)w);
}

/**
 * Encodes and appends the serialized Ruby object `serializable` to `w`
 *
 * Examples of serializable objects include arrays, hashes, strings, numbers
 * (integers, floats), booleans, and nil.
 */
void watchman_dump(watchman_t* w, VALUE serializable) {
  switch (TYPE(serializable)) {
    case T_ARRAY:
      return watchman_dump_array(w, serializable);
    case T_HASH:
      return watchman_dump_hash(w, serializable);
    case T_STRING:
      return watchman_dump_string(w, serializable);
    case T_FIXNUM: // up to 63 bits
      return watchman_dump_int(w, FIX2LONG(serializable));
    case T_BIGNUM:
      return watchman_dump_int(w, NUM2LL(serializable));
    case T_FLOAT:
      return watchman_dump_double(w, NUM2DBL(serializable));
    case T_TRUE:
      return watchman_append(w, &watchman_true, sizeof(watchman_true));
    case T_FALSE:
      return watchman_append(w, &watchman_false, sizeof(watchman_false));
    case T_NIL:
      return watchman_append(w, &watchman_nil, sizeof(watchman_nil));
    default:
      rb_raise(rb_eTypeError, "unsupported type");
  }
}

/**
 * Extract and return the int encoded at `ptr`
 *
 * Moves `ptr` past the extracted int.
 *
 * Will raise an ArgumentError if extracting the int would take us beyond the
 * end of the buffer indicated by `end`, or if there is no int encoded at `ptr`.
 *
 * @returns The extracted int
 */
int64_t watchman_load_int(char** ptr, char* end) {
  char* val_ptr = *ptr + sizeof(int8_t);
  int64_t val = 0;

  if (val_ptr >= end) {
    rb_raise(rb_eArgError, "insufficient int storage");
  }

  switch (*ptr[0]) {
    case WATCHMAN_INT8_MARKER:
      if (val_ptr + sizeof(int8_t) > end) {
        rb_raise(rb_eArgError, "overrun extracting int8_t");
      }
      val = *(int8_t*)val_ptr;
      *ptr = val_ptr + sizeof(int8_t);
      break;
    case WATCHMAN_INT16_MARKER:
      if (val_ptr + sizeof(int16_t) > end) {
        rb_raise(rb_eArgError, "overrun extracting int16_t");
      }
      val = *(int16_t*)val_ptr;
      *ptr = val_ptr + sizeof(int16_t);
      break;
    case WATCHMAN_INT32_MARKER:
      if (val_ptr + sizeof(int32_t) > end) {
        rb_raise(rb_eArgError, "overrun extracting int32_t");
      }
      val = *(int32_t*)val_ptr;
      *ptr = val_ptr + sizeof(int32_t);
      break;
    case WATCHMAN_INT64_MARKER:
      if (val_ptr + sizeof(int64_t) > end) {
        rb_raise(rb_eArgError, "overrun extracting int64_t");
      }
      val = *(int64_t*)val_ptr;
      *ptr = val_ptr + sizeof(int64_t);
      break;
    default:
      rb_raise(
          rb_eArgError, "bad integer marker 0x%02x", (unsigned int)*ptr[0]);
      break;
  }

  return val;
}

/**
 * Reads and returns a string encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
VALUE watchman_load_string(char** ptr, char* end) {
  if (*ptr >= end) {
    rb_raise(rb_eArgError, "unexpected end of input");
  }

  if (*ptr[0] != WATCHMAN_STRING_MARKER) {
    rb_raise(rb_eArgError, "not a number");
  }

  *ptr += sizeof(int8_t);
  if (*ptr >= end) {
    rb_raise(rb_eArgError, "invalid string header");
  }

  int64_t len = watchman_load_int(ptr, end);
  if (len == 0) { // special case for zero-length strings
    return rb_str_new2("");
  } else if (*ptr + len > end) {
    rb_raise(rb_eArgError, "insufficient string storage");
  }

  VALUE string = rb_str_new(*ptr, len);
  *ptr += len;
  return string;
}

/**
 * Reads and returns a double encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
double watchman_load_double(char** ptr, char* end) {
  *ptr += sizeof(int8_t); // caller has already verified the marker
  if (*ptr + sizeof(double) > end) {
    rb_raise(rb_eArgError, "insufficient double storage");
  }
  double val = *(double*)*ptr;
  *ptr += sizeof(double);
  return val;
}

/**
 * Helper method which returns length of the array encoded in the Watchman
 * binary protocol format, starting at `ptr` and finishing at or before `end`
 */
int64_t watchman_load_array_header(char** ptr, char* end) {
  if (*ptr >= end) {
    rb_raise(rb_eArgError, "unexpected end of input");
  }

  // verify and consume marker
  if (*ptr[0] != WATCHMAN_ARRAY_MARKER) {
    rb_raise(rb_eArgError, "not an array");
  }
  *ptr += sizeof(int8_t);

  // expect a count
  if (*ptr + sizeof(int8_t) * 2 > end) {
    rb_raise(rb_eArgError, "incomplete array header");
  }
  return watchman_load_int(ptr, end);
}

/**
 * Reads and returns an array encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
VALUE watchman_load_array(char** ptr, char* end) {
  int64_t count, i;
  VALUE array;

  count = watchman_load_array_header(ptr, end);
  array = rb_ary_new2(count);

  for (i = 0; i < count; i++) {
    rb_ary_push(array, watchman_load(ptr, end));
  }

  return array;
}

/**
 * Reads and returns a hash encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
VALUE watchman_load_hash(char** ptr, char* end) {
  int64_t count, i;
  VALUE hash, key, value;

  *ptr += sizeof(int8_t); // caller has already verified the marker

  // expect a count
  if (*ptr + sizeof(int8_t) * 2 > end) {
    rb_raise(rb_eArgError, "incomplete hash header");
  }
  count = watchman_load_int(ptr, end);

  hash = rb_hash_new();

  for (i = 0; i < count; i++) {
    key = watchman_load_string(ptr, end);
    value = watchman_load(ptr, end);
    rb_hash_aset(hash, key, value);
  }

  return hash;
}

/**
 * Reads and returns a templated array encoded in the Watchman binary protocol
 * format, starting at `ptr` and finishing at or before `end`
 *
 * Templated arrays are arrays of hashes which have repetitive key information
 * pulled out into a separate "headers" prefix.
 *
 * @see https://meta.github.io/watchman/docs/bser.html
 */
VALUE watchman_load_template(char** ptr, char* end) {
  int64_t header_items_count, i, row_count;
  VALUE array, hash, header, key, value;

  *ptr += sizeof(int8_t); // caller has already verified the marker

  // process template header array
  header_items_count = watchman_load_array_header(ptr, end);
  header = rb_ary_new2(header_items_count);
  for (i = 0; i < header_items_count; i++) {
    rb_ary_push(header, watchman_load_string(ptr, end));
  }

  // process row items
  row_count = watchman_load_int(ptr, end);
  array = rb_ary_new2(header_items_count);
  while (row_count--) {
    hash = rb_hash_new();
    for (i = 0; i < header_items_count; i++) {
      if (*ptr >= end) {
        rb_raise(rb_eArgError, "unexpected end of input");
      }

      if (*ptr[0] == WATCHMAN_SKIP_MARKER) {
        *ptr += sizeof(uint8_t);
      } else {
        value = watchman_load(ptr, end);
        key = rb_ary_entry(header, i);
        rb_hash_aset(hash, key, value);
      }
    }
    rb_ary_push(array, hash);
  }
  return array;
}

/**
 * Reads and returns an object encoded in the Watchman binary protocol format,
 * starting at `ptr` and finishing at or before `end`
 */
VALUE watchman_load(char** ptr, char* end) {
  if (*ptr >= end) {
    rb_raise(rb_eArgError, "unexpected end of input");
  }

  switch (*ptr[0]) {
    case WATCHMAN_ARRAY_MARKER:
      return watchman_load_array(ptr, end);
    case WATCHMAN_HASH_MARKER:
      return watchman_load_hash(ptr, end);
    case WATCHMAN_STRING_MARKER:
      return watchman_load_string(ptr, end);
    case WATCHMAN_INT8_MARKER:
    case WATCHMAN_INT16_MARKER:
    case WATCHMAN_INT32_MARKER:
    case WATCHMAN_INT64_MARKER:
      return LL2NUM(watchman_load_int(ptr, end));
    case WATCHMAN_FLOAT_MARKER:
      return rb_float_new(watchman_load_double(ptr, end));
    case WATCHMAN_TRUE:
      *ptr += 1;
      return Qtrue;
    case WATCHMAN_FALSE:
      *ptr += 1;
      return Qfalse;
    case WATCHMAN_NIL:
      *ptr += 1;
      return Qnil;
    case WATCHMAN_TEMPLATE_MARKER:
      return watchman_load_template(ptr, end);
    default:
      rb_raise(rb_eTypeError, "unsupported type");
  }

  return Qnil; // keep the compiler happy
}

/**
 * RubyWatchman.load(serialized)
 *
 * Converts the binary object, `serialized`, from the Watchman binary protocol
 * format into a normal Ruby object.
 */
VALUE RubyWatchman_load(VALUE self, VALUE serialized) {
  serialized = StringValue(serialized);
  long len = RSTRING_LEN(serialized);
  char* ptr = RSTRING_PTR(serialized);
  char* end = ptr + len;

  // expect at least the binary marker and a int8_t length counter
  if ((size_t)len < sizeof(WATCHMAN_BINARY_MARKER) - 1 + sizeof(int8_t) * 2) {
    rb_raise(rb_eArgError, "undersized header");
  }

  int mismatched =
      memcmp(ptr, WATCHMAN_BINARY_MARKER, sizeof(WATCHMAN_BINARY_MARKER) - 1);
  if (mismatched) {
    rb_raise(rb_eArgError, "missing binary marker");
  }

  // get size marker
  ptr += sizeof(WATCHMAN_BINARY_MARKER) - 1;
  uint64_t payload_size = watchman_load_int(&ptr, end);
  if (!payload_size) {
    rb_raise(rb_eArgError, "empty payload");
  }

  // sanity check length
  if (ptr + payload_size != end) {
    rb_raise(
        rb_eArgError,
        "payload size mismatch (%lu)",
        (unsigned long)(end - (ptr + payload_size)));
  }

  VALUE loaded = watchman_load(&ptr, end);

  // one more sanity check
  if (ptr != end) {
    rb_raise(
        rb_eArgError,
        "payload termination mismatch (%lu)",
        (unsigned long)(end - ptr));
  }

  return loaded;
}

/**
 * RubyWatchman.dump(serializable)
 *
 * Converts the Ruby object, `serializable`, into a binary string in the
 * Watchman binary protocol format.
 *
 * Examples of serializable objects include arrays, hashes, strings, numbers
 * (integers, floats), booleans, and nil.
 */
VALUE RubyWatchman_dump(VALUE self, VALUE serializable) {
  watchman_t* w = watchman_init();
  watchman_dump(w, serializable);

  // update header with final length information
  uint64_t* len =
      (uint64_t*)(w->data + sizeof(WATCHMAN_HEADER) - sizeof(uint64_t) - 1);
  *len = w->len - sizeof(WATCHMAN_HEADER) + 1;

  // prepare final return value
  VALUE serialized = rb_str_buf_new(w->len);
  rb_str_buf_cat(serialized, (const char*)w->data, w->len);
  watchman_free(w);
  return serialized;
}

// How far we have to look to figure out the size of the PDU header
#define WATCHMAN_SNIFF_BUFFER_SIZE \
  sizeof(WATCHMAN_BINARY_MARKER) - 1 + sizeof(int8_t)

// How far we have to peek, at most, to figure out the size of the PDU itself
#define WATCHMAN_PEEK_BUFFER_SIZE                                      \
  sizeof(WATCHMAN_BINARY_MARKER) - 1 + sizeof(WATCHMAN_INT64_MARKER) + \
      sizeof(int64_t)

/**
 * RubyWatchman.query(query, socket)
 *
 * Converts `query`, a Watchman query comprising Ruby objects, into the Watchman
 * binary protocol format, transmits it over socket, and unserializes and
 * returns the result.
 */
VALUE RubyWatchman_query(VALUE self, VALUE query, VALUE socket) {
  VALUE error = Qnil;
  VALUE errorClass = Qnil;
  VALUE loaded = Qnil;
  char* buffer = NULL;
  int fileno = NUM2INT(rb_funcall(socket, rb_intern("fileno"), 0));

  // do blocking I/O to simplify the following logic
  int flags = fcntl(fileno, F_GETFL);
  if (!(flags & O_NONBLOCK) &&
      fcntl(fileno, F_SETFL, flags & ~O_NONBLOCK) == -1) {
    error = rb_str_new2("unable to clear O_NONBLOCK flag");
    goto cleanup;
  }

  // send the message
  VALUE serialized = RubyWatchman_dump(self, query);
  long query_len = RSTRING_LEN(serialized);
  ssize_t sent = send(fileno, RSTRING_PTR(serialized), query_len, 0);
  if (sent == -1) {
    goto system_call_fail;
  } else if (sent != query_len) {
    error = rb_str_new2("sent byte count mismatch");
    goto cleanup;
  }

  // sniff to see how large the header is
  int8_t peek[WATCHMAN_PEEK_BUFFER_SIZE];
  ssize_t received =
      recv(fileno, peek, WATCHMAN_SNIFF_BUFFER_SIZE, MSG_PEEK | MSG_WAITALL);
  if (received == -1) {
    goto system_call_fail;
  } else if (received != WATCHMAN_SNIFF_BUFFER_SIZE) {
    error = rb_str_new2("failed to sniff PDU header");
    goto cleanup;
  }

  // peek at size of PDU
  int8_t sizes[] = {0, 0, 0, 1, 2, 4, 8};
  int8_t sizes_idx = peek[sizeof(WATCHMAN_BINARY_MARKER) - 1];
  if (sizes_idx < WATCHMAN_INT8_MARKER || sizes_idx > WATCHMAN_INT64_MARKER) {
    error = rb_str_new2("bad PDU size marker");
    goto cleanup;
  }
  ssize_t peek_size =
      sizeof(WATCHMAN_BINARY_MARKER) - 1 + sizeof(int8_t) + sizes[sizes_idx];

  received = recv(fileno, peek, peek_size, MSG_PEEK);
  if (received == -1) {
    goto system_call_fail;
  } else if (received != peek_size) {
    error = rb_str_new2("failed to peek at PDU header");
    goto cleanup;
  }
  int8_t* pdu_size_ptr = peek + sizeof(WATCHMAN_BINARY_MARKER) - sizeof(int8_t);
  int64_t payload_size = peek_size +
      watchman_load_int((char**)&pdu_size_ptr, (char*)peek + peek_size);

  // actually read the PDU
  buffer = xmalloc(payload_size);
  if (!buffer) {
    errorClass = rb_eNoMemError;
    error = rb_str_new2("failed to allocate");
    goto cleanup;
  }
  received = recv(fileno, buffer, payload_size, MSG_WAITALL);
  if (received == -1) {
    goto system_call_fail;
  } else if (received != payload_size) {
    error = rb_str_new2("failed to load PDU");
    goto cleanup;
  }

  if (!(flags & O_NONBLOCK) && fcntl(fileno, F_SETFL, flags) == -1) {
    error = rb_str_new2("unable to restore fnctl flags");
    goto cleanup;
  }

  char* payload = buffer + peek_size;
  loaded = watchman_load(&payload, payload + payload_size);
  goto cleanup;

system_call_fail:
  errorClass = rb_eSystemCallError;
  error = INT2FIX(errno);

cleanup:
  if (buffer) {
    xfree(buffer);
  }

  if (!(flags & O_NONBLOCK) && fcntl(fileno, F_SETFL, flags) == -1) {
    rb_raise(rb_eRuntimeError, "unable to restore fnctl flags");
  }

  if (NIL_P(errorClass)) {
    errorClass = rb_eRuntimeError;
  }

  if (!NIL_P(error)) {
    rb_exc_raise(rb_class_new_instance(1, &error, errorClass));
  }

  return loaded;
}

VALUE mRubyWatchman = 0; // module RubyWatchman

void Init_ext() {
  mRubyWatchman = rb_define_module("RubyWatchman");
  rb_define_singleton_method(mRubyWatchman, "load", RubyWatchman_load, 1);
  rb_define_singleton_method(mRubyWatchman, "dump", RubyWatchman_dump, 1);
  rb_define_singleton_method(mRubyWatchman, "query", RubyWatchman_query, 2);
}
