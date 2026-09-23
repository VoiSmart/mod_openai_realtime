#include "stream_protocol.h"

#include <cctype>
#include <cstring>

namespace stream_protocol {

JsonMessageType classify_json_message(const char *type) {
    if (!type) {
        return JsonMessageType::Unhandled;
    }
    if (std::strstr(type, "error")) {
        return JsonMessageType::Error;
    }
    if (std::strcmp(type, "input_audio_buffer.speech_started") == 0) {
        return JsonMessageType::SpeechStarted;
    }
    if (std::strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
        return JsonMessageType::SpeechStopped;
    }
    if (std::strcmp(type, "response.output_audio.delta") == 0) {
        return JsonMessageType::AudioDelta;
    }
    if (std::strcmp(type, "response.output_audio.done") == 0) {
        return JsonMessageType::AudioDone;
    }
    return JsonMessageType::Unhandled;
}

bool json_depth_exceeded(const char *json, int max_depth) {
    if (!json || max_depth < 0) {
        return true;
    }

    // Bound nesting before FreeSWITCH's recursive cJSON parser uses the calling thread's stack.
    int depth = 0;
    bool in_string = false;
    for (; *json; ++json) {
        const char c = *json;
        if (in_string) {
            if (c == '\\' && json[1]) {
                ++json;
            } else if (c == '"') {
                in_string = false;
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) {
                return true;
            }
        } else if (c == '}' || c == ']') {
            if (depth > 0) {
                --depth;
            }
        }
    }
    return false;
}

bool validate_ws_uri(const char *url, char *destination, std::size_t destination_size) {
    if (!url || !destination || destination_size == 0 || std::strpbrk(url, "\r\n")) {
        return false;
    }

    const char *host_start = nullptr;
    const char *host_end = nullptr;

    if (std::strncmp(url, "ws://", 5) == 0) {
        host_start = url + 5;
    } else if (std::strncmp(url, "wss://", 6) == 0) {
        host_start = url + 6;
    } else {
        return false;
    }

    if (*host_start == '[') {
        host_end = host_start + 1;
        while (*host_end && *host_end != ']') {
            const unsigned char c = static_cast<unsigned char>(*host_end);
            if (!std::isxdigit(c) && *host_end != ':' && *host_end != '.') {
                return false;
            }
            ++host_end;
        }
        if (*host_end != ']' || host_end == host_start + 1) {
            return false;
        }
        ++host_end;
    } else {
        host_end = host_start;
        while (*host_end && *host_end != ':' && *host_end != '/' && *host_end != '?') {
            const unsigned char c = static_cast<unsigned char>(*host_end);
            if (!std::isalnum(c) && *host_end != '-' && *host_end != '.') {
                return false;
            }
            ++host_end;
        }
        if (host_start == host_end) {
            return false;
        }
    }

    if (*host_end == ':') {
        const char *port_start = host_end + 1;
        const char *p = port_start;
        long port = 0;
        while (*p && *p != '/' && *p != '?') {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (!std::isdigit(c)) {
                return false;
            }
            port = port * 10 + (*p - '0');
            if (port > 65535) {
                return false;
            }
            ++p;
        }
        if (p == port_start || port == 0) {
            return false;
        }
        host_end = p;
    }

    if (*host_end != '\0' && *host_end != '/' && *host_end != '?') {
        return false;
    }

    const std::size_t length = std::strlen(url);
    const std::size_t extra_slash = *host_end == '?' ? 1 : 0;
    if (length >= destination_size - extra_slash) {
        return false;
    }
    if (extra_slash) {
        // The pinned IXWebSocket parser needs a path before the query.
        const std::size_t authority_length = static_cast<std::size_t>(host_end - url);
        std::memcpy(destination, url, authority_length);
        destination[authority_length] = '/';
        std::memcpy(destination + authority_length + 1, host_end, length - authority_length + 1);
    } else {
        std::memcpy(destination, url, length + 1);
    }
    return true;
}

bool is_valid_utf8(const char *text) {
    if (!text) {
        return false;
    }

    const auto *bytes = reinterpret_cast<const unsigned char *>(text);
    while (*bytes) {
        const unsigned char lead = bytes[0];
        if (lead <= 0x7F) {
            ++bytes;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            if ((bytes[1] & 0xC0) != 0x80) {
                return false;
            }
            bytes += 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            const unsigned char second = bytes[1];
            if ((second & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (lead == 0xE0 && second < 0xA0) ||
                (lead == 0xED && second > 0x9F)) {
                return false;
            }
            bytes += 3;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            const unsigned char second = bytes[1];
            if ((second & 0xC0) != 0x80 || (bytes[2] & 0xC0) != 0x80 || (bytes[3] & 0xC0) != 0x80 ||
                (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F)) {
                return false;
            }
            bytes += 4;
        } else {
            return false;
        }
    }
    return true;
}

} // namespace stream_protocol
