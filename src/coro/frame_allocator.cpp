#include <mfweb/coro/frame_allocator.hpp>

#include <new>

namespace mfweb::coro {
namespace {

constexpr std::size_t kGranularity = 64;
constexpr std::size_t kMaxPooledSize = 1024;
constexpr std::size_t kClassCount = kMaxPooledSize / kGranularity;  // 16 档
constexpr std::size_t kMaxCachedPerClass = 128;

struct free_node {
    free_node* next;
};

struct class_cache {
    free_node* head = nullptr;
    std::size_t count = 0;
};

struct thread_cache {
    class_cache classes[kClassCount];
    frame_allocator_stats stats;
};

thread_local thread_cache g_cache;
thread_local bool g_pool_enabled = true;

// 请求大小 → 档位下标；调用前必须保证 1 <= size <= kMaxPooledSize
[[nodiscard]] std::size_t class_index(std::size_t size) noexcept {
    return (size + kGranularity - 1) / kGranularity - 1;
}

// 档位下标 → 该档实际分配的块大小
[[nodiscard]] constexpr std::size_t class_size(std::size_t index) noexcept {
    return (index + 1) * kGranularity;
}

}  // namespace

void* frame_allocator::allocate(std::size_t size) {
#if MFWEB_CORO_FRAME_POOL
    if (g_pool_enabled && size != 0 && size <= kMaxPooledSize) {
        const std::size_t index = class_index(size);
        class_cache& cache = g_cache.classes[index];
        if (cache.head != nullptr) {
            free_node* node = cache.head;
            cache.head = node->next;
            --cache.count;
            ++g_cache.stats.pooled_hits;
            return node;
        }
        ++g_cache.stats.pooled_misses;
        // 关键：按档位大小分配，保证同档位内任意块都装得下该档的全部请求
        ++g_cache.stats.heap_allocs;
        return ::operator new(class_size(index));
    }
#endif
    ++g_cache.stats.heap_allocs;
    return ::operator new(size);
}

void frame_allocator::deallocate(void* ptr, std::size_t size) noexcept {
    if (ptr == nullptr) { return; }

#if MFWEB_CORO_FRAME_POOL
    if (g_pool_enabled && size != 0 && size <= kMaxPooledSize) {
        class_cache& cache = g_cache.classes[class_index(size)];
        if (cache.count < kMaxCachedPerClass) {
            auto* node = static_cast<free_node*>(ptr);
            node->next = cache.head;
            cache.head = node;
            ++cache.count;
            ++g_cache.stats.pool_releases;
            return;
        }
    }
#endif
    ++g_cache.stats.heap_frees;
    ::operator delete(ptr);
}

frame_allocator_stats frame_allocator::stats() noexcept {
    frame_allocator_stats s = g_cache.stats;
    s.cached_blocks = 0;
    for (const auto& c : g_cache.classes) { s.cached_blocks += c.count; }
    return s;
}

void frame_allocator::reset_stats() noexcept {
    const std::size_t cached = stats().cached_blocks;
    g_cache.stats = frame_allocator_stats{};
    g_cache.stats.cached_blocks = cached;
}

void frame_allocator::set_pool_enabled(bool enabled) noexcept { g_pool_enabled = enabled; }

bool frame_allocator::pool_enabled() noexcept { return g_pool_enabled; }

void frame_allocator::trim() noexcept {
    for (auto& cache : g_cache.classes) {
        free_node* node = cache.head;
        while (node != nullptr) {
            free_node* next = node->next;
            ::operator delete(node);
            node = next;
        }
        cache.head = nullptr;
        cache.count = 0;
    }
}

}  // namespace mfweb::coro
