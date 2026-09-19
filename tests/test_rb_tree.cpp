// 红黑树的正确性验证。
//
// 策略：自研数据结构最容易出的问题是"平时看着对、边界就崩"，因此这里用三重证据：
//   1. 每次修改后跑 validate() —— 直接检查红黑树三条不变量 + BST 有序性 + size 一致；
//   2. 与 std::map 做随机操作对照 —— 2 万次随机增删后逐项比对中序序列与大小；
//   3. 多种删除顺序的穷举 —— 升序/降序/随机删空，每次都校验不变量。

#include <mfweb/test/test.hpp>
#include <mfweb/util/rb_tree.hpp>

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using int_tree = mfweb::rb_tree<int, int>;

std::vector<int> in_order_keys(const int_tree& t) {
    std::vector<int> keys;
    t.for_each_in_order([&keys](const int_tree::node_type& n) { keys.push_back(n.key); });
    return keys;
}

}  // namespace

MFW_TEST(rb_tree, empty_tree_contract) {
    int_tree t;
    MFW_CHECK(t.empty());
    MFW_CHECK_EQ(t.size(), static_cast<std::size_t>(0));
    MFW_CHECK(t.minimum() == nullptr);
    MFW_CHECK(t.maximum() == nullptr);
    MFW_CHECK(t.find(1) == nullptr);
    MFW_CHECK(t.lower_bound(1) == nullptr);
    MFW_CHECK(t.validate());
}

MFW_TEST(rb_tree, insert_find_and_ordered_iteration) {
    int_tree t;
    const int values[] = {50, 30, 70, 20, 40, 60, 80, 10, 90};
    for (int v : values) {
        auto* n = t.insert(v, v * 10);
        MFW_CHECK_MSG(n != nullptr, "插入唯一键却返回 nullptr");
        MFW_CHECK(t.validate());
    }

    MFW_CHECK_EQ(t.size(), static_cast<std::size_t>(9));
    MFW_CHECK_EQ(t.minimum()->key, 10);
    MFW_CHECK_EQ(t.maximum()->key, 90);
    MFW_CHECK_EQ(t.find(40)->value, 400);
    MFW_CHECK(t.find(41) == nullptr);

    const std::vector<int> expected{10, 20, 30, 40, 50, 60, 70, 80, 90};
    MFW_CHECK_EQ(in_order_keys(t), expected);
}

MFW_TEST(rb_tree, duplicate_key_is_rejected) {
    int_tree t;
    MFW_CHECK(t.insert(5, 1) != nullptr);
    MFW_CHECK_MSG(t.insert(5, 2) == nullptr, "重复键应当被拒绝");
    MFW_CHECK_EQ(t.size(), static_cast<std::size_t>(1));
    MFW_CHECK_EQ(t.find(5)->value, 1);
    MFW_CHECK(t.validate());
}

MFW_TEST(rb_tree, lower_bound_semantics) {
    int_tree t;
    for (int v : {10, 20, 30, 40}) { t.insert(v, v); }

    MFW_CHECK_EQ(t.lower_bound(5)->key, 10);
    MFW_CHECK_EQ(t.lower_bound(10)->key, 10);
    MFW_CHECK_EQ(t.lower_bound(25)->key, 30);
    MFW_CHECK_EQ(t.lower_bound(40)->key, 40);
    MFW_CHECK_MSG(t.lower_bound(41) == nullptr, "lower_bound 越界应返回 nullptr");
}

MFW_TEST(rb_tree, successor_and_predecessor_walk) {
    int_tree t;
    for (int v : {50, 30, 70, 20, 40, 60, 80}) { t.insert(v, v); }

    std::vector<int> forward;
    for (auto* n = t.minimum(); n != nullptr; n = t.successor(n)) { forward.push_back(n->key); }
    const std::vector<int> expected{20, 30, 40, 50, 60, 70, 80};
    MFW_CHECK_EQ(forward, expected);

    std::vector<int> backward;
    for (auto* n = t.maximum(); n != nullptr; n = t.predecessor(n)) { backward.push_back(n->key); }
    std::vector<int> expected_rev(expected.rbegin(), expected.rend());
    MFW_CHECK_EQ(backward, expected_rev);

    MFW_CHECK(t.successor(t.maximum()) == nullptr);
    MFW_CHECK(t.predecessor(t.minimum()) == nullptr);
}

MFW_TEST(rb_tree, erase_by_node_updates_invariants) {
    int_tree t;
    for (int i = 1; i <= 64; ++i) { t.insert(i, i); }

    // 删掉全部偶数键：每次删除后都校验不变量
    for (int i = 2; i <= 64; i += 2) {
        auto* n = t.find(i);
        MFW_CHECK(n != nullptr);
        t.erase(n);
        MFW_CHECK(t.validate());
    }

    MFW_CHECK_EQ(t.size(), static_cast<std::size_t>(32));
    MFW_CHECK_EQ(t.find(2), nullptr);
    MFW_CHECK_EQ(t.find(3)->value, 3);

    std::vector<int> expected;
    for (int i = 1; i <= 64; i += 2) { expected.push_back(i); }
    MFW_CHECK_EQ(in_order_keys(t), expected);
}

MFW_TEST(rb_tree, erase_absent_key_returns_false) {
    int_tree t;
    t.insert(1, 1);
    MFW_CHECK(!t.erase_key(2));
    MFW_CHECK(t.erase_key(1));
    MFW_CHECK(t.empty());
    MFW_CHECK(t.validate());
}

MFW_TEST(rb_tree, erase_all_in_ascending_order) {
    int_tree t;
    for (int i = 1; i <= 256; ++i) { t.insert(i, i); }
    for (int i = 1; i <= 256; ++i) {
        MFW_CHECK(t.erase_key(i));
        MFW_CHECK(t.validate());
    }
    MFW_CHECK(t.empty());
}

MFW_TEST(rb_tree, erase_all_in_descending_order) {
    int_tree t;
    for (int i = 1; i <= 256; ++i) { t.insert(i, i); }
    for (int i = 256; i >= 1; --i) {
        MFW_CHECK(t.erase_key(i));
        MFW_CHECK(t.validate());
    }
    MFW_CHECK(t.empty());
}

MFW_TEST(rb_tree, erase_all_in_random_order) {
    std::mt19937 rng(20260919);
    std::vector<int> keys;
    for (int i = 1; i <= 512; ++i) { keys.push_back(i); }
    std::shuffle(keys.begin(), keys.end(), rng);

    int_tree t;
    for (int k : keys) { t.insert(k, k); }
    for (int k : keys) {
        MFW_CHECK(t.erase_key(k));
        MFW_CHECK(t.validate());
    }
    MFW_CHECK(t.empty());
}

MFW_TEST(rb_tree, randomized_agrees_with_std_map) {
    // 最强的一条：2 万次随机增删，全程与 std::map 对照
    std::mt19937 rng(0xC0FFEE);
    int_tree tree;
    std::map<int, int> ref;

    constexpr int kOperations = 20000;
    constexpr int kKeySpace = 512;

    for (int i = 0; i < kOperations; ++i) {
        const int key = static_cast<int>(rng() % kKeySpace);
        const bool do_insert = (rng() % 2U) == 0U;

        if (do_insert) {
            auto* node = tree.insert(key, i);
            const auto it = ref.find(key);
            if (it == ref.end()) {
                MFW_CHECK_MSG(node != nullptr, "对照表允许插入但红黑树拒绝了");
                ref.emplace(key, i);
            } else {
                MFW_CHECK_MSG(node == nullptr, "对照表已有该键但红黑树仍插入成功");
            }
        } else {
            const bool erased_tree = tree.erase_key(key);
            const bool erased_ref = ref.erase(key) > 0;
            MFW_CHECK_EQ(erased_tree, erased_ref);
        }

        if ((i % 97) == 0) {
            MFW_CHECK_MSG(tree.validate(), "随机操作过程中红黑树不变量被破坏");
            MFW_CHECK_EQ(tree.size(), ref.size());
        }
    }

    MFW_CHECK(tree.validate());
    MFW_CHECK_EQ(tree.size(), ref.size());

    // 逐项比对中序遍历
    std::vector<int> tree_keys = in_order_keys(tree);
    std::vector<int> ref_keys;
    ref_keys.reserve(ref.size());
    for (const auto& kv : ref) { ref_keys.push_back(kv.first); }
    MFW_CHECK_EQ(tree_keys, ref_keys);

    // 逐项比对后继链与查找
    auto* node = tree.minimum();
    for (int expected : ref_keys) {
        MFW_CHECK(node != nullptr);
        MFW_CHECK_EQ(node->key, expected);
        MFW_CHECK_EQ(tree.find(expected), node);
        node = tree.successor(node);
    }
    MFW_CHECK(node == nullptr);
}

MFW_TEST(rb_tree, node_pointers_survive_unrelated_operations) {
    // 这条契约是定时器 Awaiter 缓存节点指针的前提：
    // 除被删除的那个节点外，任何插入/删除都不能让已有节点指针失效。
    using string_tree = mfweb::rb_tree<int, std::string>;
    string_tree tree;

    std::vector<string_tree::node_type*> kept;
    kept.reserve(100);
    for (int i = 0; i < 100; ++i) {
        kept.push_back(tree.insert(i * 2, std::to_string(i * 2)));
    }

    // 大量无关插入
    for (int i = 0; i < 500; ++i) { tree.insert(i * 2 + 1000, "noise"); }
    MFW_CHECK(tree.validate());

    // 删掉一半持有指针的节点
    for (int i = 0; i < 50; ++i) { tree.erase(kept[static_cast<std::size_t>(i)]); }
    MFW_CHECK(tree.validate());

    // 其余指针必须仍然有效且指向原来的键值
    for (int i = 50; i < 100; ++i) {
        auto* n = kept[static_cast<std::size_t>(i)];
        MFW_CHECK_EQ(n->key, i * 2);
        MFW_CHECK_EQ(n->value, std::to_string(i * 2));
    }
}

MFW_TEST(rb_tree, clear_then_reuse) {
    int_tree t;
    for (int i = 0; i < 1000; ++i) { t.insert(i, i); }
    MFW_CHECK_EQ(t.size(), static_cast<std::size_t>(1000));

    t.clear();
    MFW_CHECK(t.empty());
    MFW_CHECK(t.minimum() == nullptr);
    MFW_CHECK(t.validate());

    for (int i = 0; i < 100; ++i) { t.insert(i, i * 3); }
    MFW_CHECK_EQ(t.size(), static_cast<std::size_t>(100));
    MFW_CHECK_EQ(t.find(50)->value, 150);
    MFW_CHECK(t.validate());
}

MFW_TEST(rb_tree, large_random_workload) {
    // 20 万次操作，周期性校验；用于暴露只在特定旋转组合下才出现的问题
    std::mt19937 rng(777);
    int_tree t;
    std::map<int, int> ref;

    for (int i = 0; i < 200000; ++i) {
        const int key = static_cast<int>(rng() % 20000U);
        if ((rng() % 3U) != 0U) {
            if (t.insert(key, i) != nullptr) { ref.emplace(key, i); }
            else { MFW_CHECK(ref.count(key) == 1); }
        } else {
            const bool a = t.erase_key(key);
            const bool b = ref.erase(key) > 0;
            MFW_CHECK_EQ(a, b);
        }
        if ((i % 9973) == 0) { MFW_CHECK_MSG(t.validate(), "大规模随机负载下不变量被破坏"); }
    }

    MFW_CHECK(t.validate());
    MFW_CHECK_EQ(t.size(), ref.size());
}
