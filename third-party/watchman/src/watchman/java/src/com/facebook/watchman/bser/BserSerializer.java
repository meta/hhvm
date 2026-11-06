/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

package com.facebook.watchman.bser;

// CHECKSTYLE.OFF: AvoidStarImport
import static com.facebook.watchman.bser.BserConstants.*;

import com.google.common.io.BaseEncoding;

import java.io.OutputStream;
import java.io.IOException;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CharsetEncoder;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.channels.Channels;
import java.nio.channels.WritableByteChannel;

import java.util.List;
import java.util.Map;

/**
 * Encoder for the BSER binary JSON format used by the Watchman service:
 *
 * https://meta.github.io/watchman/docs/bser.html
 */
public class BserSerializer {
  private static final int INITIAL_BUFFER_SIZE = 8192;
  private static final byte[] EMPTY_HEADER = BaseEncoding.base16().decode("00010500000000");

  private enum BserIntegralEncodedSize {
    INT8(1),
    INT16(2),
    INT32(4),
    INT64(8);

    public final int size;

    private BserIntegralEncodedSize(int size) {
      this.size = size;
    }
  }

  private final CharsetEncoder utf8Encoder;

  public BserSerializer() {
    this.utf8Encoder = StandardCharsets.UTF_8
        .newEncoder()
        .onMalformedInput(CodingErrorAction.REPORT);
  }

  /**
   * Serializes an object using BSER encoding to the stream.
   */
  public void serializeToStream(Object value, OutputStream outputStream) throws IOException {
    ByteBuffer buffer = ByteBuffer
        .allocate(INITIAL_BUFFER_SIZE)
        .order(ByteOrder.nativeOrder());

    buffer = serializeToBuffer(value, buffer);
    buffer.flip();

    WritableByteChannel c = Channels.newChannel(outputStream);
    try {
      c.write(buffer);
    } finally {
      if (c != null) {
        c.close();
      }
    }
  }

  /**
   * Serializes an object using BSER encoding. If possible, writes the object
   * to the provided byte buffer and returns it. If the buffer is not big
   * enough to hold the object, returns a new buffer.
   *
   * After returning, buffer.position() is advanced past the last encoded byte.
   */
  public ByteBuffer serializeToBuffer(Object value, ByteBuffer buffer) throws IOException {
    buffer.put(EMPTY_HEADER);
    buffer = appendRecursive(buffer, value, utf8Encoder);

    int encodedLength = buffer.position() - EMPTY_HEADER.length;

    // Overwrite the 32-bit length field at position 3 with the actual length of the object.
    buffer.putInt(3, encodedLength);

    return buffer;
  }

  @SuppressWarnings("unchecked")
  private static ByteBuffer appendRecursive(
      ByteBuffer buffer,
      Object value,
      CharsetEncoder utf8Encoder) throws IOException {
    if (value instanceof Boolean) {
      buffer = increaseBufferCapacityIfNeeded(buffer, 1);
      buffer.put(((Boolean) value) ? BSER_TRUE : BSER_FALSE);
    } else if (value == null) {
      buffer = increaseBufferCapacityIfNeeded(buffer, 1);
      buffer.put(BSER_NULL);
    } else if (value instanceof String) {
      buffer = appendString(buffer, (String) value, utf8Encoder);
    } else if (value instanceof Double || value instanceof Float) {
      buffer = increaseBufferCapacityIfNeeded(buffer, 9);
      buffer.put(BSER_REAL);
      buffer.putDouble((Double) value);
    } else if (value instanceof Long) {
      buffer = appendLong(buffer, (Long) value);
    } else if (value instanceof Integer) {
      buffer = appendLong(buffer, (Integer) value);
    } else if (value instanceof Short) {
      buffer = appendLong(buffer, (Short) value);
    } else if (value instanceof Byte) {
      buffer = appendLong(buffer, (Byte) value);
    } else if (value instanceof Map<?, ?>) {
      Map<Object, Object> map = (Map<Object, Object>) value;
      int mapLen = map.size();
      BserIntegralEncodedSize encodedSize = getEncodedSize(mapLen);
      buffer = increaseBufferCapacityIfNeeded(buffer, 2 + encodedSize.size);
      buffer.put(BSER_OBJECT);
      buffer = appendLongWithSize(buffer, mapLen, encodedSize);
      for (Map.Entry<Object, Object> entry : map.entrySet()) {
        if (!(entry.getKey() instanceof String)) {
          throw new IOException(
              String.format(
                  "Unrecognized map key type %s, expected string",
                  entry.getKey().getClass()));
        }
        buffer = appendString(buffer, (String) entry.getKey(), utf8Encoder);
        buffer = appendRecursive(buffer, entry.getValue(), utf8Encoder);
      }
    } else if (value instanceof List<?>) {
      List<Object> list = (List<Object>) value;
      int listLen = list.size();
      BserIntegralEncodedSize encodedSize = getEncodedSize(listLen);
      buffer = increaseBufferCapacityIfNeeded(buffer, 2 + encodedSize.size);
      buffer.put(BSER_ARRAY);
      buffer = appendLongWithSize(buffer, listLen, encodedSize);
      for (Object obj : list) {
        buffer = appendRecursive(buffer, obj, utf8Encoder);
      }
    } else {
      throw new RuntimeException("Cannot encode object: " + value);
    }

    return buffer;
  }

  private static ByteBuffer appendString(
      ByteBuffer buffer,
      String value,
      CharsetEncoder utf8Encoder) throws CharacterCodingException {
    CharBuffer valueBuffer = CharBuffer.wrap(value);
    ByteBuffer utf8String = utf8Encoder.encode(valueBuffer);
    int utf8StringLenBytes = utf8String.remaining();
    BserIntegralEncodedSize utf8StringLenSize = getEncodedSize(utf8StringLenBytes);
    buffer = increaseBufferCapacityIfNeeded(
        buffer,
        2 + utf8StringLenSize.size + utf8StringLenBytes);
    buffer.put(BSER_STRING);
    buffer = appendLongWithSize(buffer, utf8StringLenBytes, utf8StringLenSize);
    buffer.put(utf8String);
    return buffer;
  }

  private static ByteBuffer appendLong(ByteBuffer buffer, long value) {
    BserIntegralEncodedSize encodedSize = getEncodedSize(value);
    buffer = increaseBufferCapacityIfNeeded(buffer, 1 + encodedSize.size);
    return appendLongWithSize(buffer, value, encodedSize);
  }

  private static BserIntegralEncodedSize getEncodedSize(long value) {
    if (value >= -0x80 && value <= 0x7F) {
      return BserIntegralEncodedSize.INT8;
    } else if (value >= -0x8000 && value <= 0x7FFF) {
      return BserIntegralEncodedSize.INT16;
    } else if (value >= -0x80000000 && value <= 0x7FFFFFFF) {
      return BserIntegralEncodedSize.INT32;
    } else if (value >= -0x8000000000000000L && value <= 0x7FFFFFFFFFFFFFFFL) {
      return BserIntegralEncodedSize.INT64;
    } else {
      // We shouldn't be able to reach here.
      throw new RuntimeException("Unhandled long value: " + value);
    }
  }

  private static ByteBuffer appendLongWithSize(
      ByteBuffer buffer,
      long value,
      BserIntegralEncodedSize encodedSize) {
    // We assume we've already increased the size of the buffer to hold
    // the encoded size.
    switch (encodedSize) {
      case INT8:
        buffer.put(BSER_INT8);
        buffer.put((byte) value);
        break;
      case INT16:
        buffer.put(BSER_INT16);
        buffer.putShort((short) value);
        break;
      case INT32:
        buffer.put(BSER_INT32);
        buffer.putInt((int) value);
        break;
      case INT64:
        buffer.put(BSER_INT64);
        buffer.putLong(value);
        break;
    }
    return buffer;
  }

  private static ByteBuffer increaseBufferCapacityIfNeeded(ByteBuffer buffer, int amount) {
    int remaining = buffer.remaining();
    if (remaining < amount) {
      int capacity = buffer.capacity();
      while (remaining < amount) {
        remaining += capacity;
        capacity *= 2;
      }
      buffer = resizeBufferWithCapacity(buffer, capacity);
    }
    return buffer;
  }

  private static ByteBuffer resizeBufferWithCapacity(ByteBuffer buffer, int capacity) {
    buffer.flip();
    return ByteBuffer
        .allocate(capacity)
        .order(buffer.order())
        .put(buffer);
  }
}
