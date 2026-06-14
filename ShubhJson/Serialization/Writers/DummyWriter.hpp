// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>
#include <stddef.h>
#include <stdint.h>

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

// A writer that discards all output and only counts bytes written.
// Used by measure() to calculate serialized size without allocating a buffer.
class DummyWriter {
 public:
  size_t write(uint8_t) {
    return 1;
  }

  size_t write(const uint8_t*, size_t n) {
    return n;
  }
};

ARDUINOJSON_END_PRIVATE_NAMESPACE
