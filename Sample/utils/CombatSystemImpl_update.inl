#pragma once

// Main update loop for CombatSystem.
// Included by Sample/utils/CombatSystemImpl.inl.

inline void CombatSystem::update(Engine::ECS::ECSContext &ecs, float dt)
{
    if (!m_spatial)
        return;

    ++m_frameCounter;

    auto entityKey = [](const Engine::ECS::Entity &e) -> uint64_t
    {
        return (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
    };

    // One-time startup: log config and stagger initial cooldowns
    if (!m_loggedStart)
    {
        std::cout << "[CombatSystem] Active. range=" << m_cfg.meleeRange
                  << " dmg=[" << m_cfg.damageMin << "," << m_cfg.damageMax << "]"
                  << " miss=" << (m_cfg.missChance * 100.0f) << "%"
                  << " crit=" << (m_cfg.critChance * 100.0f) << "%"
                  << " rage=" << (m_cfg.rageMaxBonus * 100.0f) << "%\n";
        staggerInitialCooldowns(ecs);
        m_loggedStart = true;
    }

    // Ensure query exists early so stats refresh works before battle
    if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

    // ---- Phase 0: Refresh team stats (only when state changed) ----
    if (m_statsDirty)
    {
        refreshTeamStats(ecs);
        m_statsDirty = false;
    }

    // ---- Phase 1: Process pending death removals ----
    processDeathRemovals(ecs, dt);

    // If battle hasn't started yet, auto-start when armies get close enough.
    if (!m_battleStarted)
    {
        const float engageR = std::max(0.0f, m_cfg.engageRange);
        if (engageR <= 0.0f)
            return;
        const float engageR2 = engageR * engageR;

        struct UnitPos
        {
            float x;
            float z;
            uint8_t team;
        };

        std::vector<UnitPos> units;
        units.reserve(256);

        const auto &q0 = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q0.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(archetypeId);
            if (!st || !st->hasPosition() || !st->hasHealth() || !st->hasTeam())
                continue;

            const auto &pos = st->positions();
            const auto &hp = st->healths();
            const auto &tm = st->teams();
            const uint32_t n = st->size();
            for (uint32_t row = 0; row < n; ++row)
            {
                if (hp[row].value <= 0.0f)
                    continue;
                units.push_back(UnitPos{pos[row].x, pos[row].z, tm[row].id});
            }
        }

        bool shouldStart = false;
        for (size_t i = 0; i < units.size() && !shouldStart; ++i)
        {
            for (size_t j = i + 1; j < units.size(); ++j)
            {
                if (units[i].team == units[j].team)
                    continue;
                const float dx = units[i].x - units[j].x;
                const float dz = units[i].z - units[j].z;
                const float d2 = dx * dx + dz * dz;
                if (d2 <= engageR2)
                {
                    shouldStart = true;
                    break;
                }
            }
        }

        if (!shouldStart)
            return;

        startBattle();
    }

    const auto &q = ecs.queries.get(m_queryId);

    // ── Charge: issue leg-1 targets (once) ──────────────────────
    if (m_chargeActive && !m_chargeIssued)
    {
        issueClickTargets(ecs, q);
        m_chargeIssued = true;
    }
    // ── Charge: promote units near click to leg-2 ───────────────
    if (m_chargeActive)
        promoteUnitsNearClick(ecs, q);

    const float meleeRange = std::max(0.0f, m_cfg.meleeRange);
    const float meleeRange2 = meleeRange * meleeRange;
    const float disengageBuffer = 1.25f; // meters; prevents stop/chase flip-flop near range boundary
    const float disengageRange = meleeRange + disengageBuffer;
    const float disengageRange2 = disengageRange * disengageRange;

    auto hashAngle = [](uint32_t v) -> float
    {
        v ^= v >> 16;
        v *= 0x7feb352du;
        v ^= v >> 15;
        v *= 0x846ca68bu;
        v ^= v >> 16;
        const float u = (v & 0xFFFFFFu) / float(0x1000000u);
        return u * 6.28318530718f;
    };

    // Clear and alias per-frame buffers
    m_stops.clear();
    m_moves.clear();
    m_attackAnims.clear();
    m_damages.clear();
    m_damageAnims.clear();

    auto &stops = m_stops;
    auto &moves = m_moves;
    auto &attackAnims = m_attackAnims;
    auto &damages = m_damages;
    auto &damageAnims = m_damageAnims;

    // ---- Phase 2: Per-entity combat decisions (no structural mutations) ----
    for (uint32_t archetypeId : q.matchingArchetypeIds)
    {
        auto *storePtr = ecs.stores.get(archetypeId);
        if (!storePtr || !storePtr->hasPosition() || !storePtr->hasHealth() || !storePtr->hasTeam() ||
            !storePtr->hasAttackCooldown() || !storePtr->hasRenderAnimation() || !storePtr->hasFacing() ||
            !storePtr->hasMoveTarget())
        {
            continue;
        }

        auto &cooldowns = storePtr->attackCooldowns();
        auto &anims = storePtr->renderAnimations();
        const auto &pos = storePtr->positions();
        const auto &healths = storePtr->healths();
        const auto &teams = storePtr->teams();
        const auto &ents = storePtr->entities();

        const uint32_t n = storePtr->size();
        for (uint32_t row = 0; row < n; ++row)
        {
            if (healths[row].value <= 0.0f)
                continue;

            // Tick cooldown
            cooldowns[row].timer = std::max(0.0f, cooldowns[row].timer - dt);

            // Optional human gating (currently unused by SampleApp)
            if (m_humanTeamId >= 0 && teams[row].id == static_cast<uint8_t>(m_humanTeamId) && !m_humanAttacking)
                continue;

            Engine::ECS::Entity myEntity = ents[row];
            const float myX = pos[row].x;
            const float myZ = pos[row].z;
            const uint8_t myTeam = teams[row].id;

            // Persistent combat state (target + engaged hysteresis).
            const uint64_t selfKey = entityKey(myEntity);
            UnitCombatMemory &mem = m_unitMem[selfKey];
            mem.lastSeenFrame = m_frameCounter;

            Engine::ECS::Entity bestEnemy{};
            float bestEX = myX;
            float bestEZ = myZ;
            float bestDist2 = CombatTuning::BEST_DIST2_INIT;

            // Spatial lookup: nearest enemy in neighborhood
            if (m_spatial)
            {
                m_spatial->forNeighbors(myX, myZ, [&](uint32_t otherArchId, uint32_t otherRow)
                                        {
                                            auto *os = ecs.stores.get(otherArchId);
                                            if (!os || !os->hasPosition() || !os->hasHealth() || !os->hasTeam())
                                                return;
                                            if (otherRow >= os->size())
                                                return;

                                            if (os->healths()[otherRow].value <= 0.0f)
                                                return;
                                            if (os->teams()[otherRow].id == myTeam)
                                                return;

                                            const float ex = os->positions()[otherRow].x;
                                            const float ez = os->positions()[otherRow].z;
                                            const float dx = ex - myX;
                                            const float dz = ez - myZ;
                                            const float d2 = dx * dx + dz * dz;

                                            const Engine::ECS::Entity cand = os->entities()[otherRow];
                                            const float tieEps = CombatTuning::ENEMY_TIE_EPS;
                                            const bool better = (d2 < bestDist2 - tieEps) ||
                                                                (std::fabs(d2 - bestDist2) <= tieEps &&
                                                                 (!bestEnemy.valid() || cand.index < bestEnemy.index));
                                            if (better)
                                            {
                                                bestDist2 = d2;
                                                bestEX = ex;
                                                bestEZ = ez;
                                                bestEnemy = cand;
                                            } });
            }

            // Fallback: full scan only if spatial found nothing
            if (!bestEnemy.valid())
            {
                for (uint32_t otherArchId : q.matchingArchetypeIds)
                {
                    auto *os = ecs.stores.get(otherArchId);
                    if (!os || !os->hasPosition() || !os->hasHealth() || !os->hasTeam())
                        continue;

                    const auto &oPos = os->positions();
                    const auto &oHP = os->healths();
                    const auto &oTeam = os->teams();
                    const auto &oEnt = os->entities();
                    const uint32_t oN = os->size();

                    for (uint32_t oRow = 0; oRow < oN; ++oRow)
                    {
                        if (otherArchId == archetypeId && oRow == row)
                            continue;
                        if (oTeam[oRow].id == myTeam)
                            continue;
                        if (oHP[oRow].value <= 0.0f)
                            continue;

                        const float dx = oPos[oRow].x - myX;
                        const float dz = oPos[oRow].z - myZ;
                        const float d2 = dx * dx + dz * dz;

                        const Engine::ECS::Entity cand = oEnt[oRow];
                        const float tieEps = CombatTuning::ENEMY_TIE_EPS;
                        const bool better = (d2 < bestDist2 - tieEps) ||
                                            (std::fabs(d2 - bestDist2) <= tieEps &&
                                             (!bestEnemy.valid() || cand.index < bestEnemy.index));
                        if (better)
                        {
                            bestDist2 = d2;
                            bestEX = oPos[oRow].x;
                            bestEZ = oPos[oRow].z;
                            bestEnemy = cand;
                        }
                    }
                }
            }

            if (!bestEnemy.valid())
            {
                // No enemy found — during charge keep running,
                // otherwise stop.
                if (!m_chargeActive)
                {
                    float yaw = storePtr->facings()[row].yaw;
                    const bool clearVel = (storePtr->moveTargets()[row].active != 0);
                    stops.push_back({myEntity, yaw, clearVel});
                }

                // Drop target memory if nothing is around.
                mem.targetEnemy = Engine::ECS::Entity{};
                mem.engaged = false;
                continue;
            }

            // Decide whether to keep last target (stickiness) to avoid rapid swapping.
            Engine::ECS::Entity chosenEnemy = bestEnemy;
            float chosenEX = bestEX;
            float chosenEZ = bestEZ;
            float chosenDist2 = bestDist2;

            if (mem.targetEnemy.valid())
            {
                const auto *trec = ecs.entities.find(mem.targetEnemy);
                if (trec)
                {
                    auto *tst = ecs.stores.get(trec->archetypeId);
                    if (tst && trec->row < tst->size() && tst->hasPosition() && tst->hasHealth() && tst->hasTeam())
                    {
                        if (tst->healths()[trec->row].value > 0.0f && tst->teams()[trec->row].id != myTeam)
                        {
                            const float tx = tst->positions()[trec->row].x;
                            const float tz = tst->positions()[trec->row].z;
                            const float tdx = tx - myX;
                            const float tdz = tz - myZ;
                            const float tdist2 = tdx * tdx + tdz * tdz;

                            // Switch only if the new best is significantly closer.
                            // This reduces ping-pong when two enemies are nearly equidistant.
                            const float switchFrac = 0.15f;
                            const bool keepOld = !(bestDist2 < tdist2 * (1.0f - switchFrac));
                            if (keepOld)
                            {
                                chosenEnemy = mem.targetEnemy;
                                chosenEX = tx;
                                chosenEZ = tz;
                                chosenDist2 = tdist2;
                            }
                        }
                    }
                }
            }

            mem.targetEnemy = chosenEnemy;

            // Compute facing toward chosen enemy
            const float dx = chosenEX - myX;
            const float dz = chosenEZ - myZ;
            const float yaw = (dx * dx + dz * dz > CombatTuning::YAW_DIST2_EPS)
                                  ? std::atan2(dx, dz)
                                  : storePtr->facings()[row].yaw;

            // Squared-distance comparison avoids sqrt per entity per frame
            if (mem.engaged ? (chosenDist2 <= disengageRange2) : (chosenDist2 <= meleeRange2))
            {
                mem.engaged = true;
                // First melee contact ends the charge phase.
                if (m_chargeActive)
                    m_chargeActive = false;

                // In melee range — stop and attack
                const bool clearVel = (storePtr->moveTargets()[row].active != 0);
                stops.push_back({myEntity, yaw, clearVel});

                if (cooldowns[row].timer <= 0.0f)
                {
                    // Reset cooldown with jitter: interval * (1 ± jitter)
                    float jitter = 1.0f + m_realDist(m_rng) * m_cfg.cooldownJitter;
                    cooldowns[row].timer = cooldowns[row].interval * jitter;

                    // Always use the same attack animation clip to avoid flashing
                    // caused by switching between multiple attack clips.
                    const uint32_t attackClip = CombatAnims::ATTACK_START;
                    attackAnims.push_back({myEntity, attackClip, CombatTuning::ATTACK_ANIM_SPEED, false});

                    // --- Roll hit / miss ---
                    if (m_unitDist(m_rng) >= m_cfg.missChance)
                    {
                        // --- Roll damage in [damageMin, damageMax] ---
                        float baseDmg = m_cfg.damageMin +
                                        m_unitDist(m_rng) * (m_cfg.damageMax - m_cfg.damageMin);

                        // --- Berserker rage: bonus damage based on missing HP ---
                        float myHPFrac = healths[row].value / m_cfg.maxHPPerUnit;
                        float rageMult = 1.0f + m_cfg.rageMaxBonus * (1.0f - std::clamp(myHPFrac, 0.0f, 1.0f));
                        baseDmg *= rageMult;

                        // --- Roll critical hit ---
                        bool isCrit = (m_unitDist(m_rng) < m_cfg.critChance);
                        if (isCrit)
                            baseDmg *= m_cfg.critMultiplier;

                        // Queue damage on enemy
                        damages.push_back({chosenEnemy, baseDmg});

                        // Queue damage anim on enemy
                        uint32_t dmgClip = CombatAnims::DAMAGE_START +
                                           (m_rng() % (CombatAnims::DAMAGE_END - CombatAnims::DAMAGE_START + 1));
                        // Crits play damage anim faster for visual punch
                        float dmgAnimSpeed = isCrit ? CombatTuning::CRIT_DAMAGE_ANIM_SPEED : CombatTuning::DAMAGE_ANIM_SPEED;
                        damageAnims.push_back({chosenEnemy, dmgClip, dmgAnimSpeed, false});
                    }
                }
            }
            else
            {
                mem.engaged = false;
                // Out of range — chase nearest enemy.
                // During charge, only skip units still on leg 1
                // (their MoveTarget equals the click point).
                // Promoted units (past click) chase normally.
                bool skipChase = false;
                if (m_chargeActive)
                {
                    const auto &tgt = storePtr->moveTargets()[row];
                    float tdx = tgt.x - m_battleClickX;
                    float tdz = tgt.z - m_battleClickZ;
                    skipChase = tgt.active && (tdx * tdx + tdz * tdz < CombatTuning::CLICK_TARGET_MATCH_DIST2);
                }
                if (!skipChase)
                {
                    // Aim for a ring point around the enemy to avoid everyone converging
                    // to the exact same position (classic crowding / jitter trigger).
                    // This keeps motion stable because the angle is deterministic per unit.
                    float selfR = 0.0f;
                    if (storePtr->hasRadius())
                        selfR = std::max(0.0f, storePtr->radii()[row].r);

                    const float a = hashAngle(static_cast<uint32_t>(myEntity.index));
                    const float ringR = std::max(0.0f, m_cfg.meleeRange * 0.85f + selfR);
                    const float offX = std::cos(a) * ringR;
                    const float offZ = std::sin(a) * ringR;

                    moves.push_back({myEntity,
                                     chosenEX + offX, chosenEZ + offZ, true, yaw,
                                     CombatAnims::RUN,
                                     anims[row].clipIndex != CombatAnims::RUN});
                }
            }
        }
    }

    // Cleanup unit memory for entities not seen this frame.
    if (!m_unitMem.empty())
    {
        for (auto it = m_unitMem.begin(); it != m_unitMem.end();)
        {
            if (it->second.lastSeenFrame != m_frameCounter)
                it = m_unitMem.erase(it);
            else
                ++it;
        }
    }

    // ---- Phase 3: Apply deferred actions (safe to mutate stores now) ----

    // Apply stops
    for (const auto &s : stops)
    {
        auto *rec = ecs.entities.find(s.entity);
        if (!rec)
            continue;
        auto *st = ecs.stores.get(rec->archetypeId);
        if (!st || rec->row >= st->size())
            continue;

        if (s.clearVelocity && st->hasVelocity())
            st->velocities()[rec->row] = {0.0f, 0.0f, 0.0f};
        if (st->hasMoveTarget())
            st->moveTargets()[rec->row].active = 0;
        if (st->hasFacing())
            st->facings()[rec->row].yaw = s.yaw;

        if (st->hasMoveTarget())
            ecs.markDirty(m_moveTargetId, rec->archetypeId, rec->row);
        if (s.clearVelocity)
            ecs.markDirty(m_velocityId, rec->archetypeId, rec->row);
    }

    // Apply moves (set MoveTarget — PathfindingSystem + SteeringSystem handle velocity)
    for (const auto &mv : moves)
    {
        auto *rec = ecs.entities.find(mv.entity);
        if (!rec)
            continue;
        auto *st = ecs.stores.get(rec->archetypeId);
        if (!st || rec->row >= st->size())
            continue;

        if (st->hasMoveTarget())
        {
            auto &tgt = st->moveTargets()[rec->row];
            // Only re-path if target changed significantly (avoid thrashing)
            float dtx = tgt.x - mv.tx;
            float dtz = tgt.z - mv.tz;
            bool targetMoved = (dtx * dtx + dtz * dtz > CombatTuning::MOVE_TARGET_REPATH_DIST2) || !tgt.active;
            if (targetMoved)
            {
                tgt.x = mv.tx;
                tgt.y = 0.0f;
                tgt.z = mv.tz;
                tgt.active = mv.active ? 1 : 0;

                // Invalidate existing path so PathfindingSystem replans
                // A* to the NEW target instead of following the old route.
                if (st->hasPath())
                    st->paths()[rec->row].valid = false;

                ecs.markDirty(m_moveTargetId, rec->archetypeId, rec->row);
            }
        }
        if (st->hasFacing())
            st->facings()[rec->row].yaw = mv.yaw;
        if (mv.setRunAnim && st->hasRenderAnimation())
        {
            st->renderAnimations()[rec->row].clipIndex = mv.runClip;
            st->renderAnimations()[rec->row].timeSec = 0.0f;
            st->renderAnimations()[rec->row].playing = true;
            st->renderAnimations()[rec->row].loop = true;
            st->renderAnimations()[rec->row].speed = 1.0f;
            ecs.markDirty(m_renderAnimId, rec->archetypeId, rec->row);
        }
    }

    // Apply attack anims
    for (const auto &aa : attackAnims)
    {
        auto *rec = ecs.entities.find(aa.entity);
        if (!rec)
            continue;
        auto *st = ecs.stores.get(rec->archetypeId);
        if (!st || rec->row >= st->size() || !st->hasRenderAnimation())
            continue;

        st->renderAnimations()[rec->row].clipIndex = aa.clipIndex;
        st->renderAnimations()[rec->row].timeSec = 0.0f;
        st->renderAnimations()[rec->row].playing = true;
        st->renderAnimations()[rec->row].loop = aa.loop;
        st->renderAnimations()[rec->row].speed = aa.speed;
        ecs.markDirty(m_renderAnimId, rec->archetypeId, rec->row);
    }

    // Apply damage
    for (const auto &d : damages)
    {
        auto *rec = ecs.entities.find(d.target);
        if (!rec)
            continue;
        auto *st = ecs.stores.get(rec->archetypeId);
        if (!st || rec->row >= st->size() || !st->hasHealth())
            continue;

        st->healths()[rec->row].value -= d.damage;
        m_statsDirty = true;
    }

    // Apply damage anims (only if still alive — death anim will override below)
    for (const auto &da : damageAnims)
    {
        auto *rec = ecs.entities.find(da.entity);
        if (!rec)
            continue;
        auto *st = ecs.stores.get(rec->archetypeId);
        if (!st || rec->row >= st->size())
            continue;
        if (!st->hasHealth() || !st->hasRenderAnimation())
            continue;

        // Only play damage anim if still alive
        if (st->healths()[rec->row].value > 0.0f)
        {
            st->renderAnimations()[rec->row].clipIndex = da.clipIndex;
            st->renderAnimations()[rec->row].timeSec = 0.0f;
            st->renderAnimations()[rec->row].playing = true;
            st->renderAnimations()[rec->row].loop = false;
            st->renderAnimations()[rec->row].speed = da.speed;
            ecs.markDirty(m_renderAnimId, rec->archetypeId, rec->row);
        }
    }

    // ---- Phase 4: Handle newly dead entities (deferred tag migration) ----
    // Check all matching stores for entities with health <= 0 that aren't in death queue yet
    for (uint32_t archetypeId : q.matchingArchetypeIds)
    {
        auto *storePtr = ecs.stores.get(archetypeId);
        if (!storePtr || !storePtr->hasHealth())
            continue;

        const auto &hp = storePtr->healths();
        const auto &ent = storePtr->entities();
        const uint32_t n = storePtr->size();

        // Collect dead entities first (iterating while collecting, no mutations)
        m_newlyDead.clear();
        for (uint32_t row = 0; row < n; ++row)
        {
            if (hp[row].value <= 0.0f)
            {
                Engine::ECS::Entity e = ent[row];
                // O(1) set lookup instead of linear scan of death queue
                if (m_deathQueueSet.count(e.index) == 0)
                    m_newlyDead.push_back(e);
            }
        }

        // Now apply death effects — these migrate entities so must be done outside iteration
        for (const auto &deadEntity : m_newlyDead)
        {
            auto *rec = ecs.entities.find(deadEntity);
            if (!rec)
                continue;
            auto *st = ecs.stores.get(rec->archetypeId);
            if (!st || rec->row >= st->size())
                continue;

            // Play death animation
            if (st->hasRenderAnimation())
            {
                uint32_t deathClip = CombatAnims::DEATH_START +
                                     (m_rng() % (CombatAnims::DEATH_END - CombatAnims::DEATH_START + 1));
                st->renderAnimations()[rec->row].clipIndex = deathClip;
                st->renderAnimations()[rec->row].timeSec = 0.0f;
                st->renderAnimations()[rec->row].playing = true;
                st->renderAnimations()[rec->row].loop = false;
                st->renderAnimations()[rec->row].speed = 1.0f;
            }

            // Stop moving
            if (st->hasVelocity())
                st->velocities()[rec->row] = {0.0f, 0.0f, 0.0f};
            if (st->hasMoveTarget())
                st->moveTargets()[rec->row].active = 0;

            // Schedule removal and migrate to Dead archetype
            m_deathQueue.push_back({deadEntity, m_cfg.deathRemoveDelay});
            m_deathQueueSet.insert(deadEntity.index);
            m_statsDirty = true;
            ecs.addTag(deadEntity, m_deadId);
        }
    }
}
