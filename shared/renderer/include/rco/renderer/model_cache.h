#pragma once
#include "model.h"
#include "material.h"
#include <string>
#include <memory>

namespace rco::renderer {
using ModelCacheObserver = void (*)(const char* path, bool hit, const char* context);

// Returns a cached Model for path, loading it on first access.
// Subsequent calls return the existing instance. Cache retains strong
// references so prewarmed models stay resident until explicitly evicted.
std::shared_ptr<Model> ModelCacheGet(const std::string& path, MaterialManager* mm);

// Returns the model if it is already in the cache, without triggering a load.
// Returns nullptr if the model has not been loaded yet. Use this to avoid
// blocking the render thread on disk I/O (e.g., thumbnail rendering).
std::shared_ptr<Model> ModelCachePeek(const std::string& path);

// Drop the cache entry for `path` so the next ModelCacheGet reloads from disk.
void ModelCacheInvalidate(const std::string& path);

// Drop all cache entries so subsequent ModelCacheGet calls reload from disk.
void ModelCacheEvictAll();

// Optional instrumentation hook for ModelCacheGet hit/miss events.
void ModelCacheSetObserver(ModelCacheObserver observer);

// Optional per-thread context string attached to observer callbacks.
void ModelCacheSetContext(const char* context);
} // namespace rco::renderer
