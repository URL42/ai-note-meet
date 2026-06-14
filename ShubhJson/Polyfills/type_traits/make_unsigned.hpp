// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

template <typename T> struct make_unsigned {};

template <> struct make_unsigned<char>               { typedef unsigned char      type; };
template <> struct make_unsigned<signed char>        { typedef unsigned char      type; };
template <> struct make_unsigned<unsigned char>      { typedef unsigned char      type; };
template <> struct make_unsigned<short>              { typedef unsigned short     type; };
template <> struct make_unsigned<unsigned short>     { typedef unsigned short     type; };
template <> struct make_unsigned<int>                { typedef unsigned int       type; };
template <> struct make_unsigned<unsigned int>       { typedef unsigned int       type; };
template <> struct make_unsigned<long>               { typedef unsigned long      type; };
template <> struct make_unsigned<unsigned long>      { typedef unsigned long      type; };
template <> struct make_unsigned<long long>          { typedef unsigned long long type; };
template <> struct make_unsigned<unsigned long long> { typedef unsigned long long type; };

ARDUINOJSON_END_PRIVATE_NAMESPACE
