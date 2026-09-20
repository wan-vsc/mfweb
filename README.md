# mfweb

基于 **C++20 无栈协程**的跨平台异步 Web 服务器框架（个人项目）。

两个 I/O 后端都是自己实现的，业务代码通过平台别名切换，**零 `#ifdef`**：

| 平台 | 后端 | 实现方式 |
| --- | --- | --- |
| Windows | **IOCP** | 完成端口 + `AcceptEx` / `ConnectEx` / `WSARecv` / `WSASend` |
| Linux | **io_uring** | **自研系统调用与 mmap 布局封装，不链接 liburing** |

运行时零第三方依赖（仅 STL + 系统 API）。

---

## 最硬的一条：同一套测试，两个后端都通过

```
Windows (IOCP)    : === 用例 183：通过 183，失败 0，跳过 0 ===
Linux   (io_uring): === 用例 183：通过 183，失败 0，跳过 0 ===
```

**同一份业务代码、同一套 183 个用例，两个完全不同的异步 I/O 后端全绿。**

---

## 实测数据

全部为可复现实测值，方法论与原始输出见 `bench/results/`。

| 维度 | 实测 |
| --- | --- |
| 协程帧开销 | **88.4 字节 / 个**；100 万层 `co_await` 嵌套 0.103 s 不爆栈 |
| 路由匹配 | 前缀树 **1,727 万次/秒**，较线性扫描（221 万次/秒）**提升 7.8 倍** |
| 定时器 | 插入 1,066 万、触发 2,005 万次/秒；句柄取消较哈希索引 **+56.1%** |
| HTTP QPS | Windows/IOCP **59,313**（16 线程 / 2000 并发 / 多客户端）<br>Linux/io_uring **42,275**（原生 wrk，c500） |
| 大文件传输 | Linux **2.05 GB/s**（= 该机裸 loopback TCP 上限）<br>Windows **1.53 GB/s**（裸 TCP 上限 3.67，写侧是热点） |
| 连接容量 | 峰值 **408,518** 条同时在线，**每连接约 3.5 KB** |
| 断点续传 | `wget -c` ✅  `aria2c -c` ✅  6 段 Range 逐字节校验一致，越界返回 416 |

---

## 功能

**已实现**

- **协程库**：`task<T>`、`sync_wait`、`generator<T>`、`sleep_for`、帧分配器（内存池）
- **运行时**：每线程一个事件循环，完成队列共享；红黑树定时器（句柄即节点指针，O(1) 取消）
- **I/O**：IOCP 与 io_uring 双后端，同一套 `io_operation` proactor 接口
- **HTTP/1.1**：`chunked` 请求体解码（RFC 7230 §4.1 状态机）、keep-alive、Range 断点续传、静态文件流式发送
- **WebSocket**：RFC6455（握手向量、掩码、ping/pong、close 握手）
- **路由**：前缀树匹配 + 变参模板 AOP 中间件
- **编译期反射**：聚合类**无宏**反射（基于结构化绑定，不用 reinterpret_cast）、枚举名反射、字段别名
- **JSON**：序列化 / 反序列化，与反射打通
- **异步 HTTP 客户端**：`then` 链式组合、`on_error` 传播；**Qt 集成**（`post_to_qt` 把回调投递回 Qt 主线程）

**未实现 / 已知边界**（如实记录）

- HTTP/2、HTTP/3、TLS/HTTPS、模板引擎、数据库驱动、Windows RIO
- **Windows 大文件写侧仍是热点**：1.53 GB/s vs 裸 TCP 3.67 GB/s，已定位到写占 70%，根因未查明
- 「百万连接」未演示：每连接 3.5 KB × 100 万 ≈ 3.5 GB（仅服务端），加客户端与内核结构需 8~9 GB，测试机内存不足

---

## 构建

需要 CMake ≥ 3.25 与支持 C++20 的编译器（MSVC 19.4+ / GCC 13+）。

**Linux**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/tests/mfweb_tests        # 应输出：用例 183：通过 183，失败 0
```

**Windows**

```powershell
cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\tests\mfweb_tests.exe
```

> 需要 `vcvars64.bat` 的环境。若 CMake 配置失败，多半是没有注册 Visual Studio 安装——
> 用 `vcvars64.bat` + NMake 生成器即可。若路径含中文，建议用 8.3 短路径作为 `-S/-B`。

**Qt 示例**（可选）：装上 Qt6 后 CMake 会自动发现并构建 `qt_then_chain`；
没装则自动跳过，不影响其余目标。

---

## 快速上手

```cpp
#include <mfweb/net/http/server.hpp>
#include <mfweb/runtime/io_context.hpp>

int main() {
    mfweb::runtime::io_context ctx{4};         // 4 个事件循环线程
    mfweb::http::server srv{ctx};

    srv.routes().get("/hello", [](const mfweb::http::request&,
                                  mfweb::router::route_params&,
                                  mfweb::http::response& r) {
        r.status = 200;
        r.set("Content-Type", "text/plain");
        r.body = std::string("hello from mfweb");
    });

    srv.serve_static("/files", "/var/www");    // 静态目录（含 Range 续传）
    if (!srv.listen(8080, "0.0.0.0")) { return 1; }
    srv.run();
}
```

跑示例程序：

```bash
./build/examples/static_server 8080 /var/www 4
# 浏览器打开 http://127.0.0.1:8080/files/某个文件
# 断点续传：curl -D - -r 0-10 http://127.0.0.1:8080/files/某个文件
```

---

## 目录结构

| 目录 | 内容 |
| --- | --- |
| `include/mfweb/coro/` | 协程库：task / sync_wait / generator / 帧分配器 |
| `include/mfweb/io/` | `iocp_engine`、`uring_engine`、`uring_syscall`、平台别名 `native_engine` |
| `include/mfweb/runtime/` | 事件循环、红黑树定时器、`io_context` |
| `include/mfweb/net/` | buffer / socket / connection，`http/`、`websocket/`、`client/` |
| `include/mfweb/router/` | 前缀树路由 + AOP |
| `include/mfweb/reflect/` | 聚合类与枚举反射 |
| `include/mfweb/json/` | JSON |
| `tests/` | 183 个用例（自研测试基座，CTest 驱动） |
| `bench/` | 基准与压测程序 `mfbench`，原始结果在 `bench/results/` |
| `examples/` | `static_server`、`then_chain`、`qt_then_chain` |
| `docs/` | 设计决策记录、缺陷档案（含一份完整的 ASAN use-after-free 报告） |

---

## 一些值得一看的地方

这个项目里我认为最有价值的部分不是性能数字，而是**排查记录**：

- `docs/bugs/asan-connection-timeout.md` —— ASAN 抓到的连接对象 use-after-free
  （连接被超时定时器持有），完整报告与修法
- `bench/results/http-qps-linux.md` —— 三个只在换平台/换客户端后才暴露的缺陷：
  响应缺 `Content-Length`、Linux `SIGPIPE` 直接杀进程、发送时把"读到的字节数"当"写出的字节数"
- `bench/results/` 里还记录了**被实测否定的假设**（包括我自己一个错误结论的更正过程）
