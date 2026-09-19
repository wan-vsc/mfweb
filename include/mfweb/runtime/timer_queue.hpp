#pragma once

// mfweb::runtime::timer_queue —— 基于自研红黑树的高效定时器队列。
//
// 设计要点（对应简历第 3 条）：
//   1. **红黑树**（mfweb::rb_tree）保证按 (deadline, sequence) 有序，minimum() 即最近到期。
//   2. **句柄就是红黑树节点指针**。Awaiter 把它缓存下来，取消定时器时不需要任何查找，
//      直接 erase(节点)。朴素做法要额外维护 id → 节点 的哈希索引并先查表。
//   3. 键带自增 sequence，保证同一时刻到期的定时器按投递顺序 FIFO 触发，
//      同时让键在红黑树里唯一（树只做 insert_unique，不需要处理重复键）。
//
// 线程模型：本类**无内部同步**，只应由事件循环所在线程操作。
//           跨线程投递请走 event_loop::post。
//
// 时间源：本类从不主动调用 clock::now()，所有"现在"由调用方传入。
//         这不是疏漏，而是为了可测试 —— 单元测试可以用任意构造的时间点驱动。

#include <mfweb/util/rb_tree.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mfweb::runtime {

class timer_queue {
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;
    using duration = clock::duration;
    using timer_id = std::uint64_t;
    using callback = std::function<void()>;

    struct key_type {
        time_point deadline{};
        timer_id sequence = 0;

        friend bool operator<(const key_type& a, const key_type& b) noexcept {
            if (a.deadline < b.deadline) { return true; }
            if (b.deadline < a.deadline) { return false; }
            return a.sequence < b.sequence;
        }
    };

    using tree_type = rb_tree<key_type, callback>;
    using node_type = tree_type::node_type;

    // ---------------------------------------------------------------- 句柄生命周期契约
    //
    // 句柄就是红黑树节点指针。红黑树保证"除被删除的节点外，节点地址稳定"，
    // 因此句柄在下面这段时间内有效：
    //
    //      schedule() 返回  →  该定时器被 cancel() 或被 fire_expired() 触发
    //
    // **一旦定时器触发，节点即被释放，句柄立刻变成悬垂指针。**
    // 此时再拿它调用 cancel() 会读到已释放内存并破坏堆 —— 实测过一次崩在
    // 0xC0000374（STATUS_HEAP_CORRUPTION），根因就是这个。
    //
    // 这条契约由使用方负责维护，因为只有使用方知道定时器何时触发：
    //   * coro::timer_awaiter 在回调里第一件事就是 handle_.reset()，自动满足契约；
    //   * 直接使用 timer_queue 时，请在 fire_expired 之后丢弃所有已到期定时器的句柄。
    //
    // 为了不让"违约"变成难以复现的堆损坏，timer_queue 提供了 set_node_tracking()：
    // 打开后会校验句柄是否仍指向队列中存活的节点，违约时 cancel() 直接返回 false
    // 而不是踩内存。追踪有额外开销，默认关闭，供测试与调试使用。
    class handle {
    public:
        handle() noexcept = default;

        [[nodiscard]] bool valid() const noexcept { return node_ != nullptr; }
        [[nodiscard]] time_point deadline() const noexcept { return node_->key.deadline; }
        [[nodiscard]] timer_id id() const noexcept { return node_->key.sequence; }

        void reset() noexcept { node_ = nullptr; }

        friend bool operator==(const handle& a, const handle& b) noexcept {
            return a.node_ == b.node_;
        }
        friend bool operator!=(const handle& a, const handle& b) noexcept {
            return a.node_ != b.node_;
        }

    private:
        friend class timer_queue;
        explicit handle(node_type* n) noexcept : node_(n) {}
        node_type* node_ = nullptr;
    };

    timer_queue() = default;
    timer_queue(const timer_queue&) = delete;
    timer_queue& operator=(const timer_queue&) = delete;

    // ---------------------------------------------------------------- 投递

    handle schedule(time_point deadline, callback fn) {
        const timer_id id = next_id_++;
        node_type* node = tree_.insert(key_type{deadline, id}, std::move(fn));
        // key 含自增 id，不可能重复
        if (index_enabled_) { index_.emplace(id, node); }
        if (tracking_) { live_.insert(node); }
        return handle{node};
    }

    handle schedule_after(duration delay, callback fn) {
        return schedule(clock::now() + delay, std::move(fn));
    }

    // ---------------------------------------------------------------- 取消

    // 生产路径：句柄里已缓存节点，取消无需查找。返回是否真的取消了。
    // 取消后句柄被置空（避免悬垂）。
    bool cancel(handle& h) noexcept {
        node_type* node = h.node_;
        if (node == nullptr) { return false; }
        if (tracking_ && live_.find(node) == live_.end()) {
            // 句柄已悬垂（定时器早已触发）。拒绝操作而不是踩已释放内存。
            h.reset();
            return false;
        }
        h.reset();
        if (tracking_) { live_.erase(node); }
        if (index_enabled_) { index_.erase(node->key.sequence); }
        tree_.erase(node);
        return true;
    }

    // 朴素对照实现：额外维护 id → 节点 的哈希索引，取消时先查表再删除。
    // 保留它只为了基准能给出"缓存节点指针"相对"查表"的实测收益（基准 B6）。
    // 代价：每个定时器多一个哈希节点（实测约 60+ 字节）与一次哈希计算。
    bool cancel_by_id(timer_id id) {
        auto it = index_.find(id);
        if (it == index_.end()) { return false; }
        node_type* node = it->second;
        index_.erase(it);
        if (tracking_) { live_.erase(node); }
        tree_.erase(node);
        return true;
    }

    // 打开 id 索引（仅基准对照用）
    void enable_id_index(bool on) { index_enabled_ = on; }

    // 打开节点追踪：cancel/cancel_by_id 会先校验目标节点仍存活，违约时安全返回 false。
    // 用于测试与调试，避免把"句柄违约"变成难以复现的堆损坏。
    void set_node_tracking(bool on) {
        tracking_ = on;
        live_.clear();
        if (on) {
            // for_each_in_order 是 const 方法，回调拿到 const 引用；
            // 这里只是把地址登记进追踪集合，不修改节点，const_cast 是安全的。
            tree_.for_each_in_order(
                [this](const node_type& n) { live_.insert(const_cast<node_type*>(&n)); });
        }
    }

    // 句柄当前是否指向本队列中存活的定时器（追踪关闭时退化为"非空"判断）
    [[nodiscard]] bool is_live(const handle& h) const {
        if (h.node_ == nullptr) { return false; }
        if (!tracking_) { return true; }
        return live_.find(h.node_) != live_.end();
    }

    // ---------------------------------------------------------------- 驱动

    // 触发所有 deadline <= now 的定时器，返回触发个数。
    //
    // 关键顺序：先把回调**移出**节点，再 erase 节点，最后才调用回调。
    //   * 先移出：节点被删除后回调对象仍然存活于局部变量中；
    //   * 先 erase 再调用：回调里若再次 schedule/cancel（包括取消自身）不会破坏遍历，
    //     也不会二次释放同一节点。
    std::size_t fire_expired(time_point now) {
        std::size_t fired = 0;
        for (;;) {
            node_type* node = tree_.minimum();
            if (node == nullptr || now < node->key.deadline) { break; }

            callback fn = std::move(node->value);
            node->value = nullptr;
            const timer_id id = node->key.sequence;
            if (index_enabled_) { index_.erase(id); }
            if (tracking_) { live_.erase(node); }
            tree_.erase(node);
            ++fired;

            if (fn) { fn(); }
        }
        return fired;
    }

    // ---------------------------------------------------------------- 查询

    [[nodiscard]] bool empty() const noexcept { return tree_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return tree_.size(); }

    // 最近一个到期时刻；无定时器时返回 time_point::max()
    [[nodiscard]] time_point next_deadline() const noexcept {
        node_type* node = tree_.minimum();
        return (node == nullptr) ? time_point::max() : node->key.deadline;
    }

    void clear() noexcept {
        tree_.clear();
        index_.clear();
        live_.clear();
    }

    // 测试与调试用：校验底层红黑树不变量
    [[nodiscard]] bool validate() const noexcept { return tree_.validate(); }

private:
    tree_type tree_;
    std::unordered_map<timer_id, node_type*> index_;
    std::unordered_set<node_type*> live_;  // 仅 track 打开时维护
    bool index_enabled_ = false;
    bool tracking_ = false;
    timer_id next_id_ = 1;
};

}  // namespace mfweb::runtime
