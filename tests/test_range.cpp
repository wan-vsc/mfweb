#include <mfweb/net/http/range.hpp>
#include <mfweb/test/test.hpp>

#include <string>
#include <vector>

namespace {

struct parsed {
    bool ok;
    bool unsat;
    std::vector<mfweb::http::byte_range> ranges;
};

parsed parse(const char* header, std::uint64_t size) {
    parsed p;
    p.ok = mfweb::http::parse_range(header, size, p.ranges, p.unsat);
    return p;
}

}  // namespace

MFW_TEST(http_range, no_header_means_full) {
    const parsed p = parse("", 100);
    MFW_CHECK(p.ok);
    MFW_CHECK(!p.unsat);
    MFW_CHECK_EQ(p.ranges.size(), static_cast<std::size_t>(0));
}

MFW_TEST(http_range, single_closed_range) {
    const parsed p = parse("bytes=0-99", 1000);
    MFW_CHECK(p.ok && !p.unsat);
    MFW_CHECK_EQ(p.ranges.size(), static_cast<std::size_t>(1));
    MFW_CHECK_EQ(p.ranges[0].first, static_cast<std::uint64_t>(0));
    MFW_CHECK_EQ(p.ranges[0].last, static_cast<std::uint64_t>(99));
    MFW_CHECK_EQ(p.ranges[0].length(), static_cast<std::uint64_t>(100));
}

MFW_TEST(http_range, open_ended_range) {
    const parsed p = parse("bytes=950-", 1000);
    MFW_CHECK(p.ok && !p.unsat);
    MFW_CHECK_EQ(p.ranges[0].first, static_cast<std::uint64_t>(950));
    MFW_CHECK_EQ(p.ranges[0].last, static_cast<std::uint64_t>(999));
    MFW_CHECK_EQ(p.ranges[0].length(), static_cast<std::uint64_t>(50));
}

MFW_TEST(http_range, suffix_range) {
    const parsed p = parse("bytes=-10", 1000);
    MFW_CHECK(p.ok && !p.unsat);
    MFW_CHECK_EQ(p.ranges[0].first, static_cast<std::uint64_t>(990));
    MFW_CHECK_EQ(p.ranges[0].last, static_cast<std::uint64_t>(999));
}

MFW_TEST(http_range, suffix_larger_than_file_clamps) {
    const parsed p = parse("bytes=-5000", 1000);
    MFW_CHECK(p.ok && !p.unsat);
    MFW_CHECK_EQ(p.ranges[0].first, static_cast<std::uint64_t>(0));
    MFW_CHECK_EQ(p.ranges[0].last, static_cast<std::uint64_t>(999));
}

MFW_TEST(http_range, end_beyond_file_clamps) {
    const parsed p = parse("bytes=990-999999", 1000);
    MFW_CHECK(p.ok && !p.unsat);
    MFW_CHECK_EQ(p.ranges[0].last, static_cast<std::uint64_t>(999));
}

MFW_TEST(http_range, start_out_of_range_is_unsatisfiable) {
    const parsed p = parse("bytes=1000-", 1000);
    MFW_CHECK(p.ok && p.unsat);
}

MFW_TEST(http_range, reversed_range_is_unsatisfiable) {
    const parsed p = parse("bytes=5-2", 1000);
    MFW_CHECK(p.ok && p.unsat);
}

MFW_TEST(http_range, zero_suffix_is_unsatisfiable) {
    const parsed p = parse("bytes=-0", 1000);
    MFW_CHECK(p.ok && p.unsat);
}

MFW_TEST(http_range, multiple_ranges) {
    const parsed p = parse("bytes=0-9,20-29", 1000);
    MFW_CHECK(p.ok && !p.unsat);
    MFW_CHECK_EQ(p.ranges.size(), static_cast<std::size_t>(2));
    MFW_CHECK_EQ(p.ranges[0].first, static_cast<std::uint64_t>(0));
    MFW_CHECK_EQ(p.ranges[1].first, static_cast<std::uint64_t>(20));
}

MFW_TEST(http_range, empty_file_is_unsatisfiable) {
    const parsed p = parse("bytes=0-", 0);
    MFW_CHECK(p.ok && p.unsat);
}

MFW_TEST(http_range, garbage_is_syntax_error) {
    const parsed p = parse("bytes=abc", 1000);
    MFW_CHECK_MSG(!p.ok, "非数字区间应判语法错误");
}

MFW_TEST(http_range, etag_matching) {
    const std::string etag = mfweb::http::make_etag(12345, 1700000000);
    MFW_CHECK(mfweb::http::etag_matches(etag, etag));
    MFW_CHECK(mfweb::http::etag_matches("*", etag));
    MFW_CHECK(mfweb::http::etag_matches("W/foo, " + etag, etag));
    MFW_CHECK_MSG(!mfweb::http::etag_matches("\"other\"", etag), "不同 ETag 不应命中");
    MFW_CHECK_MSG(!mfweb::http::etag_matches("", etag), "空头不应命中");
}
