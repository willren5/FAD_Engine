#pragma once

namespace fad::engine
{
// Optional application-owned metadata, prepared before publication and immutable
// afterwards. Renderers never inspect this object. The containing asset and its
// attachment are destroyed only by the existing off-audio retirement owner.
// No audio, host, file or service capability is required by this interface.
struct AssetAttachment
{
    virtual ~AssetAttachment() = default;
};
}
