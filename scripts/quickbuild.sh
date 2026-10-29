#!/usr/bin/env bash
# 一键编译 + 跑测试。自动挑选可用的 C++20 编译器。
#   ./scripts/quickbuild.sh          # 编译全部并跑测试
#   ./scripts/quickbuild.sh --run    # 编译后再跑一遍 demo 与轨迹探针
set -euo pipefail
cd "$(dirname "$0")/.."

CXX="${CXX:-}"
if [ -z "$CXX" ]; then
  if command -v g++ >/dev/null 2>&1; then
    CXX=g++
  elif command -v clang++ >/dev/null 2>&1; then
    CXX=clang++
  elif python -c "import ziglang" >/dev/null 2>&1; then
    CXX="python -m ziglang c++"
  else
    echo "找不到 C++20 编译器。请安装 g++ / clang++ / MSVC，或执行：pip install ziglang"
    exit 1
  fi
fi

echo "编译器: $CXX"
mkdir -p build/traces

SRC="src/crc32c.cpp src/cc.cpp src/packet.cpp src/sim.cpp src/transport.cpp src/udp.cpp"

# Windows 上 socket API 在 ws2_32 里，必须显式链接；POSIX 上不需要额外库
EXTRA=""
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) EXTRA="-lws2_32" ;;
esac
FLAGS="-std=c++20 -O2 -Iinclude"

# shellcheck disable=SC2086
$CXX $FLAGS $SRC tests/test_main.cpp tests/test_wire.cpp tests/test_cc.cpp tests/test_transport.cpp -o build/xport_tests $EXTRA
# shellcheck disable=SC2086
$CXX $FLAGS $SRC apps/xport_demo.cpp -o build/xport_demo $EXTRA
# shellcheck disable=SC2086
$CXX $FLAGS $SRC apps/xport_bench.cpp -o build/xport_bench $EXTRA
# shellcheck disable=SC2086
$CXX $FLAGS $SRC apps/xport_ccprobe.cpp -o build/xport_ccprobe $EXTRA
$CXX $FLAGS $SRC apps/xport_cpubench.cpp -o build/xport_cpubench $EXTRA
$CXX $FLAGS $SRC apps/xport_udp_bench.cpp -o build/xport_udp_bench $EXTRA

echo
./build/xport_tests

if [ "${1:-}" = "--run" ]; then
  echo
  ./build/xport_demo --mb 8 --loss 1
  echo
  ./build/xport_ccprobe --scenario cubic --out build/traces/cubic.csv
  ./build/xport_cpubench
fi
