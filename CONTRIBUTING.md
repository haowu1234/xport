# 参与 xport

这是一个以**可验证**为前提的协议实现项目。欢迎 issue 和 PR，但有一条硬性要求：
性能结论必须能复现，正确性结论必须有失败用例支撑。

## 环境

需要一个支持 C++20 的编译器（GCC 11+ / Clang 14+ / MSVC 19.3+）。没有第三方依赖。
画图脚本需要 Python 3.9+，只用标准库。

```bash
./scripts/quickbuild.sh          # 不需要 CMake，直接编译 + 跑测试
# 或
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure
```

## 提交 PR 之前

1. **测试要全绿**：`ctest --test-dir build --output-on-failure`。
2. **新行为要有新用例**，并且这个用例在修改前的代码上必须失败——否则它守不住任何东西。
3. **编译零警告**：CI 用 `-Wall -Wextra -Wpedantic`（MSVC 用 `/W4`）。
4. **改了协议行为**，跑一遍仿真矩阵并在 PR 里贴出前后对比：

   ```bash
   ./build/apps/xport_bench --csv build/bench.csv
   ```

5. **改了性能相关代码**，说明测试机器和命令。回环基准单次运行能飘 ±10%，
   至少跑 5 次取中位数；不要只贴一次结果。

## 代码风格

- 跟着现有代码走：2 空格缩进，列宽 100，类型 `PascalCase`，函数 `PascalCase`，
  变量 `snake_case`，成员变量带尾下划线 `srtt_us_`。
- **注释写"为什么"，不写"做什么"**。现有注释里大量记录的是踩过的坑和取舍，
  这比重述代码有价值得多。
- 热路径（每包、每 ACK 执行的代码）不要引入堆分配。有定长结构可用就用定长。
- 任何可能静默降级的地方都要有计数器（参考 `Stats` 里的 `ack_overflow`、`rx_overflow`）。

## 提交信息

用 Conventional Commits 的前缀，正文说清动机和影响：

```
fix: 应用受限时不增窗（cwnd 验证，RFC 7661）

<为什么要改、实测影响、怎么验证的>
```

常用前缀：`feat` `fix` `perf` `docs` `test` `refactor` `build`。

## 性能数字的规矩

README 和 docs 里的每个数字都标注了测试环境。补充或修改数字时：

- 写清硬件、操作系统、编译器、命令。
- 仿真数字要带种子（固定种子下逐位可复现）。
- 回环数字要给中位数和范围，不要只给最好的一次。
- 不要把不同机器的数字放在同一张表里横向对比。

## 报告问题

- **正确性问题**：给出能复现的参数（仿真场景请带 `--seed`）。
- **性能问题**：给出机器配置、命令、多次运行的结果。
