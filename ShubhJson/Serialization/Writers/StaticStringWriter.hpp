// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>
#include <stddef.h>
#include <stdint.h>

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

// A writer that writes into a fixed-size char buffer.
// Used by serialize(doc, buffer, size).
class StaticStringWriter {
 public:
  StaticStringWriter(char* buf, size_t size)
      : begin_(buf), end_(buf + size), cursor_(buf) {}

  size_t write(uint8_t c) {
    if (cursor_ >= end_) return 0;
    *cursor_++ = static_cast<char>(c);
    return 1;
  }

  size_t write(const uint8_t* s, size_t n) {
    size_t available = static_cast<size_t>(end_ - cursor_);
    if (n > available) n = available;
    for (size_t i = 0; i < n; i++) cursor_[i] = static_cast<char>(s[i]);
    cursor_ += n;
    return n;
  }

  // Null-terminate without counting the terminator in the returned size.
  void commit() {
    if (cursor_ < end_) *cursor_ = '\0';
    else if (begin_ < end_) *(end_ - 1) = '\0';
  }

 private:
  char* begin_;
  char* end_;
  char* cursor_;
};

ARDUINOJSON_END_PRIVATE_NAMESPACE
