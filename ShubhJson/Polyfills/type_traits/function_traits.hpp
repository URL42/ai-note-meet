// ArduinoJson - https://arduinojson.org
// Copyright © 2014-2026, Benoit BLANCHON
// MIT License

#pragma once

#include <ShubhJson/Namespace.hpp>

ARDUINOJSON_BEGIN_PRIVATE_NAMESPACE

template <typename T>
struct function_traits : function_traits<decltype(&T::operator())> {};

template <typename Ret, typename... Args>
struct function_traits<Ret(Args...)> {
  using return_type = Ret;
};

template <typename Ret, typename... Args>
struct function_traits<Ret (*)(Args...)> {
  using return_type = Ret;
};

template <typename C, typename Ret, typename... Args>
struct function_traits<Ret (C::*)(Args...)> {
  using return_type = Ret;
};

template <typename C, typename Ret, typename... Args>
struct function_traits<Ret (C::*)(Args...) const> {
  using return_type = Ret;
};

ARDUINOJSON_END_PRIVATE_NAMESPACE
