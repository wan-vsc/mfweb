// mfbench —— 框架自带基准与压测程序。
//
// 为什么不用 wrk：wrk 依赖 epoll/kqueue，Windows 上没有原生版本；把 wrk 放进虚拟机
// 打 Windows 服务端又会被 NAT 带宽卡死。因此 Windows 侧的主数字由 mfbench 产出，
// 报告必须连带环境、参数与命令一起给出；Linux 侧另用原生 wrk 做外部交叉验证。
//
// 约定：所有基准把结果以 CSV 行写到 stdout，第一行是表头，便于脚本收集与绘图。

#include <mfweb/version.hpp>

#include <cstdio>
#include <string_view>

namespace mfweb::bench {
int run_coro(int argc, char** argv);
int run_timer(int argc, char** argv);
}  // namespace mfweb::bench

namespace {

void print_usage() {
    std::printf("mfbench %.*s\n", static_cast<int>(mfweb::version_string().size()),
                mfweb::version_string().data());
    std::printf("用法: mfbench <bench> [参数...]\n\n");
    std::printf("可用基准:\n");
    std::printf("  coro <mode> [n] [pool|nopool]   协程基准\n");
    std::printf("      mode = create | run | live | nest\n");
    std::printf("      create : 创建并销毁 n 个未启动的协程（测帧分配器）\n");
    std::printf("      run    : 完整跑完 n 个协程（测协程生命周期吞吐）\n");
    std::printf("      live   : 同时驻留 n 个协程（测每协程内存开销）\n");
    std::printf("      nest   : 单次 n 层嵌套 co_await（测对称转移的栈安全）\n");
    std::printf("  timer <mode> [n]               定时器基准\n");
    std::printf("      mode = insert | fire | cancel-handle | cancel-index\n");
    std::printf("      cancel-handle : Awaiter 缓存节点指针，取消无需查找（生产路径）\n");
    std::printf("      cancel-index  : 朴素做法，额外维护 id→节点 哈希索引，先查表再删除\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    const std::string_view command(argv[1]);
    if (command == "--help" || command == "-h") {
        print_usage();
        return 0;
    }
    if (command == "coro") { return mfweb::bench::run_coro(argc - 1, argv + 1); }
    if (command == "timer") { return mfweb::bench::run_timer(argc - 1, argv + 1); }

    std::printf("未知基准: %.*s\n\n", static_cast<int>(command.size()), command.data());
    print_usage();
    return 1;
}
