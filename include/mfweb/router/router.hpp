#pragma once

// mfweb::router —— 非递归路由表。
//
// 结构：段级 trie。每层有
//   * 静态孩子（按段名精确匹配）
//   * 参数孩子  {name}     匹配恰好一段
//   * 通配孩子  {name...}  匹配剩余全部段（至少一段，可含 '/'）
// 每个节点按 HTTP 方法挂多个 handler。
//
// **为什么强调"非递归"**：路由匹配在通配/参数并存时需要回溯（例如 /a/b 既可能命中
// 静态 /a/b，也可能命中 /a/{x}/{y}）。递归下降会把回溯栈放进**调用栈**，
// 深路径下既有栈溢出风险，也有函数调用开销。这里改为**显式栈**（std::vector<frame>），
// 回溯深度与调用栈无关。基准 B2 用同一棵树对照了递归/非递归两种实现。
//
// 优先级：静态 > 参数 > 通配（显式栈的压栈顺序保证先弹出的优先级更高）。

#include <mfweb/net/http/http_common.hpp>
#include <mfweb/net/http/response.hpp>
#include <mfweb/util/small_vector.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mfweb::router {

// 一个路径参数。
//
// 这里**故意不用 std::pair**：libstdc++ 的 std::pair 有用户提供的拷贝构造函数，
// 因此 std::is_trivially_copyable_v 为 **false**（MSVC 的 STL 则是 true）。
// small_vector 依赖平凡复制做 memcpy 扩容，所以换成显式 POD。
// —— 这是本项目"Windows 上编得过、Linux 上编不过"的第一处真实差异。
struct param_entry {
    std::string_view name;
    std::string_view value;
};

// 路径参数（值指向请求目标串，生命周期随请求处理）
class route_params {
public:
    void clear() noexcept { items_.clear(); }

    void add(std::string_view name, std::string_view value) {
        items_.push_back(param_entry{name, value});
    }

    void truncate(std::size_t n) noexcept { items_.resize(n); }

    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }

    [[nodiscard]] std::string_view get(std::string_view name) const noexcept {
        for (const auto& [k, v] : items_) {
            if (k == name) { return v; }
        }
        return {};
    }

    [[nodiscard]] std::string_view at(std::size_t i) const noexcept { return items_[i].value; }
    [[nodiscard]] std::string_view name_at(std::size_t i) const noexcept {
        return items_[i].name;
    }

private:
    // 用 small_vector 而非 std::vector：路由参数每请求都要构造一次。
    // 内联容量取 4（而不是 8）：这个对象每请求构造一次，内联缓冲越大，
    // 构造成本越高；典型路由参数不超过 4 个，够了。
    util::small_vector<param_entry, 4> items_;
};

enum class match_result { found, not_found, method_not_allowed };

using handler = std::function<void(const http::request&, route_params&, http::response&)>;

class router {
public:
    router();
    ~router();
    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) noexcept;
    router& operator=(router&&) noexcept;

    // 注册路由。pattern 形如 "/users/{id}" 或 "/files/{path...}"
    void add(http::method m, std::string_view pattern, handler h);

    void get(std::string_view pattern, handler h) { add(http::method::get, pattern, std::move(h)); }
    void head(std::string_view pattern, handler h) { add(http::method::head, pattern, std::move(h)); }
    void post(std::string_view pattern, handler h) { add(http::method::post, pattern, std::move(h)); }
    void put(std::string_view pattern, handler h) { add(http::method::put, pattern, std::move(h)); }
    void del(std::string_view pattern, handler h) { add(http::method::delete_, pattern, std::move(h)); }

    // 生产路径：显式栈匹配
    match_result match(http::method m, std::string_view target, route_params& params,
                       const handler*& out) const;

    // 对照实现：同样的树、同样的语义，递归下降。
    // **仅供基准对照**（bench B2），生产路径请用 match()。
    // 保留它是因为简历声称"非递归结构提升性能"——那就必须有同树对照的实测数据支撑。
    match_result match_recursive(http::method m, std::string_view target, route_params& params,
                                 const handler*& out) const;

    [[nodiscard]] std::size_t route_count() const noexcept { return route_count_; }

private:
    struct node;
    struct frame;

    // 内联容量：路径段数与回溯深度在真实路由里都很浅（<16 / <32），
    // 这样每次匹配都**零堆分配** —— 早期用 std::vector 时每次匹配一次 malloc，
    // 实测让非递归实现比递归下降还慢 14%（见 bench B2 的说明）。
    using segment_list = util::small_vector<std::string_view, 16>;
    using offset_list = util::small_vector<std::size_t, 16>;
    using frame_stack = util::small_vector<frame, 32>;

    static constexpr std::uint32_t k_no_wildcard = 0xFFFFFFFFu;

    [[nodiscard]] static segment_list split(std::string_view target, offset_list& offsets);
    node* ensure_static_child(node* parent, std::string_view seg);
    node* ensure_param_child(node* parent, std::string name);
    node* ensure_wildcard_child(node* parent, std::string name);
    match_result match_recursive_impl(const node* n, const segment_list& segs,
                                      const offset_list& offsets, std::size_t index,
                                      http::method m, route_params& params,
                                      const handler*& out, bool& method_mismatch,
                                      std::string_view target) const;

    std::unique_ptr<node> root_;
    std::size_t route_count_ = 0;
};

}  // namespace mfweb::router
