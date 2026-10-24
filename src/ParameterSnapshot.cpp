#include <fad/engine/parameters/ParameterSnapshot.h>

namespace fadmodel1::parameters
{
ParameterSnapshot::ParameterSnapshot() noexcept
{
    oscillators[0].level = 0.8f;
    oscillators[1].level = 0.0f;

    ampEnvelope = {0.01f, 0.25f, 1.0f, 0.3f};
    filterEnvelope = {0.02f, 0.3f, 0.0f, 0.4f};
    modEnvelope = {0.02f, 0.3f, 1.0f, 0.3f};

    if (! modulation.lfos.empty())
        modulation.lfos[0].rateHz = 1.25f;
    for (size_t i = 1; i < modulation.lfos.size(); ++i)
        modulation.lfos[i].rateHz = 0.4f;
}

}
