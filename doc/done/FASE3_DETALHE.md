# Fase 3 detalhada — `CMakeLists.txt` do `rco_renderer` + verificações de runtime

Tutorial auto-contido. Execute os passos **em ordem**, na íntegra.
Esta fase é curta mas crítica: se o CMakeLists ficar mal-configurado, erros de link vão assombrar as Fases 4 e 5 com mensagens enganosas.

## Pré-requisitos

- Fases 1 e 2 completas.
- `shared/renderer/CMakeLists.txt` na forma transitória do Passo 2.6 (com `engine.cpp`, `pipeline.cpp` + `shader_old.cpp`, `model_old.cpp`).
- `rco_client.exe` ainda compila e roda usando o caminho antigo.
- vcpkg disponível em `C:/vcpkg`.

## Regras gerais

1. **Static lib, nada de exe**: `rco_renderer` é `STATIC` — é consumido via `add_subdirectory` de outros CMakeLists.
2. **`PUBLIC` vs `PRIVATE` importa**:
   - Dependência que aparece em headers públicos (em `include/`) → `PUBLIC`.
   - Dependência usada só em `.cpp` (em `src/`) → `PRIVATE`.
   - Errar isso gera o clássico "`unresolved external symbol`" quando um consumer liga.
3. **Include dirs**:
   - `include/` é `PUBLIC` (consumers precisam enxergar `rco/renderer/*.h`).
   - `src/` é `PRIVATE` (arquivos privados do build).
   - `${Stb_INCLUDE_DIR}` é `PRIVATE` — nenhum header público do `rco_renderer` expõe `stb_image.h`.
4. **`CMAKE_CXX_EXTENSIONS OFF`**: desliga extensões GNU que quebram MSVC. Obrigatório.
5. **Nenhuma dependência transitiva oculta**: se um header em `include/rco/renderer/*.h` precisa de `<glm/glm.hpp>`, o alvo `glm::glm` precisa ser `PUBLIC`. Consumers não devem ter que re-find_package.

---

## Passo 3.1 — Estado final do `shared/renderer/CMakeLists.txt`

**Depois da Fase 2.6**, o CMakeLists já está perto do final. A Fase 3 só termina de alinhar.

Escrever `shared/renderer/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(rco_renderer LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language config
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Idempotent include guard: a single consumer tree with multiple subtargets
# (rco_client + rco_gue + rco_terrain) each add_subdirectory() this folder
# from different build dirs. CMake usually dedupes by path but being explicit
# avoids duplicate-target errors in edge cases.
if(TARGET rco_renderer)
    return()
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(glad   CONFIG REQUIRED)
find_package(glm    CONFIG REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(Stb    REQUIRED)

# ---------------------------------------------------------------------------
# Library target
# ---------------------------------------------------------------------------
add_library(rco_renderer STATIC
    # Fase 1 — .ixx → .h/.cpp conversions
    src/light.cpp
    src/buffers.cpp
    src/texture.cpp
    src/material.cpp
    src/mesh.cpp
    src/shader.cpp
    src/helpers.cpp
    src/compile_shaders.cpp

    # Fase 2 — deferred PBR engine
    src/engine.cpp
    src/pipeline.cpp

    # RCO's skinned Assimp model (pre-existing, kept)
    src/model.cpp

    # STB single-translation-unit implementation
    src/stb_image_impl.cpp

    # TRANSITIONAL — preserved until Fase 6.2 deletes them.
    # Uncomment while the client is still on the old render path:
    src/shader_old.cpp
    src/model_old.cpp
)

target_include_directories(rco_renderer
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE
        "${Stb_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(rco_renderer
    PUBLIC
        glad::glad   # headers <glad/glad.h> are re-exported by engine.h / pipeline.h
        glm::glm     # same (mat4, vec3 in public API)
    PRIVATE
        assimp::assimp   # only used inside model.cpp
)

# ---------------------------------------------------------------------------
# Compile options — keep cross-compiler neutral, per-platform quirks only
# ---------------------------------------------------------------------------
if(MSVC)
    # /Zc:preprocessor — required for the shader-include inliner in shader.cpp
    # to use conforming __VA_ARGS__ expansion. Harmless on code that doesn't need it.
    target_compile_options(rco_renderer PRIVATE /Zc:preprocessor /MP /permissive-)

    # Silence "unreferenced formal parameter" warnings from Assimp integration
    target_compile_options(rco_renderer PRIVATE /wd4100)
else()
    target_compile_options(rco_renderer PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Position-independent code so the lib can be linked into DLL/SO consumers later.
set_target_properties(rco_renderer PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Consumers: typical usage
#
#   add_subdirectory("${CMAKE_SOURCE_DIR}/../shared/renderer"
#                    "${CMAKE_BINARY_DIR}/_rco_renderer")
#   target_link_libraries(my_exe PRIVATE rco_renderer)
#
# Public headers: #include "rco/renderer/engine.h", etc.
# ---------------------------------------------------------------------------
```

### Por que cada bloco

- **`if(TARGET rco_renderer) return()`** — blinda contra dupla inclusão quando um repo tem 3 tools no mesmo build tree. CMake 3.20+ normalmente deduplica, mas o guard elimina ambiguidade.
- **`PUBLIC glad::glad glm::glm`** — `engine.h` inclui `<glad/glad.h>` e `<glm/glm.hpp>`; se fossem `PRIVATE`, consumers que incluíssem `engine.h` teriam que re-find_package. Como são `PUBLIC`, o link leak é automático.
- **`PRIVATE assimp::assimp`** — só `model.cpp` toca Assimp. Nenhum header em `include/` menciona `aiScene`/`aiNode` (forward-declarados em `model.h`). Consumer não precisa de Assimp.
- **`PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src`** — permite `#include "_internal_helper.h"` entre `.cpp`s do lib sem expor.
- **`/Zc:preprocessor`** — a Fase 1.5.i implementa `resolveIncludes` com macros. Sem esse flag, MSVC usa o preprocessor não-conformante de VS2015 que trata `__VA_ARGS__` de forma diferente. Sintoma sem o flag: compilação passa mas algumas substituições de macro em arquivos GLSL ficam erradas em runtime.
- **`/MP`** — compilação paralela MSVC. Ganha ~40% em `rco_renderer` (vários `.cpp` independentes).
- **`POSITION_INDEPENDENT_CODE ON`** — se no futuro uma tool virar DLL, a lib já compila sem refactor.

**Sucesso do 3.1**:
```bash
cd client
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```
`rco_renderer.lib` compila com 0 erros. `rco_client.exe` compila e roda.

---

## Passo 3.2 — Verificação de `GL_ARB_bindless_texture` em runtime

O pipeline deferred depende de texturas bindless (`glGetTextureHandleARB`, `glMakeTextureHandleResidentARB`, etc). Se a placa/driver não suporta, os shaders compilam mas o `engine.Init` vai causar crashes enigmáticos no primeiro `glTextureHandle*`.

Melhor: **detectar no startup e falhar com mensagem clara**.

### 3.2.a — Adicionar check em `Engine::Init`

Em `shared/renderer/src/engine.cpp`, logo depois do `installDebugCallback_()` (ou no lugar dele se debug_output estiver desligado):

```cpp
void Engine::Init(const EngineConfig& cfg) {
    // ... existing config copies ...

    if (cfg.enable_debug_output) {
        installDebugCallback_();
    }

    // -------- OpenGL feature requirements --------
    auto hasExtension = [](const char* name) {
        GLint numExt = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &numExt);
        for (GLint i = 0; i < numExt; ++i) {
            const char* e = reinterpret_cast<const char*>(
                glGetStringi(GL_EXTENSIONS, i));
            if (e && std::strcmp(e, name) == 0) return true;
        }
        return false;
    };

    if (!hasExtension("GL_ARB_bindless_texture")) {
        const char* vendor   = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const char* version  = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        std::fprintf(stderr,
            "[rco_renderer] FATAL: GL_ARB_bindless_texture not supported.\n"
            "  Vendor:   %s\n"
            "  Renderer: %s\n"
            "  Version:  %s\n"
            "Upgrade your GPU driver, or run on a GPU that supports bindless\n"
            "textures (GTX 700+ on NVIDIA, GCN 1.2+ on AMD, Arc on Intel).\n",
            vendor ? vendor : "?",
            renderer ? renderer : "?",
            version ? version : "?");
        throw std::runtime_error("GL_ARB_bindless_texture unavailable");
    }

    // Similar guard for multi-draw indirect (used by the static scene pass).
    // Core in 4.6, so this is belt-and-suspenders — should always pass.
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    if (major < 4 || (major == 4 && minor < 6)) {
        std::fprintf(stderr,
            "[rco_renderer] FATAL: OpenGL 4.6 required, got %d.%d\n", major, minor);
        throw std::runtime_error("OpenGL 4.6 unavailable");
    }

    glEnable(GL_MULTISAMPLE);
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &deviceAnisotropy_);

    // ... rest of Init (bluenoise, vertex buffer, createFramebuffers_, ...) ...
}
```

Adicionar `#include <cstring>` e `#include <stdexcept>` no topo se não tiver.

### 3.2.b — Confirmar que `glad` foi gerado com suporte a bindless

O glad do vcpkg **precisa** ter sido gerado com a extensão `GL_ARB_bindless_texture`. Checar rapidamente:

```bash
grep -l "glGetTextureHandleARB" $VCPKG_ROOT/installed/x64-windows/include/glad/glad.h
```

Se o arquivo não aparece: o glad precisa ser regenerado. Opções:

1. **vcpkg port com extensões** — verificar se a versão atual do pacote `glad` no vcpkg já inclui `GL_ARB_bindless_texture` por default (a partir de 2022 geralmente sim).
2. **Gerar glad local** — usar https://glad.dav1d.de/ com OpenGL 4.6 + extensão `GL_ARB_bindless_texture` marcada. Colocar em `shared/renderer/third_party/glad/` e ajustar `target_include_directories`.
3. **vcpkg feature** — `vcpkg install glad[bindless-texture]` se o port oferecer (em 2026 alguns ports têm features; verificar `vcpkg search glad`).

Registrar a escolha em `vcpkg.json` do projeto se houver, ou documentar em `CLAUDE.md`.

**Sucesso do 3.2**:
- `rco_client.exe` rodando numa GPU moderna: nenhum log, sobe normal.
- Rodar num software renderer / Intel antigo: log de erro claro e exit rápido em vez de crash.
- Gerar o erro artificialmente: renomear `glMakeTextureHandleResidentARB` no glad.c temporariamente — o extension check reporta FATAL antes de chegar lá.

---

## Passo 3.3 — Integração dos consumers

Depois da Fase 3, três exes precisam linkar `rco_renderer`:

| Consumer | Quando foi hooked | Ação na Fase 3 |
|---|---|---|
| `rco_client` | Já tem `add_subdirectory(.../shared/renderer)` e `target_link_libraries(... rco_renderer ...)` | **confirmar**, não alterar |
| `rco_gue`    | Já tem desde antes do refactor | **confirmar** |
| `rco_terrain` | **Não linka hoje** — será hookado na Fase 5.1.a | **não mexer aqui**, só confirmar ausência |

### 3.3.a — `client/CMakeLists.txt`

Confirmar que tem:

```cmake
add_subdirectory(
    "${CMAKE_SOURCE_DIR}/../shared/renderer"
    "${CMAKE_BINARY_DIR}/_rco_renderer"
)
# ...
target_link_libraries(rco_client PRIVATE ... rco_renderer ...)
```

Se estiver, nada a fazer.

### 3.3.b — `tools/gue/CMakeLists.txt`

Mesmo ritual — deveria já ter `add_subdirectory` + `target_link_libraries` para `rco_renderer`.

### 3.3.c — `tools/terrain-editor/CMakeLists.txt`

**Ainda não linka** nesta fase. Não mexer. A Fase 5.1.a fará isso.

**Sucesso do 3.3**: nenhuma mudança real de arquivo; só check.

---

## Passo 3.4 — Build clean de todos os targets

Forçar build completo do zero para validar o CMakeLists.

```bash
# Limpar builds anteriores
rm -rf client/build tools/gue/build shared/renderer/build

# Client
cd client
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# GUE
cd ../tools/gue
cmake -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

**Critério**:
- Ambos os builds terminam com 0 erros.
- `rco_renderer.lib` fica em `client/build/_rco_renderer/Release/` e `tools/gue/build/_rco_renderer/Release/` (duas cópias por ora — OK; vcpkg + MSVC não tem problema com isso).
- `rco_client.exe` e `rco_gue.exe` rodam.
- `rco_client.exe` no startup imprime (se debug output ligado):
  - Versão do GL (`4.6`)
  - Nenhum log FATAL de extensão faltando.
- Parar `rco_client.exe` com ESC / Alt+F4 — nenhum crash no shutdown.

---

## Passo 3.5 — Sanidade do link via `nm` / `dumpbin`

Opcional mas útil. Inspeciona a lib estaticamente para confirmar que símbolos que deveriam ser públicos são exportados e que nada de Assimp vaza.

### 3.5.a — Símbolos de `rco_renderer` que DEVEM aparecer:

```bash
dumpbin /symbols client/build/_rco_renderer/Release/rco_renderer.lib | grep -E "Engine::(Init|Shutdown)|Pipeline::(Begin|End|SubmitDynamic|SubmitSkinned)"
```

Esperado: várias linhas com `?Init@Engine@renderer@rco@@QEAAXAEBUEngineConfig@...`

### 3.5.b — Símbolos que NÃO devem aparecer em headers públicos:

```bash
grep -rn "aiScene\|aiNode\|aiMesh" shared/renderer/include/
```

Esperado: zero (todos forward-declarados, nunca definidos em header público).

```bash
grep -rn "stb_image\|stbi_" shared/renderer/include/
```

Esperado: zero.

Se qualquer um desses aparecer em `include/`, é um leak de dependência privada — consumer vai precisar linkar Assimp/Stb mesmo sem querer.

---

## Critérios de sucesso da Fase 3

- [ ] `shared/renderer/CMakeLists.txt` usa `PUBLIC`/`PRIVATE` corretamente (glad/glm público, assimp privado).
- [ ] MSVC flags `/Zc:preprocessor /MP /permissive-` aplicados.
- [ ] Guard `if(TARGET rco_renderer) return()` presente.
- [ ] `Engine::Init` valida `GL_ARB_bindless_texture` e `GL 4.6`.
- [ ] Build clean de `rco_client` e `rco_gue` passa do zero.
- [ ] Nenhum header público (`include/rco/renderer/*.h`) expõe `aiScene`/`stb_image`.
- [ ] `rco_client.exe` roda e mantém visual idêntico ao fim da Fase 2.

---

## Problemas comuns e como resolver

**`error C1189: #error:  This code depends on conforming preprocessor support`** em `shader.cpp`
→ `/Zc:preprocessor` não foi aplicado. Verificar que o bloco `if(MSVC) target_compile_options(...)` tem `/Zc:preprocessor`. Reconfigurar CMake (apagar `build/`).

**`unresolved external symbol "glm::mat4"` no consumer**
→ `glm::glm` ficou `PRIVATE` em vez de `PUBLIC`. Consumers vêem `glm::mat4` via header público mas o alvo não propaga o include. Mudar para `PUBLIC`.

**Consumer precisa de `find_package(assimp)` mesmo sem usar**
→ Um header público de `rco_renderer` incluiu `<assimp/...>` em vez de forward-declarar. Abrir `model.h` e ver: os tipos `aiScene/aiNode/aiMesh` **têm que** ser forward-declarados no `.h`; o `#include <assimp/...>` fica só em `.cpp`. Fase 1 passa isso correto; um refactor pode ter quebrado.

**`LNK2005: "stbi_load" already defined`**
→ Um consumer adicionou `#define STB_IMAGE_IMPLEMENTATION` e também linkou `rco_renderer` (que já define). Só `stb_image_impl.cpp` do `rco_renderer` pode definir isso no projeto inteiro. Consumer usa `#include <stb_image.h>` sem define.

**GUE ou terrain-editor: `fatal error C1083: Cannot open include file: 'rco/renderer/engine.h'`**
→ O `add_subdirectory(... shared/renderer ...)` do consumer não foi configurado, ou `target_link_libraries(... rco_renderer)` está faltando. Se o `add_subdirectory` existe mas o include falha, `target_include_directories` em `rco_renderer` não está marcado `PUBLIC`. Voltar no passo 3.1 e verificar.

**Build em RelWithDebInfo / Debug quebra com link errors em STB**
→ vcpkg tem configurações Debug separadas. `${Stb_INCLUDE_DIR}` vem do `find_package(Stb REQUIRED)` via `FindStb.cmake` — deveria resolver para ambas configs. Se só Debug quebra, reinstalar: `vcpkg install stb:x64-windows --clean-buildtrees-after-build`.

**Engine::Init crasha com access violation ANTES de imprimir version info**
→ `glad` foi inicializado? O `Engine::Init` assume `gladLoadGL*` já rodou. Adicionar um assert no topo:
```cpp
if (!glGetString) {
    throw std::runtime_error("Engine::Init called before gladLoadGL");
}
```

**Mudança no CMakeLists não é pega ao rodar `cmake --build` direto**
→ O CMake gera os Visual Studio files uma vez no `cmake -B build`. Se você só editou o `CMakeLists.txt` depois, precisa reconfigurar: `cmake -B build -DCMAKE_TOOLCHAIN_FILE=...`. Se ainda assim não pega, deletar `build/` e recriar.

**`rco_renderer.lib` com 800+ MB no Debug**
→ Esperado. Static lib em Debug com info de debug completa fica enorme. Release fica ~30 MB. Nada a fazer.

**Warnings de Assimp poluindo o build log**
→ O `/wd4100` silencia `unreferenced formal parameter`. Outros warnings do Assimp (ex.: `C4267`) podem aparecer — adicionar ao bloco MSVC: `/wd4267 /wd4244`. Não mascarar warnings do seu código novo — só do Assimp.

**Duplicate symbol `CompileShaders`** quando linkando `rco_client` e também um teste que usa `rco_renderer`
→ `compile_shaders.cpp` registra shaders em um map global estático. Se dois binários compartilham a lib via linking, fine; se uma DLL expõe dele e a exe também, conflita. Como o projeto usa STATIC só, não deve ocorrer. Se der, revisar se alguém virou `SHARED` por engano.
