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
} // namespace rco::renderer
