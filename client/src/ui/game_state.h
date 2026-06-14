#pragma once

#include <cstdint>
#include <string>

#include "core/derived_stats.h"

namespace rco {

// ---------------------------------------------------------------------------
// Top-level game state machine
// ---------------------------------------------------------------------------
enum class GameState {
    Login,
    CharacterSelect,
    InGame,
};

// ---------------------------------------------------------------------------
// Data returned by PPlayableDefs — one entry per playable actor def
// ---------------------------------------------------------------------------
struct PlayableDef {
    uint16_t    id         = 0;
    std::string name;
    std::string race;
    std::string charClass;
};

// ---------------------------------------------------------------------------
// Data returned by PCharListResult for one character slot
// ---------------------------------------------------------------------------
struct CharacterInfo {
    int         slot        = 0;
    std::string name;
    std::string race;
    std::string charClass;
    uint16_t    level       = 0;
    std::string area;
    int32_t     health      = 0;
    int32_t     healthMax   = 0;
    uint16_t    actorDefID  = 0;
};

// ---------------------------------------------------------------------------
// Local player state — populated from PStartGame and PStandardUpdate
// ---------------------------------------------------------------------------
struct PlayerState {
    uint32_t    runtimeId  = 0;
    float       x          = 0.f;
    float       y          = 0.f;
    float       z          = 0.f;
    float       yaw        = 0.f;
    int32_t     health     = 0;
    int32_t     healthMax  = 0;
    int32_t     mana       = 0;
    int32_t     manaMax    = 0;
    int32_t     stamina    = 0;
    int32_t     staminaMax = 0;
    uint16_t    level      = 1;
    uint32_t    xp         = 0;
    uint32_t    xp_current_level = 0;
    uint32_t    xp_next    = 100;
    rco::stats::PrimaryStats primary; // STR/DEX/INT/WIS/PER base (mirror of server PrimaryStats)
    rco::stats::PrimaryStats primary_effective; // base + item primary bonuses (from PFullStats)
    rco::stats::DerivedStats derived; // received from server via PFullStats (includes gear/item bonuses)
    int32_t     unspent_stat_points = 0;
    int32_t     free_respecs_used = 0;
    std::string name;
    std::string race;
    std::string charClass;
    std::string areaName;
};

} // namespace rco
