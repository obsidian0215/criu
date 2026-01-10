# 恢复路径优化与细粒度计时 — 当前工作说明 🎯

## 概要

本文件简要说明最近在恢复（restore）路径上所做的工作：
- **目的**：给出基于源码的精确恢复耗时归因（读 / 比对 / 复制 / 传输），并实现若干低风险、可配置的内存路径优化以降低恢复延迟。
- **分支**：所有改动已提交到 `dirtylog` 分支（见提交日志）。

---

## 已实现（主要改动） ✅

- **细粒度计时（microseconds）**
  - 新增 timing 枚举与统计字段：
    - TIME_READ_PAGES、TIME_COMPARE_PAGES、TIME_COPY_PAGES、TIME_PAGE_XFER
  - 位置：`include/stats.h`（枚举）、`images/stats.proto`（proto 字段）、`criu/stats.c`（编码与显示）。
  - 在 `--display-stats` 时会打印这些细粒度时间字段。

- **读页计时**
  - 在页读取路径加入时间统计（`pread` / `preadv` / `read`）：
    - 文件：`criu/pagemap.c`（`read_local_page`、`process_async_reads`、`maybe_read_page_img_streamer`）。

- **批量读 + 批量比对/按页复制（受控）**
  - 在恢复私有（COW）继承页时实现：一次性读取 `chunk` 页到临时缓冲，先对整块做 `memcmp`，若不相同则对每页单独比较并 `memcpy` 差异页。
  - 控制参数：`--restore-bulk-pages N`（默认 64 页）。
  - 文件：`criu/mem.c`（`restore_priv_vma_content` 的实现和计时）。
  - 对比与复制分别计时（TIME_COMPARE_PAGES/TIME_COPY_PAGES）。

- **批量 madvise（减少 syscalls）**
  - 当连续要 drop 的页数量达到 `--madvise-batch-min-pages` 且 `--batch-madvise` 启用时，用一次 `madvise(addr, len)` 批量释放；否则回退到逐页 `madvise`。
  - 文件：`criu/mem.c`。

- **页面传输计时**
  - 在页面写入网络或本地镜像处加入计时（TLS / splice）：
    - 文件：`criu/page-xfer.c`（`write_pages_to_server` / `write_pages_loc`）。

- **CLI / 帮助 / config 支持**
  - 新增并解析以下选项：
    - `--restore-bulk-pages N`（默认 64）
    - `--pagemap-max-bunch-size N`（默认 256）
    - `--batch-madvise / --no-batch-madvise`（默认启用）
    - `--madvise-batch-min-pages N`（默认 1）
  - 帮助文本已更新：`criu/crtools.c`。
  - 配置文件（如 `/etc/criu/criu.conf` 或 `~/.criu.conf`）支持同样的行级设置。

---

## 如何启用与验证（快速上手） 🧪

1. 构建（在 Linux 上）

   - 必要依赖（示例，Ubuntu/Debian）：

```bash
sudo apt update
sudo apt install build-essential pkg-config protobuf-c-compiler libprotobuf-c-dev libnl-3-dev libssl-dev make gcc g++
```

   - 构建：

```bash
make -j$(nproc)
```

2. 运行一个简单的 smoke 测试

```bash
# 假设已存在 dump 在 /tmp/dump
criu restore -D /tmp/dump --display-stats
```

3. 对比不同参数的效果（示例）

```bash
# 基线：不启用批量比较
criu restore -D /tmp/dump --display-stats --restore-bulk-pages 0

# 启用批量比较
criu restore -D /tmp/dump --display-stats --restore-bulk-pages 128
```

观察 `--display-stats` 输出中的：
- Pages read time（读页耗时）
- Pages compare time（比对耗时）
- Pages copy time（复制耗时）
- Page transfer time（页面传输耗时）

4. 建议测量方法
- 使用 `time` 或 `perf stat` 测总时延。
- 使用 `strace -c -f -o strace.out criu restore ...` 统计 syscall 次数（观察 pread/preadv、madvise 的数量是否下降）。

---

## 注意事项与风险 ⚠️

- 批量读会额外占用临时缓冲（`chunk * PAGE_SIZE`），请勿把 chunk 设置得过大以免内存峰值问题。
- 在“最差情况”（几乎所有页都不同）下，批量比对的开销可能不会比逐页更小；建议先用保守默认（64页）并做 A/B 测试。
- page-server / 远程场景下应根据网络 / 服务器吞吐量调优 `restore-bulk-pages` 与 `pagemap-max-bunch-size`。
- 目前实现尽量保持低风险：所有特性均通过可配置选项控制，默认值保守。

---

## 未完成 / 后续工作（建议） 🔧

- 在 CI/Linux 上跑系统性基准与参数扫描，确定稳健默认值。
- 为比对/复制路径增加更细的单元测试 / 回归测试与基准测试。
- 探索更激进的优化：io_uring、并行应用页、指纹化跳过比对等（需额外评估风险与收益）。

---

## 参考与提交信息

- 相关文件：`criu/mem.c`, `criu/pagemap.c`, `criu/page-xfer.c`, `criu/stats.c`, `include/stats.h`, `images/stats.proto`, `criu/config.c`, `criu/crtools.c`, `Documentation/perf-tuning.txt`。
- 提交到分支：`dirtylog`（请查看最近的提交信息以获取完整变更描述）。

---

如需我：
- 我可以生成一组在 Linux 上运行的 benchmark 脚本（构建、dump/restore 流程、参数 sweep、收集并生成对比报告）；或
- 我可以把当前实现抽出可测试的单元测试/基准脚本并提交到仓库。

请告诉我你想我下一步做哪项。✍️
