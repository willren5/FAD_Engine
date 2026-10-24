#include "../common/DemoPatch.h"
#include <fad/engine/fx/DynamicFxChain.h>
#include <fad/engine/synth/RenderEngine.h>
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
using namespace fad::engine;
using namespace fad::engine::example;
using Message = fadmodel1::dsp::midi::ShortMessage;
using Event = fadmodel1::dsp::midi::RenderMidiEvent;
struct Options
{
    std::filesystem::path output;
    int rate = 48000, block = 512, note = 60, velocity = 100;
    double seconds = 3;
    uint32_t seed = demoSeed;
    DemoState state;
};
template<class T> T number(std::string_view input)
{
    T value{};
    if constexpr (std::is_floating_point_v<T>)
    {
        // Floating from_chars in Apple's libc++ requires macOS 26. Preserve
        // the engine's deployment floor; CLI parsing is outside audio work.
        std::istringstream stream{std::string(input)};
        stream.imbue(std::locale::classic());
        if (!(stream >> std::noskipws >> value) || stream.peek() != std::char_traits<char>::eof())
            throw std::invalid_argument("Invalid numeric argument");
    }
    else
    {
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
            throw std::invalid_argument("Invalid numeric argument");
    }
    return value;
}
void usage()
{
    std::cout << "FadEngineRender --output NEW.wav [--sample-rate 48000] [--block-size 512]\n"
                 "  [--seconds 3] [--note 60] [--velocity 100] [--seed 1178682369]\n"
                 "  [--gain 0.5] [--tone 0.75] [--attack 0.04] [--release 0.3]\n"
                 "Stereo IEEE float32 WAV; generated oscillator; no account, presets or SDK.\n"
                 "Controls are normalized 0..1. Existing output files are refused.\n";
}
Options parse(int argc, char** argv)
{
    Options o;
    for (int arg = 1; arg < argc; ++arg)
    {
        const std::string_view key(argv[arg]);
        if (arg + 1 == argc) throw std::invalid_argument("Missing option value");
        const std::string_view value(argv[++arg]);
        if (key == "--output") o.output = argv[arg];
        else if (key == "--sample-rate") o.rate = number<int>(value);
        else if (key == "--block-size") o.block = number<int>(value);
        else if (key == "--seconds") o.seconds = number<double>(value);
        else if (key == "--note") o.note = number<int>(value);
        else if (key == "--velocity") o.velocity = number<int>(value);
        else if (key == "--seed") o.seed = number<uint32_t>(value);
        else if (key == "--gain") o.state.values[0] = number<float>(value);
        else if (key == "--tone") o.state.values[1] = number<float>(value);
        else if (key == "--attack") o.state.values[2] = number<float>(value);
        else if (key == "--release") o.state.values[3] = number<float>(value);
        else throw std::invalid_argument("Unknown option");
    }
    if (o.output.empty() || o.rate < 8000 || o.rate > 384000 || o.block < 1 || o.block > 8192
        || !std::isfinite(o.seconds) || o.seconds < .01 || o.seconds > 60
        || o.note < 0 || o.note > 127 || o.velocity < 1 || o.velocity > 127
        || o.seed == 0 || !valid(o.state))
        throw std::invalid_argument("Output or render option is outside its supported range");
    return o;
}
class WaveFile
{
public:
    WaveFile(const std::filesystem::path& path, uint32_t frames, uint32_t rate)
    {
        // C11 exclusive creation prevents truncating an existing file or symlink.
        file_.reset(std::fopen(path.string().c_str(), "wbx"));
        if (!file_) throw std::runtime_error("Cannot exclusively create output WAV");
        text("RIFF"); u32(50 + frames * 8); text("WAVE");
        text("fmt "); u32(18); u16(3); u16(2); u32(rate); u32(rate * 8);
        u16(8); u16(32); u16(0);
        text("fact"); u32(4); u32(frames);
        text("data"); u32(frames * 8);
    }
    void write(std::span<const unsigned char> bytes)
    {
        if (std::fwrite(bytes.data(), 1, bytes.size(), file_.get()) != bytes.size())
            throw std::runtime_error("WAV write failed; output may be incomplete");
    }
    void finish()
    {
        auto* file = file_.release();
        if (std::fclose(file) != 0) throw std::runtime_error("WAV close failed; output may be incomplete");
    }
private:
    void text(const char* data)
    { write({reinterpret_cast<const unsigned char*>(data), 4}); }
    void u16(uint16_t n)
    { const std::array<unsigned char, 2> b{static_cast<unsigned char>(n), static_cast<unsigned char>(n >> 8)}; write(b); }
    void u32(uint32_t n)
    { std::array<unsigned char, 4> b{}; for (unsigned i = 0; i < 4; ++i) b[i] = static_cast<unsigned char>(n >> (8 * i)); write(b); }
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{nullptr, &std::fclose};
};
void render(const Options& options)
{
    const auto frames64 = static_cast<uint64_t>(std::llround(options.seconds * options.rate));
    if (frames64 > (std::numeric_limits<uint32_t>::max() - 50) / 8)
        throw std::invalid_argument("Render exceeds RIFF WAV capacity");
    const auto frames = static_cast<uint32_t>(frames64);
    fadmodel1::dsp::wavetable::RuntimeWavetableBank bank;
    fadmodel1::dsp::fx::DynamicFxChainCore graph;
    fadmodel1::dsp::synth::RenderEngine engine(bank, graph);
    engine.stageIsolatedOfflineInitialSnapshot(makeSnapshot(options.state));
    engine.prepare({static_cast<double>(options.rate), static_cast<uint32_t>(options.block), 2}, options.seed);
    engine.setRealtimeProcessing(false);
    detail::PlanarBuffer buffer(2, options.block);
    std::vector<unsigned char> encoded(static_cast<size_t>(options.block) * 8);
    WaveFile file(options.output, frames, static_cast<uint32_t>(options.rate));
    const auto noteOff = static_cast<uint32_t>(static_cast<uint64_t>(frames) * 2 / 3);
    double peak = 0, energy = 0;
    for (uint32_t start = 0; start < frames;)
    {
        const auto count = std::min<uint32_t>(static_cast<uint32_t>(options.block), frames - start);
        std::array<Event, 2> events{};
        size_t eventCount = 0;
        if (start == 0) events[eventCount++] = {Message::noteOn(1, options.note, options.velocity), 0};
        if (noteOff >= start && noteOff < start + count)
            events[eventCount++] = {Message::noteOff(1, options.note), static_cast<int>(noteOff - start)};
        const auto view = buffer.view().subview(0, static_cast<int>(count));
        engine.render(view, {events.data(), eventCount});
        for (uint32_t sample = 0; sample < count; ++sample)
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto value = view.channel(channel)[sample];
                if (!std::isfinite(value)) throw std::runtime_error("Engine produced non-finite audio");
                peak = std::max(peak, std::abs(static_cast<double>(value)));
                energy += static_cast<double>(value) * value;
                const auto bits = std::bit_cast<uint32_t>(value);
                const auto offset = (static_cast<size_t>(sample) * 2 + static_cast<size_t>(channel)) * 4;
                for (unsigned byte = 0; byte < 4; ++byte) encoded[offset + byte] = static_cast<unsigned char>(bits >> (8 * byte));
            }
        file.write({encoded.data(), static_cast<size_t>(count) * 8});
        start += count;
    }
    file.finish();
    std::cout << "Wrote " << options.output << " frames=" << frames << " sample_rate=" << options.rate
              << " channels=2 format=float32 peak=" << peak << " rms=" << std::sqrt(energy / (frames * 2.0))
              << " finite=true seed=" << options.seed << '\n';
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string_view(argv[1]) == "--help") { usage(); return 0; }
        render(parse(argc, argv));
        return 0;
    }
    catch (const std::exception& error)
    { std::cerr << "FadEngineRender: " << error.what() << '\n'; return 1; }
}
