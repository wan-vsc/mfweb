// HTTP/1.1 请求解析器测试。

#include <mfweb/net/http/parser.hpp>
#include <mfweb/test/test.hpp>

#include <string>

namespace {

mfweb::http::request parse_one(const std::string& raw, mfweb::http::request_parser& parser) {
    mfweb::http::request req;
    std::size_t consumed = 0;
    const auto r = parser.feed(raw.data(), raw.size(), req, consumed);
    MFW_CHECK_MSG(r == mfweb::http::request_parser::result::complete, "请求未完整解析");
    MFW_CHECK_EQ(consumed, raw.size());
    return req;
}

}  // namespace

MFW_TEST(http_parser, simple_get) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n", parser);

    MFW_CHECK_EQ(req.method_, mfweb::http::method::get);
    MFW_CHECK_EQ(req.target, std::string_view("/index.html"));
    MFW_CHECK_EQ(req.minor_version, 1);
    MFW_CHECK_EQ(req.headers.size(), static_cast<std::size_t>(1));
    MFW_CHECK_EQ(req.header("host"), std::string_view("example.com"));
    MFW_CHECK(req.keep_alive);
    MFW_CHECK(!req.chunked);
    MFW_CHECK_EQ(req.body.size(), static_cast<std::size_t>(0));
}

MFW_TEST(http_parser, header_name_is_case_insensitive) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("GET / HTTP/1.1\r\nCoNtEnT-LenGtH: 5\r\n\r\nhello", parser);
    MFW_CHECK_EQ(req.header("content-length"), std::string_view("5"));
    MFW_CHECK_EQ(req.body, std::string_view("hello"));
}

MFW_TEST(http_parser, content_length_body) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("POST /submit HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello", parser);
    MFW_CHECK_EQ(req.method_, mfweb::http::method::post);
    MFW_CHECK_EQ(req.body, std::string_view("hello"));
}

MFW_TEST(http_parser, connection_close_disables_keepalive) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("GET / HTTP/1.1\r\nConnection: close\r\n\r\n", parser);
    MFW_CHECK_MSG(!req.keep_alive, "Connection: close 时应关闭 keep-alive");
}

MFW_TEST(http_parser, http10_defaults_to_close) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("GET / HTTP/1.0\r\n\r\n", parser);
    MFW_CHECK_EQ(req.minor_version, 0);
    MFW_CHECK_MSG(!req.keep_alive, "HTTP/1.0 默认不 keep-alive");
}

MFW_TEST(http_parser, http10_keepalive_header) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", parser);
    MFW_CHECK(req.keep_alive);
}

MFW_TEST(http_parser, chunked_transfer_encoding_detected) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", parser);
    MFW_CHECK(req.chunked);
}

MFW_TEST(http_parser, header_value_trimmed) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("GET / HTTP/1.1\r\nX-A:   spaced  \r\n\r\n", parser);
    MFW_CHECK_EQ(req.header("x-a"), std::string_view("spaced"));
}

MFW_TEST(http_parser, multiple_headers) {
    mfweb::http::request_parser parser;
    const auto req =
        parse_one("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nA: 3\r\n\r\n", parser);
    MFW_CHECK_EQ(req.headers.size(), static_cast<std::size_t>(3));
    MFW_CHECK_EQ(req.header("a"), std::string_view("1"));  // 返回第一个
}

MFW_TEST(http_parser, header_too_large) {
    mfweb::http::request_parser parser(/*max_head=*/64);
    mfweb::http::request req;
    std::size_t consumed = 0;
    const std::string big(128, 'x');
    const auto r = parser.feed(("GET / HTTP/1.1\r\nX: " + big + "\r\n\r\n").data(),
                               big.size() + 20, req, consumed);
    MFW_CHECK_EQ(r, mfweb::http::request_parser::result::error);
}

MFW_TEST(http_parser, unsupported_http_version) {
    mfweb::http::request_parser parser;
    mfweb::http::request req;
    std::size_t consumed = 0;
    const auto r = parser.feed("GET / HTTP/2.0\r\n\r\n", 18, req, consumed);
    MFW_CHECK_EQ(r, mfweb::http::request_parser::result::error);
}

MFW_TEST(http_parser, malformed_request_line) {
    mfweb::http::request_parser parser;
    mfweb::http::request req;
    std::size_t consumed = 0;
    const auto r = parser.feed("GARBAGE\r\n\r\n", 11, req, consumed);
    MFW_CHECK_EQ(r, mfweb::http::request_parser::result::error);
}

MFW_TEST(http_parser, pipelined_requests_report_partial_consumption) {
    // 两个请求粘在一起：第一次 feed 应只消费第一个请求的字节
    mfweb::http::request_parser parser;
    const std::string two = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
    mfweb::http::request req;
    std::size_t consumed = 0;
    const auto r = parser.feed(two.data(), two.size(), req, consumed);

    MFW_CHECK_EQ(r, mfweb::http::request_parser::result::complete);
    MFW_CHECK_MSG(consumed < two.size(), "应当只消费第一个请求");
    MFW_CHECK_EQ(req.target, std::string_view("/a"));
    MFW_CHECK_EQ(two.substr(consumed), std::string_view("GET /b HTTP/1.1\r\n\r\n"));
}

MFW_TEST(http_parser, empty_get_with_head) {
    mfweb::http::request_parser parser;
    const auto req = parse_one("HEAD /big.bin HTTP/1.1\r\n\r\n", parser);
    MFW_CHECK_EQ(req.method_, mfweb::http::method::head);
}
