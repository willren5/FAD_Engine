#pragma once

#include <cstdint>
#include <utility>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "WavetableData.h"

namespace fadmodel1::dsp::wavetable
{
class RuntimeWavetableBank
{
public:
    static constexpr int builtinSlotIndex = 0;
    static constexpr int maxUserSlots = 16;

    struct PendingSlotWrite
    {
        int slotIndex = 0;
        std::shared_ptr<const WavetableData> wavetable;
    };

    struct PendingTransaction
    {
        bool clearAllUserSlots = false;
        std::vector<PendingSlotWrite> writes;
    };

    struct PublishedOwnership
    {
        std::shared_ptr<const WavetableData> builtinTable;
        std::array<std::shared_ptr<const WavetableData>, maxUserSlots> userTables{};
        uint32_t generation = 0;
        uint32_t signalGeneration = 0;
        mutable std::atomic<uint32_t> audioReaderCount{0};
    };

    struct RetiredObject
    {
        std::shared_ptr<const void> object;
        const std::atomic<uint32_t>* activeAudioReaders = nullptr;
        const std::atomic<uint32_t>* pendingPublishedOwnershipReaders = nullptr;
    };

    struct CommitRetireList
    {
        void add(std::shared_ptr<const void> object,
                 const std::atomic<uint32_t>* activeAudioReaders = nullptr,
                 const std::atomic<uint32_t>* pendingOwnershipReadersGuard = nullptr) noexcept
        {
            if (!object || count >= static_cast<int>(retiredObjects.size()))
                return;

            auto& entry = retiredObjects[static_cast<size_t>(count++)];
            entry.object = std::move(object);
            entry.activeAudioReaders = activeAudioReaders;
            entry.pendingPublishedOwnershipReaders = pendingOwnershipReadersGuard;
        }

        void addPublishedOwnership(
            std::shared_ptr<const PublishedOwnership> ownership,
            const std::atomic<uint32_t>* pendingOwnershipReadersGuard = nullptr) noexcept
        {
            const auto* activeReaders = ownership != nullptr ? &ownership->audioReaderCount : nullptr;
            add(std::static_pointer_cast<const void>(std::move(ownership)),
                activeReaders,
                pendingOwnershipReadersGuard);
        }

        std::array<RetiredObject, maxUserSlots + 2> retiredObjects{};
        int count = 0;
    };

    struct PublishedSnapshot
    {
        std::shared_ptr<const WavetableData> builtinTableOwner;
        std::array<std::shared_ptr<const WavetableData>, maxUserSlots> userTableOwners{};
        const WavetableData* builtinTable = nullptr;
        std::array<const WavetableData*, maxUserSlots> userTables{};
        uint32_t generation = 0;
    };

    struct PublishedPointerSnapshot
    {
        std::shared_ptr<const PublishedOwnership> owner;
        const WavetableData* builtinTable = nullptr;
        std::array<const WavetableData*, maxUserSlots> userTables{};
        uint32_t generation = 0;
    };

    class PublishedOwnershipResolutionGuard
    {
    public:
        explicit PublishedOwnershipResolutionGuard(const RuntimeWavetableBank& bankIn) noexcept
            : bank(&bankIn)
        {
            // This counter and publishedOwnershipRaw form one handoff protocol.
            // Sequential consistency lets retirement distinguish a resolver
            // that started before its guard sample from one that must observe
            // the already-published replacement pointer.
            bank->pendingPublishedOwnershipReaders.fetch_add(
                1,
                RuntimeWavetableBank::publishedOwnershipHandoffMemoryOrder);
        }

        PublishedOwnershipResolutionGuard(PublishedOwnershipResolutionGuard&& other) noexcept
            : bank(std::exchange(other.bank, nullptr))
        {
        }

        PublishedOwnershipResolutionGuard& operator=(PublishedOwnershipResolutionGuard&& other) noexcept
        {
            if (this == &other)
                return *this;

            release();
            bank = std::exchange(other.bank, nullptr);
            return *this;
        }

        PublishedOwnershipResolutionGuard(const PublishedOwnershipResolutionGuard&) = delete;
        PublishedOwnershipResolutionGuard& operator=(const PublishedOwnershipResolutionGuard&) = delete;

        ~PublishedOwnershipResolutionGuard()
        {
            release();
        }

    private:
        void release() noexcept
        {
            if (bank == nullptr)
                return;

            bank->pendingPublishedOwnershipReaders.fetch_sub(
                1,
                RuntimeWavetableBank::publishedOwnershipHandoffMemoryOrder);
            bank = nullptr;
        }

        const RuntimeWavetableBank* bank = nullptr;
    };

    // Construction/preparation and every queue/commit/shared-owner query below
    // belong to non-audio services. They may allocate and use the control mutex.
    explicit RuntimeWavetableBank(std::shared_ptr<const WavetableData> builtin = {});

    void queueUserWavetableData(int slotIndex, std::shared_ptr<const WavetableData> data);
    void queueClearUserWavetable(int slotIndex);
    bool hasPendingTransaction() const noexcept;
    void discardPendingTransaction() noexcept;
    // With active readers, the caller must supply and drain retireList only
    // after both reader guards settle. nullptr is for quiescent control use.
    bool commitPendingTransaction(CommitRetireList* retireList = nullptr) noexcept;
    bool hasQueuedOrPublishedUserWavetable(int slotIndex) const;
    // Control-thread only. The caller gates parameter publication and retires
    // the returned ownership through the engine's existing retirement queue.
    bool publishNoiseSampleSlot(int slotIndex, std::shared_ptr<const WavetableData> asset,
                                CommitRetireList& retireList);

    std::shared_ptr<const WavetableData> getWavetable(int slotIndex) const noexcept;
    PublishedSnapshot getPublishedSnapshot() const noexcept;
    PublishedPointerSnapshot getPublishedPointerSnapshot() const noexcept;
    // Audio-safe raw resolution: hold scopedPublishedOwnershipResolution(),
    // pin audioReaderCount on the returned cohort, then release the resolver.
    // Do not use the shared-owner snapshot queries above in a callback.
    const PublishedOwnership* getPublishedOwnershipPointer() const noexcept;
    PublishedOwnershipResolutionGuard scopedPublishedOwnershipResolution() const noexcept
    {
        return PublishedOwnershipResolutionGuard{ *this };
    }
    int getLoadedSlotCount() const noexcept;

    int findFirstEmptyUserSlot() const;

    uint32_t getGeneration() const noexcept { return generation.load(std::memory_order_acquire); }


protected:
    // Control-thread services in the private facade use this same owner.
    static constexpr auto publishedOwnershipHandoffMemoryOrder =
        std::memory_order_seq_cst;
    std::shared_ptr<const PublishedOwnership> makePublishedOwnership(
        const std::array<std::shared_ptr<const WavetableData>, maxUserSlots>& tables,
        uint32_t publishedGeneration) const;
    void publishCurrentTables(CommitRetireList* retireList = nullptr) noexcept;
    void applyTransaction(const PendingTransaction& transaction, CommitRetireList* retireList = nullptr) noexcept;
    std::array<std::shared_ptr<const WavetableData>, maxUserSlots> snapshotUserTables(const PendingTransaction* pending) const;

    std::shared_ptr<const WavetableData> builtinTablePtr;
    std::array<std::shared_ptr<const WavetableData>, maxUserSlots> userTables;
    std::atomic<uint32_t> generation{0};
    mutable std::mutex pendingTransactionMutex;
    std::shared_ptr<const PendingTransaction> pendingTransaction;
    std::shared_ptr<const PublishedOwnership> publishedOwnership;
    std::atomic<const PublishedOwnership*> publishedOwnershipRaw{nullptr};
    mutable std::atomic<uint32_t> pendingPublishedOwnershipReaders{0};
};
} // namespace fadmodel1::dsp::wavetable
