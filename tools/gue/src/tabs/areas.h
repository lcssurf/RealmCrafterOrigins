#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

struct AreaConfig {
    std::string name;
    int         music_track  = 1;  // 0=Stop 1=StarterZone 2=Forest 3=Combat
    float       fog_density  = 0.f;
};

struct AreaPortal {
    int         id          = 0;
    std::string area_name;
    float       x = 0.f, z = 0.f, radius = 3.f;
    std::string target_area;
    float       dest_x = 0.f, dest_y = 0.f, dest_z = 0.f, dest_yaw = 0.f;
};

class AreasTab {
public:
    void Draw(sqlite3* db);

private:
    void EnsureTables(sqlite3* db);
    void FetchAreas(sqlite3* db);
    void FetchPortals(sqlite3* db, const std::string& area);
    bool SaveArea(sqlite3* db, AreaConfig& a, bool isNew);
    bool DeleteArea(sqlite3* db, const std::string& name);
    bool SavePortal(sqlite3* db, AreaPortal& p);
    bool DeletePortal(sqlite3* db, int id);

    std::vector<AreaConfig> areas_;
    std::vector<AreaPortal> portals_;

    int  selectedArea_   = -1;
    int  selectedPortal_ = -1;
    bool needFetchAreas_ = true;
    bool needFetchPortals_ = false;

    bool     dirtyArea_   = false;
    bool     dirtyPortal_ = false;
    bool     showNewArea_ = false;
    bool     showNewPortal_ = false;

    AreaConfig editingArea_;
    AreaConfig newArea_;
    AreaPortal editingPortal_;
    AreaPortal newPortal_;

    char statusMsg_[256] = {};
};

} // namespace gue
