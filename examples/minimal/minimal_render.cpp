// Documentation example: one exclusively owned offline render instance.
// This is not a host adapter, a preset codec or a hardware callback audit.
#include <fad/engine/synth/RenderEngine.h>
#include <fad/engine/fx/DynamicFxChain.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

int main()
{
    try
    {
        namespace synth = fadmodel1::dsp::synth;
        namespace midi = fadmodel1::dsp::midi;
        constexpr int sampleRate = 48000;
        constexpr int blockSize = 256;
        constexpr int totalFrames = 48000;
        constexpr int noteOffFrame = 24000;
        constexpr std::uint32_t seed = 0x46414401;

        // Declaration order matters: engine borrows bank and graph, and must
        // finish its worker/lifetime teardown before they are destroyed.
        fadmodel1::dsp::wavetable::RuntimeWavetableBank bank;
        fadmodel1::dsp::fx::DynamicFxChainCore graph;
        synth::RenderEngine engine(bank, graph);

        fadmodel1::parameters::ParameterSnapshot sound;
        auto& osc = sound.oscillators[0];
        osc.enabled = true;
        osc.mode = fadmodel1::parameters::OscillatorMode::analog;
        osc.analogWave = fadmodel1::parameters::AnalogWave::sine;
        osc.level = 0.25f;
        osc.unisonVoices = 1;
        osc.unisonRand = 0.0f;
        osc.drift = 0.0f;
        osc.phaseDegrees = 0.0f;
        sound.oscillators[1].enabled = false;
        sound.subEnabled = false;
        sound.noise.enabled = false;
        sound.filter.enabled = false;
        sound.ampEnvelope.attackSeconds = 0.005f;
        sound.ampEnvelope.decaySeconds = 0.1f;
        sound.ampEnvelope.sustainLevel = 0.8f;
        sound.ampEnvelope.releaseSeconds = 0.15f;
        sound.dynamicFx.hasExplicitTopology = true;
        sound.dynamicFx.slotCount = 0;

        // Only for a new, exclusively owned offline instance before prepare.
        // A live host must use its coherent publication/acceptance path.
        engine.stageIsolatedOfflineInitialSnapshot(sound);
        engine.prepare({sampleRate, blockSize, 2}, seed);
        engine.setRealtimeProcessing(false);

        std::array<float, blockSize> left{}, right{};
        float* planes[] = {left.data(), right.data()};
        double peak = 0.0;
        double sumSquares = 0.0;

        for (int start = 0; start < totalFrames;)
        {
            const int count = std::min(blockSize, totalFrames - start);
            std::array<midi::RenderMidiEvent, 2> events{};
            std::size_t used = 0;
            if (start == 0)
                events[used++] = {midi::ShortMessage::noteOn(1, 60, 100), 0};
            if (noteOffFrame >= start && noteOffFrame < start + count)
                events[used++] = {midi::ShortMessage::noteOff(1, 60), noteOffFrame - start};

            // The pointer table, both sample planes and event array remain
            // alive throughout this synchronous render call.
            const fad::engine::AudioBufferView output(planes, 2, count);
            engine.render(output, {events.data(), used});

            // Offline measurements happen after the engine call; logging and
            // file encoding must not be moved into a real audio callback.
            for (int channel = 0; channel < 2; ++channel)
                for (int sample = 0; sample < count; ++sample)
                {
                    const float value = output.channel(channel)[sample];
                    if (!std::isfinite(value))
                        throw std::runtime_error("non-finite audio");
                    peak = std::max(peak, std::abs(static_cast<double>(value)));
                    sumSquares += static_cast<double>(value) * value;
                }
            start += count;
        }
        if (peak <= 1.0e-6)
            throw std::runtime_error("silent render; check snapshot and MIDI");
        std::cout << "FadEngineMinimal PASS frames=" << totalFrames
                  << " sample_rate=" << sampleRate << " channels=2 seed=" << seed
                  << " finite=true non_silent=true peak=" << peak
                  << " rms=" << std::sqrt(sumSquares / (2.0 * totalFrames)) << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FadEngineMinimal: " << error.what() << '\n';
        return 1;
    }
}
