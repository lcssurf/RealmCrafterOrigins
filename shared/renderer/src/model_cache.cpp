#include "rco/renderer/model_cache.h"
#include <unordered_map>
#include <cstdio>

namespace rco::renderer {

static std::unordered_map<std::string, std::shared_ptr<Model>> s_cache;
static ModelCacheObserver s_observer = nullptr;
static thread_local const char* s_context = nullptr;

std::shared_ptr<Model> ModelCacheGet(const std::string& path, MaterialManager* mm) {
    auto it = s_cache.find(path);
    if (it != s_cache.end()) {
        if (s_observer) s_observer(path.c_str(), true, s_context);
        return it->second;
    }
    auto m = std::make_shared<Model>();
    if (path == kSpherePrimitivePath) {
        m->GenerateSpherePrimitive();
    } else {
        m->Load(path.c_str(), mm);
    }
    s_cache[path] = m;
    if (s_observer) s_observer(path.c_str(), false, s_context);
    return m;
}

std::shared_ptr<Model> ModelCachePeek(const std::string& path) {
    auto it = s_cache.find(path);
    if (it == s_cache.end()) return nullptr;
    return it->second;
}

void ModelCacheInvalidate(const std::string& path) {
    s_cache.erase(path);
}

void ModelCacheEvictAll() {
    s_cache.clear();
}

void ModelCacheSetObserver(ModelCacheObserver observer) {
    s_observer = observer;
}

void ModelCacheSetContext(const char* context) {
    s_context = context;
}

} // namespace rco::renderer
