#include "update.h"

namespace Sample
{
    void SystemRunner::Initialize(Engine::ECS::ECSContext &ecs)
    {
        if (m_initialized)
            return;

        // Ensure queries get incrementally updated as new stores appear.
        ecs.WireQueryManager();

        auto &registry = ecs.components;

        // Ensure common IDs exist up-front (also used by scenario spawner selection).
        (void)registry.ensureId("Selected");

        m_command.buildMasks(registry);
        m_steering.buildMasks(registry);
        m_navGridBuilder.buildMasks(registry);
        {
            NavGridBuilderSystem::Config cfg;
            cfg.extraInflation = 2.5f;
            m_navGridBuilder.setConfig(cfg);
        }
        m_pathfinding.buildMasks(registry);
        m_movement.buildMasks(registry);
        m_spatialIndex.buildMasks(registry);
        m_localAvoidance.buildMasks(registry);
        m_combat.buildMasks(registry);
        m_combat.setSpatialIndex(&m_spatialIndex);
        m_locomotionAnim.buildMasks(registry);
        m_animPlayback.buildMasks(registry);
        m_poseUpdate.buildMasks(registry);
        m_renderModel.buildMasks(registry);

        // Initialize NavGrid (cover map area)
        m_navGrid.rebuild(2.0f, -400.0f, -400.0f, 400.0f, 400.0f);

        m_initialized = true;
    }

    void SystemRunner::Update(Engine::ECS::ECSContext &ecs, float dtSeconds)
    {
        if (!m_initialized)
            Initialize(ecs);

        if (dtSeconds <= 0.0f)
            return;

        // 1. Input
        m_command.update(ecs, dtSeconds);

        // 2. Spatial index rebuild (used by combat neighbor queries)
        m_spatialIndex.update(ecs, dtSeconds);

        // 3. Combat (may set move targets/stop units)
        m_combat.update(ecs, dtSeconds);

        // 4. NavGrid (Rebuild grid from static obstacles)
        m_navGridBuilder.update(ecs, dtSeconds);

        // 5. Pathfinding (Plan paths for units with invalid/new targets)
        m_pathfinding.update(ecs, dtSeconds);

        // 6. Steering (Follow waypoints, update facing) writes preferred velocity
        m_steering.update(ecs, dtSeconds);

        // 7. Local avoidance adjusts velocity to reduce overlaps
        m_localAvoidance.update(ecs, dtSeconds);

        // 8. Movement integration (uses velocity produced by steering+avoidance)
        m_movement.update(ecs, dtSeconds);

        // 9. Animation selection (sample policy)
        m_locomotionAnim.update(ecs, dtSeconds);

        // 10. Animation playback (engine)
        m_animPlayback.update(ecs, dtSeconds);

        // 11. Pose update
        m_poseUpdate.update(ecs, dtSeconds);

        // 12. Render
        m_renderModel.update(ecs, dtSeconds);
    }

    void SystemRunner::SetAssetManager(Engine::AssetManager *assets)
    {
        m_animPlayback.setAssetManager(assets);
        m_poseUpdate.setAssetManager(assets);
        m_renderModel.setAssetManager(assets);
        m_combat.setAssetManager(assets);
    }

    void SystemRunner::SetRenderer(Engine::Renderer *renderer)
    {
        m_renderModel.setRenderer(renderer);
    }

    void SystemRunner::SetCamera(Engine::Camera *camera)
    {
        m_renderModel.setCamera(camera);
    }

    void SystemRunner::SetGlobalMoveTarget(float x, float y, float z)
    {
        m_command.SetGlobalMoveTarget(x, y, z);
    }
}
