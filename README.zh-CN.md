# LogGuard

**给 C++ 应用的崩溃安全黑匣子。**

程序崩溃（段错误、`abort()`、`kill -9`、看门狗强杀）之后，普通日志随进程一起没了——真正需要的崩溃前 60 秒恰恰丢失。

LogGuard 用一个固定大小的内存映射文件做环形缓冲，常驻磁盘。热路径只有一次原子取号加一次定长 memcpy：无分配、无锁、无系统调用。异常退出后再次启动时，崩溃前窗口自动导出为单个自包含报告文件，格式是标准的 **[MCAP](https://mcap.dev/)**（ROS 2 默认 rosbag2 格式），官方工具和 Foxglove Studio 直接可读，无私有格式锁定。

> **状态：v0.4，仅 Linux，已实现并有测试。** 崩溃恢复链路、压缩导出、脱敏、触发规则、磁盘留存全部可用；导出包带滚动 chunk 与尾部时间索引，支持按时间窗口 `O(log n)` 定位（`lgtool range`），解析器经确定性变异 fuzz 加固。Windows（VEH）在路线图上。

## 30 秒体验

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

./build/crash_demo /tmp/app.ring 50 crash   # 第 50 次迭代段错误
./build/lgtool recover /tmp/app.ring        # 离线导出崩溃前窗口
./build/lgtool cat /tmp/crash-*.lgp | tail -3
```

`kill -9` 结果相同——ring 是普通文件，由内核负责落盘，不依赖信号处理器。

## 快速接入

```cpp
#include <logguard/logguard.h>

int main() {
    lg_init("/var/lib/myapp/logguard.ring");   // 上次崩溃则自动导出 crash-<ts>.lgp
    LG_INFO("orders", "request_id=%d accepted", id);
}
```

## 故障保证矩阵

| 故障类型 | 保证 |
|---|---|
| SIGSEGV / SIGABRT / SIGBUS / SIGILL / SIGFPE | 已提交槽位全部保留，marker 记录信号与 fault 地址 |
| `kill -9`、OOM、线程内 abort | 全部已提交槽位保留（无需处理器） |
| 断电 / 内核 panic | 保留到最近一次后台 `msync`（约 1 秒窗口） |
| ring 写满 | 最旧记录被覆写，文件永不增长 |
| 磁盘满 / 文件系统损坏 | 尽力而为，错误计入健康状态，绝不向调用方抛异常 |

## 能力一览

- **崩溃黑匣子**：定长 mmap 环形缓冲，无锁 ticket + 两阶段 magic 提交；信号路径只做异步信号安全的定长操作，符号化、快照、压缩全部在导出线程。
- **自包含 MCAP 导出包**：zstd 运行时 `dlopen`，缺库自动降级为不压缩；manifest 与崩溃时刻机器快照（负载、内存、CPU、RSS、线程、FD 数）随包写出。
- **滚动 chunk + 时间索引**：导出包按 2 MiB 阈值滚动 chunk，文件尾部追加 `CHUNK_INDEX` 段（opcode `0x40`，记录每个 chunk 的单调时间跨度与文件偏移）。
- **隐私脱敏闭环**：手机/邮箱/身份证/银行卡/IPv4/密钥等模式在导出时掩码，manifest 带分类命中报告；`lgtool redact` 可对已有包离线脱敏，`lgtool recover --redact` 导出即脱敏。
- **源码级栈位置**：导出期调用 `addr2line` 批量解析，栈帧在模块+偏移、最近符号之外附 `函数 (文件:行号)`，缺 binutils/调试信息时静默降级。
- **按时间窗口 O(log n) 提取**：`lgtool range` 先二分定位、只解压与窗口相交的 chunk；没有索引的旧包自动回退为线性扫描，老读取器遇到未知 opcode 直接跳过，前后双向兼容。
- **磁盘留存**：常驻进程可按「最多包数 / 最大容量 / 最大年龄」自动清理（`lgtool prune` 也可离线执行）。
- **编译期开关**：`-DLG_LOGGING_ENABLED=OFF` 整体擦除日志宏（连同参数构造），`-DLG_COMPILE_MIN_LEVEL=N` 按级别裁剪，零开销发布。
- **变异 fuzz 加固**：确定性 LCG 对合法包做位翻转、长度污染、截断、插入、追加，ASan + UBSan 下验证解析器零越界零崩溃。

## 常用命令

```bash
lgtool recover <ring>                            # 从 ring 离线导出崩溃前窗口
lgtool info <file.lgp>                           # 包结构与校验摘要
lgtool range <file.lgp> --from-ns A --to-ns B    # 单调时间窗口，索引二分定位
lgtool redact <file.lgp> <out.lgp> [--zstd]      # 脱敏重写已有导出包
lgtool manifest <file.lgp>                       # 查看 manifest / 系统快照
lgtool prune <dir> --files N --mb M --age-hours H [--dry-run]
lgtool verify <file.lgp>                         # MCAP 结构校验（fuzz 加固）
```

## 文档

- [架构设计](docs/ARCHITECTURE.md)
- [ring 文件格式](docs/ring-format.md)
- [MCAP 映射规范](docs/mcap-mapping.md)
- [安全与隐私](docs/security.md)
- [路线图](docs/roadmap.md)
- [变更记录](CHANGELOG.md)

MIT 协议。
