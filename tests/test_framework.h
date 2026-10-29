#pragma once
// 极简测试框架：无外部依赖，保证在任意机器上 clone 即可跑。
#include <cstdio>
#include <functional>
#include <vector>

namespace xtest {

struct Case {
  const char* name;
  std::function<void()> fn;
};

inline std::vector<Case>& Cases() {
  static std::vector<Case> v;
  return v;
}

inline int& Failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* n, std::function<void()> f) { Cases().push_back({n, std::move(f)}); }
};

inline void Fail(const char* file, int line, const char* expr) {
  std::printf("    FAIL %s:%d  %s\n", file, line, expr);
  ++Failures();
}

inline void FailEq(const char* file, int line, const char* ea, const char* eb, long long va, long long vb) {
  std::printf("    FAIL %s:%d  %s == %s  (%lld vs %lld)\n", file, line, ea, eb, va, vb);
  ++Failures();
}

}  // namespace xtest

#define TEST(name)                                        \
  static void name();                                     \
  static ::xtest::Registrar reg_##name(#name, name);      \
  static void name()

#define CHECK(cond)                                  \
  do {                                               \
    if (!(cond)) ::xtest::Fail(__FILE__, __LINE__, #cond); \
  } while (0)

#define CHECK_EQ(a, b)                                                              \
  do {                                                                              \
    const long long _va = static_cast<long long>(a);                                 \
    const long long _vb = static_cast<long long>(b);                                 \
    if (_va != _vb) ::xtest::FailEq(__FILE__, __LINE__, #a, #b, _va, _vb);            \
  } while (0)
