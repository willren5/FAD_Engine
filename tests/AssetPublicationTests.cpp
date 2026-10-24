#include <fad/engine/assets/WavetableBank.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
using namespace fadmodel1::dsp::wavetable;
int checks = 0;
void require(bool condition, const char* message)
{ ++checks; if (!condition) throw std::runtime_error(message); }
struct Attachment final : fad::engine::AssetAttachment
{
    std::atomic<int>& destroyed;
    std::atomic<int>& destroyedOffControl;
    std::thread::id control;
    Attachment(std::atomic<int>& d, std::atomic<int>& wrong) : destroyed(d), destroyedOffControl(wrong), control(std::this_thread::get_id()) {}
    ~Attachment() override { ++destroyed; if (std::this_thread::get_id() != control) ++destroyedOffControl; }
};
std::shared_ptr<const WavetableData> pcm(float value, std::shared_ptr<const fad::engine::AssetAttachment> attachment = {})
{
    auto asset = std::make_shared<WavetableData>();
    asset->frameSize = 256; asset->frameCount = 1;
    asset->frames.assign(1, std::vector<float>(256, value));
    asset->sampleLength = 256; asset->sampleRate = 48000;
    asset->applicationData = std::move(attachment);
    return asset;
}
}
int main()
{
    try
    {
        std::atomic<int> destroyed{0}, wrongThread{0};
        RuntimeWavetableBank bank;
        RuntimeWavetableBank::CommitRetireList retired;
        auto first = pcm(.25f, std::make_shared<Attachment>(destroyed, wrongThread));
        require(bank.publishNoiseSampleSlot(1, first, retired), "first PCM publication failed");
        retired = {}; first.reset();
        const RuntimeWavetableBank::PublishedOwnership* pinned = nullptr;
        {
            auto resolution = bank.scopedPublishedOwnershipResolution();
            pinned = bank.getPublishedOwnershipPointer();
            pinned->audioReaderCount.fetch_add(1, std::memory_order_acq_rel);
            const auto generation = pinned->generation, signalGeneration = pinned->signalGeneration;
            require(bank.publishNoiseSampleSlot(1, pcm(-.25f), retired), "replacement PCM publication failed");
            const auto* current = bank.getPublishedOwnershipPointer();
            require(current->generation == generation + 1 && current->signalGeneration == signalGeneration,
                    "Noise changed the signal cohort generation");
            require(retired.count == 1 && retired.retiredObjects[0].activeAudioReaders == &pinned->audioReaderCount,
                    "retirement did not retain the pinned owner");
            require(retired.retiredObjects[0].pendingPublishedOwnershipReaders->load() == 1,
                    "resolver guard was detached from retirement");
        }
        require(destroyed == 0, "attachment destroyed while old cohort still pinned");
        bool oldSampleStayedValid = false;
        std::thread audio([&] {
            oldSampleStayedValid = pinned->userTables[0]->sampleAtPosition(0, .5f) == .25f;
            pinned->audioReaderCount.fetch_sub(1, std::memory_order_acq_rel);
        });
        audio.join();
        require(oldSampleStayedValid && destroyed == 0, "reader release destroyed an asset on the reader thread");
        require(retired.retiredObjects[0].activeAudioReaders->load() == 0
                && retired.retiredObjects[0].pendingPublishedOwnershipReaders->load() == 0,
                "retirement guards did not settle");
        retired = {};
        require(destroyed == 1 && wrongThread == 0, "metadata was not reclaimed with its asset on control thread");
        const auto before = bank.getGeneration();
        bank.queueUserWavetableData(2, pcm(.5f));
        require(!bank.publishNoiseSampleSlot(1, pcm(.75f), retired) && bank.getGeneration() == before,
                "Noise raced a pending full transaction");
        require(bank.commitPendingTransaction(&retired), "queued transaction failed");
        const auto snapshot = bank.getPublishedPointerSnapshot();
        require(snapshot.owner->signalGeneration == snapshot.generation && snapshot.generation == before + 1,
                "full transaction did not advance both generations");
        require(snapshot.userTables[0]->frames[0][0] == -.25f && snapshot.userTables[1]->frames[0][0] == .5f,
                "published cohort was torn");
        std::cout << "AssetPublicationTests PASS checks=" << checks << " attachment_destroyed_off_control=" << wrongThread << '\n';
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
