#pragma once

#include "ECS/ECSContext.h"

#include "ECS/systems/CommandSystem.h"
#include "ECS/systems/SteeringSystem.h"
#include "ECS/systems/NavGrid.h"
#include "ECS/systems/NavGridBuilderSystem.h"
#include "ECS/systems/PathfindingSystem.h"
#include "ECS/systems/MovementSystem.h"
#include "ECS/systems/RenderTransformUpdateSystem.h"
#include "systems/LocomotionAnimationControllerSystem.h"
#include "ECS/systems/Animation/AnimationPlaybackSystem.h"
#include "ECS/systems/PoseUpdateSystem.h"
#include "ECS/systems/RenderSystem.h"
#include "ECS/systems/SpatialIndexSystem.h"
#include "ECS/systems/LocalAvoidanceSystem.h"
#include "systems/CombatSystem.h"

namespace Engine
{
    class AssetManager;
    class Renderer;
    class Camera;
}

namespace Sample
{
    // Owns and runs Sample gameplay systems in a consistent order.
    class SystemRunner
    {
    public:
        void Initialize(Engine::ECS::ECSContext &ecs);
        void Update(Engine::ECS::ECSContext &ecs, float dtSeconds);

        void SetAssetManager(Engine::AssetManager *assets);
        void SetRenderer(Engine::Renderer *renderer);
        void SetCamera(Engine::Camera *camera);
        void SetGlobalMoveTarget(float x, float y, float z);

        /// Access combat system for HUD stats
        const CombatSystem &GetCombatSystem() const { return m_combat; }
        /// Mutable access for config loading
        CombatSystem &GetCombatSystemMut() { return m_combat; }

    private:
        bool m_initialized = false;

        CommandSystem m_command;
        SteeringSystem m_steering;
        MovementSystem m_movement;
        RenderTransformUpdateSystem m_renderTransform;

        NavGrid m_navGrid;
        NavGridBuilderSystem m_navGridBuilder{&m_navGrid};
        PathfindingSystem m_pathfinding{&m_navGrid};

        SpatialIndexSystem m_spatialIndex{2.0f};
        LocalAvoidanceSystem m_localAvoidance{&m_spatialIndex};
        CombatSystem m_combat;

        LocomotionAnimationControllerSystem m_locomotionAnim;
        AnimationPlaybackSystem m_animPlayback;

        PoseUpdateSystem m_poseUpdate;

        RenderSystem m_renderModel;
    };
}
