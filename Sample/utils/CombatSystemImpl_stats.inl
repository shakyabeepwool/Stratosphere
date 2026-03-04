#pragma once

// Stats / lifetime helpers for CombatSystem.
// Included by Sample/utils/CombatSystemImpl.inl.

inline void CombatSystem::staggerInitialCooldowns(Engine::ECS::ECSContext &ecs)
{
    const float stagger01 = std::clamp(m_cfg.staggerMax, 0.0f, 1.0f);
    if (stagger01 <= 0.0f)
        return;

    if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

    const auto &q = ecs.queries.get(m_queryId);
    for (uint32_t archetypeId : q.matchingArchetypeIds)
    {
        auto *st = ecs.stores.get(archetypeId);
        if (!st || !st->hasAttackCooldown() || !st->hasHealth())
            continue;

        auto &cd = st->attackCooldowns();
        const auto &hp = st->healths();
        const uint32_t n = st->size();
        for (uint32_t row = 0; row < n; ++row)
        {
            if (hp[row].value <= 0.0f)
                continue;
            const float interval = std::max(0.0f, cd[row].interval);
            cd[row].timer = m_unitDist(m_rng) * (stagger01 * interval);
            ecs.markDirty(m_attackCooldownId, archetypeId, row);
        }
    }
}

inline void CombatSystem::refreshTeamStats(Engine::ECS::ECSContext &ecs)
{
    // Reset counts
    for (auto &[id, s] : m_teamStats)
    {
        s.alive = 0;
        s.currentHP = 0.0f;
    }

    if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        return;

    const auto &q = ecs.queries.get(m_queryId);
    for (uint32_t archetypeId : q.matchingArchetypeIds)
    {
        auto *st = ecs.stores.get(archetypeId);
        if (!st || !st->hasHealth() || !st->hasTeam())
            continue;

        const auto &hp = st->healths();
        const auto &teams = st->teams();
        const uint32_t n = st->size();
        for (uint32_t row = 0; row < n; ++row)
        {
            if (hp[row].value <= 0.0f)
                continue;

            auto &ts = m_teamStats[teams[row].id];
            ts.alive++;
            ts.currentHP += std::max(0.0f, hp[row].value);
        }
    }

    // Simplification: infer totalSpawned from peak alive count seen so far.
    for (auto &[id, s] : m_teamStats)
    {
        if (s.alive > s.totalSpawned)
            s.totalSpawned = s.alive;
        s.maxHP = s.totalSpawned * m_cfg.maxHPPerUnit;
    }
}

inline void CombatSystem::processDeathRemovals(Engine::ECS::ECSContext &ecs, float dt)
{
    // Swap-and-pop instead of erase — O(1) per removal instead of O(N)
    size_t i = 0;
    while (i < m_deathQueue.size())
    {
        m_deathQueue[i].timeRemaining -= dt;
        if (m_deathQueue[i].timeRemaining <= 0.0f)
        {
            auto &pd = m_deathQueue[i];
            // Destroy the entity
            if (ecs.entities.isAlive(pd.entity))
            {
                auto *rec = ecs.entities.find(pd.entity);
                if (rec)
                {
                    auto *store = ecs.stores.get(rec->archetypeId);
                    if (store)
                    {
                        Engine::ECS::Entity moved = store->destroyRowSwap(rec->row);
                        if (moved.valid())
                            ecs.entities.attach(moved, rec->archetypeId, rec->row);
                    }
                }
                ecs.entities.destroy(pd.entity);
            }
            m_deathQueueSet.erase(pd.entity.index);
            // Swap with last and pop — don't increment i
            pd = m_deathQueue.back();
            m_deathQueue.pop_back();
        }
        else
        {
            ++i;
        }
    }
}
