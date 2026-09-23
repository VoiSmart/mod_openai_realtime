"""PCM16 recording measurements used by the integration assertions."""

import math
import sys
import wave
from array import array

PCM16_BYTES_PER_SAMPLE = 2
AUDIBLE_SAMPLE_THRESHOLD = 500


def read_mono_pcm16(path):
    with wave.open(str(path), "rb") as recording:
        channels = recording.getnchannels()
        sample_width = recording.getsampwidth()
        sample_rate = recording.getframerate()
        frames = recording.readframes(recording.getnframes())

    if sample_width != PCM16_BYTES_PER_SAMPLE:
        raise AssertionError(f"expected PCM16 recording, got {sample_width * 8}-bit samples")

    samples = array("h")
    samples.frombytes(frames)
    if sys.byteorder == "big":
        samples.byteswap()
    if channels > 1:
        samples = array(
            "h",
            (sum(samples[index : index + channels]) // channels for index in range(0, len(samples), channels)),
        )
    return sample_rate, samples


def goertzel_power(samples, sample_rate, frequency):
    coefficient = 2 * math.cos(2 * math.pi * frequency / sample_rate)
    previous = 0.0
    previous_previous = 0.0
    for sample in samples:
        current = sample + coefficient * previous - previous_previous
        previous_previous = previous
        previous = current
    return previous_previous**2 + previous**2 - coefficient * previous * previous_previous


def dominant_frequency(samples, sample_rate):
    frequencies = range(300, min(3000, sample_rate // 2), 25)
    return max(frequencies, key=lambda frequency: goertzel_power(samples, sample_rate, frequency))


def audible_duration(samples, sample_rate):
    return audio_activity_metrics(samples, sample_rate, window_ms=20)[1]


def audio_activity_metrics(samples, sample_rate, window_ms=5, threshold=AUDIBLE_SAMPLE_THRESHOLD):
    window_size = max(sample_rate * window_ms // 1000, 1)
    runs = 0
    active = False
    active_windows = 0
    first_active_offset = None
    last_active_end = None
    for offset in range(0, len(samples) - window_size + 1, window_size):
        window = samples[offset : offset + window_size]
        mean_square = sum(sample * sample for sample in window) / len(window)
        window_active = mean_square >= threshold**2
        if window_active:
            active_windows += 1
            if first_active_offset is None:
                first_active_offset = offset
            last_active_end = offset + window_size
            if not active:
                runs += 1
        active = window_active

    active_duration = active_windows * window_size / sample_rate
    active_span = 0.0 if first_active_offset is None else (last_active_end - first_active_offset) / sample_rate
    return runs, active_duration, active_span


def longest_silent_gap(samples, sample_rate, window_ms=20, threshold=AUDIBLE_SAMPLE_THRESHOLD):
    window_size = max(sample_rate * window_ms // 1000, 1)
    activity = []
    for offset in range(0, len(samples) - window_size + 1, window_size):
        window = samples[offset : offset + window_size]
        mean_square = sum(sample * sample for sample in window) / len(window)
        activity.append(mean_square >= threshold**2)

    try:
        first_active = activity.index(True)
        last_active = len(activity) - 1 - activity[::-1].index(True)
    except ValueError:
        return 0.0

    longest = 0
    current = 0
    for active in activity[first_active : last_active + 1]:
        if active:
            longest = max(longest, current)
            current = 0
        else:
            current += 1
    return max(longest, current) * window_size / sample_rate


def tone_durations(samples, sample_rate, frequencies, window_ms=20, threshold=AUDIBLE_SAMPLE_THRESHOLD):
    durations = dict.fromkeys(frequencies, 0.0)
    for frequency, duration in tone_runs(samples, sample_rate, frequencies, window_ms, threshold):
        durations[frequency] += duration
    return durations


def tone_runs(samples, sample_rate, frequencies, window_ms=20, threshold=AUDIBLE_SAMPLE_THRESHOLD):
    """Return ordered (frequency, duration) runs of audible, matching tones."""
    window_size = max(sample_rate * window_ms // 1000, 1)
    runs = []
    for offset in range(0, len(samples) - window_size + 1, window_size):
        window = samples[offset : offset + window_size]
        mean_square = sum(sample * sample for sample in window) / len(window)
        if mean_square < threshold**2:
            continue
        powers = {frequency: goertzel_power(window, sample_rate, frequency) for frequency in frequencies}
        winner = max(powers, key=powers.get)
        # A matching tone must account for at least half the window's signal energy.
        tone_energy = 2 * powers[winner] / window_size
        if tone_energy < mean_square * window_size / 2:
            continue
        if runs and runs[-1][0] == winner:
            runs[-1] = (winner, runs[-1][1] + window_size / sample_rate)
        else:
            runs.append((winner, window_size / sample_rate))
    return runs
