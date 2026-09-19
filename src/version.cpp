#include <mfweb/version.hpp>

namespace mfweb {
namespace {

constexpr int kMajor = 0;
constexpr int kMinor = 1;
constexpr int kPatch = 0;

#if defined(_MSC_VER)
constexpr std::string_view kCompiler = "MSVC";
#elif defined(__clang__)
constexpr std::string_view kCompiler = "Clang";
#elif defined(__GNUC__)
constexpr std::string_view kCompiler = "GCC";
#else
constexpr std::string_view kCompiler = "unknown";
#endif

}  // namespace

version_info version() noexcept {
    return version_info{kMajor, kMinor, kPatch, "dev", kCompiler};
}

std::string_view version_string() noexcept { return "0.1.0-dev"; }

}  // namespace mfweb
