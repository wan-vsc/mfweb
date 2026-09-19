// SHA-1 与 Base64 的标准测试向量验证。
// SHA-1 向量来自 FIPS 180-1 / 常见参考实现；Base64 向量来自 RFC 4648。

#include <mfweb/test/test.hpp>
#include <mfweb/util/base64.hpp>
#include <mfweb/util/sha1.hpp>

#include <string>
#include <vector>

MFW_TEST(sha1, standard_vectors) {
    // FIPS 180-1 附录 A
    MFW_CHECK_EQ(mfweb::util::sha1_hex(""), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    MFW_CHECK_EQ(mfweb::util::sha1_hex("abc"), std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    MFW_CHECK_EQ(mfweb::util::sha1_hex("The quick brown fox jumps over the lazy dog"),
                 std::string("2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"));
}

MFW_TEST(sha1, million_a_vector) {
    // FIPS 180-1：1,000,000 个 'a'
    const std::string big(1000000, 'a');
    MFW_CHECK_EQ(mfweb::util::sha1_hex(big),
                 std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

MFW_TEST(base64, rfc4648_vectors) {
    MFW_CHECK_EQ(mfweb::util::base64_encode(""), std::string(""));
    MFW_CHECK_EQ(mfweb::util::base64_encode("f"), std::string("Zg=="));
    MFW_CHECK_EQ(mfweb::util::base64_encode("fo"), std::string("Zm8="));
    MFW_CHECK_EQ(mfweb::util::base64_encode("foo"), std::string("Zm9v"));
    MFW_CHECK_EQ(mfweb::util::base64_encode("foob"), std::string("Zm9vYg=="));
    MFW_CHECK_EQ(mfweb::util::base64_encode("fooba"), std::string("Zm9vYmE="));
    MFW_CHECK_EQ(mfweb::util::base64_encode("foobar"), std::string("Zm9vYmFy"));
}

MFW_TEST(base64, roundtrip_random_lengths) {
    for (int len = 0; len < 256; ++len) {
        std::string input;
        input.reserve(static_cast<std::size_t>(len));
        for (int i = 0; i < len; ++i) {
            input.push_back(static_cast<char>((i * 7 + len * 13) & 0xFF));
        }
        const std::string encoded = mfweb::util::base64_encode(input);
        std::vector<std::uint8_t> decoded;
        MFW_CHECK_MSG(mfweb::util::base64_decode(encoded, decoded), "解码失败");
        MFW_CHECK_EQ(decoded.size(), input.size());
        for (std::size_t i = 0; i < input.size(); ++i) {
            MFW_CHECK_EQ(decoded[i], static_cast<std::uint8_t>(input[i]));
        }
    }
}

MFW_TEST(base64, rejects_invalid_input) {
    std::vector<std::uint8_t> out;
    MFW_CHECK_MSG(!mfweb::util::base64_decode("Zg", out), "长度非 4 倍数应拒绝");
    MFW_CHECK_MSG(!mfweb::util::base64_decode("@@@@", out), "非法字符应拒绝");
}
