#include "playback_queue.h"
#include "test_support.h"

#include <stdexcept>
#include <vector>

namespace {

using audio_stream::PlaybackQueue;

void expect_chunk(PlaybackQueue& queue, const std::vector<int16_t>& expected) {
    std::vector<int16_t> actual;
    CHECK(queue.pop(actual));
    CHECK(actual == expected);
}

void test_fifo_and_chunking() {
    PlaybackQueue queue(20, 4);
    const std::vector<int16_t> audio{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    CHECK(queue.push(audio) == PlaybackQueue::PushResult::Accepted);
    expect_chunk(queue, {1, 2, 3, 4});
    expect_chunk(queue, {5, 6, 7, 8});
    expect_chunk(queue, {9, 10});

    std::vector<int16_t> empty;
    CHECK(!queue.pop(empty));
}

void test_empty_audio_is_a_noop() {
    PlaybackQueue queue(10, 5);

    CHECK(queue.push({}) == PlaybackQueue::PushResult::Accepted);

    std::vector<int16_t> audio;
    CHECK(!queue.pop(audio));
}

void test_overflow_episode_and_recovery() {
    PlaybackQueue queue(10, 10);
    const std::vector<int16_t> six_samples(6, 1);
    const std::vector<int16_t> five_samples(5, 2);

    CHECK(queue.push(six_samples) == PlaybackQueue::PushResult::Accepted);
    CHECK(queue.push(five_samples) == PlaybackQueue::PushResult::OverflowStarted);
    CHECK(queue.push(five_samples) == PlaybackQueue::PushResult::OverflowOngoing);

    expect_chunk(queue, six_samples);
    CHECK(queue.push(six_samples) == PlaybackQueue::PushResult::Accepted);
    CHECK(queue.push(five_samples) == PlaybackQueue::PushResult::OverflowStarted);
}

void test_overflow_hysteresis_waits_for_queue_drain() {
    PlaybackQueue queue(10, 4);
    const std::vector<int16_t> nine_samples(9, 1);
    const std::vector<int16_t> two_samples(2, 2);
    const std::vector<int16_t> six_samples(6, 3);
    const std::vector<int16_t> ten_samples(10, 4);

    CHECK(queue.push(nine_samples) == PlaybackQueue::PushResult::Accepted);
    CHECK(queue.push(two_samples) == PlaybackQueue::PushResult::OverflowStarted);
    expect_chunk(queue, {1, 1, 1, 1});
    CHECK(queue.push(two_samples) == PlaybackQueue::PushResult::Accepted);
    CHECK(queue.push(six_samples) == PlaybackQueue::PushResult::OverflowOngoing);
    expect_chunk(queue, {1, 1, 1, 1});
    CHECK(queue.push(ten_samples) == PlaybackQueue::PushResult::OverflowStarted);
}

void test_clear_resets_queue_and_overflow_state() {
    PlaybackQueue queue(5, 5);
    const std::vector<int16_t> full(5, 1);
    const std::vector<int16_t> one(1, 2);

    CHECK(queue.push(full) == PlaybackQueue::PushResult::Accepted);
    CHECK(queue.push(one) == PlaybackQueue::PushResult::OverflowStarted);
    queue.clear();

    std::vector<int16_t> audio;
    CHECK(!queue.pop(audio));
    CHECK(queue.push(full) == PlaybackQueue::PushResult::Accepted);
    CHECK(queue.push(one) == PlaybackQueue::PushResult::OverflowStarted);
}

void test_single_sample_capacity_recovers_from_overflow() {
    PlaybackQueue queue(1, 1);
    const std::vector<int16_t> one_sample(1, 1);

    CHECK(queue.push(one_sample) == PlaybackQueue::PushResult::Accepted);
    CHECK(queue.push(one_sample) == PlaybackQueue::PushResult::OverflowStarted);
    expect_chunk(queue, one_sample);
    CHECK(queue.push(one_sample) == PlaybackQueue::PushResult::Accepted);
    CHECK(queue.push(one_sample) == PlaybackQueue::PushResult::OverflowStarted);
}

void test_invalid_limits_are_rejected() {
    bool rejected_capacity = false;
    try {
        PlaybackQueue queue(0, 1);
    } catch (const std::invalid_argument&) {
        rejected_capacity = true;
    }
    CHECK(rejected_capacity);

    bool rejected_chunk_size = false;
    try {
        PlaybackQueue queue(1, 0);
    } catch (const std::invalid_argument&) {
        rejected_chunk_size = true;
    }
    CHECK(rejected_chunk_size);
}

} // namespace

int main() {
    int failures = 0;
    failures += run_test("playback queue FIFO chunking", test_fifo_and_chunking);
    failures += run_test("playback queue empty input", test_empty_audio_is_a_noop);
    failures += run_test("playback queue overflow recovery", test_overflow_episode_and_recovery);
    failures += run_test("playback queue overflow hysteresis", test_overflow_hysteresis_waits_for_queue_drain);
    failures += run_test("playback queue clear", test_clear_resets_queue_and_overflow_state);
    failures +=
        run_test("single-sample capacity recovers from overflow", test_single_sample_capacity_recovers_from_overflow);
    failures += run_test("playback queue limits", test_invalid_limits_are_rejected);
    return failures == 0 ? 0 : 1;
}
