#pragma once

#include "../fx_preview_viewport.h"

#include <sqlite3.h>

#include <string>
#include <vector>

namespace gue {

struct FXTemplateRow {
    int         id = 0;
    std::string fx_key;
    std::string display_name;

    int   burst_count = 0;
    float stream_interval = 0.04f;
    float lifetime_seconds = 1.0f;

    float speed_min = 1.0f;
    float speed_max = 3.0f;
    float velocity_bias_x = 0.0f;
    float velocity_bias_y = 2.0f;
    float velocity_bias_z = 0.0f;
    float velocity_spread = 0.5f;

    float color_start[4] = {1.0f, 0.5f, 0.0f, 1.0f};
    float color_end[4]   = {1.0f, 0.0f, 0.0f, 0.0f};
    float size_start = 8.0f;
    float size_end = 2.0f;
    std::string texture_path;

    bool enabled = true;
};

class FXTemplatesTab {
public:
    void Draw(sqlite3* db);

private:
    void FetchAll(sqlite3* db);
    bool SaveTemplate(sqlite3* db, FXTemplateRow& row);
    bool DeleteTemplate(sqlite3* db, int id);
    bool DrawFields(FXTemplateRow& row);
    rco::renderer::FXParams RowToFXParams(const FXTemplateRow& r) const;
    void SetStatus(const char* fmt, ...);

    std::vector<FXTemplateRow> templates_;
    int  selected_ = -1;
    bool need_fetch_ = true;
    bool dirty_ = false;
    bool show_new_ = false;
    FXTemplateRow new_row_;
    FXTemplateRow editing_row_;
    int preview_for_id_ = -2;
    bool show_new_preview_primed_ = false;
    FXPreviewViewport preview_;
    char status_msg_[256] = {};
};

} // namespace gue
