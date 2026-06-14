// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>
#include "integral_constant.hpp"

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

template <typename T>
struct is_unsigned : false_type {};

template <> struct is_unsigned<unsigned char>      : true_type {};
template <> struct is_unsigned<unsigned short>     : true_type {};
template <> struct is_unsigned<unsigned int>       : true_type {};
template <> struct is_unsigned<unsigned long>      : true_type {};
template <> struct is_unsigned<unsigned long long> : true_type {};

ARDUINOJSON_END_PRIVATE_NAMESPACE
