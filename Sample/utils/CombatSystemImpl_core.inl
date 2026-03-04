#pragma once

// Core wiring for CombatSystem (constructor + component masks).
// Included by Sample/utils/CombatSystemImpl.inl.

inline CombatSystem::CombatSystem()
{
    setRequiredNames({"Position", "Health", "Velocity", "MoveTarget", "MoveSpeed",
                      "Facing", "Team", "AttackCooldown", "RenderAnimation"});
    setExcludedNames({"Dead", "Disabled"});

    // Non-deterministic seed so each run plays differently
    std::random_device rd;
    m_rng.seed(rd());
}

inline void CombatSystem::buildMasks(Engine::ECS::ComponentRegistry &registry)
{
    Engine::ECS::SystemBase::buildMasks(registry);
    m_positionId = registry.ensureId("Position");
    m_healthId = registry.ensureId("Health");
    m_velocityId = registry.ensureId("Velocity");
    m_moveTargetId = registry.ensureId("MoveTarget");
    m_teamId = registry.ensureId("Team");
    m_attackCooldownId = registry.ensureId("AttackCooldown");
    m_renderAnimId = registry.ensureId("RenderAnimation");
    m_facingId = registry.ensureId("Facing");
    m_deadId = registry.ensureId("Dead");
}
