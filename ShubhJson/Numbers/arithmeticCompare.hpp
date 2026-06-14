// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>
#include <ShubhJson/Polyfills/type_traits.hpp>

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

enum CompareResult {
  COMPARE_RESULT_DIFFER          = 0,
  COMPARE_RESULT_EQUAL           = 1,
  COMPARE_RESULT_LESS            = 2,
  COMPARE_RESULT_GREATER         = 4,
  COMPARE_RESULT_LESS_OR_EQUAL   = COMPARE_RESULT_LESS | COMPARE_RESULT_EQUAL,
  COMPARE_RESULT_GREATER_OR_EQUAL = COMPARE_RESULT_GREATER | COMPARE_RESULT_EQUAL,
};

template <typename A, typename B>
enable_if_t<is_integral<A>::value && is_integral<B>::value, CompareResult>
arithmeticCompare(const A& lhs, const B& rhs) {
  if (lhs < rhs) return COMPARE_RESULT_LESS;
  if (lhs > rhs) return COMPARE_RESULT_GREATER;
  return COMPARE_RESULT_EQUAL;
}

template <typename A, typename B>
enable_if_t<is_floating_point<A>::value || is_floating_point<B>::value,
            CompareResult>
arithmeticCompare(const A& lhs, const B& rhs) {
  if (lhs < static_cast<A>(rhs)) return COMPARE_RESULT_LESS;
  if (lhs > static_cast<A>(rhs)) return COMPARE_RESULT_GREATER;
  return COMPARE_RESULT_EQUAL;
}

ARDUINOJSON_END_PRIVATE_NAMESPACE
