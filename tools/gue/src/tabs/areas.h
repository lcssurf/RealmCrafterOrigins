#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

struct AreaConfig {
    std::string name;
    int         music_track  = 1;  // 0=Stop 1=StarterZone 2=Forest 3=Combat
    float       fog_density  = 0.f;
    std::string skybox_hdr;  // filename relative to assets/ibl/ — empty = default.hdr
};

struct AreaPortal {
    int         id          = 0;
    std::string area_name;
    float       x = 0.f, z = 0.f, radius = 3.f;
    std::string target_area;
    float       dest_x = 0.f, dest_y = 0.f, dest_z = 0.f, dest_yaw = 0.f;
};

struct AreaWaypoint {
    int         id        = 0;
    std::string area_name;
    float       x = 0.f, y = 0.f, z = 0.f;
    int         next_a    = 0;  // ID of next waypoint (0 = end)
    int         next_b    = 0;  // ID of alternate branch (0 = none)
    int         pause_ms  = 0;  // ms to pause at this node
};

struct AreaWorldObject {
    int         id       = 0;
    std::string area_name;
    int         model_id = 0;
    float       x = 0.f, y = 0.f, z = 0.f;
    float       yaw      = 0.f;
    float       scale    = 1.f;
};

class AreasTab {
public:
    void Draw(sqlite3* db);

private:
    void EnsureTables(sqlite3* db);
    void FetchAreas(sqlite3* db);
    void FetchPortals(sqlite3* db, const std::string& area);
    void FetchWaypoints(sqlite3* db, const std::string& area);
    void FetchWorldObjects(sqlite3* db, const std::string& area);
    bool SaveArea(sqlite3* db, AreaConfig& a, bool isNew);
    bool DeleteArea(sqlite3* db, const std::string& name);
    bool SavePortal(sqlite3* db, AreaPortal& p);
    bool DeletePortal(sqlite3* db, int id);
    bool SaveWaypoint(sqlite3* db, AreaWaypoint& w);
    bool DeleteWaypoint(sqlite3* db, int id);
    bool SaveWorldObject(sqlite3* db, AreaWorldObject& w);
    bool DeleteWorldObject(sqlite3* db, int id);

    std::vector<AreaConfig>      areas_;
    std::vector<AreaPortal>      portals_;
    std::vector<AreaWaypoint>    waypoints_;
    std::vector<AreaWorldObject> world_objects_;

    // Model list for the world-object picker (id + name, loaded alongside world objects)
    struct ModelEntry { int id; std::string name; };
    std::vector<ModelEntry> wo_models_;

    int  selectedArea_        = -1;
    int  selectedPortal_      = -1;
    int  selectedWaypoint_    = -1;
    int  selectedWorldObject_ = -1;
    bool needFetchAreas_       = true;
    bool needFetchPortals_     = false;
    bool needFetchWaypoints_   = false;
    bool needFetchWorldObjects_ = false;

    bool     dirtyArea_         = false;
    bool     dirtyPortal_       = false;
    bool     dirtyWaypoint_     = false;
    bool     dirtyWorldObject_  = false;
    bool     showNewArea_       = false;
    bool     showNewPortal_     = false;
    bool     showNewWaypoint_   = false;
    bool     showNewWorldObject_ = false;

    AreaConfig      editingArea_;
    AreaConfig      newArea_;
    AreaPortal      editingPortal_;
    AreaPortal      newPortal_;
    AreaWaypoint    editingWaypoint_;
    AreaWaypoint    newWaypoint_;
    AreaWorldObject editingWorldObject_;
    AreaWorldObject newWorldObject_;

    char statusMsg_[256] = {};
};

} // namespace gue
