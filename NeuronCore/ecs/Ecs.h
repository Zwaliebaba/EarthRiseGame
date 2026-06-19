#pragma once
// ECS — custom data-oriented entity-component system for NeuronCore.
//
// Handles: 32-bit — low 20 bits = slot index, high 12 bits = generation.
// Generation 0 at index 0 is the canonical null handle.
// Components: typed dense arrays keyed by ComponentTypeId (uint8_t [0, 64)).
// Iteration: deterministic (ascending entity-index order across alive entities).

#include <array>
#include <cassert>
#include <cstdint>
#include <span>
#include <vector>

namespace Neuron::ECS
{

// ---------------------------------------------------------------------------
// Entity handle
// ---------------------------------------------------------------------------
struct EntityHandle
{
    uint32_t value{ 0 };

    static constexpr uint32_t kIndexBits = 20;
    static constexpr uint32_t kGenBits   = 12;
    static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1u;
    static constexpr uint32_t kMaxIndex  = kIndexMask;
    static constexpr uint32_t kMaxGen    = (1u << kGenBits) - 1u;

    [[nodiscard]] uint32_t Index()      const noexcept { return value & kIndexMask; }
    [[nodiscard]] uint32_t Generation() const noexcept { return value >> kIndexBits; }
    [[nodiscard]] bool     IsNull()     const noexcept { return value == 0; }

    bool operator==(const EntityHandle&) const = default;
    bool operator!=(const EntityHandle&) const = default;

    static EntityHandle Make(uint32_t idx, uint32_t gen) noexcept
    {
        assert(idx <= kMaxIndex && gen <= kMaxGen);
        return { (gen << kIndexBits) | idx };
    }
    static EntityHandle Null() noexcept { return { 0 }; }
};

// ---------------------------------------------------------------------------
// Component type ID
// ---------------------------------------------------------------------------
using ComponentTypeId = uint8_t;
static constexpr ComponentTypeId kMaxComponentTypes = 64;

// Declare once per component type in a header; define (set value) once per TU
// via NEURON_DEFINE_COMPONENT(Type, N).
template <typename T>
struct ComponentId { static ComponentTypeId value; };

#define NEURON_DEFINE_COMPONENT(Type, IdNum) \
    template<> ::Neuron::ECS::ComponentTypeId ::Neuron::ECS::ComponentId<Type>::value = (IdNum)

// ---------------------------------------------------------------------------
// Type-erased component storage interface
// ---------------------------------------------------------------------------
struct IComponentStorage
{
    virtual ~IComponentStorage() = default;
    // Reserve slot for the entity at index; insert default-constructed element.
    virtual void     Insert(uint32_t entityIdx) = 0;
    // Swap-remove by entity index; fix up the moved entity's slot.
    // Returns the entity index that was moved into the removed slot
    // (UINT32_MAX if nothing was moved).
    virtual uint32_t Remove(uint32_t entityIdx) = 0;
    // Raw pointer to element by entity index; nullptr if not present.
    virtual void*    Get(uint32_t entityIdx)    = 0;
};

template <typename T>
struct ComponentStorage final : IComponentStorage
{
    // Sparse map: entityIndex -> dense index (UINT32_MAX = absent)
    std::vector<uint32_t> sparse; // indexed by entity index
    std::vector<T>        dense;  // packed component data
    std::vector<uint32_t> denseToEntity; // dense index -> entity index

    void EnsureSparse(uint32_t entityIdx)
    {
        if (entityIdx >= sparse.size())
            sparse.resize(static_cast<size_t>(entityIdx) + 1, UINT32_MAX);
    }

    void Insert(uint32_t entityIdx) override
    {
        EnsureSparse(entityIdx);
        assert(sparse[entityIdx] == UINT32_MAX && "component already present");
        sparse[entityIdx] = static_cast<uint32_t>(dense.size());
        dense.emplace_back();
        denseToEntity.push_back(entityIdx);
    }

    uint32_t Remove(uint32_t entityIdx) override
    {
        assert(entityIdx < sparse.size() && sparse[entityIdx] != UINT32_MAX);
        const uint32_t di      = sparse[entityIdx];
        const uint32_t lastDi  = static_cast<uint32_t>(dense.size()) - 1;
        uint32_t movedEntity   = UINT32_MAX;

        if (di != lastDi) {
            // Swap with last dense element
            dense[di]          = std::move(dense[lastDi]);
            movedEntity        = denseToEntity[lastDi];
            denseToEntity[di]  = movedEntity;
            sparse[movedEntity] = di;
        }

        dense.pop_back();
        denseToEntity.pop_back();
        sparse[entityIdx] = UINT32_MAX;
        return movedEntity;
    }

    void* Get(uint32_t entityIdx) override
    {
        if (entityIdx >= sparse.size() || sparse[entityIdx] == UINT32_MAX)
            return nullptr;
        return &dense[sparse[entityIdx]];
    }

    T& GetRef(uint32_t entityIdx)
    {
        assert(entityIdx < sparse.size() && sparse[entityIdx] != UINT32_MAX);
        return dense[sparse[entityIdx]];
    }

    // Iterate all present components in ascending entity-index order.
    // For deterministic order, sort denseToEntity (O(n log n)) or accept
    // insertion order. M0: iterate denseToEntity in insertion order (stable
    // when entities are created in ascending order, which they are).
    [[nodiscard]] std::span<T>       Data()       noexcept { return dense; }
    [[nodiscard]] std::span<const T> Data() const noexcept { return dense; }
    [[nodiscard]] std::span<const uint32_t> EntityIndices() const noexcept { return denseToEntity; }
};

// ---------------------------------------------------------------------------
// Entity record (stored in the sparse entity table)
// ---------------------------------------------------------------------------
struct EntityRecord
{
    uint32_t generation{ 0 };
    uint64_t componentMask{ 0 };
    bool     alive{ false };
};

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
class World
{
public:
    World()
    {
        // Index 0 is reserved for the null entity; push a sentinel record.
        m_records.push_back({ .generation = 0, .componentMask = 0, .alive = false });
    }

    ~World()
    {
        for (auto* s : m_storages)
            delete s;
    }

    // -- Component registration (call once per type before any entity uses it) --

    template <typename T>
    void RegisterComponent()
    {
        const ComponentTypeId id = ComponentId<T>::value;
        assert(id < kMaxComponentTypes);
        assert(m_storages[id] == nullptr && "already registered");
        m_storages[id] = new ComponentStorage<T>();
    }

    // -- Entity lifecycle --

    [[nodiscard]] EntityHandle CreateEntity()
    {
        uint32_t idx;
        if (!m_freeList.empty()) {
            idx = m_freeList.back();
            m_freeList.pop_back();
            // generation already incremented on destroy
        } else {
            idx = static_cast<uint32_t>(m_records.size());
            assert(idx <= EntityHandle::kMaxIndex);
            m_records.push_back({});
        }
        auto& rec    = m_records[idx];
        rec.alive         = true;
        rec.componentMask = 0;
        return EntityHandle::Make(idx, rec.generation);
    }

    void DestroyEntity(EntityHandle e)
    {
        if (!IsAlive(e)) return;
        auto& rec = m_records[e.Index()];
        // Remove all components
        for (ComponentTypeId cid = 0; cid < kMaxComponentTypes; ++cid) {
            if ((rec.componentMask >> cid) & 1u) {
                if (m_storages[cid])
                    m_storages[cid]->Remove(e.Index());
            }
        }
        rec.componentMask = 0;
        rec.alive         = false;
        rec.generation    = (rec.generation + 1) & EntityHandle::kMaxGen;
        m_freeList.push_back(e.Index());
    }

    [[nodiscard]] bool IsAlive(EntityHandle e) const noexcept
    {
        const uint32_t idx = e.Index();
        if (idx == 0 || idx >= m_records.size()) return false;
        const auto& rec = m_records[idx];
        return rec.alive && rec.generation == e.Generation();
    }

    // -- Component add/remove/get --

    template <typename T>
    T& AddComponent(EntityHandle e)
    {
        assert(IsAlive(e));
        const ComponentTypeId cid = ComponentId<T>::value;
        auto& rec = m_records[e.Index()];
        assert(!(rec.componentMask & (uint64_t(1) << cid)) && "component already present");
        assert(m_storages[cid] != nullptr && "component type not registered");
        m_storages[cid]->Insert(e.Index());
        rec.componentMask |= uint64_t(1) << cid;
        return static_cast<ComponentStorage<T>*>(m_storages[cid])->GetRef(e.Index());
    }

    template <typename T>
    void RemoveComponent(EntityHandle e)
    {
        assert(IsAlive(e));
        const ComponentTypeId cid = ComponentId<T>::value;
        auto& rec = m_records[e.Index()];
        assert((rec.componentMask & (uint64_t(1) << cid)) && "component not present");
        m_storages[cid]->Remove(e.Index());
        rec.componentMask &= ~(uint64_t(1) << cid);
    }

    template <typename T>
    [[nodiscard]] T* GetComponent(EntityHandle e) noexcept
    {
        if (!IsAlive(e)) return nullptr;
        const ComponentTypeId cid = ComponentId<T>::value;
        if (!m_storages[cid]) return nullptr;
        return static_cast<T*>(m_storages[cid]->Get(e.Index()));
    }

    template <typename T>
    [[nodiscard]] bool HasComponent(EntityHandle e) const noexcept
    {
        if (!IsAlive(e)) return false;
        const ComponentTypeId cid = ComponentId<T>::value;
        return (m_records[e.Index()].componentMask >> cid) & 1u;
    }

    // -- Deterministic iteration --
    // ForEach<T0, T1, ...>(fn) calls fn(T0&, T1&, ...) for every alive entity
    // that has all listed components, in ascending entity-index order.
    template <typename... Ts, typename Fn>
    void ForEach(Fn&& fn)
    {
        // Component IDs are runtime statics (set by NEURON_DEFINE_COMPONENT),
        // so the required mask is computed at run time, not constexpr.
        const uint64_t required = (... | (uint64_t(1) << ComponentId<Ts>::value));

        const uint32_t n = static_cast<uint32_t>(m_records.size());
        for (uint32_t idx = 1; idx < n; ++idx) {
            const auto& rec = m_records[idx];
            if (!rec.alive) continue;
            if ((rec.componentMask & required) != required) continue;
            fn(static_cast<ComponentStorage<Ts>*>(m_storages[ComponentId<Ts>::value])->GetRef(idx)...);
        }
    }

    // Read-only variant
    template <typename... Ts, typename Fn>
    void ForEach(Fn&& fn) const
    {
        // Component IDs are runtime statics (set by NEURON_DEFINE_COMPONENT),
        // so the required mask is computed at run time, not constexpr.
        const uint64_t required = (... | (uint64_t(1) << ComponentId<Ts>::value));
        const uint32_t n = static_cast<uint32_t>(m_records.size());
        for (uint32_t idx = 1; idx < n; ++idx) {
            const auto& rec = m_records[idx];
            if (!rec.alive) continue;
            if ((rec.componentMask & required) != required) continue;
            fn(*static_cast<const Ts*>(m_storages[ComponentId<Ts>::value]->Get(idx))...);
        }
    }

    [[nodiscard]] uint32_t EntityCount() const noexcept
    {
        return static_cast<uint32_t>(m_records.size() - 1 - m_freeList.size());
    }

private:
    std::vector<EntityRecord>                        m_records;
    std::vector<uint32_t>                            m_freeList;
    std::array<IComponentStorage*, kMaxComponentTypes> m_storages{};
};

} // namespace Neuron::ECS
