#pragma once
#include "model.h"
#include "material.h"
#include <string>
#include <memory>

namespace rco::renderer {
// Returns a cached Model for path, loading it on first access.
// Subsequent calls return the existing instance. Cache uses weak_ptrs so
// the Model is freed when no Actor holds a reference.
std::shared_ptr<Model> ModelCacheGet(const std::string& path, MaterialManager* mm);

// Drop the cache entry for `path` so the next ModelCacheGet reloads from disk.
// Use after editing a sidecar (.uv) or texture file the GUE has just saved.
void ModelCacheInvalidate(const std::string& path);
} // namespace rco::renderer
