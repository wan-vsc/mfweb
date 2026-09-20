// 路由表测试。重点：
//   1. 语义正确（静态/参数/通配/优先级/回溯/405/404）；
//   2. **非递归实现与递归实现在同一棵树上结果完全一致** —— 差分测试。

#include <mfweb/router/aop.hpp>
#include <mfweb/router/router.hpp>
#include <mfweb/test/test.hpp>

#include <random>
#include <string>
#include <vector>

namespace {

using mfweb::router::router;
using mfweb::router::route_params;
using mfweb::router::match_result;
namespace http = mfweb::http;

http::request make_req(http::method m, std::string_view target) {
    http::request r;
    r.method_ = m;
    r.target = target;
    return r;
}

// 命中路由时返回它的名字（handler 把名字写进 response.body）
[[nodiscard]] std::string resolve(const router& rt, http::method m, std::string_view target,
                                  route_params& params, bool recursive = false) {
    const mfweb::router::handler* h = nullptr;
    const auto r = recursive ? rt.match_recursive(m, target, params, h)
                             : rt.match(m, target, params, h);
    if (r != match_result::found) {
        return r == match_result::method_not_allowed ? "<405>" : "<404>";
    }
    http::request req = make_req(m, target);
    http::response resp;
    (*h)(req, params, resp);
    return resp.body;
}

}  // namespace

MFW_TEST(router, static_segments) {
    router rt;
    rt.get("/", [](const http::request&, route_params&, http::response& r) { r.body = "root"; });
    rt.get("/users", [](const http::request&, route_params&, http::response& r) { r.body = "users"; });
    rt.get("/users/list",
           [](const http::request&, route_params&, http::response& r) { r.body = "list"; });

    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/", p), std::string("root"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/users", p), std::string("users"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/users/list", p), std::string("list"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/nope", p), std::string("<404>"));
}

MFW_TEST(router, path_parameters) {
    router rt;
    rt.get("/users/{id}", [](const http::request&, route_params& p, http::response& r) {
        r.body = std::string(p.get("id"));
    });

    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/users/42", p), std::string("42"));
    MFW_CHECK_EQ(p.size(), static_cast<std::size_t>(1));
}

MFW_TEST(router, multiple_parameters) {
    router rt;
    rt.get("/a/{x}/b/{y}", [](const http::request&, route_params& p, http::response& r) {
        r.body = std::string(p.get("x")) + ":" + std::string(p.get("y"));
    });
    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/a/1/b/2", p), std::string("1:2"));
}

MFW_TEST(router, wildcard_captures_rest) {
    router rt;
    rt.get("/files/{path...}", [](const http::request&, route_params& p, http::response& r) {
        r.body = std::string(p.get("path"));
    });
    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/files/a/b/c.txt", p),
                 std::string("a/b/c.txt"));
}

MFW_TEST(router, wildcard_requires_at_least_one_segment) {
    router rt;
    rt.get("/files/{path...}", [](const http::request&, route_params&, http::response& r) {
        r.body = "wild";
    });
    rt.get("/files", [](const http::request&, route_params&, http::response& r) { r.body = "dir"; });

    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/files", p), std::string("dir"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/files/x", p), std::string("wild"));
}

MFW_TEST(router, priority_static_over_param_over_wildcard) {
    router rt;
    rt.get("/a/static", [](const http::request&, route_params&, http::response& r) { r.body = "static"; });
    rt.get("/a/{p}", [](const http::request&, route_params&, http::response& r) { r.body = "param"; });
    rt.get("/a/{w...}", [](const http::request&, route_params&, http::response& r) { r.body = "wild"; });

    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/a/static", p), std::string("static"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/a/other", p), std::string("param"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/a/x/y", p), std::string("wild"));
}

MFW_TEST(router, backtracks_when_static_path_does_not_lead_to_match) {
    // 关键用例：/a/b 有静态分支但不完整，必须回溯到 /a/{x}/c
    router rt;
    rt.get("/a/b/c", [](const http::request&, route_params&, http::response& r) { r.body = "abc"; });
    rt.get("/a/{x}/c", [](const http::request&, route_params& p, http::response& r) {
        r.body = "param:" + std::string(p.get("x"));
    });

    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/a/b/c", p), std::string("abc"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/a/z/c", p), std::string("param:z"));
}

MFW_TEST(router, backtracking_restores_parameters) {
    // 回溯后参数表必须恢复，不能残留上一分支的参数
    router rt;
    rt.get("/{a}/{b}/x", [](const http::request&, route_params& p, http::response& r) {
        r.body = std::string(p.get("a")) + "/" + std::string(p.get("b"));
    });
    rt.get("/{a}/fixed/y", [](const http::request&, route_params& p, http::response& r) {
        r.body = "second:" + std::string(p.get("a"));
    });

    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/one/fixed/y", p), std::string("second:one"));
    MFW_CHECK_EQ(p.size(), static_cast<std::size_t>(1));
}

MFW_TEST(router, method_not_allowed) {
    router rt;
    rt.get("/x", [](const http::request&, route_params&, http::response& r) { r.body = "get"; });

    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::post, "/x", p), std::string("<405>"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/y", p), std::string("<404>"));
}

MFW_TEST(router, query_string_is_ignored) {
    router rt;
    rt.get("/search", [](const http::request&, route_params&, http::response& r) { r.body = "ok"; });
    rt.get("/u/{id}", [](const http::request&, route_params& p, http::response& r) {
        r.body = std::string(p.get("id"));
    });

    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/search?q=1", p), std::string("ok"));
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/u/7?x=1", p), std::string("7"));
}

MFW_TEST(router, trailing_slash_is_tolerated) {
    router rt;
    rt.get("/users", [](const http::request&, route_params&, http::response& r) { r.body = "u"; });
    route_params p;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/users/", p), std::string("u"));
}

MFW_TEST(router, iterative_and_recursive_agree_on_random_routes) {
    // 差分测试：随机生成路由表与请求路径，两种实现必须给出完全一致的结果
    std::mt19937 rng(20260919);
    const char* words[] = {"a", "b", "c", "list", "item"};

    router rt;
    std::vector<std::string> patterns;
    for (int i = 0; i < 120; ++i) {
        std::string pat;
        const int depth = 1 + static_cast<int>(rng() % 4);
        for (int d = 0; d < depth; ++d) {
            const int kind = static_cast<int>(rng() % 6);
            pat += "/";
            if (kind == 0) {
                pat += "{p" + std::to_string(d) + "}";
            } else if (kind == 1 && d == depth - 1) {
                pat += "{rest...}";
            } else {
                pat += words[rng() % 5];
            }
        }
        patterns.push_back(pat);
        const bool is_get = (rng() % 2U) == 0U;
        const std::string body = "r" + std::to_string(i);
        auto mk = [body](const http::request&, route_params&, http::response& r) { r.body = body; };
        rt.add(is_get ? http::method::get : http::method::post, pat, mk);
    }

    int checked = 0;
    for (int i = 0; i < 2000; ++i) {
        std::string path;
        const int depth = 1 + static_cast<int>(rng() % 5);
        for (int d = 0; d < depth; ++d) {
            path += "/";
            path += words[rng() % 5];
        }
        const http::method m = (rng() % 2U) == 0U ? http::method::get : http::method::post;

        route_params p1;
        route_params p2;
        const std::string a = resolve(rt, m, path, p1, /*recursive=*/false);
        const std::string b = resolve(rt, m, path, p2, /*recursive=*/true);

        MFW_CHECK_MSG(a == b, ("两种实现结果不一致: " + path).c_str());
        MFW_CHECK_EQ(p1.size(), p2.size());
        for (std::size_t k = 0; k < p1.size(); ++k) {
            MFW_CHECK_EQ(p1.name_at(k), p2.name_at(k));
            MFW_CHECK_EQ(p1.at(k), p2.at(k));
        }
        ++checked;
    }
    MFW_CHECK_EQ(checked, 2000);
}

// ---------------------------------------------------------------- AOP

namespace {

struct log_aspect {
    static void before(const http::request&, route_params&, http::response&) {
        order() += "L.b ";
    }
    static void after(const http::request&, http::response&) { order() += "L.a "; }
    static std::string& order() {
        static std::string s;
        return s;
    }
};

struct auth_aspect {
    static bool ok;
    static bool before(const http::request&, route_params&, http::response& r) {
        log_aspect::order() += "A.b ";
        if (!ok) {
            r.status = 401;
            r.body = "denied";
            return false;
        }
        return true;
    }
    static void after(const http::request&, http::response&) { log_aspect::order() += "A.a "; }
};

bool auth_aspect::ok = true;

struct only_after_aspect {
    static void after(const http::request&, http::response& r) { r.body += "+tail"; }
};

}  // namespace

MFW_TEST(router_aop, aspect_order_is_onion) {
    log_aspect::order().clear();
    auth_aspect::ok = true;

    auto h = mfweb::router::with_aop<log_aspect, auth_aspect>(
        [](const http::request&, route_params&, http::response& r) {
            log_aspect::order() += "core ";
            r.body = "ok";
        });

    http::request req = make_req(http::method::get, "/x");
    route_params p;
    http::response resp;
    h(req, p, resp);

    MFW_CHECK_EQ(log_aspect::order(), std::string("L.b A.b core A.a L.a "));
    MFW_CHECK_EQ(resp.body, std::string("ok"));
}

MFW_TEST(router_aop, before_false_short_circuits_core) {
    log_aspect::order().clear();
    auth_aspect::ok = false;

    bool core_ran = false;
    auto h = mfweb::router::with_aop<log_aspect, auth_aspect>(
        [&core_ran](const http::request&, route_params&, http::response&) { core_ran = true; });

    http::request req = make_req(http::method::get, "/x");
    route_params p;
    http::response resp;
    h(req, p, resp);

    MFW_CHECK_MSG(!core_ran, "before 返回 false 时核心处理不应执行");
    MFW_CHECK_EQ(resp.status, 401);
    // after 仍然执行（访问日志需要记录被拒请求）
    MFW_CHECK_EQ(log_aspect::order(), std::string("L.b A.b A.a L.a "));
    auth_aspect::ok = true;
}

MFW_TEST(router_aop, aspects_may_omit_members) {
    // 只有 after 的切面应当能编译并通过 requires 检测
    auto h = mfweb::router::with_aop<only_after_aspect>(
        [](const http::request&, route_params&, http::response& r) { r.body = "base"; });

    http::request req = make_req(http::method::get, "/x");
    route_params p;
    http::response resp;
    h(req, p, resp);
    MFW_CHECK_EQ(resp.body, std::string("base+tail"));
}

MFW_TEST(router_aop, empty_chain_is_passthrough) {
    auto h = mfweb::router::with_aop<>(
        [](const http::request&, route_params&, http::response& r) { r.body = "plain"; });
    http::request req = make_req(http::method::get, "/x");
    route_params p;
    http::response resp;
    h(req, p, resp);
    MFW_CHECK_EQ(resp.body, std::string("plain"));
}

MFW_TEST(router_aop, works_through_router) {
    router rt;
    rt.get("/secure/{id}", mfweb::router::with_aop<auth_aspect>(
                               [](const http::request&, route_params& p, http::response& r) {
                                   r.body = "got:" + std::string(p.get("id"));
                               }));

    route_params p;
    auth_aspect::ok = true;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/secure/9", p), std::string("got:9"));

    auth_aspect::ok = false;
    MFW_CHECK_EQ(resolve(rt, http::method::get, "/secure/9", p), std::string("denied"));
    auth_aspect::ok = true;
}
