#pragma once

// mfweb::net —— Qt 事件循环适配。
//
// 场景：Qt（尤其是 GUI）不允许在非主线程碰界面对象，而 mfweb 的回调在
// io_context 的事件循环线程上执行。两者之间需要一个明确的"投递回 Qt 线程"的动作。
//
// 设计：不引入"Qt executor"这种重型抽象，只提供一个最小原语 ——
// 把任意可调用对象用 Qt::QueuedConnection 投递到目标 QObject 所属线程。
// then 链里在需要碰界面的那一步调用它即可。
//
// ✅ **已在真实 Qt 6.8.3 (MSVC2022 x64) 下编译并运行验证**（examples/qt_then_chain.cpp）。
//    验证内容不只是"能编译"，还包括用 QThread::currentThread() == qApp->thread()
//    断言回调确实被投递回了 Qt 主线程。
//    它同时用 __has_include 门控：没装 Qt 的机器上包含它不会报错，只是不提供任何东西。

#if defined(__has_include)
#if __has_include(<QCoreApplication>) && __has_include(<QMetaObject>)
#define MFWEB_HAS_QT 1
#endif
#endif

#ifdef MFWEB_HAS_QT

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
// 必须显式包含：on_qt_main_thread() 用了 QThread::currentThread()。
// 这个疏漏是**接上真实 Qt 才暴露的** —— 没有 Qt 的机器上 __has_include 直接跳过整个头，
// 桩测试也覆盖不到，所以"看起来没问题"了很久。
#include <QThread>

#include <functional>
#include <type_traits>
#include <utility>

namespace mfweb::net {

// 把 fn 投递到 context 所属的 Qt 线程执行（非阻塞）。
// context 为 nullptr 时退化为直接调用（便于无 GUI 场景复用同一套代码）。
template <class F>
void post_to_qt(QObject* context, F&& fn) {
    if (context == nullptr) {
        std::forward<F>(fn)();
        return;
    }
    // invokeMethod 的 functor 版本在 Qt 5.10+ 可用；QueuedConnection 保证在
    // context 的线程上执行 —— 这正是"从 mfweb 事件循环跳回 Qt 主线程"的那一跳。
    QMetaObject::invokeMethod(
        context, [f = std::forward<F>(fn)]() mutable { f(); }, Qt::QueuedConnection);
}

// 当前是否在 Qt 主线程
[[nodiscard]] inline bool on_qt_main_thread() {
    return QCoreApplication::instance() != nullptr &&
           QThread::currentThread() == QCoreApplication::instance()->thread();
}

}  // namespace mfweb::net

#endif  // MFWEB_HAS_QT
