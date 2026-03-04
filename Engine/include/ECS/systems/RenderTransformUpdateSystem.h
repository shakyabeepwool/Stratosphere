#pragma once

#include "ECS/SystemFormat.h"
#include "ECS/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// Updates Engine::ECS::RenderTransform from Position (+ optional Facing).
// Uses dirty queries so it only runs when Position/Facing are marked dirty.
class RenderTransformUpdateSystem : public Engine::ECS::SystemBase
{
public:
    RenderTransformUpdateSystem()
    {
        setRequiredNames({"Position", "RenderTransform"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "RenderTransformUpdateSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_positionId = registry.ensureId("Position");
        m_facingId = registry.ensureId("Facing");
    }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_positionId);
            dirty.set(m_facingId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;

            if (!store.hasPosition() || !store.hasRenderTransform())
                continue;

            auto dirtyRows = ecs.queries.consumeDirtyRows(m_queryId, archetypeId);
            if (dirtyRows.empty())
                continue;

            const uint32_t n = store.size();
            auto &positions = const_cast<std::vector<Engine::ECS::Position> &>(store.positions());
            auto &transforms = const_cast<std::vector<Engine::ECS::RenderTransform> &>(store.renderTransforms());
            const bool hasFacing = store.hasFacing();
            auto &facings = hasFacing ? const_cast<std::vector<Engine::ECS::Facing> &>(store.facings()) : m_dummyFacings;

            for (uint32_t row : dirtyRows)
            {
                if (row >= n)
                    continue;

                const auto &pos = positions[row];
                const float yaw = hasFacing ? facings[row].yaw : 0.0f;

                glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(pos.x, pos.y, pos.z));
                if (hasFacing)
                    world = glm::rotate(world, yaw, glm::vec3(0.0f, 1.0f, 0.0f));

                auto &rt = transforms[row];
                rt.world = world;
                rt.transformVersion += 1u;
            }
        }
    }

private:
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_positionId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_facingId = Engine::ECS::ComponentRegistry::InvalidID;

    std::vector<Engine::ECS::Facing> m_dummyFacings;
};
