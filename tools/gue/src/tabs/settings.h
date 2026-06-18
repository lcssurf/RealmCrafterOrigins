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
    void DrawGeneralSettings(sqlite3* db);

    // Animation Vocabulary sub-tab (Phase A.1)
    struct AnimVocabNode {
        int id;
        std::string name;
        int parent_id;
    };
    void LoadAnimVocabulary(sqlite3* db);
    void DrawAnimVocabulary(sqlite3* db);
    void DrawAnimVocabNode(sqlite3* db, const AnimVocabNode& node);
    bool AnimVocabNameExists(sqlite3* db, const std::string& name);
    void AnimVocabAddNode(sqlite3* db, const std::string& name, int parent_id);
    void AnimVocabRenameNode(sqlite3* db, int id, const std::string& new_name);
    void AnimVocabDeleteNode(sqlite3* db, int id);
    bool AnimVocabHasChildren(int id) const;

    std::vector<std::pair<int, std::string>> media_models_;
    std::vector<std::string> blood_fx_keys_;
    int  default_drop_model_id_ = 0;
    float default_drop_model_scale_ = 1.f;
    std::string blood_fx_key_;
    std::string blood_fx_mode_ = "basic";
    bool need_fetch_ = true;
    bool dirty_ = false;
    char status_msg_[256] = {};

    std::vector<AnimVocabNode> anim_vocab_;
    bool anim_vocab_need_fetch_ = true;
    char anim_vocab_status_[256] = {};
    char anim_vocab_new_root_name_[64] = {};
    char anim_vocab_rename_buf_[64] = {};
    int  anim_vocab_rename_id_ = 0;

    // Socket Vocabulary sub-tab (Arco B / B2)
    struct SocketEntry {
        int id;
        std::string name;
    };
    void LoadSocketVocab(sqlite3* db);
    void DrawSocketVocab(sqlite3* db);
    bool SocketNameExists(const std::string& name) const;
    void SocketAdd(sqlite3* db, const std::string& name);
    void SocketRename(sqlite3* db, int id, const std::string& new_name);
    void SocketDelete(sqlite3* db, int id);

    std::vector<SocketEntry> socket_vocab_;
    bool socket_vocab_need_fetch_ = true;
    char socket_vocab_status_[256] = {};
    char socket_vocab_new_name_[64] = {};
    char socket_vocab_rename_buf_[64] = {};
    int  socket_vocab_rename_id_ = 0;
};

} // namespace gue
