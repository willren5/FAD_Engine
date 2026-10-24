#pragma once

#include <algorithm>
#include <cstdint>
#include <fad/engine/AudioView.h>
#include <array>
#include <atomic>
#include <cmath>

namespace fadmodel1::dsp::diagnostics
{

struct SessionMetricsSnapshot
{
    uint32_t noteOnCount = 0;
    uint32_t pitchWheelCount = 0;
    uint32_t modWheelCount = 0;
    uint32_t aftertouchCount = 0;
    std::array<uint32_t, 8> velocityBins{};
    uint32_t notesLow = 0;
    uint32_t notesMid = 0;
    uint32_t notesHigh = 0;
    uint32_t chordCount = 0;
    uint32_t sustainPedalCount = 0;
    uint32_t activeNoteCount = 0;
    uint32_t maxActiveNoteCount = 0;
    uint32_t voiceStealCount = 0;
    std::array<uint32_t, 12> pitchClassHistogram{};
    float lastBpm = 120.0f;
    uint32_t bpmChangeCount = 0;
    uint32_t transportStartCount = 0;
    uint64_t totalPlayingSamples = 0;
    int lastTimeSigNum = 4;
    int lastTimeSigDen = 4;
    uint64_t sampleCount = 0;
    uint64_t clippingSampleCount = 0;
    uint64_t nonFiniteSampleCount = 0;
    uint64_t emergencyLimitedSampleCount = 0;
};

/// Lightweight per-session metrics collected on the audio thread.
///
/// Contract:
/// - Audio-thread recorders (recordNoteOn / recordActiveNoteCount /
///   recordVoiceSteal / recordAudioBlock / recordAudioSummary /
///   recordEmergencyLimitedSamples) are lock-free, allocation-free, and
///   bounded: relaxed atomics only, no locks, no I/O.
/// - Message-thread consumers take cumulative snapshot() baselines and compute
///   deltaSince(baseline) instead of resetting while the audio thread writes.
///   Counter deltas saturate non-negative: if reset() ran between baseline and
///   now, the delta falls back to the post-reset cumulative value instead of
///   underflowing.
/// - maxActiveNoteCount is a session peak, not a counter: deltaSince passes the
///   current peak through unchanged. Editor/session-reopen isolation therefore
///   requires resetSessionPeaks() at the boundary before taking a new baseline.
/// - activeNoteCount, lastBpm, and lastTimeSigNum/Den are gauges; deltaSince
///   reports their current values.
/// - reset() is a message-thread session-boundary operation; concurrent audio
///   writes can lose at most in-flight increments, never tear a field.
struct SessionMetrics
{
    static_assert(std::atomic<uint32_t>::is_always_lock_free,
                  "audio-thread counters must be lock-free");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "audio-thread sample accumulators must be lock-free");
    static_assert(std::atomic<float>::is_always_lock_free
                      && std::atomic<int>::is_always_lock_free,
                  "audio-thread gauges must be lock-free");

    /// Kept as a metrics-facing alias for tests and consumers. The producer
    /// authority is AudioSafetyStage::emergencyKneeStart.
    static constexpr float emergencyLimitThreshold = 4.0f;

    // MIDI behavior
    std::atomic<uint32_t> noteOnCount{0};
    std::atomic<uint32_t> pitchWheelCount{0};
    std::atomic<uint32_t> modWheelCount{0};
    std::atomic<uint32_t> aftertouchCount{0};

    // Velocity histogram: 8 bins of 16 values each (0-15, 16-31, ..., 112-127)
    std::array<std::atomic<uint32_t>, 8> velocityBins{};

    // Note range: low (C0-B2 = 0-47), mid (C3-B4 = 48-71), high (C5+ = 72-127)
    std::atomic<uint32_t> notesLow{0};
    std::atomic<uint32_t> notesMid{0};
    std::atomic<uint32_t> notesHigh{0};

    // MIDI behavior (expanded)
    std::atomic<uint32_t> chordCount{0};        // simultaneous notes >= 3
    std::atomic<uint32_t> sustainPedalCount{0}; // CC64 on events
    // Actual Voice::isVoiceActive count, including audible release/sustain
    // tails. This is a gauge, not the number of physically held keys.
    std::atomic<uint32_t> activeNoteCount{0};
    std::atomic<uint32_t> maxActiveNoteCount{0};
    std::atomic<uint32_t> voiceStealCount{0};

    // Pitch class histogram for key estimation (C=0, C#=1, ..., B=11)
    std::array<std::atomic<uint32_t>, 12> pitchClassHistogram{};

    // Environment / Transport
    std::atomic<float> lastBpm{120.0f};
    std::atomic<uint32_t> bpmChangeCount{0};
    std::atomic<uint32_t> transportStartCount{0};
    std::atomic<uint64_t> totalPlayingSamples{0};
    std::atomic<int> lastTimeSigNum{4};
    std::atomic<int> lastTimeSigDen{4};

    // Audio output. Only metrics with an authoritative production writer are
    // exposed; speculative jitter/RMS/centroid placeholders were removed.
    std::atomic<uint64_t> sampleCount{0};
    std::atomic<uint64_t> clippingSampleCount{0};
    std::atomic<uint64_t> nonFiniteSampleCount{0};
    std::atomic<uint64_t> emergencyLimitedSampleCount{0};

    /// Record a note-on (call from audio thread).
    /// currentActiveNotes is the actual Voice::isVoiceActive count observed
    /// AFTER the note-on has taken effect (post allocation / steal /
    /// mono-legato). Passing speculative pre-allocation counts corrupts
    /// chordCount and maxActiveNoteCount.
    void recordNoteOn(int noteNumber, int velocity, int currentActiveNotes) noexcept
    {
        noteOnCount.fetch_add(1, std::memory_order_relaxed);

        // Velocity histogram
        const auto bin = static_cast<size_t>(std::clamp(velocity / 16, 0, 7));
        velocityBins[bin].fetch_add(1, std::memory_order_relaxed);

        // Callers should clamp, but this boundary must not index out of range
        // for hostile or buggy note numbers.
        const auto safeNote = std::clamp(noteNumber, 0, 127);

        // Note range
        if (safeNote < 48) notesLow.fetch_add(1, std::memory_order_relaxed);
        else if (safeNote < 72) notesMid.fetch_add(1, std::memory_order_relaxed);
        else notesHigh.fetch_add(1, std::memory_order_relaxed);

        // Pitch class for key estimation
        pitchClassHistogram[static_cast<size_t>(safeNote % 12)].fetch_add(1, std::memory_order_relaxed);

        // Chord detection
        if (currentActiveNotes >= 3)
            chordCount.fetch_add(1, std::memory_order_relaxed);

        recordActiveNoteCount(currentActiveNotes);
    }

    void recordActiveNoteCount(int currentActiveNotes) noexcept
    {
        const auto safeActiveNotes =
            static_cast<uint32_t>(std::max(0, currentActiveNotes));
        activeNoteCount.store(safeActiveNotes, std::memory_order_relaxed);
        auto observedMax = maxActiveNoteCount.load(std::memory_order_relaxed);
        while (observedMax < safeActiveNotes
               && ! maxActiveNoteCount.compare_exchange_weak(observedMax,
                                                              safeActiveNotes,
                                                              std::memory_order_relaxed,
                                                              std::memory_order_relaxed))
        {
        }
    }

    void recordVoiceSteal() noexcept
    {
        voiceStealCount.fetch_add(1, std::memory_order_relaxed);
    }

    template<class Buffer> void recordAudioBlock(const Buffer& buffer) noexcept
    {
        const auto frames = std::max(0, buffer.getNumSamples());
        if (frames == 0)
            return;

        uint64_t clipped = 0;
        uint64_t nonFinite = 0;
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* samples = buffer.getReadPointer(channel);
            for (int sample = 0; sample < frames; ++sample)
            {
                const auto value = samples[sample];
                if (! std::isfinite(value))
                {
                    ++nonFinite;
                    ++clipped;
                }
                else
                {
                    const auto magnitude = std::abs(value);
                    if (magnitude > 1.0f)
                        ++clipped;
                }
            }
        }

        // This helper has no AudioSafetyStage intervention context, so it must
        // not label raw over-knee samples as actually emergency-limited.
        recordAudioSummary(frames, clipped, nonFinite);
    }

    void recordAudioSummary(int frames,
                            uint64_t clipped,
                            uint64_t nonFinite = 0,
                            uint64_t emergencyLimited = 0) noexcept
    {
        // emergencyLimited is reserved for the exact intervention count
        // returned by AudioSafetyStage::process, never raw over-knee input.
        if (frames <= 0)
            return;

        sampleCount.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
        clippingSampleCount.fetch_add(clipped, std::memory_order_relaxed);
        nonFiniteSampleCount.fetch_add(nonFinite, std::memory_order_relaxed);
        emergencyLimitedSampleCount.fetch_add(emergencyLimited, std::memory_order_relaxed);
    }

    void recordEmergencyLimitedSamples(uint64_t interventionCount) noexcept
    {
        emergencyLimitedSampleCount.fetch_add(interventionCount,
                                               std::memory_order_relaxed);
    }

    SessionMetricsSnapshot snapshot() const noexcept
    {
        SessionMetricsSnapshot result;
        result.noteOnCount = noteOnCount.load(std::memory_order_relaxed);
        result.pitchWheelCount = pitchWheelCount.load(std::memory_order_relaxed);
        result.modWheelCount = modWheelCount.load(std::memory_order_relaxed);
        result.aftertouchCount = aftertouchCount.load(std::memory_order_relaxed);
        for (size_t i = 0; i < velocityBins.size(); ++i)
            result.velocityBins[i] = velocityBins[i].load(std::memory_order_relaxed);
        result.notesLow = notesLow.load(std::memory_order_relaxed);
        result.notesMid = notesMid.load(std::memory_order_relaxed);
        result.notesHigh = notesHigh.load(std::memory_order_relaxed);
        result.chordCount = chordCount.load(std::memory_order_relaxed);
        result.sustainPedalCount = sustainPedalCount.load(std::memory_order_relaxed);
        result.activeNoteCount = activeNoteCount.load(std::memory_order_relaxed);
        result.maxActiveNoteCount = maxActiveNoteCount.load(std::memory_order_relaxed);
        result.voiceStealCount = voiceStealCount.load(std::memory_order_relaxed);
        for (size_t i = 0; i < pitchClassHistogram.size(); ++i)
            result.pitchClassHistogram[i] = pitchClassHistogram[i].load(std::memory_order_relaxed);
        result.lastBpm = lastBpm.load(std::memory_order_relaxed);
        result.bpmChangeCount = bpmChangeCount.load(std::memory_order_relaxed);
        result.transportStartCount = transportStartCount.load(std::memory_order_relaxed);
        result.totalPlayingSamples = totalPlayingSamples.load(std::memory_order_relaxed);
        result.lastTimeSigNum = lastTimeSigNum.load(std::memory_order_relaxed);
        result.lastTimeSigDen = lastTimeSigDen.load(std::memory_order_relaxed);
        result.sampleCount = sampleCount.load(std::memory_order_relaxed);
        result.clippingSampleCount = clippingSampleCount.load(std::memory_order_relaxed);
        result.nonFiniteSampleCount = nonFiniteSampleCount.load(std::memory_order_relaxed);
        result.emergencyLimitedSampleCount = emergencyLimitedSampleCount.load(std::memory_order_relaxed);
        return result;
    }

    SessionMetricsSnapshot deltaSince(const SessionMetricsSnapshot& baseline) const noexcept
    {
        const auto current = snapshot();
        SessionMetricsSnapshot result = current;

        result.noteOnCount = subtractCumulative(current.noteOnCount, baseline.noteOnCount);
        result.pitchWheelCount = subtractCumulative(current.pitchWheelCount, baseline.pitchWheelCount);
        result.modWheelCount = subtractCumulative(current.modWheelCount, baseline.modWheelCount);
        result.aftertouchCount = subtractCumulative(current.aftertouchCount, baseline.aftertouchCount);
        for (size_t i = 0; i < result.velocityBins.size(); ++i)
            result.velocityBins[i] = subtractCumulative(current.velocityBins[i], baseline.velocityBins[i]);
        result.notesLow = subtractCumulative(current.notesLow, baseline.notesLow);
        result.notesMid = subtractCumulative(current.notesMid, baseline.notesMid);
        result.notesHigh = subtractCumulative(current.notesHigh, baseline.notesHigh);
        result.chordCount = subtractCumulative(current.chordCount, baseline.chordCount);
        result.sustainPedalCount = subtractCumulative(
            current.sustainPedalCount, baseline.sustainPedalCount);
        result.voiceStealCount = subtractCumulative(current.voiceStealCount, baseline.voiceStealCount);
        for (size_t i = 0; i < result.pitchClassHistogram.size(); ++i)
        {
            result.pitchClassHistogram[i] = subtractCumulative(
                current.pitchClassHistogram[i], baseline.pitchClassHistogram[i]);
        }
        result.bpmChangeCount = subtractCumulative(current.bpmChangeCount, baseline.bpmChangeCount);
        result.transportStartCount = subtractCumulative(
            current.transportStartCount, baseline.transportStartCount);
        result.totalPlayingSamples = subtractCumulative(
            current.totalPlayingSamples, baseline.totalPlayingSamples);
        result.sampleCount = subtractCumulative(current.sampleCount, baseline.sampleCount);
        result.clippingSampleCount = subtractCumulative(
            current.clippingSampleCount, baseline.clippingSampleCount);
        result.nonFiniteSampleCount = subtractCumulative(
            current.nonFiniteSampleCount, baseline.nonFiniteSampleCount);
        result.emergencyLimitedSampleCount = subtractCumulative(
            current.emergencyLimitedSampleCount, baseline.emergencyLimitedSampleCount);
        return result;
    }

    /// Reset all counters (call from message thread at session boundary)
    void reset() noexcept
    {
        noteOnCount.store(0, std::memory_order_relaxed);
        pitchWheelCount.store(0, std::memory_order_relaxed);
        modWheelCount.store(0, std::memory_order_relaxed);
        aftertouchCount.store(0, std::memory_order_relaxed);
        chordCount.store(0, std::memory_order_relaxed);
        sustainPedalCount.store(0, std::memory_order_relaxed);
        activeNoteCount.store(0, std::memory_order_relaxed);
        maxActiveNoteCount.store(0, std::memory_order_relaxed);
        voiceStealCount.store(0, std::memory_order_relaxed);
        for (auto& bin : velocityBins)
            bin.store(0, std::memory_order_relaxed);
        notesLow.store(0, std::memory_order_relaxed);
        notesMid.store(0, std::memory_order_relaxed);
        notesHigh.store(0, std::memory_order_relaxed);
        for (auto& pc : pitchClassHistogram)
            pc.store(0, std::memory_order_relaxed);
        lastBpm.store(120.0f, std::memory_order_relaxed);
        bpmChangeCount.store(0, std::memory_order_relaxed);
        transportStartCount.store(0, std::memory_order_relaxed);
        totalPlayingSamples.store(0, std::memory_order_relaxed);
        lastTimeSigNum.store(4, std::memory_order_relaxed);
        lastTimeSigDen.store(4, std::memory_order_relaxed);
        sampleCount.store(0, std::memory_order_relaxed);
        clippingSampleCount.store(0, std::memory_order_relaxed);
        nonFiniteSampleCount.store(0, std::memory_order_relaxed);
        emergencyLimitedSampleCount.store(0, std::memory_order_relaxed);
    }

    void resetSessionPeaks() noexcept
    {
        maxActiveNoteCount.store(0, std::memory_order_relaxed);
    }

private:
    template <typename Integer>
    static Integer subtractCumulative(Integer current, Integer baseline) noexcept
    {
        return current >= baseline ? current - baseline : current;
    }
};

} // namespace fadmodel1::dsp::diagnostics
