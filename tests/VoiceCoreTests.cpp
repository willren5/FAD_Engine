#include <fad/engine/synth/VoiceCore.h>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
}
int main()
{
    try
    {
        using Voice = fadmodel1::dsp::synth::VoiceCore;
        fadmodel1::parameters::ParameterSnapshot patch;
        patch.oscillators[0].level = .2f; patch.oscillators[0].unisonRand = 0; patch.oscillators[0].drift = 0;
        patch.oscillators[1].enabled = false; patch.filter.enabled = false;
        patch.ampEnvelope = {.001f, .01f, 1.f, .01f};
        Voice voice; voice.prepare({48000, 64, 2}); voice.setParameterSnapshot(&patch);
        voice.setMidiSource(fadmodel1::dsp::midi::Source::touch);
        voice.startRealtimeNote(2, 60, .8f, 8192, 17, false);
        require(voice.isVoiceActive() && voice.isKeyDown() && voice.getRealtimeMidiNote() == 60
                && voice.getRealtimeMidiChannel() == 2 && voice.getRealtimeStartOrder() == 17, "note ownership not activated coherently");
        std::array<float, 64> left{}, right{}; std::array<float*, 2> data{left.data(), right.data()};
        fad::engine::AudioBufferView audio(data.data(), 2, 64);
        bool audible = false;
        for (int block = 0; block < 32; ++block)
        {
            audio.clear(); voice.renderNextBlock(audio, 0, 64);
            for (size_t i = 0; i < left.size(); ++i)
            { require(std::isfinite(left[i]) && std::isfinite(right[i]), "nonfinite voice sample"); audible |= std::abs(left[i]) > 1.e-7f; }
        }
        require(audible, "standalone VoiceCore did not sound");
        voice.setSustainPedalDown(true); voice.setKeyDown(false);
        require(voice.isSustainPedalDown() && !voice.isKeyDown() && voice.isVoiceActive(), "pedal/key ownership diverged");
        voice.setSustainPedalDown(false); voice.releasePedalHeldNote();
        for (int block = 0; block < 64; ++block) { audio.clear(); voice.renderNextBlock(audio, 0, 64); }
        require(!voice.isVoiceActive() && voice.getRealtimeMidiNote() == -1 && !voice.isKeyDown()
                && !voice.isSustainPedalDown() && !voice.isSostenutoPedalDown(), "retired note retained ownership");
        require(voice.getMidiSource() == fadmodel1::dsp::midi::Source::touch, "lifecycle reset changed source identity");
        voice.startRealtimeNote(3, 67, .7f, 8192, 18, false); voice.setSostenutoPedalDown(true); voice.hardReset();
        require(!voice.isVoiceActive() && !voice.isSostenutoPedalDown() && voice.getRealtimeMidiNote() == -1, "hard reset did not clear the single note owner");
        std::cout << "VoiceCoreTests PASS standalone_non_silent=true single_note_owner=true\n";
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
