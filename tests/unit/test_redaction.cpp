// Unit tests for the privacy redactor and recorder config validation.
#include "lgtest.hpp"

#include "export/redaction.hpp"
#include "capture/recorder_config.hpp"

using namespace lg::export_;

static void test_phone() {
    LG_CASE("redact CN mobile numbers");
    Redactor r;
    std::string s = "call 13812345678 now";
    auto st = r.apply(s);
    CHECK_STREQ(s, "call <phone> now");
    CHECK_EQ(st.phone, 1);
}

static void test_email() {
    LG_CASE("redact emails");
    Redactor r;
    std::string s = "contact alice.bob@example.com please";
    r.apply(s);
    CHECK(s.find("@example.com") == std::string::npos);
    CHECK(s.find("<email>") != std::string::npos);
}

static void test_id_and_bank() {
    LG_CASE("redact id card and bank card, keep ordinary counts");
    Redactor r;
    std::string s = "id 110101199003077651 card 6222020200011112222 done";
    auto st = r.apply(s);
    CHECK_EQ(st.id_card, 1);
    CHECK_EQ(st.bank_card, 1);
    CHECK(s.find("110101199003077651") == std::string::npos);

}

static void test_plain_counts_untouched() {
    LG_CASE("ordinary short numbers stay (no over-masking)");
    Redactor r;
    std::string s = "retry 3 times, queue 12 deep";
    r.apply(s);
    CHECK_STREQ(s, "retry 3 times, queue 12 deep");
}

static void test_ipv4() {
    LG_CASE("redact ipv4");
    Redactor r;
    std::string s = "connect to 192.168.0.10 ok";
    r.apply(s);
    CHECK(s.find("192.168.0.10") == std::string::npos);
    CHECK(s.find("<ip>") != std::string::npos);
}

static void test_secret_assignment() {
    LG_CASE("redact token/secret assignments");
    Redactor r;
    std::string s = R"(api_key="sk-live-ABCDEF123456" and token=abcXYZ999 end)";
    auto st = r.apply(s);
    CHECK_EQ(st.secret, 2);
    CHECK(s.find("sk-live-ABCDEF123456") == std::string::npos);
    CHECK(s.find("abcXYZ999") == std::string::npos);
}

static void test_custom_token_and_disable() {
    LG_CASE("custom token masking and family disable");
    Redactor r;
    r.add_custom_token("SUPERSECRET-VALUE");
    std::string s = "header SUPERSECRET-VALUE";
    r.apply(s);
    CHECK(s.find("SUPERSECRET-VALUE") == std::string::npos);

    Redactor r2;
    r2.enable(SensitiveKind::Phone, false);
    std::string s2 = "call 13812345678";
    auto st = r2.apply(s2);
    CHECK_STREQ(s2, "call 13812345678");
    CHECK_EQ(st.phone, 0);
}

static void test_config_normalize() {
    LG_CASE("config normalization and validation");
    using namespace lg::cap;
    RecorderConfig c;
    std::string err;
    CHECK(!c.normalize(&err)); // empty path rejected

    RecorderConfig ok;
    ok.ring_path = "/tmp/a/app.ring";
    ok.ring_slots = 3000; // not power of two -> rounds to 4096
    ok.level = 9;        // clamps to 5
    CHECK(ok.normalize());
    CHECK_EQ(ok.ring_slots, 4096u);
    CHECK_EQ(ok.level, (uint16_t)5);
    CHECK(!ok.output_dir.empty());

    auto d = RecorderConfig::for_path("/data/x.ring");
    CHECK_EQ(d.ring_slots, 1u << 18);
}

void run_all_tests() {
    test_phone();
    test_email();
    test_id_and_bank();
    test_plain_counts_untouched();
    test_ipv4();
    test_secret_assignment();
    test_custom_token_and_disable();
    test_config_normalize();
}

LG_TEST_MAIN()
