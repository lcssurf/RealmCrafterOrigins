# Fase 1 detalhada — Cópia e conversão `.ixx` → `.h/.cpp`

Tutorial auto-contido. Execute os passos **em ordem**, na íntegra, sem pular.
Cada passo tem critério de sucesso no final.

## Pré-requisitos

- Diretório de trabalho: `D:/Github/RealmCrafterOrigins/`
- `glRenderer/` existe e tem os arquivos fonte esperados
- `shared/renderer/` existe com `shader.h/.cpp`, `model.h/.cpp`, `stb_image_impl.cpp`
- `dist/client/shaders/` existe com shaders antigos do RCO

## Regras gerais (aplicam a TODOS os arquivos convertidos)

1. **Namespace obrigatório**: todo código convertido de `.ixx` entra em `namespace rco::renderer { ... }`
2. **Remover `module;`, `export module X;`, `export`**: essas palavras-chave não existem em `.h/.cpp`
3. **Substituir `import X;` por `#include "X.h"`** com path relativo
4. **MSVC-específico — trocar**:
   - `_countof(x)` → `std::size(x)` (precisa `#include <iterator>`)
   - `#pragma warning(disable : ...; suppress : ...)` → remover (código funciona sem)
   - `__forceinline` → `inline`
5. **Split `.h` vs `.cpp`**:
   - **Fica no `.h`**: declarações de struct/class, inline methods, templates, constexpr, `static inline` members
   - **Vai pro `.cpp`**: métodos com corpo não-trivial (≥3 linhas ou chamam OpenGL), free functions com corpo grande, definição de `static` members não-inline
6. **Include guards**: usar `#pragma once` no topo de todo `.h`

---

## Passo 1.1 — Preparar diretórios

Criar diretórios que podem não existir:

```bash
mkdir -p D:/Github/RealmCrafterOrigins/dist/client/assets/ibl
mkdir -p D:/Github/RealmCrafterOrigins/dist/client/assets/textures
mkdir -p D:/Github/RealmCrafterOrigins/shared/renderer/include/rco/renderer
mkdir -p D:/Github/RealmCrafterOrigins/shared/renderer/src
```

**Sucesso**: os 4 diretórios existem.

---

## Passo 1.2 — Copiar 35 shaders GLSL

Copiar **todo o conteúdo** de `glRenderer/Resources/Shaders/` para `dist/client/shaders/`
**sem sobrescrever** os shaders antigos do RCO (serão removidos na Fase 6).

Arquivos a copiar (35 total):
```
atrous.fs                 atrous_ssao.fs            atrous_volumetric.fs
basic.fs                  basic.vs
calc_exposure.cs          common.h                  esm_copy.fs
fullscreen_tex.fs         fullscreen_tri.vs         fxaa.fs
gaussian.cs               gBuffer.fs                gBuffer.vs
gBufferBindless.fs        gBufferBindless.vs
generate_histogram.cs     gPhongGlobal.fs           gPhongManyLocal.fs
hdri_skybox.fs            irradiance_convolve.cs
lightGeom.vs              msm_copy.fs               pbr_common.h
shadow.vs                 shadowBindless.vs         ssao.fs
ssr.fs                    texture.fs                texture_depth.fs
tonemap.fs                volumetric.fs             vsm.fs
vsm_copy.fs
```

**Importante**: `common.h` e `pbr_common.h` são arquivos GLSL (com `#include` resolvido em
runtime), copiar para o mesmo diretório dos outros shaders.

**Sucesso**: `ls dist/client/shaders/` mostra pelo menos 35 arquivos dos listados acima.

---

## Passo 1.3 — Copiar bluenoise e HDRI

**Bluenoise** (usado pelo volumetric shader):
```
glRenderer/Resources/Textures/bluenoise_64.png → dist/client/assets/textures/bluenoise_64.png
```

**HDRI para IBL + skybox** — usar **exatamente** o arquivo:
```
glRenderer/Resources/IBL/14-Hamarikyu_Bridge_B_3k.hdr → dist/client/assets/ibl/default.hdr
```

(Escolhido porque é o mesmo que o glRenderer carrega por padrão no Scene1.)

**Sucesso**:
- `dist/client/assets/textures/bluenoise_64.png` existe
- `dist/client/assets/ibl/default.hdr` existe e tem > 1 MB

---

## Passo 1.4 — Preservar código antigo (rename para `_old`)

O `shared/renderer/` já tem `shader.h/.cpp` e `model.h/.cpp` que são usados pelo client
atual. Renomear para preservar durante a transição:

```
shared/renderer/include/rco/renderer/shader.h  → shader_old.h
shared/renderer/include/rco/renderer/model.h   → model_old.h
shared/renderer/src/shader.cpp                 → shader_old.cpp
shared/renderer/src/model.cpp                  → model_old.cpp
```

Dentro dos arquivos renomeados, trocar as referências internas:
- Em `shader_old.cpp`: `#include "shader.h"` → `#include "rco/renderer/shader_old.h"` (ou manter relativo)
- Em `model_old.cpp`: mesma lógica

Atualizar `shared/renderer/CMakeLists.txt` temporariamente para referenciar os novos nomes:

```cmake
add_library(rco_renderer STATIC
    src/shader_old.cpp
    src/model_old.cpp
    src/stb_image_impl.cpp
)
```

**Sucesso**:
- `cmake --build` do `client` ainda funciona (client usa o código antigo)
- Arquivos `_old.h/.cpp` existem

---

## Passo 1.5 — Converter `.ixx` em ordem de dependência

**Ordem obrigatória** (a→j). Cada sub-passo gera 1 ou 2 arquivos. Não inverter a ordem.

### 1.5.a — `Utilities.ixx` → `utilities.h` (header-only)

Fonte: `glRenderer/Utilities.ixx` (68 linhas)

Gerar: `shared/renderer/include/rco/renderer/utilities.h`

Conteúdo:

```cpp
#pragma once
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <ratio>

namespace rco::renderer {

namespace detail {
    inline thread_local uint64_t xshf_x = 123456789;
    inline thread_local uint64_t xshf_y = 362436069;
    inline thread_local uint64_t xshf_z = 521288629;

    inline uint64_t xorshf96() {
        xshf_x ^= xshf_x << 16;
        xshf_x ^= xshf_x >> 5;
        xshf_x ^= xshf_x << 1;
        uint64_t t = xshf_x;
        xshf_x = xshf_y;
        xshf_y = xshf_z;
        xshf_z = t ^ xshf_x ^ xshf_y;
        return xshf_z;
    }
}

class Timer {
public:
    Timer() : beg_(clock_::now()) {}
    void reset() { beg_ = clock_::now(); }
    double elapsed() const {
        return std::chrono::duration_cast<second_>(clock_::now() - beg_).count();
    }
private:
    using clock_  = std::chrono::high_resolution_clock;
    using second_ = std::chrono::duration<double, std::ratio<1>>;
    std::chrono::time_point<clock_> beg_;
};

inline double rng() {
    uint64_t bits = 1023ull << 52ull | (detail::xorshf96() & 0xfffffffffffffull);
    return *reinterpret_cast<double*>(&bits) - 1.0;
}

template<typename T, typename Q>
T map(T val, Q r1s, Q r1e, Q r2s, Q r2e) {
    return (val - r1s) / (r1e - r1s) * (r2e - r2s) + r2s;
}

inline double rng(double low, double high) {
    return map(rng(), 0.0, 1.0, low, high);
}

inline void hash_combine(std::size_t&) {}

template <typename T, typename... Rest>
void hash_combine(std::size_t& seed, const T& v, Rest... rest) {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    hash_combine(seed, rest...);
}

} // namespace rco::renderer
```

**Sucesso**: arquivo existe, compila sozinho com `-std=c++20`.

### 1.5.b — `IndirectDraw.ixx` → `indirect.h` (header-only)

Fonte: `glRenderer/IndirectDraw.ixx` (22 linhas)

Gerar: `shared/renderer/include/rco/renderer/indirect.h`

```cpp
#pragma once
#include <glad/glad.h>

namespace rco::renderer {

struct DrawElementsIndirectCommand {
    GLuint count         = 0;
    GLuint instanceCount = 0;
    GLuint firstIndex    = 0;
    GLuint baseVertex    = 0;
    GLuint baseInstance  = 0;
};

struct DrawArraysIndirectCommand {
    GLuint count         = 0;
    GLuint instanceCount = 0;
    GLuint first         = 0;
    GLuint baseInstance  = 0;
};

} // namespace rco::renderer
```

**Sucesso**: arquivo existe.

### 1.5.c — `Light.ixx` → `light.h` + `light.cpp`

Fonte: `glRenderer/Light.ixx` (53 linhas)

`MakeLightMatrix` e `CalcRadiusSquared` têm corpos não-triviais — vão pro `.cpp`.

Gerar `shared/renderer/include/rco/renderer/light.h`:

```cpp
#pragma once
#include <glm/glm.hpp>

namespace rco::renderer {

struct PointLight {
    glm::vec4 diffuse       {};
    glm::vec4 position      {};
    float     linear        {};
    float     quadratic     {};
    float     radiusSquared {};
    float     _padding      {};

    float CalcRadiusSquared(float epsilon) const;
};

struct DirLight {
    glm::vec3 diffuse   {};
    glm::vec3 direction {};
};

glm::mat4 MakeLightMatrix(const DirLight& light, glm::vec3 eye,
                          glm::vec2 dim, glm::vec2 depthRange);

} // namespace rco::renderer
```

Gerar `shared/renderer/src/light.cpp`:

```cpp
#include "rco/renderer/light.h"
#include <cassert>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rco::renderer {

float PointLight::CalcRadiusSquared(float epsilon) const {
    assert(epsilon > 0.0f);
    float luminance = glm::max(diffuse.x, glm::max(diffuse.y, diffuse.z));
    float L = linear;
    float Q = quadratic;
    if (glm::epsilonEqual(Q, 0.f, .001f)) {
        return luminance / (L * epsilon);
    }
    float discriminant = glm::sqrt(L * L - 4 * (Q * (-1 / epsilon)));
    float root1 = (-L + discriminant) / (2 * Q);
    return luminance * glm::pow(root1, 2.0f);
}

glm::mat4 MakeLightMatrix(const DirLight& light, glm::vec3 eye,
                          glm::vec2 dim, glm::vec2 depthRange) {
    glm::mat4 lightView = glm::lookAt(eye, eye + light.direction, glm::vec3(0, 1, 0));
    glm::mat4 lightProj = glm::ortho(
        -dim.x / 2, dim.x / 2, -dim.y / 2, dim.y / 2,
        depthRange.x, depthRange.y);
    return lightProj * lightView;
}

} // namespace rco::renderer
```

**Sucesso**: ambos arquivos existem.

### 1.5.d — `Object.ixx` → `object.h` (header-only)

Fonte: `glRenderer/Object.ixx` (50 linhas)

`ObjectBatched` depende de `MeshInfo` que vem em 1.5.h. Por ordem de dependência,
**pular `ObjectBatched` por enquanto** — adicionar depois quando `mesh.h` existir.

Gerar `shared/renderer/include/rco/renderer/object.h`:

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rco::renderer {

struct Transform {
    glm::vec3 translation { 0, 0, 0 };
    glm::quat rotation    { 1, 0, 0, 0 };
    glm::vec3 scale       { 1, 1, 1 };

    glm::mat4 GetModelMatrix() const {
        glm::mat4 m(1);
        m = glm::translate(m, translation);
        m *= mat4_cast(rotation);
        m = glm::scale(m, scale);
        return m;
    }
    glm::mat4 GetNormalMatrix() const {
        return glm::transpose(glm::inverse(glm::mat3(GetModelMatrix())));
    }
};

struct alignas(16) ObjectUniforms {
    glm::mat4 modelMatrix   {};
    uint32_t  materialIndex {};
};

// ObjectBatched adicionado no passo 1.5.h (depende de MeshInfo)

} // namespace rco::renderer
```

**Sucesso**: arquivo existe.

### 1.5.e — `StaticBuffer.ixx` + `DynamicBuffer.ixx` → `buffers.h` + `buffers.cpp`

**Unificar** em um único par `.h/.cpp` (as duas classes são correlatas).

Gerar `shared/renderer/include/rco/renderer/buffers.h`:

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <utility>
#include <glad/glad.h>
#include "rco/renderer/utilities.h"

namespace rco::renderer {

class StaticBuffer {
public:
    StaticBuffer(const void* data, size_t size, uint32_t glflags);
    StaticBuffer(StaticBuffer&& other) noexcept;
    ~StaticBuffer();

    StaticBuffer(const StaticBuffer&)            = delete;
    StaticBuffer& operator=(const StaticBuffer&) = delete;
    StaticBuffer& operator=(StaticBuffer&&)      = delete;
    bool operator==(const StaticBuffer&) const = default;

    void SubData(const void* data, size_t size, size_t offset);
    void Bind(uint32_t target);
    void BindBase(uint32_t target, uint32_t index);
    uint32_t ID() { return id_; }

private:
    uint32_t id_ = 0;
};

class DynamicBuffer {
public:
    struct allocationData {
        uint64_t handle {};
        double   time   {};
        uint32_t flags  {};
        uint32_t _pad   {};
        uint32_t offset {};
        uint32_t size   {};
    };

    DynamicBuffer(uint32_t size, uint32_t alignment);
    ~DynamicBuffer();

    uint64_t Allocate(const void* data, size_t size);
    bool     Free(uint64_t handle);
    void     Clear();
    uint64_t FreeOldest();

    const allocationData& GetAlloc(uint64_t handle);
    const std::vector<allocationData>& GetAllocs() { return allocs_; }
    GLuint   ActiveAllocs()   { return numActiveAllocs_; }
    GLuint   GetBufferHandle(){ return buffer; }
    std::pair<uint64_t, GLuint> GetStateInfo() { return { nextHandle, numActiveAllocs_ }; }

    const GLsizei align_;
    constexpr size_t AllocSize() const { return sizeof(allocationData); }

protected:
    std::vector<allocationData> allocs_;
    using Iterator = decltype(allocs_.begin());

    void stateChanged();
    void maybeMerge(Iterator it);

    GLuint   buffer           {};
    uint64_t nextHandle       { 1 };
    GLuint   numActiveAllocs_ { 0 };
    const GLuint capacity_;
    Timer timer;
};

} // namespace rco::renderer
```

Gerar `shared/renderer/src/buffers.cpp`:

Copiar **todos os corpos de método** de `StaticBuffer.ixx` e `DynamicBuffer.ixx`
(linhas 34–64 e 78–278 respectivamente), ajustando:

```cpp
#include "rco/renderer/buffers.h"
#include <algorithm>

namespace rco::renderer {

// === StaticBuffer ===
StaticBuffer::StaticBuffer(const void* data, size_t size, uint32_t glflags) {
    glCreateBuffers(1, &id_);
    glNamedBufferStorage(id_, static_cast<GLsizeiptr>(size), data,
                         static_cast<GLbitfield>(glflags));
}

StaticBuffer::StaticBuffer(StaticBuffer&& other) noexcept {
    id_ = std::exchange(other.id_, 0);
}

StaticBuffer::~StaticBuffer() { glDeleteBuffers(1, &id_); }

void StaticBuffer::Bind(uint32_t target)                     { glBindBuffer(target, id_); }
void StaticBuffer::BindBase(uint32_t target, uint32_t index) {
    glBindBuffer(target, id_);
    glBindBufferBase(target, index, id_);
}
void StaticBuffer::SubData(const void* data, size_t size, size_t offset) {
    glNamedBufferSubData(id_, static_cast<GLintptr>(offset),
                         static_cast<GLsizeiptr>(size), data);
}

// === DynamicBuffer ===
// Copiar as implementações de DynamicBuffer.ixx linhas 78-246 aqui
// (DynamicBuffer::DynamicBuffer, ~DynamicBuffer, Allocate, Free, Clear,
//  FreeOldest, stateChanged, maybeMerge, GetAlloc)
// Remover o corpo de dbgVerify (é só comentários, vira função vazia)

// ... (CONTINUAR COM TODAS AS IMPLEMENTAÇÕES DE DynamicBuffer) ...

} // namespace rco::renderer
```

**⚠️ Nota pra IA executora**: para `DynamicBuffer`, copie TODAS as 8 implementações de
`DynamicBuffer.ixx` (linhas 78-246), aplicando as regras gerais (remover `export`, etc).

**Sucesso**: ambos arquivos existem, compilam juntos sem erros.

### 1.5.f — `Texture.ixx` → `texture.h` + `texture.cpp`

Fonte: `glRenderer/Texture.ixx` (126 linhas)

**⚠️ Remover `#define STB_IMAGE_IMPLEMENTATION`** — já está em `stb_image_impl.cpp`.
Usar só `#include <stb_image.h>` (caminho depende do setup do vcpkg — pode ser
`<stb/stb_image.h>`).

Gerar `shared/renderer/include/rco/renderer/texture.h`:

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <glm/glm.hpp>
#include <glad/glad.h>

namespace rco::renderer {

struct TextureCreateInfo {
    std::string path;
    bool sRGB        = false;
    bool generateMips= true;
    bool HDR         = false;
    int  minFilter   = GL_LINEAR_MIPMAP_LINEAR;
    int  magFilter   = GL_LINEAR;
};

class Texture2D {
public:
    explicit Texture2D(const TextureCreateInfo& info);
    Texture2D(const Texture2D&)            = delete;
    Texture2D& operator=(Texture2D&&) noexcept;
    Texture2D(Texture2D&&) noexcept;
    ~Texture2D();

    void     Bind(unsigned slot = 0) const;
    unsigned GetID()               const { return id_; }
    uint64_t GetBindlessHandle()   const { return bindlessHandle_; }
    bool     Valid()               const { return id_ != 0; }
    glm::ivec2 GetSize()           const { return dim_; }

private:
    unsigned   id_             {};
    uint64_t   bindlessHandle_ {};
    glm::ivec2 dim_            {};
};

} // namespace rco::renderer
```

Gerar `shared/renderer/src/texture.cpp`:

Copiar corpos de `Texture.ixx` linhas 47–126 dentro de `namespace rco::renderer { ... }`,
ajustando includes:

```cpp
#include "rco/renderer/texture.h"
#include <cassert>
#include <filesystem>
#include <stb_image.h>  // ou <stb/stb_image.h> — confirmar com CMake

namespace rco::renderer {

Texture2D::Texture2D(const TextureCreateInfo& createInfo) {
    assert(!(createInfo.sRGB && createInfo.HDR));
    stbi_set_flip_vertically_on_load(true);

    std::string tex = createInfo.path;
    bool hasTex = std::filesystem::exists(tex) &&
                  std::filesystem::is_regular_file(tex);
    if (!hasTex) return;

    int n;
    auto pixels = stbi_loadf(tex.c_str(), &dim_.x, &dim_.y, &n, 4);
    assert(pixels != nullptr);

    glCreateTextures(GL_TEXTURE_2D, 1, &id_);

    GLfloat a;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &a);
    glTextureParameterf(id_, GL_TEXTURE_MAX_ANISOTROPY, a);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, createInfo.minFilter);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, createInfo.magFilter);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_REPEAT);

    GLuint levels = 1;
    if (createInfo.generateMips) {
        levels = (GLuint)glm::ceil(glm::log2((float)glm::min(dim_.x, dim_.y)));
    }

    const GLenum internalFormat = createInfo.sRGB ? GL_SRGB8_ALPHA8
                                 : createInfo.HDR  ? GL_RGBA16F
                                                    : GL_RGBA8;
    glTextureStorage2D(id_, levels, internalFormat, dim_.x, dim_.y);
    glTextureSubImage2D(id_, 0, 0, 0, dim_.x, dim_.y, GL_RGBA, GL_FLOAT, pixels);
    stbi_image_free(pixels);

    if (createInfo.generateMips) glGenerateTextureMipmap(id_);

    bindlessHandle_ = glGetTextureHandleARB(id_);
    glMakeTextureHandleResidentARB(bindlessHandle_);
}

Texture2D& Texture2D::operator=(Texture2D&& rhs) noexcept {
    if (&rhs == this) return *this;
    return *new (this) Texture2D(std::move(rhs));
}

Texture2D::Texture2D(Texture2D&& rhs) noexcept {
    id_  = std::exchange(rhs.id_, 0);
    dim_ = rhs.dim_;
    bindlessHandle_ = std::exchange(rhs.bindlessHandle_, 0);
}

Texture2D::~Texture2D() { glDeleteTextures(1, &id_); }

void Texture2D::Bind(unsigned slot) const { glBindTextureUnit(slot, id_); }

} // namespace rco::renderer
```

**Sucesso**: ambos arquivos existem.

### 1.5.g — `Material.ixx` → `material.h` + `material.cpp`

Fonte: `glRenderer/Material.ixx` (106 linhas)

Gerar `shared/renderer/include/rco/renderer/material.h`:

```cpp
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "rco/renderer/texture.h"

namespace rco::renderer {

struct Material {
    Texture2D* albedoTex            {};
    Texture2D* roughnessTex         {};
    Texture2D* metalnessTex         {};
    Texture2D* normalTex            {};
    Texture2D* ambientOcclusionTex  {};
};

struct BindlessMaterial {
    uint64_t albedoHandle            {};
    uint64_t roughnessHandle         {};
    uint64_t metalnessHandle         {};
    uint64_t normalHandle            {};
    uint64_t ambientOcclusionHandle  {};
};

class MaterialManager {
public:
    MaterialManager() = default;
    ~MaterialManager();

    std::optional<Material> GetMaterial(const std::string& mat);

    Material& MakeMaterial(std::string name,
                           std::string albedoTexName,
                           std::string roughnessTexName,
                           std::string metalnessTexName,
                           std::string normalTexName,
                           std::string ambientOcclusionTexName);

    std::vector<std::pair<std::string, Material>> GetLinearMaterials() {
        return { materials.begin(), materials.end() };
    }

private:
    std::unordered_map<std::string, Material> materials;
};

} // namespace rco::renderer
```

Gerar `shared/renderer/src/material.cpp`:

Copiar corpos de `Material.ixx` linhas 52–106:

```cpp
#include "rco/renderer/material.h"
#include <glad/glad.h>

namespace rco::renderer {

MaterialManager::~MaterialManager() {
    for (auto& p : materials) {
        delete p.second.albedoTex;
        delete p.second.roughnessTex;
        delete p.second.metalnessTex;
        delete p.second.normalTex;
        delete p.second.ambientOcclusionTex;
    }
}

std::optional<Material> MaterialManager::GetMaterial(const std::string& mat) {
    auto it = materials.find(mat);
    if (it == materials.end()) return std::nullopt;
    return it->second;
}

Material& MaterialManager::MakeMaterial(std::string name,
                                        std::string albedoTexName,
                                        std::string roughnessTexName,
                                        std::string metalnessTexName,
                                        std::string normalTexName,
                                        std::string ambientOcclusionTexName) {
    if (auto it = materials.find(name); it != materials.end()) {
        return it->second;
    }

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

    auto p = materials.insert({ std::move(name), material });
    return p.first->second;
}

} // namespace rco::renderer
```

**Sucesso**: ambos arquivos existem.

### 1.5.h — `Mesh.ixx` → `mesh.h` + `mesh.cpp` (+ finalizar `object.h`)

Fonte: `glRenderer/Mesh.ixx` (336 linhas)

**⚠️ Mudança importante**: as funções `LoadObjMesh` e `LoadObjBatch` usam TinyOBJ.
**Não copiar essas funções** — o RCO usa Assimp. Elas serão substituídas por adaptadores
sobre `model.h` numa fase futura.

Gerar `shared/renderer/include/rco/renderer/mesh.h`:

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include "rco/renderer/utilities.h"
#include "rco/renderer/material.h"

namespace rco::renderer {

struct Vertex {
    glm::vec3 position {};
    glm::vec3 normal   {};
    glm::vec2 uv       {};
    glm::vec3 tangent  {};

    bool operator==(const Vertex& v) const {
        return position == v.position && normal == v.normal && uv == v.uv;
    }
};

class Mesh {
public:
    Mesh() = default;
    Mesh(const std::vector<Vertex>& vertices,
         const std::vector<uint32_t>& indices,
         const Material& mat);
    ~Mesh();

    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    unsigned GetVBOID()       const { return vboID; }
    unsigned GetEBOID()       const { return eboID; }
    size_t   GetVertexCount() const { return vertexCount; }
    const Material& GetMaterial() const { return material; }

private:
    unsigned vboID       {};
    unsigned eboID       {};
    size_t   vertexCount {};
    Material material    {};
};

struct MeshInfo {
    uint64_t verticesAllocHandle {};
    uint64_t indicesAllocHandle  {};
    std::string materialName     {};
    uint32_t materialIndex       {};
};

} // namespace rco::renderer

// Especialização de std::hash para Vertex (fora do namespace)
namespace std {
    template<> struct hash<rco::renderer::Vertex> {
        std::size_t operator()(const rco::renderer::Vertex& v) const noexcept {
            std::size_t h1 = std::hash<glm::vec3>{}(v.position);
            std::size_t h2 = std::hash<glm::vec3>{}(v.normal);
            std::size_t h3 = std::hash<glm::vec2>{}(v.uv);
            std::size_t seed = 0;
            rco::renderer::hash_combine(seed, h1, h2, h3);
            return seed;
        }
    };
}
```

Gerar `shared/renderer/src/mesh.cpp`:

```cpp
#include "rco/renderer/mesh.h"
#include <utility>
#include <new>

namespace rco::renderer {

Mesh::Mesh(const std::vector<Vertex>& vertices,
           const std::vector<uint32_t>& indices,
           const Material& mat)
    : vertexCount(indices.size()), material(mat) {
    glCreateBuffers(1, &vboID);
    glCreateBuffers(1, &eboID);
    glNamedBufferStorage(vboID, vertices.size() * sizeof(Vertex), vertices.data(), 0);
    glNamedBufferStorage(eboID, indices.size() * sizeof(uint32_t), indices.data(), 0);
}

Mesh::~Mesh() {
    glDeleteBuffers(1, &vboID);
    glDeleteBuffers(1, &eboID);
}

Mesh::Mesh(Mesh&& other) noexcept : material(other.material) {
    vboID       = std::exchange(other.vboID, 0);
    eboID       = std::exchange(other.eboID, 0);
    vertexCount = std::exchange(other.vertexCount, 0);
}

Mesh& Mesh::operator=(Mesh&& other) noexcept {
    if (&other == this) return *this;
    this->~Mesh();
    return *new(this) Mesh(std::move(other));
}

} // namespace rco::renderer
```

**Agora finalizar `object.h`** — adicionar `ObjectBatched` que depende de `MeshInfo`:

Adicionar ao final de `object.h` (antes do `} // namespace`):

```cpp
struct ObjectBatched {
    Transform transform;
    std::vector<MeshInfo> meshes;
};
```

E adicionar `#include "rco/renderer/mesh.h"` no topo do `object.h`.

**Sucesso**: `mesh.h`, `mesh.cpp` existem; `object.h` tem `ObjectBatched` agora.

### 1.5.i — `Shader.h/.cpp` adaptado (sem shaderc)

Fonte: `glRenderer/Shader.h` (194 linhas) + `Shader.cpp` (285 linhas)

**Esta é a conversão mais delicada**. Mudanças vs original:
1. Remover `#include <shaderc/shaderc.hpp>` e toda a classe `IncludeHandler`
2. Remover `spvPreprocessAndCompile` e `preprocessShader`
3. Adicionar função `resolveIncludes` (inlining manual de `#include`)
4. `ShaderInfo::replace` (vector) vira `ShaderInfo::preamble` (string)
5. `compileShader` usa multi-string `glShaderSource` para injetar preamble
6. `loadFile` path vira configurável via `shader_dir_`
7. Entrar no namespace `rco::renderer`

Gerar `shared/renderer/include/rco/renderer/shader.h`:

```cpp
#pragma once
#include <cassert>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>

namespace rco::renderer {

struct ShaderInfo {
    std::string path;      // relativo a shader_dir_
    GLenum      type;
    std::string preamble;  // injetado antes do source (ex: "#define HAS_SKINNING\n")

    ShaderInfo(std::string p, GLenum t, std::string pre = "")
        : path(std::move(p)), type(t), preamble(std::move(pre)) {}
};

class Shader {
public:
    Shader() = default;
    explicit Shader(std::vector<ShaderInfo> shaderInfos);
    Shader(Shader&& other) noexcept;
    ~Shader();

    void Bind() const { glUseProgram(programID); }

    // Setters de uniforms — copiar tal qual Shader.h do glRenderer linhas 51–160
    void SetBool(std::string uniform, bool value);
    void SetInt(std::string uniform, int value);
    void SetUInt(std::string uniform, unsigned int value);
    void SetFloat(std::string uniform, float value);
    void Set1FloatArray(std::string uniform, std::span<const float> value);
    void Set1FloatArray(std::string uniform, const float* value, GLsizei count);
    void Set2FloatArray(std::string uniform, std::span<const glm::vec2> value);
    void Set3FloatArray(std::string uniform, std::span<const glm::vec3> value);
    void Set4FloatArray(std::string uniform, std::span<const glm::vec4> value);
    void SetIntArray(std::string uniform, std::span<const int> value);
    void SetVec2(std::string uniform, const glm::vec2& value);
    void SetVec2(std::string uniform, float x, float y);
    void SetIVec2(std::string uniform, const glm::ivec2& value);
    void SetIVec2(std::string uniform, int x, int y);
    void SetVec3(std::string uniform, const glm::vec3& value);
    void SetVec3(std::string uniform, float x, float y, float z);
    void SetVec4(std::string uniform, const glm::vec4& value);
    void SetVec4(std::string uniform, float x, float y, float z, float w);
    void SetMat3(std::string uniform, const glm::mat3& mat);
    void SetMat4(std::string uniform, const glm::mat4& mat);
    void SetHandle(std::string uniform, uint64_t handle);
    void SetHandleArray(std::string uniform, std::span<const uint64_t> handles);

    // Mapa global de shaders (igual ao glRenderer)
    static inline std::unordered_map<std::string, std::optional<Shader>> shaders;

    // Configuração de path de shaders
    static inline std::string shader_dir_ = "shaders/";
    static void SetShaderDir(const std::string& dir) { shader_dir_ = dir; }

private:
    GLuint programID { 0 };
    std::unordered_map<std::string, GLint> Uniforms;

    static std::string loadFile(const std::string& path);
    static std::string resolveIncludes(const std::string& src);

    GLuint compileShader(GLenum type, const std::string& src,
                         const std::string& preamble, const std::string& path);
    void   initUniforms();
    void   checkLinkStatus(std::vector<std::string_view> files);
};

} // namespace rco::renderer
```

Gerar `shared/renderer/src/shader.cpp`:

```cpp
#include "rco/renderer/shader.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <utility>

namespace rco::renderer {

std::string Shader::loadFile(const std::string& path) {
    std::string full = shader_dir_ + path;
    std::ifstream ifs(full);
    if (!ifs) {
        std::cout << "Failed to open shader file: " << full << '\n';
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

std::string Shader::resolveIncludes(const std::string& src) {
    std::string out;
    std::istringstream ss(src);
    std::string line;
    while (std::getline(ss, line)) {
        auto pos = line.find("#include");
        if (pos != std::string::npos) {
            auto q1 = line.find('"', pos);
            auto q2 = line.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) {
                out += line + "\n";
                continue;
            }
            std::string inc = line.substr(q1 + 1, q2 - q1 - 1);
            std::string content = loadFile(inc);
            out += resolveIncludes(content); // recursivo
        } else {
            out += line + "\n";
        }
    }
    return out;
}

GLuint Shader::compileShader(GLenum type, const std::string& src,
                             const std::string& preamble,
                             const std::string& path) {
    GLuint shader = glCreateShader(type);

    // Detectar #version e injetar preamble DEPOIS dela (GLSL exige #version primeiro)
    std::string versionLine;
    std::string rest = src;
    if (auto vp = src.find("#version"); vp != std::string::npos) {
        auto nl = src.find('\n', vp);
        versionLine = src.substr(0, nl + 1);
        rest        = src.substr(nl + 1);
    }

    std::string finalSrc = versionLine + preamble + rest;
    const GLchar* str = finalSrc.c_str();
    glShaderSource(shader, 1, &str, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLchar infoLog[2048];
        glGetShaderInfoLog(shader, 2048, nullptr, infoLog);
        std::cout << "File: " << path << '\n';
        std::cout << "Error compiling shader type " << type << '\n'
                  << infoLog << '\n';
    }
    return shader;
}

Shader::Shader(std::vector<ShaderInfo> shaderInfos) {
    std::vector<GLuint> shaderIDs;
    for (auto& [path, type, preamble] : shaderInfos) {
        std::string raw    = loadFile(path);
        std::string inlined = resolveIncludes(raw);
        GLuint id = compileShader(type, inlined, preamble, path);
        shaderIDs.push_back(id);
    }

    programID = glCreateProgram();
    for (auto id : shaderIDs) glAttachShader(programID, id);
    glLinkProgram(programID);

    std::vector<std::string_view> strs;
    for (const auto& si : shaderInfos) strs.push_back(si.path);
    checkLinkStatus(strs);
    initUniforms();

    for (auto id : shaderIDs) {
        glDetachShader(programID, id);
        glDeleteShader(id);
    }
}

Shader::Shader(Shader&& other) noexcept
    : programID(std::exchange(other.programID, 0)),
      Uniforms(std::move(other.Uniforms)) {}

Shader::~Shader() {
    if (programID != 0) glDeleteProgram(programID);
}

void Shader::initUniforms() {
    GLint count = 0;
    glGetProgramiv(programID, GL_ACTIVE_UNIFORMS, &count);
    if (count == 0) return;

    GLint maxLen = 0;
    glGetProgramiv(programID, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maxLen);
    auto name = std::make_unique<char[]>(maxLen);

    for (GLint i = 0; i < count; ++i) {
        GLsizei length = 0, cnt = 0;
        GLenum  type   = GL_NONE;
        glGetActiveUniform(programID, i, maxLen, &length, &cnt, &type, name.get());
        GLint loc = glGetUniformLocation(programID, name.get());
        Uniforms.emplace(std::string(name.get(), length), loc);
    }
}

void Shader::checkLinkStatus(std::vector<std::string_view> files) {
    GLint success;
    glGetProgramiv(programID, GL_LINK_STATUS, &success);
    if (!success) {
        GLchar infoLog[2048];
        glGetProgramInfoLog(programID, 2048, nullptr, infoLog);
        std::cout << "File(s): ";
        for (auto f : files) std::cout << f << ' ';
        std::cout << "\nFailed to link:\n" << infoLog << '\n';
    }
}

// Setters — copiar de Shader.h do glRenderer (linhas 51-160), ajustando:
// - remover `inline` e mover corpo para cá
// - usar `glProgramUniform*` com `programID` e `Uniforms[uniform]`

void Shader::SetBool(std::string u, bool v)        { assert(Uniforms.count(u)); glProgramUniform1i(programID, Uniforms[u], (int)v); }
void Shader::SetInt(std::string u, int v)          { assert(Uniforms.count(u)); glProgramUniform1i(programID, Uniforms[u], v); }
void Shader::SetUInt(std::string u, unsigned v)    { assert(Uniforms.count(u)); glProgramUniform1ui(programID, Uniforms[u], v); }
void Shader::SetFloat(std::string u, float v)      { assert(Uniforms.count(u)); glProgramUniform1f(programID, Uniforms[u], v); }
void Shader::Set1FloatArray(std::string u, std::span<const float> v)   { assert(Uniforms.count(u)); glProgramUniform1fv(programID, Uniforms[u], (GLsizei)v.size(), v.data()); }
void Shader::Set1FloatArray(std::string u, const float* v, GLsizei c)  { assert(Uniforms.count(u)); glProgramUniform1fv(programID, Uniforms[u], c, v); }
void Shader::Set2FloatArray(std::string u, std::span<const glm::vec2> v){ assert(Uniforms.count(u)); glProgramUniform2fv(programID, Uniforms[u], (GLsizei)v.size(), glm::value_ptr(v.front())); }
void Shader::Set3FloatArray(std::string u, std::span<const glm::vec3> v){ assert(Uniforms.count(u)); glProgramUniform3fv(programID, Uniforms[u], (GLsizei)v.size(), glm::value_ptr(v.front())); }
void Shader::Set4FloatArray(std::string u, std::span<const glm::vec4> v){ assert(Uniforms.count(u)); glProgramUniform4fv(programID, Uniforms[u], (GLsizei)v.size(), glm::value_ptr(v.front())); }
void Shader::SetIntArray(std::string u, std::span<const int> v)        { assert(Uniforms.count(u)); glProgramUniform1iv(programID, Uniforms[u], (GLsizei)v.size(), v.data()); }
void Shader::SetVec2(std::string u, const glm::vec2& v)      { assert(Uniforms.count(u)); glProgramUniform2fv(programID, Uniforms[u], 1, glm::value_ptr(v)); }
void Shader::SetVec2(std::string u, float x, float y)        { assert(Uniforms.count(u)); glProgramUniform2f(programID, Uniforms[u], x, y); }
void Shader::SetIVec2(std::string u, const glm::ivec2& v)    { assert(Uniforms.count(u)); glProgramUniform2iv(programID, Uniforms[u], 1, glm::value_ptr(v)); }
void Shader::SetIVec2(std::string u, int x, int y)           { assert(Uniforms.count(u)); glProgramUniform2i(programID, Uniforms[u], x, y); }
void Shader::SetVec3(std::string u, const glm::vec3& v)      { assert(Uniforms.count(u)); glProgramUniform3fv(programID, Uniforms[u], 1, glm::value_ptr(v)); }
void Shader::SetVec3(std::string u, float x, float y, float z){ assert(Uniforms.count(u)); glProgramUniform3f(programID, Uniforms[u], x, y, z); }
void Shader::SetVec4(std::string u, const glm::vec4& v)      { assert(Uniforms.count(u)); glProgramUniform4fv(programID, Uniforms[u], 1, glm::value_ptr(v)); }
void Shader::SetVec4(std::string u, float x, float y, float z, float w){ assert(Uniforms.count(u)); glProgramUniform4f(programID, Uniforms[u], x, y, z, w); }
void Shader::SetMat3(std::string u, const glm::mat3& m)      { assert(Uniforms.count(u)); glProgramUniformMatrix3fv(programID, Uniforms[u], 1, GL_FALSE, glm::value_ptr(m)); }
void Shader::SetMat4(std::string u, const glm::mat4& m)      { assert(Uniforms.count(u)); glProgramUniformMatrix4fv(programID, Uniforms[u], 1, GL_FALSE, glm::value_ptr(m)); }
void Shader::SetHandle(std::string u, uint64_t h)            { assert(Uniforms.count(u)); glProgramUniformHandleui64ARB(programID, Uniforms[u], h); }
void Shader::SetHandleArray(std::string u, std::span<const uint64_t> h){ assert(Uniforms.count(u)); glProgramUniformHandleui64vARB(programID, Uniforms[u], (GLsizei)h.size(), h.data()); }

} // namespace rco::renderer
```

**Sucesso**: `shader.h`, `shader.cpp` existem. Compilam sem referência a shaderc.

### 1.5.j — `RendererHelpers.ixx` → `helpers.h/.cpp` + `compile_shaders.cpp`

Fonte: `glRenderer/RendererHelpers.ixx` (305 linhas)

**Dividir em dois arquivos**:
- `helpers.h/.cpp` — `GLerrorCB`, `drawFSTexture`, `blurTexture*`, `convolve_image`
- `compile_shaders.cpp` — função `CompileShaders()` que registra todos os shaders

Gerar `shared/renderer/include/rco/renderer/helpers.h`:

```cpp
#pragma once
#include <glad/glad.h>
#include <string>

namespace rco::renderer {

void GLAPIENTRY GLerrorCB(GLenum source, GLenum type, GLuint id,
                          GLenum severity, GLsizei length,
                          const GLchar* message, const void* userParam);

void drawFSTexture(GLuint texID);

void blurTextureRGBA32f(GLuint inOutTex, GLuint intermediateTex,
                        GLint width, GLint height, GLint passes, GLint strength);
void blurTextureRGBA16f(GLuint inOutTex, GLuint intermediateTex,
                        GLint width, GLint height, GLint passes, GLint strength);
void blurTextureRG32f  (GLuint inOutTex, GLuint intermediateTex,
                        GLint width, GLint height, GLint passes, GLint strength);
void blurTextureR32f   (GLuint inOutTex, GLuint intermediateTex,
                        GLint width, GLint height, GLint passes, GLint strength);

void convolve_image(GLuint inTex, GLuint outTex, GLint outWidth, GLint outHeight);

// Registra todos os shaders no Shader::shaders map
void CompileAllShaders();

} // namespace rco::renderer
```

Gerar `shared/renderer/src/helpers.cpp`:

Copiar corpos de `RendererHelpers.ixx` linhas 11–59 (`GLerrorCB`), 218–228 (`drawFSTexture`),
230–306 (blur helpers e `convolve_image`). Entrar em `namespace rco::renderer`.

```cpp
#include "rco/renderer/helpers.h"
#include "rco/renderer/shader.h"
#include <iostream>
#include <span>

namespace rco::renderer {

void GLAPIENTRY GLerrorCB(GLenum source, GLenum type, GLuint id,
                          GLenum severity, GLsizei, const GLchar* message, const void*) {
    if (id == 131169 || id == 131185 || id == 131218 || id == 131204) return;
    std::cout << "---------------\nDebug (" << id << "): " << message << '\n';
    // ... copiar switches de source/type/severity de RendererHelpers.ixx linhas 27-57 ...
}

void drawFSTexture(GLuint texID) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindTextureUnit(0, texID);
    auto& s = Shader::shaders["fstexture"];
    s->Bind();
    s->SetInt("u_texture", 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

namespace {
    void blurTextureBase(GLuint inOutTex, GLuint intermediateTexture,
                         GLint width, GLint height,
                         GLint passes, GLint strength,
                         std::span<std::string> shaderNames, GLenum imageFormat) {
        if (strength > (GLint)shaderNames.size() || strength <= 1) return;

        auto& shader = Shader::shaders[shaderNames[strength - 1]];
        shader->Bind();
        shader->SetIVec2("u_texSize", width, height);
        shader->SetInt("u_inTex", 0);
        shader->SetInt("u_outTex", 0);

        const int xgroups = (width  + 7) / 8;
        const int ygroups = (height + 7) / 8;

        bool horizontal = false;
        for (int i = 0; i < passes * 2; ++i) {
            glBindTextureUnit(0, inOutTex);
            glBindImageTexture(0, intermediateTexture, 0, false, 0, GL_WRITE_ONLY, imageFormat);
            shader->SetBool("u_horizontal", horizontal);
            glDispatchCompute(xgroups, ygroups, 1);
            glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            std::swap(inOutTex, intermediateTexture);
            horizontal = !horizontal;
        }
    }
}

void blurTextureRGBA32f(GLuint io, GLuint im, GLint w, GLint h, GLint p, GLint s) {
    std::string names[] = {"gaussianRGBA32f_blur1","gaussianRGBA32f_blur2","gaussianRGBA32f_blur3",
                           "gaussianRGBA32f_blur4","gaussianRGBA32f_blur5","gaussianRGBA32f_blur6"};
    blurTextureBase(io, im, w, h, p, s, names, GL_RGBA32F);
}
void blurTextureRGBA16f(GLuint io, GLuint im, GLint w, GLint h, GLint p, GLint s) {
    std::string names[] = {"gaussianRGBA16f_blur1","gaussianRGBA16f_blur2","gaussianRGBA16f_blur3",
                           "gaussianRGBA16f_blur4","gaussianRGBA16f_blur5","gaussianRGBA16f_blur6"};
    blurTextureBase(io, im, w, h, p, s, names, GL_RGBA16F);
}
void blurTextureRG32f(GLuint io, GLuint im, GLint w, GLint h, GLint p, GLint s) {
    std::string names[] = {"gaussian_blur1","gaussian_blur2","gaussian_blur3",
                           "gaussian_blur4","gaussian_blur5","gaussian_blur6"};
    blurTextureBase(io, im, w, h, p, s, names, GL_RG32F);
}
void blurTextureR32f(GLuint io, GLuint im, GLint w, GLint h, GLint p, GLint s) {
    std::string names[] = {"gaussian32f_blur1","gaussian32f_blur2","gaussian32f_blur3",
                           "gaussian32f_blur4","gaussian32f_blur5","gaussian32f_blur6"};
    blurTextureBase(io, im, w, h, p, s, names, GL_R32F);
}

void convolve_image(GLuint inTex, GLuint outTex, GLint outW, GLint outH) {
    auto& s = Shader::shaders["convolve_image"];
    s->Bind();
    s->SetInt("u_environment", 0);
    s->SetInt("u_outTex", 0);
    const int xgroups = (outW + 7) / 8;
    const int ygroups = (outH + 7) / 8;
    glBindTextureUnit(0, inTex);
    glBindImageTexture(0, outTex, 0, false, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(xgroups, ygroups, 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
}

} // namespace rco::renderer
```

Gerar `shared/renderer/src/compile_shaders.cpp`:

**Tarefa**: registrar **todos** os shaders do glRenderer. Reproduzir cada `Shader::shaders["X"].emplace(...)` de `RendererHelpers.ixx:62–216`,
**convertendo o formato `replace` (vector) para `preamble` (string)**.

Mapeamento das variantes de gaussian blur:
```
gaussian_blur{1-6}       → preamble: "#define KERNEL_RADIUS {N}\n"
gaussian32f_blur{1-6}    → preamble: "#define KERNEL_RADIUS {N}\n#define FORMAT R32f\n"
gaussianRGBA32f_blur{1-6}→ preamble: "#define KERNEL_RADIUS {N}\n#define FORMAT RGBA32f\n"
gaussianRGBA16f_blur{1-6}→ preamble: "#define KERNEL_RADIUS {N}\n#define FORMAT RGBA16f\n"
```

Exemplo:
```cpp
#include "rco/renderer/shader.h"
#include "rco/renderer/helpers.h"
#include <glad/glad.h>

namespace rco::renderer {

void CompileAllShaders() {
    // Shaders sem variantes
    Shader::shaders["gBuffer"].emplace(Shader({
        {"gBuffer.vs", GL_VERTEX_SHADER},
        {"gBuffer.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["gBufferBindless"].emplace(Shader({
        {"gBufferBindless.vs", GL_VERTEX_SHADER},
        {"gBufferBindless.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["gBufferSkinned"].emplace(Shader({
        {"gBufferBindless.vs", GL_VERTEX_SHADER,   "#define HAS_SKINNING\n"},
        {"gBufferBindless.fs", GL_FRAGMENT_SHADER, ""}
    }));
    Shader::shaders["fstexture"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"texture.fs",        GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["fstexture_depth"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"texture_depth.fs",  GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["gPhongGlobal"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"gPhongGlobal.fs",   GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["gPhongManyLocal"].emplace(Shader({
        {"lightGeom.vs",       GL_VERTEX_SHADER},
        {"gPhongManyLocal.fs", GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["generate_histogram"].emplace(Shader({
        {"generate_histogram.cs", GL_COMPUTE_SHADER}
    }));
    Shader::shaders["calc_exposure"].emplace(Shader({
        {"calc_exposure.cs", GL_COMPUTE_SHADER}
    }));
    Shader::shaders["tonemap"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"tonemap.fs",        GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["fxaa"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"fxaa.fs",           GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["shadow"].emplace(Shader({
        {"shadow.vs", GL_VERTEX_SHADER}
    }));
    Shader::shaders["shadowBindless"].emplace(Shader({
        {"shadowBindless.vs", GL_VERTEX_SHADER}
    }));
    Shader::shaders["volumetric"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"volumetric.fs",     GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["vsm_copy"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"vsm_copy.fs",       GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["esm_copy"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"esm_copy.fs",       GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["msm_copy"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"msm_copy.fs",       GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["atrous"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"atrous.fs",         GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["atrous_volumetric"].emplace(Shader({
        {"fullscreen_tri.vs",   GL_VERTEX_SHADER},
        {"atrous_volumetric.fs",GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["atrous_ssao"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"atrous_ssao.fs",    GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["ssr"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"ssr.fs",            GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["ssao"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"ssao.fs",           GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["hdri_skybox"].emplace(Shader({
        {"fullscreen_tri.vs", GL_VERTEX_SHADER},
        {"hdri_skybox.fs",    GL_FRAGMENT_SHADER}
    }));
    Shader::shaders["convolve_image"].emplace(Shader({
        {"irradiance_convolve.cs", GL_COMPUTE_SHADER}
    }));

    // Variantes do gaussian blur — 24 shaders (4 formatos × 6 raios)
    for (int r = 1; r <= 6; ++r) {
        std::string rs = std::to_string(r);

        Shader::shaders["gaussian_blur" + rs].emplace(Shader({
            {"gaussian.cs", GL_COMPUTE_SHADER,
             "#define KERNEL_RADIUS " + rs + "\n"}
        }));
        Shader::shaders["gaussian32f_blur" + rs].emplace(Shader({
            {"gaussian.cs", GL_COMPUTE_SHADER,
             "#define KERNEL_RADIUS " + rs + "\n#define FORMAT R32f\n"}
        }));
        Shader::shaders["gaussianRGBA32f_blur" + rs].emplace(Shader({
            {"gaussian.cs", GL_COMPUTE_SHADER,
             "#define KERNEL_RADIUS " + rs + "\n#define FORMAT RGBA32f\n"}
        }));
        Shader::shaders["gaussianRGBA16f_blur" + rs].emplace(Shader({
            {"gaussian.cs", GL_COMPUTE_SHADER,
             "#define KERNEL_RADIUS " + rs + "\n#define FORMAT RGBA16f\n"}
        }));
    }
}

} // namespace rco::renderer
```

**Adicionar guards `#ifndef` no `gaussian.cs`**:

Abrir `dist/client/shaders/gaussian.cs` e garantir que o topo tenha:

```glsl
#version 460 core
#ifndef KERNEL_RADIUS
#define KERNEL_RADIUS 3
#endif
#ifndef FORMAT
#define FORMAT RG32f
#endif
// ... resto do arquivo ...
```

**Sucesso**: `helpers.h/.cpp`, `compile_shaders.cpp` existem.

---

## Passo 1.6 — Renderer.h/.cpp temporário + stubs

Copiar para quebrar menos:

```
glRenderer/Renderer.h   → shared/renderer/src/_orig_renderer.h
glRenderer/Renderer.cpp → shared/renderer/src/_orig_renderer.cpp
```

**Arquivo stub 1** — `shared/renderer/src/_stub_camera.h` (Renderer usa `Camera cam;`):

```cpp
#pragma once
#include <glm/glm.hpp>

namespace rco::renderer {
class Camera {
public:
    glm::mat4 GetView()      const { return glm::mat4(1); }
    glm::mat4 GetProj()      const { return glm::mat4(1); }
    glm::mat4 GetViewProj()  const { return glm::mat4(1); }
    glm::vec3 GetPos()       const { return {}; }
    void SetFoV(float)             {}
    void SetPos(const glm::vec3&)  {}
    void SetPitch(float)           {}
    void SetYaw(float)             {}
    void Update(float)             {}
};
} // namespace rco::renderer
```

**Arquivo stub 2** — `shared/renderer/src/_stub_input.h`:

```cpp
#pragma once
namespace Input {
    inline bool IsKeyDown(int)    { return false; }
    inline bool IsKeyPressed(int) { return false; }
    inline void Update()          {}
    inline void Init(void*)       {}
    inline void SetCursorVisible(bool) {}
    struct ScreenPos { float x, y; };
    inline ScreenPos GetScreenPos() { return {0, 0}; }
}
```

Editar `_orig_renderer.h` e `_orig_renderer.cpp` para incluir:

```cpp
#include "_stub_camera.h"
#include "_stub_input.h"
```

no topo, em vez de `import Camera; import Input;`.

Aplicar as regras gerais no `_orig_renderer.h/.cpp`:
- Remover `import` lines → trocar por `#include "rco/renderer/<name>.h"`
- Remover `module;`, `export module X;` (não aplica a .h/.cpp mas remover `import`)
- Substituir `_countof` → `std::size`
- Remover `#pragma warning` lines

**Sucesso**: ambos arquivos existem. Próximo passo compila.

---

## Passo 1.7 — Validação (CMakeLists + build)

Atualizar `shared/renderer/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(rco_renderer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(glad   CONFIG REQUIRED)
find_package(glm    CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(Stb    REQUIRED)

add_library(rco_renderer STATIC
    # Novos arquivos da Fase 1
    src/light.cpp
    src/buffers.cpp
    src/texture.cpp
    src/material.cpp
    src/mesh.cpp
    src/shader.cpp
    src/helpers.cpp
    src/compile_shaders.cpp

    # Antigos preservados (ainda usados pelo client)
    src/shader_old.cpp
    src/model_old.cpp

    # STB impl compartilhado
    src/stb_image_impl.cpp

    # Temporário — removido na Fase 2
    src/_orig_renderer.cpp
)

target_include_directories(rco_renderer
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${Stb_INCLUDE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(rco_renderer
    PUBLIC  glad::glad glm::glm
    PRIVATE assimp::assimp
)
```

Build:
```bash
cd D:/Github/RealmCrafterOrigins/client
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

**Critério de sucesso da Fase 1** (todos devem ser verdade):

- [ ] `cmake --build` do `rco_renderer` completa com **0 erros**
- [ ] Warnings são aceitáveis (incluindo "unused variable" em código stub)
- [ ] `rco_client.exe` ainda compila (usa `shader_old.cpp` e `model_old.cpp`)
- [ ] `rco_client.exe` ainda **roda** (usa o renderer antigo, visual não mudou)
- [ ] `dist/client/shaders/` tem 35+ arquivos GLSL novos do glRenderer
- [ ] `dist/client/assets/ibl/default.hdr` existe
- [ ] `dist/client/assets/textures/bluenoise_64.png` existe
- [ ] Arquivos criados em `shared/renderer/`:
  - `include/rco/renderer/utilities.h`
  - `include/rco/renderer/indirect.h`
  - `include/rco/renderer/light.h`
  - `include/rco/renderer/object.h`
  - `include/rco/renderer/buffers.h`
  - `include/rco/renderer/texture.h`
  - `include/rco/renderer/material.h`
  - `include/rco/renderer/mesh.h`
  - `include/rco/renderer/shader.h`
  - `include/rco/renderer/helpers.h`
  - `src/light.cpp`
  - `src/buffers.cpp`
  - `src/texture.cpp`
  - `src/material.cpp`
  - `src/mesh.cpp`
  - `src/shader.cpp`
  - `src/helpers.cpp`
  - `src/compile_shaders.cpp`
  - `src/_orig_renderer.h` (temp)
  - `src/_orig_renderer.cpp` (temp)
  - `src/_stub_camera.h` (temp)
  - `src/_stub_input.h` (temp)

Se algum critério falha, **não avançar para a Fase 2**. Corrigir primeiro.

---

## Problemas comuns e como resolver

**Erro: `glGetTextureHandleARB` não declarado**
→ Faltando extensão bindless. Garantir que `glad` foi gerado com suporte a
  `GL_ARB_bindless_texture`. Se glad atual não tem, adicionar via extension loader.

**Erro: `std::hash<glm::vec3>` não existe**
→ Adicionar `#include <glm/gtx/hash.hpp>` em `mesh.h` (antes do `std::hash` specialization).

**Erro: `stb_image.h` não encontrado**
→ Path pode ser `<stb_image.h>` direto ou `<stb/stb_image.h>`. Testar ambos;
  verificar com `find_package(Stb)` qual vcpkg usa.

**Erro: `ObjectBatched` referenciado antes de `MeshInfo`**
→ `object.h` deve incluir `mesh.h`. Ordem: 1.5.h completa `object.h` no final.

**Erro: múltiplas definições de STB_IMAGE_IMPLEMENTATION**
→ `texture.cpp` **não deve** ter `#define STB_IMAGE_IMPLEMENTATION`. Apenas
  `stb_image_impl.cpp` define. `texture.cpp` só usa `#include <stb_image.h>`.

**Erro: `_countof` não declarado**
→ Trocar por `std::size(array)` com `#include <iterator>`.
