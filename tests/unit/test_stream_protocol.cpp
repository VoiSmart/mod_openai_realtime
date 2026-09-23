#include "stream_protocol.h"
#include "test_support.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using stream_protocol::JsonMessageType;

void test_message_classification() {
    CHECK(stream_protocol::classify_json_message("error") == JsonMessageType::Error);
    CHECK(stream_protocol::classify_json_message("response.error") == JsonMessageType::Error);
    CHECK(stream_protocol::classify_json_message("input_audio_buffer.speech_started") ==
          JsonMessageType::SpeechStarted);
    CHECK(stream_protocol::classify_json_message("input_audio_buffer.speech_stopped") ==
          JsonMessageType::SpeechStopped);
    CHECK(stream_protocol::classify_json_message("response.output_audio.delta") == JsonMessageType::AudioDelta);
    CHECK(stream_protocol::classify_json_message("response.output_audio.done") == JsonMessageType::AudioDone);
    CHECK(stream_protocol::classify_json_message("response.created") == JsonMessageType::Unhandled);
    CHECK(stream_protocol::classify_json_message("") == JsonMessageType::Unhandled);
    CHECK(stream_protocol::classify_json_message(nullptr) == JsonMessageType::Unhandled);
}

void test_json_depth_limit() {
    CHECK(!stream_protocol::json_depth_exceeded(R"({"value":[1,2,3]})", 2));
    CHECK(stream_protocol::json_depth_exceeded(R"({"value":[{"nested":true}]})", 2));
    CHECK(!stream_protocol::json_depth_exceeded(R"({"text":"[[[ {{{"})", 1));
    CHECK(!stream_protocol::json_depth_exceeded(R"({"text":"escaped quote: \" and brace: {"})", 1));
    CHECK(!stream_protocol::json_depth_exceeded("{}", 1));
    CHECK(stream_protocol::json_depth_exceeded("{}", 0));
    CHECK(stream_protocol::json_depth_exceeded("]] [[[", 2));
    CHECK(stream_protocol::json_depth_exceeded(nullptr, 1));
    CHECK(stream_protocol::json_depth_exceeded("{}", -1));
}

void expect_valid_uri(const char *uri, const char *expected = nullptr) {
    char destination[256] = {};
    CHECK(stream_protocol::validate_ws_uri(uri, destination, sizeof(destination)));
    CHECK(std::strcmp(expected ? expected : uri, destination) == 0);
}

void expect_invalid_uri(const char *uri) {
    char destination[256] = "unchanged";
    CHECK(!stream_protocol::validate_ws_uri(uri, destination, sizeof(destination)));
    CHECK(std::strcmp(destination, "unchanged") == 0);
}

void test_valid_websocket_uris() {
    expect_valid_uri("ws://localhost");
    expect_valid_uri("ws://localhost:8080");
    expect_valid_uri("wss://api.openai.com/v1/realtime?model=test");
    expect_valid_uri("wss://example-host.test:1/path");
    expect_valid_uri("wss://example.test:65535?query=1", "wss://example.test:65535/?query=1");
    expect_valid_uri("ws://example.test?query=1", "ws://example.test/?query=1");
    expect_valid_uri("ws://example.test?token=user@other.test", "ws://example.test/?token=user@other.test");
    expect_valid_uri("ws://[::1]:8080?query=1", "ws://[::1]:8080/?query=1");
    expect_valid_uri("ws://[::1]?query=1", "ws://[::1]/?query=1");
    expect_valid_uri("ws://example.test?value=%0D%0A", "ws://example.test/?value=%0D%0A");
    expect_valid_uri("ws://127.0.0.1:8080");
    expect_valid_uri("ws://[::1]:8080/path");
    expect_valid_uri("wss://[2001:db8::1]/path");
    expect_valid_uri("wss://example.test/path%0D%0A?value=%00&other=a%20b");
}

void test_invalid_websocket_uris() {
    expect_invalid_uri("");
    expect_invalid_uri("http://example.test");
    expect_invalid_uri("ws://");
    expect_invalid_uri("ws://user@example.test");
    expect_invalid_uri("ws://example.test:");
    expect_invalid_uri("ws://example.test:0");
    expect_invalid_uri("ws://example.test:65536");
    expect_invalid_uri("ws://example.test:not-a-port");
    expect_invalid_uri("ws://2001:db8::1");
    expect_invalid_uri("ws://[invalid::address]");
    for (const char *line_break : {"\r", "\n", "\r\n"}) {
        expect_invalid_uri((std::string("ws://example.test/path") + line_break + "Injected:yes").c_str());
        expect_invalid_uri((std::string("wss://example.test?query=") + line_break + "Injected:yes").c_str());
    }

    char destination[5] = "keep";
    CHECK(!stream_protocol::validate_ws_uri("ws://localhost", destination, sizeof(destination)));
    CHECK(std::strcmp(destination, "keep") == 0);
    CHECK(!stream_protocol::validate_ws_uri(nullptr, destination, sizeof(destination)));
    CHECK(!stream_protocol::validate_ws_uri("ws://localhost", nullptr, sizeof(destination)));
}

void test_uri_destination_size_boundary() {
    const struct {
        const char *input;
        const char *output;
    } cases[] = {{"wss://example.test/path", "wss://example.test/path"},
                 {"wss://example.test?query=1", "wss://example.test/?query=1"}};
    for (const auto& uri : cases) {
        const std::size_t length = std::strlen(uri.output);
        std::vector<char> exact(length + 1, 'x');
        CHECK(stream_protocol::validate_ws_uri(uri.input, exact.data(), exact.size()));
        CHECK(std::strcmp(exact.data(), uri.output) == 0);

        std::vector<char> too_small(length, 'x');
        CHECK(!stream_protocol::validate_ws_uri(uri.input, too_small.data(), too_small.size()));
        CHECK(too_small == std::vector<char>(length, 'x'));
    }
}

void test_utf8_validation() {
    CHECK(stream_protocol::is_valid_utf8(""));
    CHECK(stream_protocol::is_valid_utf8("plain ASCII"));
    CHECK(stream_protocol::is_valid_utf8(u8"こんにちは"));
    CHECK(stream_protocol::is_valid_utf8(u8"\U00010348"));

    const char continuation_only[] = {static_cast<char>(0x80), '\0'};
    const char bad_continuation[] = {static_cast<char>(0xC2), 'A', '\0'};
    const char truncated_two_byte[] = {static_cast<char>(0xC2), '\0'};
    const char overlong_two_byte[] = {static_cast<char>(0xC0), static_cast<char>(0x80), '\0'};
    const char overlong_three_byte[] = {static_cast<char>(0xE0), static_cast<char>(0x80), static_cast<char>(0x80),
                                        '\0'};
    const char surrogate[] = {static_cast<char>(0xED), static_cast<char>(0xA0), static_cast<char>(0x80), '\0'};
    const char above_unicode_limit[] = {static_cast<char>(0xF4), static_cast<char>(0x90), static_cast<char>(0x80),
                                        static_cast<char>(0x80), '\0'};
    CHECK(!stream_protocol::is_valid_utf8(continuation_only));
    CHECK(!stream_protocol::is_valid_utf8(bad_continuation));
    CHECK(!stream_protocol::is_valid_utf8(truncated_two_byte));
    CHECK(!stream_protocol::is_valid_utf8(overlong_two_byte));
    CHECK(!stream_protocol::is_valid_utf8(overlong_three_byte));
    CHECK(!stream_protocol::is_valid_utf8(surrogate));
    CHECK(!stream_protocol::is_valid_utf8(above_unicode_limit));
    CHECK(!stream_protocol::is_valid_utf8(nullptr));
}

} // namespace

int main() {
    int failures = 0;
    failures += run_test("message classification", test_message_classification);
    failures += run_test("JSON depth limit", test_json_depth_limit);
    failures += run_test("valid WebSocket URIs", test_valid_websocket_uris);
    failures += run_test("invalid WebSocket URIs", test_invalid_websocket_uris);
    failures += run_test("WebSocket URI destination boundary", test_uri_destination_size_boundary);
    failures += run_test("UTF-8 validation", test_utf8_validation);
    return failures == 0 ? 0 : 1;
}
