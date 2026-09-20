#include <mfweb/router/router.hpp>

#include <algorithm>

namespace mfweb::router {
namespace {

constexpr std::size_t kMethodCount = 7;

[[nodiscard]] std::size_t method_index(http::method m) noexcept {
    const auto v = static_cast<std::size_t>(m);
    return v < kMethodCount ? v : kMethodCount - 1;  // unknown 归到最后一格
}

}  // namespace

struct router::node {
    std::string segment;  // 静态段名（根为空）
    std::string param_name;

    std::vector<std::unique_ptr<node>> static_children;
    std::unique_ptr<node> param_child;
    std::unique_ptr<node> wildcard_child;

    handler handlers[kMethodCount];
    bool has_any_handler = false;

    [[nodiscard]] node* find_static(std::string_view seg) const {
        for (const auto& c : static_children) {
            if (c->segment == seg) { return c.get(); }
        }
        return nullptr;
    }
};

// 显式回溯栈的一帧。
//
// 只存"位置与基线"，**不存参数值** —— 参数值可以在弹出时从 segs/offsets 重新推导：
//   * 参数节点：值就是 segs[seg_index - 1]
//   * 通配节点：值就是 target[offsets[wildcard_start] ...]
// 这样帧从 48 字节缩到 24 字节、每帧少一次 string_view 拷贝。
// 实测这轮优化是必要的：早期版本因为帧太大 + 每次匹配一次堆分配，
// 非递归实现比递归下降还慢（见 bench B2）。
struct router::frame {
    const node* n = nullptr;
    std::uint32_t seg_index = 0;
    std::uint32_t params_size = 0;
    std::uint32_t wildcard_start = 0xFFFFFFFFu;  // k_no_wildcard：本帧不是通配
};

router::router() : root_(std::make_unique<node>()) {}
router::~router() = default;
router::router(router&&) noexcept = default;
router& router::operator=(router&&) noexcept = default;

router::segment_list router::split(std::string_view target, offset_list& offsets) {
    segment_list segs;
    offsets.clear();

    // 剥离查询串
    const std::size_t q = target.find('?');
    if (q != std::string_view::npos) { target = target.substr(0, q); }

    std::size_t start = 0;
    while (start < target.size()) {
        const std::size_t slash = target.find('/', start);
        const std::size_t end = (slash == std::string_view::npos) ? target.size() : slash;
        if (end > start) {
            segs.push_back(target.substr(start, end - start));
            offsets.push_back(start);
        }
        if (slash == std::string_view::npos) { break; }
        start = slash + 1;
    }
    return segs;
}

router::node* router::ensure_static_child(node* parent, std::string_view seg) {
    for (auto& c : parent->static_children) {
        if (c->segment == seg) { return c.get(); }
    }
    auto fresh = std::make_unique<node>();
    fresh->segment.assign(seg);
    node* raw = fresh.get();
    parent->static_children.push_back(std::move(fresh));
    return raw;
}

router::node* router::ensure_param_child(node* parent, std::string name) {
    if (!parent->param_child) {
        parent->param_child = std::make_unique<node>();
        parent->param_child->param_name = std::move(name);
    }
    return parent->param_child.get();
}

router::node* router::ensure_wildcard_child(node* parent, std::string name) {
    if (!parent->wildcard_child) {
        parent->wildcard_child = std::make_unique<node>();
        parent->wildcard_child->param_name = std::move(name);
    }
    return parent->wildcard_child.get();
}

void router::add(http::method m, std::string_view pattern, handler h) {
    offset_list offsets;
    const segment_list segs = split(pattern, offsets);

    node* cur = root_.get();
    for (const auto seg : segs) {
        if (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}') {
            const std::string_view inner = seg.substr(1, seg.size() - 2);
            if (inner.size() > 3 && inner.substr(inner.size() - 3) == "...") {
                cur = ensure_wildcard_child(cur, std::string(inner.substr(0, inner.size() - 3)));
            } else {
                cur = ensure_param_child(cur, std::string(inner));
            }
        } else {
            cur = ensure_static_child(cur, seg);
        }
    }

    cur->handlers[method_index(m)] = std::move(h);
    cur->has_any_handler = true;
    ++route_count_;
}

match_result router::match(http::method m, std::string_view target, route_params& params,
                           const handler*& out) const {
    params.clear();
    out = nullptr;

    offset_list offsets;
    const segment_list segs = split(target, offsets);

    frame_stack stack;  // 内联 32 帧，零堆分配
    stack.push_back(frame{root_.get(), 0, 0, k_no_wildcard});

    bool method_mismatch = false;

    while (!stack.empty()) {
        const frame f = stack.back();
        stack.pop_back();

        // 回溯：把参数表恢复到进入本节点之前（只在真需要时截断）
        if (params.size() > f.params_size) { params.truncate(f.params_size); }

        // 参数值在弹出时按帧类型推导，不再随帧拷贝
        if (f.wildcard_start != k_no_wildcard) {
            const std::size_t off = offsets[f.wildcard_start];
            std::string_view rest = target.substr(off);
            const std::size_t q = rest.find('?');
            if (q != std::string_view::npos) { rest = rest.substr(0, q); }
            params.add(f.n->param_name, rest);
        } else if (!f.n->param_name.empty()) {
            params.add(f.n->param_name, segs[f.seg_index - 1]);
        }

        // 本节点处理完后参数表的真实大小 —— 子帧要以此为回溯基线。
        // 早期版本这里错用了 f.params_size（父节点的基线），
        // 结果每深入一层就把祖先收集到的参数截断掉（差分测试抓到的 bug）。
        const std::size_t child_base = params.size();

        if (f.seg_index == segs.size()) {
            const handler& h = f.n->handlers[method_index(m)];
            if (h) {
                out = &h;
                return match_result::found;
            }
            if (f.n->has_any_handler) { method_mismatch = true; }
            continue;
        }

        const std::string_view seg = segs[f.seg_index];

        // 压栈顺序 = 优先级逆序：先压通配（最后弹），再参数，最后静态（最先弹）
        const auto child_base32 = static_cast<std::uint32_t>(child_base);
        const auto next_seg = f.seg_index + 1;
        if (f.n->wildcard_child) {
            stack.push_back(frame{f.n->wildcard_child.get(), static_cast<std::uint32_t>(segs.size()),
                                  child_base32, f.seg_index});  // 记住通配起点
        }
        if (f.n->param_child) {
            stack.push_back(frame{f.n->param_child.get(), next_seg, child_base32, k_no_wildcard});
        }
        if (const node* sc = f.n->find_static(seg)) {
            stack.push_back(frame{sc, next_seg, child_base32, k_no_wildcard});
        }
    }

    return method_mismatch ? match_result::method_not_allowed : match_result::not_found;
}

match_result router::match_recursive_impl(const node* n, const segment_list& segs,
                                          const offset_list& offsets,
                                          std::size_t index, http::method m, route_params& params,
                                          const handler*& out, bool& method_mismatch,
                                          std::string_view target) const {
    if (index == segs.size()) {
        const handler& h = n->handlers[method_index(m)];
        if (h) {
            out = &h;
            return match_result::found;
        }
        if (n->has_any_handler) { method_mismatch = true; }
        return match_result::not_found;
    }

    const std::string_view seg = segs[index];

    // 优先级：静态 > 参数 > 通配
    if (const node* sc = n->find_static(seg)) {
        const auto r = match_recursive_impl(sc, segs, offsets, index + 1, m, params, out,
                                            method_mismatch, target);
        if (r == match_result::found) { return r; }
    }

    if (n->param_child) {
        const std::size_t mark = params.size();
        params.add(n->param_child->param_name, seg);
        const auto r = match_recursive_impl(n->param_child.get(), segs, offsets, index + 1, m,
                                            params, out, method_mismatch, target);
        if (r == match_result::found) { return r; }
        params.truncate(mark);
    }

    if (n->wildcard_child) {
        const std::size_t mark = params.size();
        const std::size_t off = offsets[index];
        std::string_view rest = target.substr(off);
        const std::size_t q = rest.find('?');
        if (q != std::string_view::npos) { rest = rest.substr(0, q); }
        params.add(n->wildcard_child->param_name, rest);
        const auto r = match_recursive_impl(n->wildcard_child.get(), segs, offsets, segs.size(), m,
                                            params, out, method_mismatch, target);
        if (r == match_result::found) { return r; }
        params.truncate(mark);
    }

    return match_result::not_found;
}

match_result router::match_recursive(http::method m, std::string_view target, route_params& params,
                                     const handler*& out) const {
    params.clear();
    out = nullptr;
    offset_list offsets;
    const segment_list segs = split(target, offsets);
    bool method_mismatch = false;
    const auto r = match_recursive_impl(root_.get(), segs, offsets, 0, m, params, out,
                                        method_mismatch, target);
    if (r == match_result::found) { return r; }
    return method_mismatch ? match_result::method_not_allowed : match_result::not_found;
}

}  // namespace mfweb::router
