// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>
#include <Arduino.h>

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

class ArduinoStringWriter {
 public:
  explicit ArduinoStringWriter(::String& str) : str_(str) {}

  size_t write(uint8_t c) {
    str_ += static_cast<char>(c);
    return 1;
  }

  size_t write(const uint8_t* s, size_t n) {
    size_t written = 0;
    for (size_t i = 0; i < n; i++) {
      str_ += static_cast<char>(s[i]);
      written++;
    }
    return written;
  }

 private:
  ::String& str_;
};

ARDUINOJSON_END_PRIVATE_NAMESPACE
