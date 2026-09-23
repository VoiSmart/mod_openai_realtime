#ifndef PLAYBACK_QUEUE_H
#define PLAYBACK_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <vector>

namespace audio_stream {

class PlaybackQueue {
  public:
    enum class PushResult : std::uint8_t {
        Accepted,
        OverflowStarted,
        OverflowOngoing,
    };

    PlaybackQueue(std::size_t capacity_samples, std::size_t max_chunk_samples);

    PushResult push(std::vector<int16_t> audio);
    bool pop(std::vector<int16_t>& audio);
    void clear();

  private:
    const std::size_t m_capacity_samples;
    const std::size_t m_max_chunk_samples;
    std::queue<std::vector<int16_t>> m_chunks;
    std::mutex m_mutex;
    std::size_t m_queued_samples = 0;
    bool m_overflowed = false;
};

} // namespace audio_stream

#endif // PLAYBACK_QUEUE_H
