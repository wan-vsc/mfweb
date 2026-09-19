#pragma once

// mfweb 自研红黑树。
//
// 为什么不用 std::map：
//   1. 项目的红线是核心数据结构自研；定时器的复杂度与常数因子要能讲到实现层。
//   2. 需要把"节点地址稳定"写进**接口契约** —— 定时器 Awaiter 直接缓存节点指针来免去查找。
//      依赖标准容器的实现细节来保证迭代器稳定是不负责任的；这里由实现明确承诺：
//      **除了被 erase 的那个节点，任何插入/删除都不会让已有节点指针失效。**
//
// 为什么不用迭代器而直接把 node* 当句柄：
//   本容器的使用场景（定时器、路由）都是"持有句柄 → 稍后直接删除"，
//   迭代器包装层只会增加跳转与状态。node* 就是最轻的稳定句柄。
//
// 实现（CLRS 风格）：
//   * 哨兵 nil_ 是一个真实的 node_base 对象，消掉 fixup 里所有空指针判断；
//   * 节点各自 new/delete，地址稳定；
//   * erase 走 CLRS RB-DELETE（含双孩子时的后继移植与 fixup）；
//   * validate() 把红黑树的三条不变量做成可调用的自检，测试与调试期可直接断言。
//
// 线程安全：无内部同步。一个实例只应由一个线程使用（这是设计前提，不是遗漏）。

#include <cstddef>
#include <functional>
#include <iterator>
#include <new>
#include <utility>

namespace mfweb {

template <class Key, class Value, class Compare = std::less<Key>>
class rb_tree {
public:
    // 只含链接信息的节点基类。哨兵用它，因此哨兵不需要 Key/Value 可默认构造。
    struct node_base {
        node_base* parent = nullptr;
        node_base* left = nullptr;
        node_base* right = nullptr;
        bool red = true;
    };

    struct node_type : node_base {
        Key key;
        Value value;

        template <class K, class V>
        node_type(K&& k, V&& v) : key(std::forward<K>(k)), value(std::forward<V>(v)) {}
    };

    rb_tree() noexcept : root_(&nil_) {
        // 哨兵必须完全自洽，否则 fixup 会在边界上踩空：
        //   * 颜色必须为黑 —— insert_fixup 的循环条件 "z->parent->red" 依赖哨兵是黑的才能
        //     在根节点处正确终止。若哨兵为红，根会被判成"父亲是红的"，进而读 nil->parent
        //     解引用空指针（实测：段错误 0xC0000005）。
        //   * 三个指针必须自指 —— erase_fixup 会读兄弟节点的 w->left->red / w->right->red，
        //     当兄弟就是哨兵时，若其孩子为 nullptr 就会解引用空指针。
        nil_.parent = &nil_;
        nil_.left = &nil_;
        nil_.right = &nil_;
        nil_.red = false;
    }
    rb_tree(const rb_tree&) = delete;
    rb_tree& operator=(const rb_tree&) = delete;
    rb_tree(rb_tree&&) = delete;
    rb_tree& operator=(rb_tree&&) = delete;
    ~rb_tree() { clear(); }

    // ---------------------------------------------------------------- 容量
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    // ---------------------------------------------------------------- 访问
    [[nodiscard]] node_type* minimum() const noexcept {
        if (root_ == &nil_) { return nullptr; }
        return node_cast(subtree_min(root_));
    }

    [[nodiscard]] node_type* maximum() const noexcept {
        if (root_ == &nil_) { return nullptr; }
        return node_cast(subtree_max(root_));
    }

    [[nodiscard]] node_type* successor(const node_type* n) const noexcept {
        return node_cast(successor_base(n));
    }

    [[nodiscard]] node_type* predecessor(const node_type* n) const noexcept {
        return node_cast(predecessor_base(n));
    }

    [[nodiscard]] node_type* find(const Key& key) const {
        node_base* cur = root_;
        while (cur != &nil_) {
            auto* typed = node_cast(cur);
            if (comp_(key, typed->key)) {
                cur = cur->left;
            } else if (comp_(typed->key, key)) {
                cur = cur->right;
            } else {
                return typed;
            }
        }
        return nullptr;
    }

    // 第一个 key >= 给定键的节点
    [[nodiscard]] node_type* lower_bound(const Key& key) const {
        node_base* cur = root_;
        node_base* best = &nil_;
        while (cur != &nil_) {
            auto* typed = node_cast(cur);
            if (!comp_(typed->key, key)) {
                best = cur;
                cur = cur->left;
            } else {
                cur = cur->right;
            }
        }
        // best 仍指向哨兵时说明没有满足条件的键，必须返回 nullptr ——
        // 直接 node_cast(&nil_) 会得到一个非空的"假节点"。
        return (best == &nil_) ? nullptr : node_cast(best);
    }

    // ---------------------------------------------------------------- 修改

    // 插入（键唯一）。返回新节点；键已存在时返回 nullptr 且不修改树。
    template <class K, class V>
    node_type* insert(K&& key, V&& value) {
        node_base* parent = &nil_;
        node_base* cur = root_;
        while (cur != &nil_) {
            parent = cur;
            auto* typed = node_cast(cur);
            if (comp_(key, typed->key)) {
                cur = cur->left;
            } else if (comp_(typed->key, key)) {
                cur = cur->right;
            } else {
                return nullptr;  // 键重复
            }
        }

        auto* fresh = new node_type(std::forward<K>(key), std::forward<V>(value));
        fresh->parent = parent;
        fresh->left = &nil_;
        fresh->right = &nil_;
        fresh->red = true;

        if (parent == &nil_) {
            root_ = fresh;
        } else if (comp_(fresh->key, node_cast(parent)->key)) {
            parent->left = fresh;
        } else {
            parent->right = fresh;
        }

        ++size_;
        insert_fixup(fresh);
        return fresh;
    }

    // 删除指定节点（O(log n)，无需查找 —— 这正是 Awaiter 缓存节点指针的意义）
    void erase(node_type* target) noexcept {
        if (target == nullptr || size_ == 0) { return; }

        node_base* z = target;
        node_base* y = z;
        node_base* x = &nil_;
        bool y_was_red = y->red;

        if (z->left == &nil_) {
            x = z->right;
            transplant(z, z->right);
        } else if (z->right == &nil_) {
            x = z->left;
            transplant(z, z->left);
        } else {
            y = subtree_min(z->right);
            y_was_red = y->red;
            x = y->right;
            if (y->parent == z) {
                x->parent = y;
            } else {
                transplant(y, y->right);
                y->right = z->right;
                y->right->parent = y;
            }
            transplant(z, y);
            y->left = z->left;
            y->left->parent = y;
            y->red = z->red;
        }

        if (!y_was_red) { erase_fixup(x); }

        delete target;
        --size_;
    }

    // 按键删除；返回是否删掉了东西
    bool erase_key(const Key& key) {
        node_type* n = find(key);
        if (n == nullptr) { return false; }
        erase(n);
        return true;
    }

    void clear() noexcept {
        destroy_subtree(root_);
        root_ = &nil_;
        size_ = 0;
    }

    // ---------------------------------------------------------------- 自检与遍历

    // 红黑树三条不变量 + BST 有序性 + size 一致性。测试与调试期可直接断言。
    [[nodiscard]] bool validate() const noexcept {
        if (root_ == &nil_) { return size_ == 0; }
        if (root_->red) { return false; }             // 1) 根是黑的
        if (root_->parent != &nil_) { return false; }  // 根的父亲是哨兵
        if (!validate_subtree(root_, nullptr, nullptr)) { return false; }
        if (black_height(root_) < 0) { return false; }  // 2) 3) 红红相邻 / 黑高一致
        std::size_t counted = 0;
        count_subtree(root_, counted);
        return counted == size_;
    }

    // 中序遍历（按 key 升序）
    template <class F>
    void for_each_in_order(F&& fn) const {
        in_order(root_, fn);
    }

private:
    [[nodiscard]] static node_type* node_cast(node_base* p) noexcept {
        return (p == nullptr) ? nullptr : static_cast<node_type*>(p);
    }
    [[nodiscard]] static const node_type* node_cast(const node_base* p) noexcept {
        return (p == nullptr) ? nullptr : static_cast<const node_type*>(p);
    }

    [[nodiscard]] bool is_red(const node_base* p) const noexcept {
        return p != &nil_ && p->red;
    }

    [[nodiscard]] node_base* subtree_min(node_base* n) const noexcept {
        while (n->left != &nil_) { n = n->left; }
        return n;
    }

    [[nodiscard]] node_base* subtree_max(node_base* n) const noexcept {
        while (n->right != &nil_) { n = n->right; }
        return n;
    }

    [[nodiscard]] node_base* successor_base(const node_type* n) const noexcept {
        if (n == nullptr) { return nullptr; }
        auto* p = const_cast<node_base*>(static_cast<const node_base*>(n));
        if (p->right != &nil_) { return subtree_min(p->right); }
        node_base* parent = p->parent;
        while (parent != &nil_ && p == parent->right) {
            p = parent;
            parent = parent->parent;
        }
        return (parent == &nil_) ? nullptr : parent;
    }

    [[nodiscard]] node_base* predecessor_base(const node_type* n) const noexcept {
        if (n == nullptr) { return nullptr; }
        auto* p = const_cast<node_base*>(static_cast<const node_base*>(n));
        if (p->left != &nil_) { return subtree_max(p->left); }
        node_base* parent = p->parent;
        while (parent != &nil_ && p == parent->left) {
            p = parent;
            parent = parent->parent;
        }
        return (parent == &nil_) ? nullptr : parent;
    }

    void rotate_left(node_base* x) noexcept {
        node_base* y = x->right;
        x->right = y->left;
        if (y->left != &nil_) { y->left->parent = x; }
        y->parent = x->parent;
        if (x->parent == &nil_) {
            root_ = y;
        } else if (x == x->parent->left) {
            x->parent->left = y;
        } else {
            x->parent->right = y;
        }
        y->left = x;
        x->parent = y;
    }

    void rotate_right(node_base* x) noexcept {
        node_base* y = x->left;
        x->left = y->right;
        if (y->right != &nil_) { y->right->parent = x; }
        y->parent = x->parent;
        if (x->parent == &nil_) {
            root_ = y;
        } else if (x == x->parent->right) {
            x->parent->right = y;
        } else {
            x->parent->left = y;
        }
        y->right = x;
        x->parent = y;
    }

    void transplant(node_base* u, node_base* v) noexcept {
        if (u->parent == &nil_) {
            root_ = v;
        } else if (u == u->parent->left) {
            u->parent->left = v;
        } else {
            u->parent->right = v;
        }
        v->parent = u->parent;
    }

    void insert_fixup(node_base* z) noexcept {
        while (z->parent->red) {  // 父亲是红的（哨兵必为黑，故此处父亲不是哨兵）
            node_base* grand = z->parent->parent;
            if (z->parent == grand->left) {
                node_base* uncle = grand->right;
                if (uncle->red) {
                    z->parent->red = false;
                    uncle->red = false;
                    grand->red = true;
                    z = grand;
                } else {
                    if (z == z->parent->right) {
                        z = z->parent;
                        rotate_left(z);
                    }
                    z->parent->red = false;
                    z->parent->parent->red = true;
                    rotate_right(z->parent->parent);
                }
            } else {
                node_base* uncle = grand->left;
                if (uncle->red) {
                    z->parent->red = false;
                    uncle->red = false;
                    grand->red = true;
                    z = grand;
                } else {
                    if (z == z->parent->left) {
                        z = z->parent;
                        rotate_right(z);
                    }
                    z->parent->red = false;
                    z->parent->parent->red = true;
                    rotate_left(z->parent->parent);
                }
            }
        }
        root_->red = false;
    }

    void erase_fixup(node_base* x) noexcept {
        while (x != root_ && !x->red) {
            if (x == x->parent->left) {
                node_base* w = x->parent->right;
                if (w->red) {
                    w->red = false;
                    x->parent->red = true;
                    rotate_left(x->parent);
                    w = x->parent->right;
                }
                if (!w->left->red && !w->right->red) {
                    // w 可能是哨兵：绝不能把哨兵染红，否则后续所有 fixup 的终止条件都会失效
                    if (w != &nil_) { w->red = true; }
                    x = x->parent;
                } else {
                    if (!w->right->red) {
                        w->left->red = false;
                        w->red = true;
                        rotate_right(w);
                        w = x->parent->right;
                    }
                    w->red = x->parent->red;
                    x->parent->red = false;
                    w->right->red = false;
                    rotate_left(x->parent);
                    x = root_;
                }
            } else {
                node_base* w = x->parent->left;
                if (w->red) {
                    w->red = false;
                    x->parent->red = true;
                    rotate_right(x->parent);
                    w = x->parent->left;
                }
                if (!w->right->red && !w->left->red) {
                    if (w != &nil_) { w->red = true; }
                    x = x->parent;
                } else {
                    if (!w->left->red) {
                        w->right->red = false;
                        w->red = true;
                        rotate_left(w);
                        w = x->parent->left;
                    }
                    w->red = x->parent->red;
                    x->parent->red = false;
                    w->left->red = false;
                    rotate_right(x->parent);
                    x = root_;
                }
            }
        }
        x->red = false;
    }

    void destroy_subtree(node_base* n) noexcept {
        if (n == &nil_ || n == nullptr) { return; }
        destroy_subtree(n->left);
        destroy_subtree(n->right);
        delete static_cast<node_type*>(n);
    }

    // 返回 -1 表示不变量被破坏
    [[nodiscard]] int black_height(const node_base* n) const noexcept {
        if (n == &nil_) { return 1; }
        if (n->red && (n->left->red || n->right->red)) { return -1; }  // 红节点不能有红孩子
        const int left = black_height(n->left);
        if (left < 0) { return -1; }
        const int right = black_height(n->right);
        if (right < 0 || left != right) { return -1; }
        return left + (n->red ? 0 : 1);
    }

    [[nodiscard]] bool validate_subtree(const node_base* n, const Key* lo, const Key* hi) const {
        if (n == &nil_) { return true; }
        const auto* typed = static_cast<const node_type*>(n);
        if (lo != nullptr && comp_(typed->key, *lo)) { return false; }
        if (hi != nullptr && comp_(*hi, typed->key)) { return false; }
        return validate_subtree(n->left, lo, &typed->key) &&
               validate_subtree(n->right, &typed->key, hi);
    }

    void count_subtree(const node_base* n, std::size_t& out) const noexcept {
        if (n == &nil_) { return; }
        ++out;
        count_subtree(n->left, out);
        count_subtree(n->right, out);
    }

    template <class F>
    void in_order(const node_base* n, F& fn) const {
        if (n == &nil_) { return; }
        in_order(n->left, fn);
        fn(*static_cast<const node_type*>(n));
        in_order(n->right, fn);
    }

    // 哨兵声明为 mutable：const 成员函数（find/lower_bound/successor 等）里 &nil_ 会是
    // const node_base*，而本容器的接口约定是从 const 树也能取到稳定的可变节点句柄
    // （定时器场景需要）。实现上从不修改哨兵的字段，mutable 只为绕开 const 指针转换。
    mutable node_base nil_;
    node_base* root_ = &nil_;
    std::size_t size_ = 0;
    [[no_unique_address]] Compare comp_{};
};

}  // namespace mfweb
