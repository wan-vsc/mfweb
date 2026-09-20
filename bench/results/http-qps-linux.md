# Linux / io_uring 实测结果（P10）

> 本文件是 **P10 的原始证据**：同一份代码在 Windows(IOCP) 与 Linux(io_uring)
> 两个后端上的实测结果。汇总解读见交付根的 `03_性能实测报告.md` §3.5。

## 1. 环境

| 项 | 值 |
| --- | --- |
| 系统 | Ubuntu 24.04.5 Server（cloud image，非安装器安装） |
| 内核 | **6.8.0-139-generic**（io_uring 可用） |
| CPU / 内存 | 4 vCPU / 8 GB（VMware Workstation 16.2.1，NAT） |
| 编译器 | g++ 13.3.0，C++20，`-DCMAKE_BUILD_TYPE=Release` |
| 压测端 | **wrk 4.1.0**（第三方，非自研） |
| 负载 | 128 字节响应 / loopback / keep-alive |

## 2. 测试用例：双平台 183/183

```
Windows (IOCP)    : === 用例 183：通过 183，失败 0，跳过 0 ===
Linux   (io_uring): === 用例 183：通过 183，失败 0，跳过 0 ===
```

## 3. 原生 wrk 吞吐（2 个事件循环线程，单次运行贯穿各并发档位）

| 并发 | QPS | 延迟均值 | 服务端 |
| --- | --- | --- | --- |
| 10 | 23,863 | 0.50 ms | 存活 |
| 100 | 36,721 | 2.91 ms | 存活 |
| **500** | **42,275** | 16.1 ms | 存活（本轮最好） |
| 1,000 | 31,713 | 49.8 ms | 存活 |
| 2,000 | 28,004 | 54.0 ms | 存活 |
| 4,000 | 25,930 | 85.7 ms | 存活 |
| **8,000** | **27,502** | 190 ms | 存活 |

单线程服务端另测：c10 → 30,578 QPS；c100 → 56,774 QPS。

**不要与 Windows 那组直接比**：Windows 是 24 逻辑核宿主 + 16 线程服务端；
这里是 4 vCPU 虚拟机、服务端只给 1~2 个线程。可比的是"同一份代码两个后端都跑得起来"。

## 4. 断点续传互操作（Range）

服务端：`examples/static_server`，5 MiB 随机文件，挂载在 `/files`。

### 4.1 Range 正确性逐段校验（curl 取段 vs dd 取同段）

| Range | 结果 |
| --- | --- |
| `0-1023` | OK |
| `100-200` | OK |
| `1048576-1049599` | OK |
| `2097152-2098175` | OK |
| `3000000-3000511` | OK |
| `5241856-5242879` | OK |

**6/6 逐字节一致。** 边界形式 `bytes=0-`（整文件）与 `bytes=-1024`（末尾 1024）均返回 206 且长度正确。
越界请求 `bytes=99999999-` 返回 **416 Range Not Satisfiable**。

### 4.2 真实客户端续传

| 客户端 | 结果 |
| --- | --- |
| `wget -c`（断在 1 MiB） | **✅ 通过**，续传到 5,242,880 字节且 md5 与原文一致 |
| `aria2c -c -x1 -s1`（断在 2 MiB） | **✅ 通过**，md5 一致 |
| `aria2c -c`（默认 -x16） | ❌ 失败：大小对、内容错 |

**关于 aria2c 默认多连接失败**：这不是服务端的问题。
aria2 的 `--continue` 语义是"续传由**浏览器或其它程序顺序下载**留下的文件"；
没有 aria2 自己的 `.aria2` 控制文件时，它无法得知已有哪些分段，
多连接模式下会按自己的分段假设重下，结果自然对不上。
**服务端侧已被 §4.1 证明对任意 Range 都返回逐字节正确的数据**，
且单连接续传 `-x1 -s1` 完全通过。

## 5. 本阶段挖出的两个"只在 Linux 暴露"的真实缺陷

### 5.1 响应缺少 Content-Length（协议违规）

`write_response` 从不写 Content-Length；响应既无长度、又非 chunked、也非 101 升级，
**curl / wrk 无从判断 body 结束位置，直接挂死到超时**。
长期未发现的原因是自研压测端 `mfbench http-load` 只统计收到多少字节、不校验完整性。
已改为框架层自动补齐（已有则不重复加，101 不加）。

### 5.2 SIGPIPE 杀死进程

并发 c≈500 以上时服务端"凭空消失"：无异常、无日志、退出码正常。
gdb 栈：

```
Thread 2 received signal SIGPIPE, Broken pipe
#0 syscall()
#1 mfweb::io::uring::sys_enter
#2 enter()
#3 mfweb::io::uring_engine::post_write      <- 写一个已被对端关闭的 socket
#4 mfweb::coro::write_awaiter::submit
#5 mfweb::coro::io_awaiter::await_suspend
#6 mfweb::http::server::write_response
```

Linux 下向已被对端关闭的 socket 写入会产生 SIGPIPE，**默认动作是直接杀死进程**。
Windows 没有这个信号（write 只返回错误码），所以 **IOCP 后端永远看不到这个问题**。
已改为后端初始化时进程级忽略 SIGPIPE（nginx / libuv / Asio 同做法）。
修后 8000 并发不再崩溃。

## 6. 测量方法上的两次自我更正

1. **自研压测端掩盖了协议缺陷** —— 见 §5.1。
2. **测试脚本误判服务端存活** —— 早期脚本用 `pkill/pgrep -f 'mfbench http-server'`，
   而 **tmux 会话的命令行里也含这个字符串**，导致误杀与误判"服务端死亡"。
   改为 `setsid` 起进程 + 精确匹配后才得到可信结论。

> 两次都是**测量工具的缺陷伪装成被测对象的缺陷**。
