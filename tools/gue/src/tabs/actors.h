#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

class MediaTab; // fwd

struct NpcSpawn {
    int         id               = 0;
    std::string name             = "NPC";
    std::string race             = "Human";
    std::string class_           = "Warrior";
    int         level            = 1;
    std::string area_name        = "Starter Zone";
    float       x = 0.f, y = 0.f, z = 0.f, yaw = 0.f;
    int         aggressiveness   = 0; // 0=passive 1=defensive 2=aggressive 3=dialog-only
    float       aggressive_range = 8.f; // detection / chase trigger radius
    float       attack_range     = 2.f; // melee ~2, ranged ~15-25
    int         respawn_delay_ms  = 30000;
    int         actor_def_id      = 0;   // FK → media_actor_defs.id (0 = unset)
    int         start_waypoint_id  = 0;   // FK → area_waypoints.id (0 = no patrol)
    float       wander_radius      = 0.f; // 0 = no wander
    int         wander_pause_min   = 2000;
    int         wander_pause_max   = 5000;
};

class ActorsTab {
public:
    // media is optional; when null the actor def picker is disabled.
    void Draw(sqlite3* db, MediaTab* media = nullptr);

private:
    void EnsureTable(sqlite3* db);
    void Fetch(sqlite3* db);
    bool Save(sqlite3* db, NpcSpawn& n);
    bool Delete(sqlite3* db, int id);

    std::vector<NpcSpawn> spawns_;
    int      selected_  = -1;
    bool     dirty_     = false;
    bool     needFetch_ = true;
    char     statusMsg_[256] = {};
    NpcSpawn editing_;
    NpcSpawn newSpawn_;
    bool     showNew_   = false;
};

} // namespace gue
