#pragma once

// Implementation split-out for maintainability.
// This file is included at the bottom of Sample/systems/CombatSystem.h.

// Aggregator: keep this file small and split the heavy code into focused includes.

#include "utils/CombatSystemImpl_core.inl"
#include "utils/CombatSystemImpl_update.inl"
#include "utils/CombatSystemImpl_stats.inl"
#include "utils/CombatSystemImpl_charge.inl"
