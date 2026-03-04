#pragma once

#include "ECS/SystemFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class CommandSystem : public Engine::ECS::SystemBase
{
public:
    struct Config
    {
        float spacing = 0.5f;
        float minWorld = -10000.0f;
        float maxWorld = 10000.0f;
        bool log = false;
    };

    CommandSystem()
    {
        // Only command selected, movable units.
        setRequiredNames({"MoveTarget", "MoveSpeed", "Selected"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "CommandSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_moveTargetId = registry.ensureId("MoveTarget");
    }

    void setConfig(const Config &cfg) { m_cfg = cfg; }

    // Set the last clicked target; system will write it to entities on next update.
    void SetGlobalMoveTarget(float x, float y, float z)
    {
        m_hasPending = true;
        m_pendingX = x;
        m_pendingY = y;
        m_pendingZ = z;
    }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        if (!m_hasPending)
            return;

        const float spacing = m_cfg.spacing;
        const float kMinWorld = m_cfg.minWorld;
        const float kMaxWorld = m_cfg.maxWorld;

        auto clamp = [](float v, float a, float b)
        { return std::max(a, std::min(v, b)); };

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        struct SelectedRow
        {
            uint32_t archetypeId;
            uint32_t row;
            uint64_t sortKey;
        };

        std::vector<SelectedRow> selected;
        selected.reserve(512);

        float maxInflatedRadius = 0.0f; // (r + sep) upper bound across selection

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;

            const uint32_t n = store.size();
            if (n == 0)
                continue;

            const bool hasRadius = store.hasRadius();
            const bool hasSep = store.hasSeparation();
            const auto &radii = store.radii();
            const auto &seps = store.separations();
            const auto &ents = store.entities();

            for (uint32_t row = 0; row < n; ++row)
            {
                const Engine::ECS::Entity e = ents[row];
                const uint64_t key = (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
                selected.push_back(SelectedRow{archetypeId, row, key});

                const float r = (hasRadius && row < radii.size()) ? radii[row].r : 0.0f;
                const float s = (hasSep && row < seps.size()) ? seps[row].value : 0.0f;
                maxInflatedRadius = std::max(maxInflatedRadius, std::max(0.0f, r) + std::max(0.0f, s));
            }
        }

        const uint32_t selCount = static_cast<uint32_t>(selected.size());
        if (selCount == 0)
        {
            m_hasPending = false;
            return;
        }

        // Auto-space based on selected units' collision+separation footprint.
        // Desired center-to-center distance between two similar units is roughly:
        // (r1+r2) + (sep1+sep2) ~= 2 * max(r+sep).
        const float autoSpacing = std::max(0.0f, 2.0f * maxInflatedRadius);
        const float resolvedSpacing = std::max(std::max(0.0f, spacing), autoSpacing);

        // Stable slot assignment: sort by entity id so mixed archetypes don't each form their own grid.
        std::sort(selected.begin(), selected.end(), [](const SelectedRow &a, const SelectedRow &b)
                  { return a.sortKey < b.sortKey; });

        const uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(selCount))));
        const float half = (static_cast<float>(side) - 1.0f) * 0.5f;

        for (uint32_t k = 0; k < selCount; ++k)
        {
            const uint32_t row = k / side;
            const uint32_t col = k % side;
            const float ox = (static_cast<float>(col) - half) * resolvedSpacing;
            const float oz = (static_cast<float>(row) - half) * resolvedSpacing;

            const SelectedRow sr = selected[k];
            Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(sr.archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;
            if (!store.hasMoveTarget() || sr.row >= store.size())
                continue;

            auto &targets = const_cast<std::vector<Engine::ECS::MoveTarget> &>(store.moveTargets());
            targets[sr.row].x = clamp(m_pendingX + ox, kMinWorld, kMaxWorld);
            targets[sr.row].y = m_pendingY;
            targets[sr.row].z = clamp(m_pendingZ + oz, kMinWorld, kMaxWorld);
            targets[sr.row].active = 1;
            ecs.markDirty(m_moveTargetId, sr.archetypeId, sr.row);
        }

        if (m_cfg.log)
            onLog(selCount, side, resolvedSpacing);

        m_hasPending = false;
    }

protected:
    // Samples can override or ignore by keeping log disabled.
    virtual void onLog(uint32_t /*selectedCount*/, uint32_t /*gridSide*/, float /*spacing*/) {}

private:
    Config m_cfg{};

    bool m_hasPending = false;
    float m_pendingX = 0.0f, m_pendingY = 0.0f, m_pendingZ = 0.0f;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_moveTargetId = Engine::ECS::ComponentRegistry::InvalidID;
};
