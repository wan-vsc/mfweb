// 定时器与事件循环的验证。
//
// 重点验证简历第 3 条承诺的三件事，每一件都要有可执行的证据：
//   1. 句柄（红黑树节点指针）缓存 → 取消不需要查找；
//   2. RAII：协程在等待期间被销毁时，定时器必须被自动摘除 ——
//      否则到期后会 resume 一个已销毁的协程帧（"连接超时导致的段错误"的成因）；
//   3. 单次生效：定时器触发与取消两条路径不会重复生效。

#include <mfweb/coro/sleep_for.hpp>
#include <mfweb/runtime/event_loop.hpp>
#include <mfweb/runtime/timer_queue.hpp>
#include <mfweb/test/test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace {

using mfweb::runtime::timer_queue;
using seconds = std::chrono::seconds;
using milliseconds = std::chrono::milliseconds;

const timer_queue::time_point kBase{};  // 任意基准时刻，测试全部用相对它构造

// 手动驱动的最小根协程：允许在挂起状态下直接销毁（sync_wait 会断言挂起，不适合这里）
struct manual_root {
    struct promise_type {
        manual_root get_return_object() noexcept {
            return manual_root{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
    std::coroutine_handle<promise_type> handle{};
};

manual_root sleep_forever(timer_queue& q) {
    co_await mfweb::coro::sleep_for(q, seconds(3600));
}

manual_root sleep_then_mark(timer_queue& q, milliseconds d, bool& done) {
    co_await mfweb::coro::sleep_for(q, d);
    done = true;
}

manual_root sleep_three_times(timer_queue& q, std::vector<int>& order) {
    co_await mfweb::coro::sleep_for(q, milliseconds(1));
    order.push_back(1);
    co_await mfweb::coro::sleep_for(q, milliseconds(1));
    order.push_back(2);
    co_await mfweb::coro::sleep_for(q, milliseconds(1));
    order.push_back(3);
}

}  // namespace

// ---------------------------------------------------------------- timer_queue 基础

MFW_TEST(timer_queue, fires_in_deadline_order) {
    timer_queue q;
    std::vector<std::string> fired;

    q.schedule(kBase + seconds(3), [&fired] { fired.emplace_back("c"); });
    q.schedule(kBase + seconds(1), [&fired] { fired.emplace_back("a"); });
    q.schedule(kBase + seconds(2), [&fired] { fired.emplace_back("b"); });

    MFW_CHECK_EQ(q.size(), static_cast<std::size_t>(3));
    MFW_CHECK_EQ(q.next_deadline(), kBase + seconds(1));
    MFW_CHECK(q.validate());

    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(1)), static_cast<std::size_t>(1));
    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(2)), static_cast<std::size_t>(1));
    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(3)), static_cast<std::size_t>(1));

    const std::vector<std::string> expected{"a", "b", "c"};
    MFW_CHECK_EQ(fired, expected);
    MFW_CHECK(q.empty());
    MFW_CHECK(q.validate());
}

MFW_TEST(timer_queue, same_deadline_fires_in_insertion_order) {
    // 键里带自增 sequence，保证同一时刻到期的定时器按投递顺序 FIFO 触发
    timer_queue q;
    std::vector<int> order;
    for (int i = 0; i < 8; ++i) {
        q.schedule(kBase + seconds(1), [&order, i] { order.push_back(i); });
    }
    MFW_CHECK(q.validate());
    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(1)), static_cast<std::size_t>(8));

    const std::vector<int> expected{0, 1, 2, 3, 4, 5, 6, 7};
    MFW_CHECK_EQ(order, expected);
}

MFW_TEST(timer_queue, cancel_by_handle_prevents_fire) {
    timer_queue q;
    bool fired = false;

    auto h = q.schedule(kBase + seconds(1), [&fired] { fired = true; });
    MFW_CHECK(h.valid());
    MFW_CHECK_EQ(q.size(), static_cast<std::size_t>(1));

    MFW_CHECK(q.cancel(h));
    MFW_CHECK_MSG(!h.valid(), "取消后句柄必须失效，否则会悬垂");
    MFW_CHECK(q.empty());

    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(10)), static_cast<std::size_t>(0));
    MFW_CHECK_MSG(!fired, "已取消的定时器仍然触发了");
    MFW_CHECK(q.validate());
}

MFW_TEST(timer_queue, cancel_is_idempotent) {
    timer_queue q;
    auto h = q.schedule(kBase + seconds(1), [] {});
    MFW_CHECK(q.cancel(h));
    MFW_CHECK_MSG(!q.cancel(h), "重复取消应当返回 false");
}

MFW_TEST(timer_queue, cancel_by_id_matches_handle_path) {
    // 朴素路径（查表）与优化路径（缓存节点）必须语义一致
    timer_queue q;
    q.enable_id_index(true);

    int fired = 0;
    auto a = q.schedule(kBase + seconds(1), [&fired] { ++fired; });
    auto b = q.schedule(kBase + seconds(2), [&fired] { ++fired; });
    q.schedule(kBase + seconds(3), [&fired] { ++fired; });

    MFW_CHECK(q.cancel_by_id(a.id()));
    MFW_CHECK(q.cancel(b));
    MFW_CHECK_EQ(q.size(), static_cast<std::size_t>(1));

    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(10)), static_cast<std::size_t>(1));
    MFW_CHECK_EQ(fired, 1);
    MFW_CHECK(q.validate());
}

MFW_TEST(timer_queue, callback_may_schedule_and_cancel) {
    // 回调里再次 schedule / cancel（含取消另一个定时器）不得破坏遍历
    timer_queue q;
    std::vector<int> order;
    timer_queue::handle victim;

    victim = q.schedule(kBase + seconds(5), [&order] { order.push_back(99); });

    q.schedule(kBase + seconds(1), [&q, &order, &victim] {
        order.push_back(1);
        q.cancel(victim);
        q.schedule(kBase + seconds(2), [&order] { order.push_back(2); });
    });

    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(1)), static_cast<std::size_t>(1));
    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(2)), static_cast<std::size_t>(1));
    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(10)), static_cast<std::size_t>(0));

    const std::vector<int> expected{1, 2};
    MFW_CHECK_EQ(order, expected);
    MFW_CHECK(q.validate());
}

MFW_TEST(timer_queue, stale_handle_is_detected_not_corrupted) {
    // 定时器触发后节点即被释放，句柄随之悬垂。
    // 打开追踪后取消必须被安全拒绝，而不是读取已释放内存 ——
    // 实测过一次未加防护的版本在这里崩于 0xC0000374（堆损坏）。
    timer_queue q;
    q.set_node_tracking(true);

    auto h = q.schedule(kBase + seconds(1), [] {});
    MFW_CHECK(q.is_live(h));

    MFW_CHECK_EQ(q.fire_expired(kBase + seconds(1)), static_cast<std::size_t>(1));
    MFW_CHECK_MSG(!q.is_live(h), "触发后句柄应当已失效");

    MFW_CHECK_MSG(!q.cancel(h), "对悬垂句柄的取消必须被拒绝而不是踩内存");
    MFW_CHECK(!h.valid());
    MFW_CHECK(q.validate());
}

MFW_TEST(timer_queue, randomized_operations_keep_invariants) {
    // 句柄生命周期契约要求使用方在定时器触发后丢弃对应句柄。
    // 这条测试自己记账：只对"尚未到期"的定时器保留句柄，从而始终在契约内操作。
    timer_queue q;
    q.set_node_tracking(true);

    struct pending_entry {
        timer_queue::handle handle;
        timer_queue::time_point deadline;
    };
    std::vector<pending_entry> pending;
    int fired = 0;

    auto drop_expired = [&pending](timer_queue::time_point now) {
        pending.erase(std::remove_if(pending.begin(), pending.end(),
                                     [now](const pending_entry& e) { return e.deadline <= now; }),
                      pending.end());
    };

    for (int i = 0; i < 4000; ++i) {
        const int action = i % 3;
        const auto moment = kBase + milliseconds(1 + (i % 500));

        if (action == 0) {
            pending.push_back(pending_entry{q.schedule(moment, [&fired] { ++fired; }), moment});
        } else if (action == 1 && !pending.empty()) {
            const std::size_t idx = static_cast<std::size_t>(i) % pending.size();
            MFW_CHECK_MSG(q.is_live(pending[idx].handle), "记账逻辑有误：持有到了失效句柄");
            MFW_CHECK(q.cancel(pending[idx].handle));
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(idx));
        } else {
            q.fire_expired(moment);
            drop_expired(moment);  // 关键：触发后立刻丢弃相应句柄
        }

        MFW_CHECK_EQ(pending.size(), q.size());
        if ((i % 251) == 0) { MFW_CHECK_MSG(q.validate(), "随机操作破坏了红黑树不变量"); }
    }
    MFW_CHECK(q.validate());
}

// ---------------------------------------------------------------- Awaiter 与 RAII

MFW_TEST(timer_awaiter, destroying_suspended_coroutine_cancels_timer) {
    // 简历第 3 条的核心保证：RAII 析构自动取消。
    // 若这条不成立，协程帧被销毁后定时器仍会到点，回调去 resume 已释放的帧 → 段错误。
    timer_queue q;
    {
        manual_root root = sleep_forever(q);
        root.handle.resume();  // 跑到 sleep_for 处挂起
        MFW_CHECK_MSG(!root.handle.done(), "协程没有在定时器上挂起");
        MFW_CHECK_EQ(q.size(), static_cast<std::size_t>(1));

        root.handle.destroy();  // 销毁协程帧 → Awaiter 析构 → 取消定时器
    }
    MFW_CHECK_MSG(q.empty(), "协程已销毁但定时器仍留在队列里（悬垂回调）");
    MFW_CHECK(q.validate());
}

MFW_TEST(timer_awaiter, move_transfers_scheduled_timer) {
    // 直接手工调用 await_suspend 把定时器挂上，从而能在协程之外检验移动语义
    timer_queue q;

    mfweb::coro::timer_awaiter holder = mfweb::coro::sleep_until(q, kBase + seconds(1));
    {
        mfweb::coro::timer_awaiter temp = mfweb::coro::sleep_until(q, kBase + seconds(2));
        temp.await_suspend(std::noop_coroutine());
        MFW_CHECK(temp.pending());
        MFW_CHECK_EQ(q.size(), static_cast<std::size_t>(1));

        holder = std::move(temp);  // 移动赋值：所有权转移给 holder
    }  // temp 在此析构 —— 绝不能把已经转移出去的定时器取消掉

    MFW_CHECK_MSG(!q.empty(), "移动来源析构时误取消了已转移的定时器");
    MFW_CHECK(holder.pending());
    MFW_CHECK_EQ(q.size(), static_cast<std::size_t>(1));

    holder.cancel();
    MFW_CHECK_MSG(q.empty(), "holder 取消后定时器应当被摘除");
    MFW_CHECK(q.validate());
}

MFW_TEST(timer_awaiter, sleep_for_actually_resumes) {
    mfweb::runtime::event_loop loop;
    bool done = false;

    manual_root root = sleep_then_mark(loop.timers(), milliseconds(10), done);
    root.handle.resume();
    MFW_CHECK_MSG(!done, "10ms 的定时器不可能立刻到期");

    const auto limit = std::chrono::steady_clock::now() + seconds(10);
    while (!done && std::chrono::steady_clock::now() < limit) {
        loop.run_once(true);
    }

    MFW_CHECK_MSG(done, "定时器到点后协程没有被恢复");
    MFW_CHECK_MSG(loop.timers().empty(), "触发后定时器应当已从队列移除");
    MFW_CHECK(loop.timers().validate());
    root.handle.destroy();
}

MFW_TEST(timer_awaiter, sequential_sleeps_resume_in_order) {
    mfweb::runtime::event_loop loop;
    std::vector<int> order;

    manual_root root = sleep_three_times(loop.timers(), order);
    root.handle.resume();

    const auto limit = std::chrono::steady_clock::now() + seconds(10);
    while (order.size() < 3 && std::chrono::steady_clock::now() < limit) {
        loop.run_once(true);
    }

    const std::vector<int> expected{1, 2, 3};
    MFW_CHECK_EQ(order, expected);
    root.handle.destroy();
}

// ---------------------------------------------------------------- event_loop

MFW_TEST(event_loop, post_then_run_executes_in_order) {
    mfweb::runtime::event_loop loop;
    std::vector<int> order;
    for (int i = 0; i < 10; ++i) { loop.post([&order, i] { order.push_back(i); }); }

    const std::size_t rounds = loop.run_until_idle();
    MFW_CHECK(rounds > 0);

    std::vector<int> expected;
    for (int i = 0; i < 10; ++i) { expected.push_back(i); }
    MFW_CHECK_EQ(order, expected);
}

MFW_TEST(event_loop, cross_thread_post_is_observed) {
    mfweb::runtime::event_loop loop;
    std::atomic<int> counter{0};
    constexpr int kPosts = 500;

    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        for (int i = 0; i < kPosts; ++i) {
            loop.post([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
        }
        producer_done.store(true, std::memory_order_release);
        // 关键：再投一个哨兵任务把消费者唤醒。
        // 否则存在一个窗口——消费者刚判断完 producer_done 为 false 就阻塞等待，
        // 而生产者已经投完最后一个任务不再投递，消费者将永久阻塞。
        loop.post([] {});
    });

    while (!producer_done.load(std::memory_order_acquire)) {
        loop.run_once(true);  // 阻塞直到有新任务
    }
    producer.join();
    loop.run_until_idle();  // 清空剩余任务（stop() 语义是立即停止，不补跑）

    MFW_CHECK_EQ(counter.load(), kPosts);
}

MFW_TEST(event_loop, stop_breaks_run_immediately) {
    mfweb::runtime::event_loop loop;
    std::thread stopper([&loop] {
        std::this_thread::sleep_for(milliseconds(20));
        loop.stop();
    });

    const auto start = std::chrono::steady_clock::now();
    loop.run();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    stopper.join();

    MFW_CHECK(elapsed < seconds(5));
    MFW_CHECK(loop.stopped());
}

MFW_TEST(event_loop, run_once_returns_false_after_stop) {
    mfweb::runtime::event_loop loop;
    loop.stop();
    MFW_CHECK_MSG(!loop.run_once(false), "已停止的循环不应继续运行");
}

MFW_TEST(event_loop, tasks_can_post_more_tasks) {
    mfweb::runtime::event_loop loop;
    int depth = 0;

    // 每个任务再投递一个，直到深度达到 50 —— 验证"取快照再执行"不会自锁
    std::function<void()> step = [&] {
        ++depth;
        if (depth < 50) { loop.post(step); }
    };
    loop.post(step);

    for (int i = 0; i < 200 && depth < 50; ++i) { loop.run_once(false); }
    MFW_CHECK_EQ(depth, 50);
}
