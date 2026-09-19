# mfweb

基于 C++20 协程的跨平台异步 Web 服务器框架（个人项目）。

- Windows 后端：IOCP（AcceptEx / ConnectEx / WSARecv / WSASend / TransmitFile）
- Linux 后端：io_uring（直接封装系统调用，不依赖 liburing）
- 自研：协程库、网络库、编译期反射库、JSON、HTTP 客户端
- 运行时零第三方依赖（仅 STL + 系统 API）

## 目录

| 目录 | 内容 |
| --- | --- |
| `include/mfweb\` | 公共头文件 |
| `src\` | 实现 |
| `tests\` | 单元测试（自研测试基座，CTest 驱动） |
| `bench\` | 基准与压测程序 mfbench |
| `examples\` | 示例程序 |
| `docs\` | 设计决策记录与缺陷档案 |
| `cmake\` | 构建辅助模块 |

## 构建

本机工具链有两点特殊约束（详见 `docs/toolchain-notes.md` 与交付说明）：

1. VS2026 装在 `D:\vs2026` 但未在 Visual Studio Installer 注册 → 不能用 CMake 的
   `Visual Studio 18 2026` 生成器，改用 `vcvars64.bat` + `NMake Makefiles`。
2. 交付文件夹名含中文而本机 ANSI 代码页为 936 → CMake 写出的链接响应文件编码不匹配会导致
   `LNK1201`，故构建脚本把 `-S/-B` 转成 8.3 短路径（纯 ASCII）。

因此请统一使用 `_src\build.ps1`：

```powershell
# 在交付文件夹根目录
.\_src\build.ps1 -Config Release
.\_src\build.ps1 -Config Asan      # AddressSanitizer
```
