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

// Returns the model if it is already in the cache, without triggering a load.
// Returns nullptr if the model has not been loaded yet. Use this to avoid
// blocking the render thread on disk I/O (e.g., thumbnail rendering).
std::shared_ptr<Model> ModelCachePeek(const std::string& path);

// Drop the cache entry for `path` so the next ModelCacheGet reloads from disk.
void ModelCacheInvalidate(const std::string& path);
} // namespace rco::renderer
