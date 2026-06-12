#pragma once

#include <sqlite3.h>

#include <string>
#include <vector>
#include <utility>

namespace gue {

class SettingsTab {
public:
    void Draw(sqlite3* db);

private:
    void EnsureTables(sqlite3* db);
    void LoadModels(sqlite3* db);
    void LoadBloodFXKeys(sqlite3* db);
    void LoadSettings(sqlite3* db);
    bool SaveSettings(sqlite3* db);

    std::vector<std::pair<int, std::string>> media_models_;
    std::vector<std::string> blood_fx_keys_;
    int  default_drop_model_id_ = 0;
    float default_drop_model_scale_ = 1.f;
    std::string blood_fx_key_;
    std::string blood_fx_mode_ = "basic";
    bool need_fetch_ = true;
    bool dirty_ = false;
    char status_msg_[256] = {};
};

} // namespace gue
