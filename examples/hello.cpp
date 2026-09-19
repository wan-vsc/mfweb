// 最小示例：打印框架版本，验证库可链接、可运行。
#include <mfweb/version.hpp>

#include <cstdio>

int main() {
    const auto v = mfweb::version();
    std::printf("mfweb %.*s (%d.%d.%d-%.*s) 编译器 %.*s\n",
                static_cast<int>(mfweb::version_string().size()), mfweb::version_string().data(),
                v.major, v.minor, v.patch, static_cast<int>(v.stage.size()), v.stage.data(),
                static_cast<int>(v.compiler.size()), v.compiler.data());
    return 0;
}
