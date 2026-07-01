#include "rco/renderer/material.h"
#include <glad/glad.h>
#include <algorithm>

namespace rco::renderer {

MaterialManager::~MaterialManager() {
    // Don't try to non-resident bindless handles here — for externally-owned
    // textures (Assimp models), the GL texture may already have been deleted
    // by the Model destructor, which invalidates the handle. Touching it is
    // undefined behaviour. Manager-owned Texture2D objects, on the other
    // hand, delete their GL resources in ~Texture2D which also invalidates
    // any handles the driver held for them.
    for (auto& p : materials) {
        delete p.second.albedoTex;
        delete p.second.roughnessTex;
        delete p.second.metalnessTex;
        delete p.second.normalTex;
        delete p.second.ambientOcclusionTex;
        delete p.second.opacityTex;
    }
}

int MaterialManager::appendName_(const std::string& name) {
    int idx = (int)insertionOrder_.size();
    insertionOrder_.push_back(name);
    nameToIndex_[name] = idx;
    return idx;
}

std::optional<Material> MaterialManager::GetMaterial(const std::string& mat) {
    auto it = materials.find(mat);
    if (it == materials.end()) return std::nullopt;
    return it->second;
}

int MaterialManager::IndexOf(const std::string& name) const {
    auto it = nameToIndex_.find(name);
    return (it == nameToIndex_.end()) ? -1 : it->second;
}

// Build a resident bindless handle for an existing GL texture. Returns 0 if
// the input is 0.
static uint64_t MakeResidentHandle_(unsigned int id) {
    if (!id) return 0;
    GLuint64 h = glGetTextureHandleARB(id);
    if (!h) return 0;
    if (!glIsTextureHandleResidentARB(h))
        glMakeTextureHandleResidentARB(h);
    return h;
}

Material& MaterialManager::MakeMaterial(std::string name,
                                        std::string albedoTexName,
                                        std::string roughnessTexName,
                                        std::string metalnessTexName,
                                        std::string normalTexName,
                                        std::string ambientOcclusionTexName) {
    if (auto it = materials.find(name); it != materials.end())
        return it->second;

    TextureCreateInfo info;
    info.generateMips = true;
    info.HDR          = false;
    info.minFilter    = GL_LINEAR_MIPMAP_LINEAR;
    info.magFilter    = GL_LINEAR;
    info.sRGB         = false;

    Material material;
    info.path = albedoTexName;           material.albedoTex            = new Texture2D(info);
    info.path = roughnessTexName;        material.roughnessTex         = new Texture2D(info);
    info.path = metalnessTexName;        material.metalnessTex         = new Texture2D(info);
    info.path = normalTexName;           material.normalTex            = new Texture2D(info);
    info.path = ambientOcclusionTexName; material.ambientOcclusionTex  = new Texture2D(info);

    material.albedoHandle           = material.albedoTex           ? material.albedoTex->GetBindlessHandle()           : 0;
    material.roughnessHandle        = material.roughnessTex        ? material.roughnessTex->GetBindlessHandle()        : 0;
    material.metalnessHandle        = material.metalnessTex        ? material.metalnessTex->GetBindlessHandle()        : 0;
    material.normalHandle           = material.normalTex           ? material.normalTex->GetBindlessHandle()           : 0;
    material.ambientOcclusionHandle = material.ambientOcclusionTex ? material.ambientOcclusionTex->GetBindlessHandle() : 0;

    auto p = materials.insert({ name, material });
    appendName_(name);
    return p.first->second;
}

int MaterialManager::RegisterFromHandles(const std::string& name,
                                          unsigned int albedo,
                                          unsigned int normal,
                                          unsigned int orm,
                                          unsigned int opacity,
                                          unsigned int ao,
                                          bool ormPacked,
                                          const glm::vec3& albedoFactor,
                                          float roughnessFactor,
                                          float metallicFactor,
                                          float aoFactor,
                                          bool blackCutout) {
    uint64_t aH      = MakeResidentHandle_(albedo);
    uint64_t nH      = MakeResidentHandle_(normal);
    uint64_t ormH    = MakeResidentHandle_(orm);
    uint64_t opacH   = MakeResidentHandle_(opacity);
    uint64_t aoH     = MakeResidentHandle_(ao);
    glm::vec4 baseFactor{
        std::clamp(albedoFactor.r, 0.0f, 4.0f),
        std::clamp(albedoFactor.g, 0.0f, 4.0f),
        std::clamp(albedoFactor.b, 0.0f, 4.0f),
        blackCutout ? 1.0f : 0.0f,
    };
    glm::vec4 pbrFactors{
        std::clamp(roughnessFactor, 0.0f, 1.0f),
        std::clamp(metallicFactor, 0.0f, 1.0f),
        std::clamp(aoFactor, 0.0f, 1.0f),
        ormPacked ? 1.0f : 0.0f,
    };
    uint64_t roughH = ormH;
    uint64_t metalH = ormH;
    if (!ormPacked) {
        roughH = 0;
        metalH = ormH;
    }

    if (auto it = nameToIndex_.find(name); it != nameToIndex_.end()) {
        int idx = it->second;
        Material& m = materials[name];
        m.albedoHandle           = aH;
        m.normalHandle           = nH;
        m.roughnessHandle        = roughH;
        m.metalnessHandle        = metalH;
        m.opacityHandle          = opacH;
        m.ambientOcclusionHandle = aoH;
        m.albedoFactor           = baseFactor;
        m.pbrFactors             = pbrFactors;
        return idx;
    }

    Material m;
    m.albedoHandle           = aH;
    m.normalHandle           = nH;
    m.roughnessHandle        = roughH;
    m.metalnessHandle        = metalH;
    m.opacityHandle          = opacH;
    m.ambientOcclusionHandle = aoH;
    m.albedoFactor           = baseFactor;
    m.pbrFactors             = pbrFactors;

    materials.insert({ name, m });
    return appendName_(name);
}

std::vector<std::pair<std::string, Material>> MaterialManager::GetLinearMaterials() {
    std::vector<std::pair<std::string, Material>> out;
    out.reserve(insertionOrder_.size());
    for (const auto& name : insertionOrder_) {
        auto it = materials.find(name);
        if (it != materials.end()) out.emplace_back(name, it->second);
    }
    return out;
}

std::vector<BindlessMaterial> MaterialManager::GetLinearBindless() const {
    std::vector<BindlessMaterial> out;
    out.reserve(insertionOrder_.size());
    for (const auto& name : insertionOrder_) {
        auto it = materials.find(name);
        if (it == materials.end()) continue;
        const Material& m = it->second;
        out.push_back(BindlessMaterial{
            .albedoHandle           = m.albedoHandle,
            .roughnessHandle        = m.roughnessHandle,
            .metalnessHandle        = m.metalnessHandle,
            .normalHandle           = m.normalHandle,
            .ambientOcclusionHandle = m.ambientOcclusionHandle,
            .opacityHandle          = m.opacityHandle,
            .albedoFactor           = m.albedoFactor,
            .pbrFactors             = m.pbrFactors,
        });
    }
    return out;
}

} // namespace rco::renderer
