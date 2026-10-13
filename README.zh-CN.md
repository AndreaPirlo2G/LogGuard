# LogGuard

**给 C++ 应用的崩溃安全黑匣子。**

程序崩溃（段错误、`abort()`、`kill -9`、看门狗强杀）之后，普通日志随进程一起没了——真正需要的崩溃前 60 秒恰恰丢失。

LogGuard 用一个固定大小的内存映射文件做环形缓冲，常驻磁盘。热路径只有一次原子取号加一次定长 memcpy：无分配、无锁、无系统调用。异常退出后再次启动时，崩溃前窗口自动导出为单个自包含报告文件，格式是标准的 **[MCAP](https://mcap.dev/)**（ROS 2 默认 rosbag2 格式），官方工具和 Foxglove Studio 直接可读，无私有格式锁定。

> **状态：v0.1 竖切原型，仅 Linux。** 崩溃恢复链路真实可测；Windows、压缩、触发规则、关联索引、脱敏在路线图上。

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

## 文档

- [架构设计](docs/architecture.md)
- [ring 文件格式](docs/ring-format.md)
- [MCAP 映射规范](docs/mcap-mapping.md)
- [安全与隐私](docs/security.md)
- [路线图](docs/roadmap.md)

MIT 协议。
