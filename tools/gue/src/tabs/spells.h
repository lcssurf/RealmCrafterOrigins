#pragma once

#include <string>
#include <vector>
#include <sqlite3.h>

namespace gue {

struct SpellTemplate {
    int         id          = 0;
    std::string name;
    int         spell_type  = 0; // 0=damage 1=heal 2=buff 3=debuff
    int         damage_min  = 0;
    int         damage_max  = 0;
    int         ep_cost     = 10;
    int         cooldown_ms = 2000;
    float       range       = 20.f;
    int         icon        = 0;
    int         aoe_type    = 0; // 0=single 1=around_target 2=ground_target
    float       aoe_radius  = 0.f;
};

class SpellsTab {
public:
    void Draw(sqlite3* db);

private:
    void Fetch(sqlite3* db);
    bool Save(sqlite3* db, SpellTemplate& t);
    bool Delete(sqlite3* db, int id);

    std::vector<SpellTemplate> spells_;
    int           selected_  = -1;
    bool          dirty_     = false;
    bool          needFetch_ = true;
    char          statusMsg_[256] = {};
    SpellTemplate editing_;
    SpellTemplate newSpell_;
    bool          showNew_   = false;
};

} // namespace gue
