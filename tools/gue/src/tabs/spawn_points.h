#pragma once
#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

class MediaTab;

struct SpawnPointMob {
    int         id               = 0;
    int         spawn_point_id   = 0;
    int         actor_def_id     = 0;
    int         count            = 1;
    char        name[64]         = "NPC";
    char        race[32]         = "Human";
    char        class_[32]       = "Warrior";
    int         level            = 1;
    int         aggressiveness   = 2;   // 0=passive 1=defensive 2=aggressive 3=dialog-only
    float       aggressive_range = 8.f;
    float       attack_range     = 2.f;
    int         respawn_delay_ms = 30000;
};

struct SpawnPoint {
    int         id        = 0;
    char        name[128] = "Spawn Point";
    char        area_name[128] = "Starter Zone";
    float       x = 0.f, y = 0.f, z = 0.f;
    float       radius    = 5.f;
    std::vector<SpawnPointMob> mobs;
};

class SpawnPointsTab {
public:
    void Draw(sqlite3* db, MediaTab* media = nullptr);

private:
    void EnsureTables(sqlite3* db);
    void FetchAll(sqlite3* db);
    bool SavePoint(sqlite3* db, SpawnPoint& sp);
    bool SaveMob(sqlite3* db, SpawnPointMob& m);
    bool DeletePoint(sqlite3* db, int id);
    bool DeleteMob(sqlite3* db, int id);

    std::vector<SpawnPoint> points_;
    int  sel_point_ = -1;
    int  sel_mob_   = -1;
    bool needFetch_ = true;
    char statusMsg_[256] = {};
};

} // namespace gue
