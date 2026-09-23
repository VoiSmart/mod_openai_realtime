#include "playback_queue.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace audio_stream {

// Capacity and maximum chunk size share a unit; the constructor declaration documents their order.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
PlaybackQueue::PlaybackQueue(std::size_t capacity_samples, std::size_t max_chunk_samples)
    : m_capacity_samples(capacity_samples), m_max_chunk_samples(max_chunk_samples) {
    if (m_capacity_samples == 0 || m_max_chunk_samples == 0) {
        throw std::invalid_argument("playback queue limits must be greater than zero");
    }
}

PlaybackQueue::PushResult PlaybackQueue::push(std::vector<int16_t> audio) {
    const std::size_t total = audio.size();
    if (total == 0) {
        return PushResult::Accepted;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (total > m_capacity_samples - m_queued_samples) {
        if (!m_overflowed) {
            m_overflowed = true;
            return PushResult::OverflowStarted;
        }
        return PushResult::OverflowOngoing;
    }

    if (total <= m_max_chunk_samples) {
        m_chunks.push(std::move(audio));
    } else {
        for (std::size_t offset = 0; offset < total;) {
            const std::size_t chunk_size = std::min(m_max_chunk_samples, total - offset);
            const int16_t *chunk_begin = audio.data() + offset;
            m_chunks.emplace(chunk_begin, chunk_begin + chunk_size);
            offset += chunk_size;
        }
    }
    m_queued_samples += total;
    return PushResult::Accepted;
}

bool PlaybackQueue::pop(std::vector<int16_t>& audio) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_chunks.empty()) {
        return false;
    }

    audio = std::move(m_chunks.front());
    m_chunks.pop();
    m_queued_samples -= audio.size();
    // Hysteresis prevents overflow logging from repeatedly toggling near capacity.
    if (m_overflowed && m_queued_samples <= (m_capacity_samples - 1) / 2) {
        m_overflowed = false;
    }
    return true;
}

void PlaybackQueue::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_chunks.empty()) {
        m_chunks.pop();
    }
    m_queued_samples = 0;
    m_overflowed = false;
}

} // namespace audio_stream
