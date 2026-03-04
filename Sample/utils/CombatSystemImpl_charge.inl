#pragma once

// Charge / click-point helpers for CombatSystem.
// Included by Sample/utils/CombatSystemImpl.inl.

inline void CombatSystem::issueClickTargets(Engine::ECS::ECSContext &ecs,
                                            const Engine::ECS::Query &q)
{
    for (uint32_t aid : q.matchingArchetypeIds)
    {
        auto *st = ecs.stores.get(aid);
        if (!st || !st->hasMoveTarget() || !st->hasHealth())
            continue;
        auto &mt = st->moveTargets();
        const auto &hp = st->healths();
        for (uint32_t r = 0; r < st->size(); ++r)
        {
            if (hp[r].value <= 0.0f)
                continue;
            mt[r].x = m_battleClickX;
            mt[r].y = 0.0f;
            mt[r].z = m_battleClickZ;
            mt[r].active = 1;
            ecs.markDirty(m_moveTargetId, aid, r);
        }
    }
    std::cout << "[CombatSystem] Leg-1: all units → click point ("
              << m_battleClickX << "," << m_battleClickZ << ")\n";
}

inline void CombatSystem::promoteUnitsNearClick(Engine::ECS::ECSContext &ecs,
                                                const Engine::ECS::Query &q)
{
    for (uint32_t aid : q.matchingArchetypeIds)
    {
        auto *st = ecs.stores.get(aid);
        if (!st || !st->hasMoveTarget() || !st->hasHealth() || !st->hasPosition() || !st->hasTeam())
            continue;

        auto &mt = st->moveTargets();
        const auto &pos = st->positions();
        const auto &hp = st->healths();
        const auto &tm = st->teams();
        const uint32_t n = st->size();

        for (uint32_t r = 0; r < n; ++r)
        {
            if (hp[r].value <= 0.0f)
                continue;
            if (!mt[r].active)
                continue;

            // Already promoted? (MoveTarget no longer equals click)
            const float tdx = mt[r].x - m_battleClickX;
            const float tdz = mt[r].z - m_battleClickZ;
            if (tdx * tdx + tdz * tdz > CombatTuning::CLICK_TARGET_MATCH_DIST2)
                continue;

            // Close enough to click → promote to leg 2
            const float dx = pos[r].x - m_battleClickX;
            const float dz = pos[r].z - m_battleClickZ;
            if (dx * dx + dz * dz > CombatTuning::PASS_RADIUS2)
                continue;

            const uint8_t myTeam = tm[r].id;
            float bestEX = pos[r].x;
            float bestEZ = pos[r].z;
            float bestD2 = CombatTuning::BEST_DIST2_INIT;

            if (m_spatial)
            {
                m_spatial->forNeighbors(pos[r].x, pos[r].z, [&](uint32_t nStoreId, uint32_t nRow)
                                        {
                                            auto *ns = ecs.stores.get(nStoreId);
                                            if (!ns || !ns->hasPosition() || !ns->hasHealth() || !ns->hasTeam())
                                                return;
                                            if (nRow >= ns->size())
                                                return;
                                            if (ns->teams()[nRow].id == myTeam)
                                                return;
                                            if (ns->healths()[nRow].value <= 0.0f)
                                                return;

                                            const float ex = ns->positions()[nRow].x;
                                            const float ez = ns->positions()[nRow].z;
                                            const float d2 = (ex - pos[r].x) * (ex - pos[r].x) + (ez - pos[r].z) * (ez - pos[r].z);
                                            if (d2 < bestD2)
                                            {
                                                bestD2 = d2;
                                                bestEX = ex;
                                                bestEZ = ez;
                                            } });
            }

            if (bestD2 > CombatTuning::FALLBACK_NO_ENEMY_DIST2)
            {
                for (uint32_t oaid : q.matchingArchetypeIds)
                {
                    auto *os = ecs.stores.get(oaid);
                    if (!os || !os->hasPosition() || !os->hasHealth() || !os->hasTeam())
                        continue;

                    const auto &op = os->positions();
                    const auto &oh = os->healths();
                    const auto &ot = os->teams();
                    const uint32_t on = os->size();
                    for (uint32_t orow = 0; orow < on; ++orow)
                    {
                        if (oh[orow].value <= 0.0f)
                            continue;
                        if (ot[orow].id == myTeam)
                            continue;

                        const float ex = op[orow].x;
                        const float ez = op[orow].z;
                        const float d2 = (ex - pos[r].x) * (ex - pos[r].x) + (ez - pos[r].z) * (ez - pos[r].z);
                        if (d2 < bestD2)
                        {
                            bestD2 = d2;
                            bestEX = ex;
                            bestEZ = ez;
                        }
                    }
                }
            }

            if (bestD2 <= CombatTuning::FALLBACK_NO_ENEMY_DIST2)
            {
                mt[r].x = bestEX;
                mt[r].y = 0.0f;
                mt[r].z = bestEZ;
                mt[r].active = 1;
                ecs.markDirty(m_moveTargetId, aid, r);
            }
        }
    }
}
