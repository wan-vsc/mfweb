# 本机工具链约束与规避方案（P0 实测记录）

本文记录本项目在本机（Windows 11 26200 / Intel i7-14650HX / 24 逻辑核 / 32 GB）上
实际踩到并解决的三个工具链问题。换机器时请先读本文。

---

## 1. Visual Studio 未在 Installer 注册 → 不能用 VS 生成器

**现象**

```
cmake -G "Visual Studio 18 2026" -A x64 -DCMAKE_GENERATOR_INSTANCE=D:/vs2026
  → could not find specified instance of Visual Studio: D:/vs2026
    The directory exists, but the instance is not known to the Visual Studio Installer
```

`vswhere.exe -all -products * -format json` 返回 `[]`。

**原因**：VS2026 被安装到 `D:\vs2026`，但其实例元数据位于 `D:\vs installler 0\_Instances`，
不在 Visual Studio Installer 的默认查询路径下，因此 vswhere / CMake 都发现不了它。

**规避**：不使用 VS 生成器，改为在进程内导入 `vcvars64.bat` 的环境，
再用命令行生成器构建。`D:\vs2026\VC\Auxiliary\Build\vcvars64.bat` 本身工作正常
（实测 `cl.exe` 与 `WindowsSdkDir=C:\Windows Kits\10\` 均正确设置）。

---

## 2. Ninja 在本沙箱下挂起 → 改用 NMake Makefiles

**现象**：`cmake -G Ninja` 卡在 `-- Detecting CXX compiler ABI info`，
`ninja.exe` 进程常驻但 **CPU 占用 0%**，且**没有任何 cl.exe 子进程**。等待 7 分钟无进展。

**原因**：ninja 通过管道捕获每个子进程的 stdout（用于回显编译命令与错误）。
受限模式下程序无法打开命名管道，导致 ninja 起不动编译器。

**规避**：使用 `NMake Makefiles` 生成器 —— nmake 让子进程**继承** stdio，不走管道。

**代价**：nmake 不支持并行编译（无 `/MP` 级别的文件级并行），全量构建较慢；
增量构建不受影响。

---

## 3. 中文路径 + 代码页 936 → LNK1201（本项最关键）

**现象**

```
LINK : fatal error LNK1201: 写入程序数据库"…\cmTC_46419.pdb"时出错；
       请检查是否是磁盘空间不足、路径无效或权限不足
NMAKE : fatal error U1077: … 返回代码"0xffffffff"
CMake Error: The C++ compiler is not able to compile a simple test program.
```

同一次输出里还能看到 `--filter-prefix="娉ㄦ剰: 鍖呭惈鏂囦欢:  "`，
即 MSVC 的中文提示 `注意: 包含文件:  `（GBK，代码页 936）被按 UTF-8 解码后的乱码。

**原因**：本机 ANSI 代码页为 **936(GBK)**，而 CMake 4.x 按 **UTF-8** 读写自己的
响应文件/临时文件。交付文件夹名 `2026-09-19_2027_代码_百万并发Web框架` 含中文，
路径经这一层编码转换后被破坏，link.exe 拿到的 `.pdb` 路径指向不存在的目录 → LNK1201。

**规避**：把传给 CMake 的 `-S` 与 `-B` 换成 **8.3 短路径**（纯 ASCII）：

```
D:\Dsh Work Poll\2026-09-19_2027_代码_百万并发Web框架
  → D:\DSHWOR~1\2026-0~3
```

`_src\msvc-env.ps1` 中的 `Get-ShortPathName` 通过 `kernel32!GetShortPathName` 动态解析，
不作硬编码（短名与创建顺序有关，可能变化）。同时设置 `VSLANG=1033` 让 MSVC 输出英文消息，
减少 GBK/UTF-8 混用面。

**注意**：`GetShortPathName` 要求路径**已存在**，所以构建脚本先创建构建目录再取短名。

**验证短名是否可用**：

```powershell
cmd /c dir /x "D:\Dsh Work Poll"     # 第三列即 8.3 短名
```

若某台机器关闭了 8.3 名称生成（`fsutil 8dot3name`），短名解析会原样返回长路径，
此时唯一解是让**构建路径全程为 ASCII**（例如在 `C:\` 下建目录联接指向本仓库）。

---

## 4. 最终可用组合（P0 验证通过）

| 项 | 值 |
| --- | --- |
| 编译器 | MSVC 19.51.36243（工具集 14.51.36231） |
| Windows SDK | 10.0.26100 |
| 生成器 | `NMake Makefiles` |
| 环境导入 | `_src\msvc-env.ps1`（`vcvars64.bat` + `VSLANG=1033`） |
| 路径处理 | `-S/-B` 使用 8.3 短路径 |
| 构建入口 | `_src\build.ps1` |

**证据**：`_work\smoke\`（P0 冒烟工程，含协程 + Winsock + IOCP + AcceptEx 断言）
构建与 `ctest` 均通过；同一组断言已固化进 `tests\test_toolchain.cpp` 长期回归。
