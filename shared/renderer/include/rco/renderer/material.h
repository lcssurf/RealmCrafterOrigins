#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include "rco/renderer/texture.h"

namespace rco::renderer {

// A registered PBR material. At most one side is populated — either the
// Texture2D* pointers (manager-owned, loaded from disk via MakeMaterial) or
// just the bindless handles (externally-owned textures registered via
// RegisterFromHandles). The SSBO layout reads only the handles.
struct Material {
    Texture2D* albedoTex           {};
    Texture2D* roughnessTex        {};
    Texture2D* metalnessTex        {};
    Texture2D* normalTex           {};
    Texture2D* ambientOcclusionTex {};
    Texture2D* opacityTex          {};

    uint64_t albedoHandle           {};
    uint64_t roughnessHandle        {};
    uint64_t metalnessHandle        {};
    uint64_t normalHandle           {};
    uint64_t ambientOcclusionHandle {};
    uint64_t opacityHandle          {};
    glm::vec4 albedoFactor {1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec4 pbrFactors   {1.0f, 0.0f, 1.0f, 0.0f};
};

// GPU-side layout — exactly 6× uint64_t per entry (matches gBufferBindless.fs).
struct BindlessMaterial {
    uint64_t albedoHandle           {};
    uint64_t roughnessHandle        {};
    uint64_t metalnessHandle        {};
    uint64_t normalHandle           {};
    uint64_t ambientOcclusionHandle {};
    uint64_t opacityHandle          {};
    glm::vec4 albedoFactor {1.0f, 1.0f, 1.0f, 0.0f};
    glm::vec4 pbrFactors   {1.0f, 0.0f, 1.0f, 0.0f};
};

class MaterialManager {
public:
    MaterialManager() = default;
    ~MaterialManager();

    std::optional<Material> GetMaterial(const std::string& mat);

    // Load textures from disk and register as a new material. Returns existing
    // entry if name was already registered. (Index is not returned — use
    // IndexOf afterwards if needed.)
    Material& MakeMaterial(std::string name,
                           std::string albedoTexName,
                           std::string roughnessTexName,
                           std::string metalnessTexName,
                           std::string normalTexName,
                           std::string ambientOcclusionTexName);

    // Register a material from already-resident GL textures (e.g. textures
    // extracted by Assimp from an embedded GLB). Any id=0 means "no texture"
    // — shader falls back to sane defaults for that channel.
    //
    // `orm` may be either:
    // - packed ORM (R=AO, G=roughness, B=metallic) when ormPacked=true
    // - metalness-only grayscale map when ormPacked=false
    //   (roughness then falls back to factor/default path)
    //
    // Returns the stable index of the material in the bindless SSBO. If the
    // same name has already been registered, returns its existing index.
    int RegisterFromHandles(const std::string& name,
                            unsigned int albedo,
                            unsigned int normal   = 0,
                            unsigned int orm      = 0,
                            unsigned int opacity  = 0,
                            unsigned int ao       = 0,
                            bool ormPacked = true,
                            const glm::vec3& albedoFactor = glm::vec3(1.0f),
                            float roughnessFactor = 1.0f,
                            float metallicFactor  = 0.0f,
                            float aoFactor        = 1.0f,
                            bool blackCutout      = false);

    // Name → index (-1 if unknown).
    int IndexOf(const std::string& name) const;

    // Returns materials in insertion order (same order used to build the SSBO).
    std::vector<std::pair<std::string, Material>> GetLinearMaterials();
    std::vector<BindlessMaterial>                 GetLinearBindless() const;

private:
    std::unordered_map<std::string, Material> materials;
    std::vector<std::string>                  insertionOrder_;
    std::unordered_map<std::string, int>      nameToIndex_;

    int appendName_(const std::string& name);
};

} // namespace rco::renderer
