#pragma once

// mfweb::io::uring —— io_uring 的**自研系统调用与内存布局封装**。
//
// 设计取舍：**不链接 liburing**。
//   1. liburing 把"提交/收割"包装成了一些便利函数，但隐藏了 SQ/CQ 环的真实内存布局；
//      自己 mmap 一遍之后，什么代价在哪一目了然（这正是本项目的调优方式：
//      P9 里四个"看代码猜瓶颈"的假设全被实测否掉，所以这里宁可少一层抽象）。
//   2. 交付环境只要能编出 <linux/io_uring.h>（内核头文件）即可，不需要额外依赖。
//
// 三个 mmap 区域的偏移由内核通过 io_uring_params 给出，见 mmap_rings()。

#include <cstddef>
#include <cstdint>

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <linux/io_uring.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mfweb::io::uring {

#ifndef _WIN32

// ---- 三个系统调用 ----------------------------------------------------------
// io_uring 的全部能力只有这三个入口，其余都是共享内存约定。

[[nodiscard]] inline long sys_setup(unsigned entries, ::io_uring_params* p) noexcept {
    return ::syscall(__NR_io_uring_setup, entries, p);
}

[[nodiscard]] inline long sys_enter(int fd, unsigned to_submit, unsigned min_complete,
                                    unsigned flags) noexcept {
    return ::syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, nullptr, 0);
}

[[nodiscard]] inline long sys_register(int fd, unsigned opcode, const void* arg,
                                       unsigned nr_args) noexcept {
    return ::syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
}

// ---- mmap 区域偏移（内核 ABI 常量）----------------------------------------
inline constexpr std::size_t kOffSqRing = IORING_OFF_SQ_RING;
inline constexpr std::size_t kOffCqRing = IORING_OFF_CQ_RING;
inline constexpr std::size_t kOffSqes = IORING_OFF_SQES;

// ---- 内部操作的 user_data 哨兵 --------------------------------------------
// 真实操作的 user_data 是 io_operation*（堆/协程帧地址，必然高位非零且远大于哨兵）。
// 内部操作（TIMEOUT / ASYNC_CANCEL）用 1、2 …… 这些小整数，收割时按值区分即可。
inline constexpr std::uint64_t kTagTimeout = 1;
inline constexpr std::uint64_t kTagCancel = 2;
inline constexpr std::uint64_t kInternalTagMax = 0x100;

[[nodiscard]] inline bool is_internal(std::uint64_t tag) noexcept {
    return tag < kInternalTagMax;
}

#endif  // !_WIN32

}  // namespace mfweb::io::uring
