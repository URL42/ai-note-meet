// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>
#include <string>

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

class StdStringWriter {
 public:
  explicit StdStringWriter(std::string& str) : str_(str) {}

  size_t write(uint8_t c) {
    str_ += static_cast<char>(c);
    return 1;
  }

  size_t write(const uint8_t* s, size_t n) {
    str_.append(reinterpret_cast<const char*>(s), n);
    return n;
  }

 private:
  std::string& str_;
};

ARDUINOJSON_END_PRIVATE_NAMESPACE
