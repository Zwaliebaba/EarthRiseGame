#pragma once

#include <exception>
#include <format>
#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#if defined(_DEBUG)
#include <crtdbg.h>
#define NEW new ( _NORMAL_BLOCK , __FILE__ , __LINE__ )
#else
#define NEW new
#endif

namespace Neuron
{
  template <class... Types>
  void DebugTrace(const std::string_view _fmt, Types&&... _args)
  {
#ifdef _DEBUG
    const std::string message = std::vformat(_fmt, std::make_format_args(_args...));
    OutputDebugStringA(message.c_str());
    OutputDebugStringA("\n");
#else
    (void)_fmt;
    ((void)_args, ...);
#endif
  }

  template <class... Types>
  void DebugTrace(const std::wstring_view _fmt, Types&&... _args)
  {
#ifdef _DEBUG
    const std::wstring message = std::vformat(_fmt, std::make_wformat_args(_args...));
    OutputDebugStringW(message.c_str());
    OutputDebugStringW(L"\n");
#else
    (void)_fmt;
    ((void)_args, ...);
#endif
  }

  template <class... Types>
  [[noreturn]] void Fatal(const std::string_view _fmt, Types&&... _args)
  {
#ifdef _DEBUG
    const std::string message = std::vformat(_fmt, std::make_format_args(_args...));
    OutputDebugStringA(message.c_str());
    OutputDebugStringA("\n");
#else
    (void)_fmt;
    ((void)_args, ...);
#endif
    __debugbreak();
    throw std::exception("Fatal Error");
  }

  template <class... Types>
  [[noreturn]] void Fatal(const std::wstring_view _fmt, Types&&... _args)
  {
#ifdef _DEBUG
    const std::wstring message = std::vformat(_fmt, std::make_wformat_args(_args...));
    OutputDebugStringW(message.c_str());
    OutputDebugStringW(L"\n");
#else
    (void)_fmt;
    ((void)_args, ...);
#endif
    __debugbreak();
    throw std::exception("Fatal Error");
  }
}

#define ASSERT(expression)                   (void)((!!(expression)) || (::Neuron::Fatal(_CRT_WIDE("Assert Failure")), 0))
#define ASSERT_TEXT(expression, ...)         (void)((!!(expression)) || (::Neuron::Fatal(__VA_ARGS__), 0))

#ifdef _DEBUG
#define DEBUG_ASSERT(expression)             ASSERT(expression)
#define DEBUG_ASSERT_TEXT(expression, ...)   ASSERT_TEXT(expression, __VA_ARGS__)
#define DEBUG_WARNING(expression, ...)       (void)((!(expression)) || (::Neuron::DebugTrace(__VA_ARGS__), 0))
#else
#define DEBUG_ASSERT(expression)             ((void)0)
#define DEBUG_ASSERT_TEXT(expression, ...)   ((void)0)
#define DEBUG_WARNING(expression, ...)       ((void)0)
#endif

// Logging shim. All EarthRise logging flows through Neuron::DebugTrace; in
// Release builds the trace calls collapse to no-ops (per project decision).
#define EARTHRISE_LOG_INFO(...)  ::Neuron::DebugTrace(__VA_ARGS__)
#define EARTHRISE_LOG_WARN(...)  ::Neuron::DebugTrace(__VA_ARGS__)
#define EARTHRISE_LOG_ERROR(...) ::Neuron::DebugTrace(__VA_ARGS__)
#define EARTHRISE_LOG_DEBUG(...) ::Neuron::DebugTrace(__VA_ARGS__)
