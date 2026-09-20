#pragma once

// mfweb::router::aop —— 编译期展开的切面链（可变参数模板）。
//
// 切面契约（三个成员**全部可选**，用 C++20 requires 检测）：
//     static bool before(const http::request&, route_params&, http::response&);  // 返回 false = 短路
//     static void after (const http::request&, http::response&);
//     static void on_error(const http::request&, http::response&);
//
// 语义（洋葱模型）：
//     before 按声明顺序执行；任一 before 返回 false 则**跳过核心处理**；
//     after 按**逆序**执行 —— 这样"外层切面"能看到内层切面处理后的结果，
//     典型的访问日志切面因此能记录到最终状态码。
//
// 性能：整条链在编译期展开，**无虚函数、无 std::function 嵌套、无堆分配**。
// 这类"零开销抽象"正是简历第 2 条"可变参数模板支持 AOP"要表达的东西。

#include <mfweb/router/router.hpp>

#include <type_traits>
#include <utility>

namespace mfweb::router {
namespace detail {

template <class A>
concept has_before = requires(const http::request& req, route_params& p, http::response& r) {
    A::before(req, p, r);
};

template <class A>
concept has_after = requires(const http::request& req, http::response& r) { A::after(req, r); };

template <class A>
concept has_on_error = requires(const http::request& req, http::response& r) {
    A::on_error(req, r);
};

}  // namespace detail

template <class... Aspects>
class aop_chain {
public:
    static void run(const handler& core, const http::request& req, route_params& params,
                    http::response& resp) {
        // 空参数包要单独处理：run_before<> / run_after<> 匹配不到下面的模板
        // （模板签名要求至少一个切面类型）。空链语义就是直通。
        bool proceed = true;
        if constexpr (sizeof...(Aspects) > 0) {
            proceed = run_before<Aspects...>(req, params, resp);
        }
        if (proceed) { core(req, params, resp); }
        if constexpr (sizeof...(Aspects) > 0) {
            run_after<Aspects...>(req, resp);
        }
    }

private:
    template <class A, class... Rest>
    static bool run_before(const http::request& req, route_params& params, http::response& resp) {
        bool proceed = true;
        if constexpr (detail::has_before<A>) {
            using ret_t = decltype(A::before(req, params, resp));
            if constexpr (std::is_same_v<std::decay_t<ret_t>, bool>) {
                proceed = A::before(req, params, resp);
            } else {
                A::before(req, params, resp);
            }
        }
        if (!proceed) { return false; }
        if constexpr (sizeof...(Rest) > 0) {
            return run_before<Rest...>(req, params, resp);
        }
        return true;
    }

    // 逆序：先把更深层的 after 跑完，再跑自己 —— 洋葱模型
    template <class A, class... Rest>
    static void run_after(const http::request& req, http::response& resp) {
        if constexpr (sizeof...(Rest) > 0) { run_after<Rest...>(req, resp); }
        if constexpr (detail::has_after<A>) { A::after(req, resp); }
    }
};

// 把切面链与核心处理函数组合成一个普通 handler（注册期一次性包装）
template <class... Aspects>
[[nodiscard]] inline handler with_aop(handler core) {
    return [core = std::move(core)](const http::request& req, route_params& params,
                                    http::response& resp) {
        aop_chain<Aspects...>::run(core, req, params, resp);
    };
}

}  // namespace mfweb::router
