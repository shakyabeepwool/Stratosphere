#pragma once

#include "ECS/SystemFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// Sample-side locomotion animation controller.
//
// Responsibilities:
// - Choose idle/run based on Velocity.
// - Uses LocomotionClips (if present) for clip indices.
// - Does not advance time (handled by AnimationPlaybackSystem).
// - Does not override active one-shot animations (loop==false && playing==true).
class LocomotionAnimationControllerSystem : public Engine::ECS::SystemBase
{
public:
    LocomotionAnimationControllerSystem()
    {
        setRequiredNames({"RenderAnimation", "Velocity"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "LocomotionAnimationControllerSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_renderAnimId = registry.ensureId("RenderAnimation");
        m_velocityId = registry.ensureId("Velocity");
    }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        // Use hysteresis so tiny avoidance-induced velocity noise doesn't flip
        // between idle/run every other frame (visible as pose snapping).
        constexpr float kStartMoveSpeed = 0.25f; // start moving above this
        constexpr float kStopMoveSpeed = 0.12f;  // stop moving below this
        constexpr float kStartMoveSpeed2 = kStartMoveSpeed * kStartMoveSpeed;
        constexpr float kStopMoveSpeed2 = kStopMoveSpeed * kStopMoveSpeed;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_velocityId);
            dirty.set(m_renderAnimId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(archetypeId);
            if (!st || !st->hasRenderAnimation() || !st->hasVelocity())
                continue;

            auto dirtyRows = ecs.queries.consumeDirtyRows(m_queryId, archetypeId);
            if (dirtyRows.empty())
                continue;

            auto &anims = st->renderAnimations();
            const auto &vels = st->velocities();
            const bool hasLoco = st->hasLocomotionClips();
            const auto *loco = hasLoco ? &st->locomotionClips() : nullptr;
            const uint32_t n = st->size();

            for (uint32_t row : dirtyRows)
            {
                if (row >= n)
                    continue;

                auto &anim = anims[row];

                // Respect active one-shots (combat, etc.).
                if (!anim.loop && anim.playing)
                    continue;

                const auto &v = vels[row];
                // Gameplay movement is on XZ plane; ignore any small Y jitter.
                const float speed2 = v.x * v.x + v.z * v.z;

                const uint32_t runClip = hasLoco ? (*loco)[row].runClip : anim.clipIndex;
                const uint32_t idleClip = hasLoco ? (*loco)[row].idleClip : anim.clipIndex;

                const bool wasMoving = (hasLoco && anim.clipIndex == runClip);
                const bool isMoving = wasMoving ? (speed2 > kStopMoveSpeed2) : (speed2 > kStartMoveSpeed2);

                bool changed = false;

                if (hasLoco)
                {
                    const uint32_t desiredClip = isMoving ? runClip : idleClip;
                    if (anim.clipIndex != desiredClip)
                    {
                        anim.clipIndex = desiredClip;
                        anim.timeSec = 0.0f;
                        changed = true;
                    }
                }

                // Keep locomotion always playing so idle can animate naturally
                // and PoseUpdateSystem doesn't snap to t=0 when "not playing".
                if (anim.playing != true)
                {
                    anim.playing = true;
                    changed = true;
                }

                // Locomotion should loop while playing.
                if (anim.loop != true)
                {
                    anim.loop = true;
                    changed = true;
                }

                if (changed)
                    ecs.markDirty(m_renderAnimId, archetypeId, row);
            }
        }
    }

private:
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_renderAnimId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_velocityId = Engine::ECS::ComponentRegistry::InvalidID;
};
