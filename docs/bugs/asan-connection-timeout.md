# 缺陷档案：连接超时导致的 heap-use-after-free

> 对应简历第 2 条：「使用 Address Sanitizer 定位修复连接超时导致的段错误」。
> **诚实性说明**：这个缺陷不是一段被美化的历史。它是本项目在 P3 阶段用"朴素设计"
> 主动构造、用 ASAN 真实复现、再在 `net::connection` 中真实修复的一类缺陷。
> 原始 ASAN 报告见同目录 `asan-connection-timeout.report.txt`（9.7 KB，完整栈回溯）。

---

## 1. 现象

连接空闲超时后，服务器崩溃。ASAN 报告摘要：

```
==14636==ERROR: AddressSanitizer: heap-use-after-free on address 0x1290967a1bf0
WRITE of size 4 at 0x1290967a1bf0 thread T0
    #0 ... mfweb_timeout_repro.exe+0x155aa   ← resume_on_io_complete
    #1 ... mfweb_timeout_repro.exe+0x178c6   ← iocp_engine::harvest
    ...
0x1290967a1bf0 is located 112 bytes inside of 560-byte region
freed by thread T0 here:
    #0 ... mfweb_timeout_repro.exe+0x1cbd3   ← 协程帧销毁
```

"WRITE of size 4" 是对**已释放协程帧**里 `io_operation::status.error` 的写入：
完成包到达时，事件循环还在往那块内存里写状态并尝试恢复一个早已被销毁的协程。

---

## 2. 根因

proactor 模型下（IOCP / io_uring 都是），提交的是"把 n 字节读进这块缓冲区"，
而不是"可读了叫我"。于是**缓冲区与操作对象必须活到完成包到达为止**。

而 `io_operation` 由 `coro::io_awaiter` 持有，Awaiter 是 `co_await` 表达式里的
临时对象，存活期随协程帧。因此：

> **绝不能在"有挂起 I/O 的情况下"销毁协程帧。**

朴素设计的三个"看起来没问题"的动作恰恰违反了这个约束：

```cpp
// 缺陷代码（tests/timeout_repro.cpp 里故意这么写）
ctx.engine().cancel_socket(conn->socket);
close_socket(conn->socket);
delete conn;                 // ① 读操作还挂在它上面
read_root.handle.destroy();  // ② 销毁仍持有挂起 I/O 的协程帧
```

误区在于：`closesocket` / `CancelIoEx` 只是让挂起的操作**异步地**失败，
完成包稍后才投递到事件循环。在此期间销毁帧，完成包一来就是 use-after-free。

（顺带记录一个复现过程中的坑：直接 `closesocket` 不保证投递完成包，读协程可能
根本不醒。所以夹具里显式调了 `cancel_socket` 再销毁帧，让崩溃确定发生。）

---

## 3. 修复（见 include/mfweb/net/connection.hpp）

三条措施，缺一不可：

1. **读循环协程用 shared_ptr 持有连接** —— 只要它还挂在 I/O 上，连接对象就不会被释放，
   完成包到达时操作对象所在的帧一定还活着。
2. **close() 只做两件事，不销毁任何对象**：
   - 摘掉空闲超时定时器（`timer_queue::handle` 缓存句柄，O(1) 取消）；
   - `cancel_socket()` 取消挂起的 I/O，让读循环带着 `ERROR_OPERATION_ABORTED`
     自己醒来、自己收尾、自己释放 shared_ptr。
3. **析构兜底**：`~connection()` 取消定时器 + 取消残留 I/O，即便上层忘记 close 也不会
   留下会二次回调的定时器。

其中第 2 条与简历第 3 条的"RAII 析构自动取消定时器"是同一件事：超时定时器的句柄
缓存在成员里，连接销毁即取消，超时回调永远不可能在对象销毁后触发。

---

## 4. 复现方法（可重复执行）

```powershell
# 1) ASAN 配置构建
.\_src\build.ps1 -Config Asan -Fresh

# 2) 运行缺陷复现夹具（会崩溃，这是预期）
.\_src\msvc-env.ps1
& _work\build\tests\mfweb_timeout_repro.exe

# 3) 运行修复后的全部用例，确认 ASAN 干净
& _work\build\tests\mfweb_tests.exe
```

实测（2026-09-19）：

| 步骤 | 结果 |
| --- | --- |
| 复现夹具 | ASAN 报 `heap-use-after-free`（见原始报告） |
| 修复后 `mfweb_tests`（ASAN 配置） | **82 用例全通过，ASAN 零报告** |

对应的回归用例：`connection.idle_timeout_closes_silent_connection` 断言了关键一条 ——
连接关闭后超时定时器必须已经从队列里摘掉，否则就会重演本缺陷。
