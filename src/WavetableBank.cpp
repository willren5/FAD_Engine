#include <fad/engine/assets/WavetableBank.h>

namespace fadmodel1::dsp::wavetable
{
RuntimeWavetableBank::RuntimeWavetableBank(std::shared_ptr<const WavetableData> builtin)
    : builtinTablePtr(std::move(builtin))
{
    if (builtinTablePtr == nullptr)
    {
        static const auto table = std::make_shared<const WavetableData>(createBuiltinWavetable());
        builtinTablePtr = table;
    }
    publishedOwnership = makePublishedOwnership({}, 0);
    publishedOwnershipRaw.store(publishedOwnership.get(), publishedOwnershipHandoffMemoryOrder);
}

std::shared_ptr<const RuntimeWavetableBank::PublishedOwnership> RuntimeWavetableBank::makePublishedOwnership(
    const std::array<std::shared_ptr<const WavetableData>, maxUserSlots>& tables,
    uint32_t publishedGeneration) const
{
    auto ownership = std::make_shared<PublishedOwnership>();
    ownership->builtinTable = builtinTablePtr;
    ownership->userTables = tables;
    ownership->generation = publishedGeneration;
    ownership->signalGeneration = publishedGeneration;
    return ownership;
}

void RuntimeWavetableBank::publishCurrentTables(CommitRetireList* retireList) noexcept
{
    std::array<std::shared_ptr<const WavetableData>, maxUserSlots> snapshot{};
    for (size_t i = 0; i < userTables.size(); ++i)
        snapshot[i] = std::atomic_load_explicit(&userTables[i], std::memory_order_acquire);

    auto nextOwnership = makePublishedOwnership(snapshot, generation.load(std::memory_order_acquire));
    auto previousOwnership = std::atomic_load_explicit(&publishedOwnership, std::memory_order_acquire);
    std::atomic_store_explicit(&publishedOwnership, nextOwnership, std::memory_order_release);
    publishedOwnershipRaw.store(nextOwnership.get(),
                                publishedOwnershipHandoffMemoryOrder);

    if (retireList != nullptr && previousOwnership != nullptr)
        retireList->addPublishedOwnership(std::move(previousOwnership), &pendingPublishedOwnershipReaders);
}

void RuntimeWavetableBank::applyTransaction(const PendingTransaction& transaction,
                                     CommitRetireList* retireList) noexcept
{
    auto currentTables = snapshotUserTables(nullptr);
    auto nextTables = currentTables;

    if (transaction.clearAllUserSlots)
        nextTables = {};

    for (const auto& write : transaction.writes)
    {
        if (write.slotIndex < 1 || write.slotIndex > maxUserSlots)
            continue;
        nextTables[static_cast<size_t>(write.slotIndex - 1)] = write.wavetable;
    }

    const auto publishedGeneration = generation.fetch_add(1, std::memory_order_acq_rel) + 1u;
    auto nextOwnership = makePublishedOwnership(nextTables, publishedGeneration);
    auto previousOwnership = std::atomic_load_explicit(&publishedOwnership, std::memory_order_acquire);

    std::atomic_store_explicit(&publishedOwnership, nextOwnership, std::memory_order_release);
    publishedOwnershipRaw.store(nextOwnership.get(),
                                publishedOwnershipHandoffMemoryOrder);

    if (retireList != nullptr && previousOwnership != nullptr)
        retireList->addPublishedOwnership(std::move(previousOwnership), &pendingPublishedOwnershipReaders);

    // Non-authoritative mirror for control-thread helpers. Public readers use
    // the published ownership above, so multi-slot commits are seen atomically.
    for (size_t i = 0; i < userTables.size(); ++i)
        std::atomic_store_explicit(&userTables[i], nextTables[i], std::memory_order_release);
}

void RuntimeWavetableBank::queueUserWavetableData(int slotIndex, std::shared_ptr<const WavetableData> data)
{
    if (slotIndex < 1 || slotIndex > maxUserSlots || ! data || ! data->isValid())
        return;

    const std::scoped_lock lock(pendingTransactionMutex);
    auto next = pendingTransaction != nullptr ? std::make_shared<PendingTransaction>(*pendingTransaction)
                                              : std::make_shared<PendingTransaction>();
    next->writes.push_back({ slotIndex, std::move(data) });
    pendingTransaction = std::shared_ptr<const PendingTransaction>(std::move(next));
}

void RuntimeWavetableBank::queueClearUserWavetable(int slotIndex)
{
    if (slotIndex < 1 || slotIndex > maxUserSlots)
        return;

    const std::scoped_lock lock(pendingTransactionMutex);
    auto next = pendingTransaction != nullptr ? std::make_shared<PendingTransaction>(*pendingTransaction)
                                              : std::make_shared<PendingTransaction>();
    next->writes.push_back({ slotIndex, {} });
    pendingTransaction = std::shared_ptr<const PendingTransaction>(std::move(next));
}

bool RuntimeWavetableBank::hasPendingTransaction() const noexcept
{
    const std::scoped_lock lock(pendingTransactionMutex);
    return pendingTransaction != nullptr;
}

void RuntimeWavetableBank::discardPendingTransaction() noexcept
{
    const std::scoped_lock lock(pendingTransactionMutex);
    pendingTransaction.reset();
}

bool RuntimeWavetableBank::commitPendingTransaction(CommitRetireList* retireList) noexcept
{
    const std::scoped_lock lock(pendingTransactionMutex);
    if (pendingTransaction == nullptr)
        return false;

    auto transaction = pendingTransaction;
    applyTransaction(*transaction, retireList);
    pendingTransaction.reset();
    if (retireList != nullptr)
        retireList->add(std::static_pointer_cast<const void>(std::move(transaction)));
    return true;
}

bool RuntimeWavetableBank::publishNoiseSampleSlot(int slotIndex,
                                               std::shared_ptr<const WavetableData> asset,
                                               CommitRetireList& retireList)
{
    if (slotIndex < 1 || slotIndex > maxUserSlots
        || (asset != nullptr && (!asset->isValid() || asset->sampleLength == 0))
        || retireList.count >= static_cast<int>(retireList.retiredObjects.size()))
        return false;
    const std::scoped_lock lock(pendingTransactionMutex);
    if (pendingTransaction != nullptr)
        return false;
    auto previous = std::atomic_load_explicit(&publishedOwnership, std::memory_order_acquire);
    auto tables = previous->userTables;
    tables[static_cast<size_t>(slotIndex - 1)] = std::move(asset);
    auto next = std::make_shared<PublishedOwnership>();
    next->builtinTable = previous->builtinTable;
    next->userTables = tables;
    next->signalGeneration = previous->signalGeneration;
    next->generation = generation.fetch_add(1, std::memory_order_acq_rel) + 1u;
    std::atomic_store_explicit(&publishedOwnership,
        std::shared_ptr<const PublishedOwnership>(next), std::memory_order_release);
    publishedOwnershipRaw.store(next.get(), publishedOwnershipHandoffMemoryOrder);
    retireList.addPublishedOwnership(std::move(previous), &pendingPublishedOwnershipReaders);
    for (size_t i = 0; i < userTables.size(); ++i)
        std::atomic_store_explicit(&userTables[i], tables[i], std::memory_order_release);
    return true;
}

bool RuntimeWavetableBank::hasQueuedOrPublishedUserWavetable(int slotIndex) const
{
    if (slotIndex < 1 || slotIndex > maxUserSlots)
        return false;

    std::shared_ptr<const PendingTransaction> pending;
    {
        const std::scoped_lock lock(pendingTransactionMutex);
        pending = pendingTransaction;
    }

    const auto snapshot = snapshotUserTables(pending.get());
    return snapshot[static_cast<size_t>(slotIndex - 1)] != nullptr;
}

std::shared_ptr<const WavetableData> RuntimeWavetableBank::getWavetable(int slotIndex) const noexcept
{
    if (slotIndex == builtinSlotIndex)
        return builtinTablePtr;

    if (slotIndex < 1 || slotIndex > maxUserSlots)
        return builtinTablePtr;

    const auto ownership = std::atomic_load_explicit(&publishedOwnership, std::memory_order_acquire);
    auto ptr = ownership != nullptr
        ? ownership->userTables[static_cast<size_t>(slotIndex - 1)]
        : std::shared_ptr<const WavetableData>{};
    return ptr ? ptr : builtinTablePtr;
}

RuntimeWavetableBank::PublishedSnapshot RuntimeWavetableBank::getPublishedSnapshot() const noexcept
{
    PublishedSnapshot snapshot{};
    auto ownership = std::atomic_load_explicit(&publishedOwnership, std::memory_order_acquire);
    if (ownership == nullptr)
        return snapshot;

    snapshot.builtinTableOwner = ownership->builtinTable;
    snapshot.userTableOwners = ownership->userTables;
    snapshot.builtinTable = snapshot.builtinTableOwner.get();
    snapshot.generation = ownership->generation;

    for (size_t i = 0; i < snapshot.userTables.size(); ++i)
        snapshot.userTables[i] = snapshot.userTableOwners[i] ? snapshot.userTableOwners[i].get() : snapshot.builtinTable;

    return snapshot;
}

RuntimeWavetableBank::PublishedPointerSnapshot RuntimeWavetableBank::getPublishedPointerSnapshot() const noexcept
{
    PublishedPointerSnapshot snapshot{};
    snapshot.owner = std::atomic_load_explicit(&publishedOwnership, std::memory_order_acquire);
    if (snapshot.owner == nullptr)
        return snapshot;

    snapshot.builtinTable = snapshot.owner->builtinTable.get();
    snapshot.generation = snapshot.owner->generation;
    for (size_t i = 0; i < snapshot.userTables.size(); ++i)
        snapshot.userTables[i] = snapshot.owner->userTables[i] ? snapshot.owner->userTables[i].get() : snapshot.builtinTable;

    return snapshot;
}

const RuntimeWavetableBank::PublishedOwnership* RuntimeWavetableBank::getPublishedOwnershipPointer() const noexcept
{
    return publishedOwnershipRaw.load(publishedOwnershipHandoffMemoryOrder);
}

int RuntimeWavetableBank::getLoadedSlotCount() const noexcept
{
    int count = 0;
    const auto ownership = std::atomic_load_explicit(&publishedOwnership, std::memory_order_acquire);
    if (ownership == nullptr)
        return count;

    for (const auto& table : ownership->userTables)
        if (table)
            ++count;

    return count;
}

std::array<std::shared_ptr<const WavetableData>, RuntimeWavetableBank::maxUserSlots>
RuntimeWavetableBank::snapshotUserTables(const PendingTransaction* pending) const
{
    std::array<std::shared_ptr<const WavetableData>, maxUserSlots> snapshot{};
    if (const auto ownership = std::atomic_load_explicit(&publishedOwnership, std::memory_order_acquire);
        ownership != nullptr)
    {
        snapshot = ownership->userTables;
    }

    if (pending == nullptr)
        return snapshot;

    if (pending->clearAllUserSlots)
        snapshot = {};

    for (const auto& write : pending->writes)
    {
        if (write.slotIndex < 1 || write.slotIndex > maxUserSlots)
            continue;
        snapshot[static_cast<size_t>(write.slotIndex - 1)] = write.wavetable;
    }

    return snapshot;
}

int RuntimeWavetableBank::findFirstEmptyUserSlot() const
{
    std::shared_ptr<const PendingTransaction> pending;
    {
        const std::scoped_lock lock(pendingTransactionMutex);
        pending = pendingTransaction;
    }
    const auto snapshot = snapshotUserTables(pending.get());

    for (int i = 0; i < maxUserSlots; ++i)
    {
        if (! snapshot[static_cast<size_t>(i)])
            return i + 1;
    }

    return -1;
}
}
