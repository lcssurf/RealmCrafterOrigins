#include "rco/renderer/model_cache.h"
#include <unordered_map>
#include <cstdio>

namespace rco::renderer {

static std::unordered_map<std::string, std::weak_ptr<Model>> s_cache;

std::shared_ptr<Model> ModelCacheGet(const std::string& path, MaterialManager* mm) {
    auto it = s_cache.find(path);
    if (it != s_cache.end()) {
        if (auto sp = it->second.lock())
            return sp;
    }
    auto m = std::make_shared<Model>();
    m->Load(path.c_str(), mm);
    s_cache[path] = m;
    return m;
}

} // namespace rco::renderer
