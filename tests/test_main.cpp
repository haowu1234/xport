#include "test_framework.h"

int main() {
  std::size_t failed = 0;
  for (auto& c : xtest::Cases()) {
    const int before = xtest::Failures();
    c.fn();
    const bool ok = (xtest::Failures() == before);
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", c.name);
    if (!ok) ++failed;
  }
  std::printf("\n%zu passed, %zu failed\n", xtest::Cases().size() - failed, failed);
  return failed == 0 ? 0 : 1;
}
