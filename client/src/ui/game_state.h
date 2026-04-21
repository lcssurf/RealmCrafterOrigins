#pragma once

#include <cstdint>
#include <string>

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
// Data returned by PCharListResult for one character slot
// ---------------------------------------------------------------------------
struct CharacterInfo {
    int         slot       = 0;
    std::string name;
    std::string race;
    std::string charClass;
    uint16_t    level      = 0;
    std::string area;
    int32_t     health     = 0;
    int32_t     healthMax  = 0;
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
    int32_t     energy     = 0;
    int32_t     energyMax  = 0;
    uint16_t    level      = 1;
    uint32_t    xp         = 0;
    uint32_t    xp_next    = 100;
    std::string name;
    std::string race;
    std::string charClass;
    std::string areaName;
};

} // namespace rco
