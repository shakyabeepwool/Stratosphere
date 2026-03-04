#pragma once

#include "ECS/SystemFormat.h"
#include "ECS/Components.h"

#include <algorithm>
#include <cmath>

class SteeringSystem : public Engine::ECS::SystemBase
{
public:
    SteeringSystem()
    {
        // Position + Velocity + MoveTarget + MoveSpeed + Path + Facing required
        setRequiredNames({"Position", "Velocity", "MoveTarget", "MoveSpeed", "Path", "Facing"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "SteeringSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_positionId = registry.ensureId("Position");
        m_velocityId = registry.ensureId("Velocity");
        m_moveTargetId = registry.ensureId("MoveTarget");
        m_facingId = registry.ensureId("Facing");
    }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_positionId);
            dirty.set(m_moveTargetId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        auto dist2 = [](float x, float z)
        { return x * x + z * z; };

        // Final target handling:
        // - Small stop radius prevents ping-pong.
        // - Slow radius provides smooth deceleration near goal.
        const float stopRadius2 = 0.04f; // 0.2^2
        const float slowRadius = 2.0f;   // meters
        const float slowRadius2 = slowRadius * slowRadius;
        const float waypointRadius2 = 0.0625f; // 0.25^2

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;

            auto dirtyRows = ecs.queries.consumeDirtyRows(m_queryId, archetypeId);
            if (dirtyRows.empty())
                continue;

            auto &positions = const_cast<std::vector<Engine::ECS::Position> &>(store.positions());
            auto &velocities = const_cast<std::vector<Engine::ECS::Velocity> &>(store.velocities());
            auto &targets = const_cast<std::vector<Engine::ECS::MoveTarget> &>(store.moveTargets());
            auto &speeds = const_cast<std::vector<Engine::ECS::MoveSpeed> &>(store.moveSpeeds());
            auto &paths = const_cast<std::vector<Engine::ECS::Path> &>(store.paths());
            auto &facings = const_cast<std::vector<Engine::ECS::Facing> &>(store.facings());

            // Optional components: used to decide when a unit should consider itself "arrived".
            const bool hasRadius = store.hasRadius();
            const bool hasSep = store.hasSeparation();
            const auto &radii = store.radii();
            const auto &seps = store.separations();

            const uint32_t n = store.size();

            for (uint32_t i : dirtyRows)
            {
                if (i >= n)
                    continue;

                auto &pos = positions[i];
                auto &vel = velocities[i];
                auto &tgt = targets[i];
                const auto &spd = speeds[i];
                auto &path = paths[i];
                auto &facing = facings[i];

                if (!tgt.active)
                    continue;

                float tx = tgt.x;
                float tz = tgt.z;
                bool isFinal = true;

                if (path.valid && path.current < path.count)
                {
                    tx = path.waypointsX[path.current];
                    tz = path.waypointsZ[path.current];
                    isFinal = false;
                }

                float dx = tx - pos.x;
                float dz = tz - pos.z;
                float d2 = dist2(dx, dz);

                // For the final target, accept a larger radius based on footprint so units don't
                // fight local avoidance forever trying to hit an exact point.
                float radiusToCheck2 = waypointRadius2;
                if (isFinal)
                {
                    const float r = (hasRadius && i < radii.size()) ? std::max(0.0f, radii[i].r) : 0.0f;
                    const float s = (hasSep && i < seps.size()) ? std::max(0.0f, seps[i].value) : 0.0f;
                    const float inflated = r + s;

                    // Clamp to slowRadius so we don't accept from too far away.
                    // NOTE: Using a footprint-based acceptance radius prevents units from
                    // fighting local avoidance forever trying to reach an exact point.
                    // The 1.25x factor gives a small buffer for avoidance displacement.
                    const float acceptR = std::max(0.35f, std::min(slowRadius, 2.5f * inflated));
                    const float acceptR2 = acceptR * acceptR;
                    radiusToCheck2 = std::max(stopRadius2, acceptR2);
                }

                if (d2 <= radiusToCheck2)
                {
                    if (isFinal)
                    {
                        vel.x = vel.y = vel.z = 0.0f;
                        tgt.active = 0;
                        path.valid = false;
                    }
                    else
                    {
                        path.current++;
                        if (path.current < path.count)
                        {
                            tx = path.waypointsX[path.current];
                            tz = path.waypointsZ[path.current];
                            dx = tx - pos.x;
                            dz = tz - pos.z;
                            d2 = dist2(dx, dz);
                        }
                        else
                        {
                            path.valid = false;
                            tx = tgt.x;
                            tz = tgt.z;
                            dx = tx - pos.x;
                            dz = tz - pos.z;
                            d2 = dist2(dx, dz);
                        }
                    }

                    if (isFinal)
                    {
                        ecs.markDirty(m_velocityId, archetypeId, i);
                        ecs.markDirty(m_moveTargetId, archetypeId, i);
                        continue;
                    }
                }

                if (d2 > 1e-8f)
                {
                    float dist = std::sqrt(d2);
                    float invDist = 1.0f / dist;
                    dx *= invDist;
                    dz *= invDist;

                    float speed = spd.value;

                    // Smooth arrival toward the final target.
                    if (isFinal && d2 < slowRadius2)
                    {
                        const float t = std::max(0.0f, std::min(dist / std::max(slowRadius, 1e-4f), 1.0f));
                        speed *= t;
                    }

                    float targetVx = dx * speed;
                    float targetVz = dz * speed;

                    const float acceleration = 15.0f;

                    float diffX = targetVx - vel.x;
                    float diffZ = targetVz - vel.z;

                    vel.x += diffX * acceleration * dt;
                    vel.z += diffZ * acceleration * dt;
                    vel.y = 0.0f;

                    facing.yaw = std::atan2(dx, dz);
                    ecs.markDirty(m_facingId, archetypeId, i);
                }

                ecs.markDirty(m_velocityId, archetypeId, i);

                if (tgt.active)
                    ecs.markDirty(m_moveTargetId, archetypeId, i);
            }
        }
    }

private:
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_positionId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_velocityId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_moveTargetId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_facingId = Engine::ECS::ComponentRegistry::InvalidID;
};
