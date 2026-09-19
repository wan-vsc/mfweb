// mfbench —— 框架自带基准与压测程序。
//
// 说明：Windows 上没有原生 wrk（wrk 依赖 epoll/kqueue），因此本机主数字由 mfbench 产出，
// 报告需连带环境、参数与命令一起给出；Linux 侧另用原生 wrk 做外部交叉验证。
//
// P0 阶段为占位实现，P1 起逐个补入：协程吞吐、定时器增删、JSON、路由、QPS、大文件吞吐。

#include <mfweb/version.hpp>

#include <cstdio>
#include <string_view>

namespace {

void print_usage() {
    std::printf("mfbench %.*s —— 尚未实现任何基准（P0 占位）\n",
                static_cast<int>(mfweb::version_string().size()), mfweb::version_string().data());
    std::printf("用法: mfbench <bench-name> [options]\n");
    std::printf("可用基准: (空，P1 起陆续加入)\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--help") {
        print_usage();
        return 0;
    }
    print_usage();
    return 0;
}
