#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "camera.h"
#include "heightmap.h"
#include "terrain_renderer.h"
#include "brush.h"
#include "material.h"
#include "splatmap.h"

#include <cstdio>
#include <string>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#else
  #include <unistd.h>
  #include <limits.h>
#endif

// Anchor cwd to the exe's directory so relative paths resolve from dist/tools/
// regardless of how the exe was launched.
static void SetCwdToExeDir() {
    std::filesystem::path exe;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return;
    exe = buf;
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    exe = buf;
#endif
    std::error_code ec;
    std::filesystem::current_path(exe.parent_path(), ec);
}

// ---------------------------------------------------------------------------
// Shaders — embedded
// ---------------------------------------------------------------------------
static const char* kVert = R"glsl(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProj;

out vec3 vWorldPos;
out vec3 vGeoNormal;

void main() {
    vec4 wp    = uModel * vec4(aPos, 1.0);
    vWorldPos  = wp.xyz;
    vGeoNormal = normalize(mat3(transpose(inverse(uModel))) * aNormal);
    gl_Position = uProj * uView * wp;
}
)glsl";

static const char* kFrag = R"glsl(
#version 460 core
in  vec3 vWorldPos;
in  vec3 vGeoNormal;
out vec4 FragColor;

// Lighting
uniform vec3  uSunDir;
uniform vec3  uSunColor;
uniform vec3  uAmbient;
uniform vec3  uCamPos;

// Brush ring
uniform vec3  uBrushPos;
uniform float uBrushRadius;
uniform int   uBrushActive;

// Misc
uniform int   uWireframe;
uniform int   uMatCount;
uniform vec2  uTerrainSize;

// Material textures — slots 0-3 albedo, 4-7 normal, 8-11 roughness, 12 splatmap
uniform sampler2D uAlbedo0,    uAlbedo1,    uAlbedo2,    uAlbedo3;
uniform sampler2D uNormal0,    uNormal1,    uNormal2,    uNormal3;
uniform sampler2D uRoughness0, uRoughness1, uRoughness2, uRoughness3;
uniform sampler2D uSplatmap;
uniform float     uTile0, uTile1, uTile2, uTile3;

// ---------------------------------------------------------------------------
// Triplanar albedo sampling — no UV stretching on any slope
// ---------------------------------------------------------------------------
vec3 TriAlbedo(sampler2D tex, vec3 wp, vec3 N, float tile) {
    vec3 blend = abs(N);
    blend = blend * blend;
    blend /= dot(blend, vec3(1.0));
    return texture(tex, wp.zy * tile).rgb * blend.x
         + texture(tex, wp.xz * tile).rgb * blend.y
         + texture(tex, wp.xy * tile).rgb * blend.z;
}

// ---------------------------------------------------------------------------
// Triplanar normal map → world-space normal
// Handles correct tangent-space → world-space conversion per face
// ---------------------------------------------------------------------------
vec3 TriNormal(sampler2D tex, vec3 wp, vec3 geoN, float tile) {
    vec3 blend = abs(geoN);
    blend = blend * blend;
    blend /= dot(blend, vec3(1.0));

    // Sample and decode [-1,1]
    vec3 nX = texture(tex, wp.zy * tile).rgb * 2.0 - 1.0; // YZ plane → X face
    vec3 nY = texture(tex, wp.xz * tile).rgb * 2.0 - 1.0; // XZ plane → Y face (dominant)
    vec3 nZ = texture(tex, wp.xy * tile).rgb * 2.0 - 1.0; // XY plane → Z face

    // Reconstruct world-space normal for each face
    // X face: u=Z, v=Y, face=X  →  world(R→Z, G→Y, B→X) * sign
    vec3 wX = vec3(nX.b, nX.g, nX.r) * sign(geoN.x);
    // Y face: u=X, v=Z, face=Y  →  world(R→X, G→Z, B→Y) * sign
    vec3 wY = vec3(nY.r, nY.b, nY.g) * sign(geoN.y);
    // Z face: u=X, v=Y, face=Z  →  world(R→X, G→Y, B→Z) * sign
    vec3 wZ = vec3(nZ.r, nZ.g, nZ.b) * sign(geoN.z);

    return normalize(wX * blend.x + wY * blend.y + wZ * blend.z);
}

// ---------------------------------------------------------------------------
// Triplanar roughness (single R channel)
// ---------------------------------------------------------------------------
float TriRoughness(sampler2D tex, vec3 wp, vec3 N, float tile) {
    vec3 blend = abs(N);
    blend = blend * blend;
    blend /= dot(blend, vec3(1.0));
    // Read from G channel to support ORM packed textures
    return texture(tex, wp.zy * tile).g * blend.x
         + texture(tex, wp.xz * tile).g * blend.y
         + texture(tex, wp.xy * tile).g * blend.z;
}

// ---------------------------------------------------------------------------
// Procedural height gradient (fallback when no materials loaded)
// ---------------------------------------------------------------------------
vec3 HeightColor(float h) {
    if      (h < -2.0) return vec3(0.05, 0.15, 0.35);
    else if (h <  0.5) return mix(vec3(0.10,0.30,0.55), vec3(0.76,0.70,0.50), clamp((h+2.0)/2.5,0.0,1.0));
    else if (h <  3.0) return mix(vec3(0.76,0.70,0.50), vec3(0.28,0.52,0.20), clamp((h-0.5)/2.5,0.0,1.0));
    else if (h < 10.0) return mix(vec3(0.28,0.52,0.20), vec3(0.50,0.46,0.40), clamp((h-3.0)/7.0,0.0,1.0));
    else               return mix(vec3(0.50,0.46,0.40), vec3(0.92,0.95,0.98), clamp((h-10.0)/5.0,0.0,1.0));
}

void main() {
    if (uWireframe != 0) { FragColor = vec4(0.3, 0.8, 0.4, 1.0); return; }

    vec3 geoN = normalize(vGeoNormal);
    vec3 N    = geoN;
    vec3 base;
    float roughness = 0.7;

    if (uMatCount == 0) {
        base = HeightColor(vWorldPos.y);
    } else {
        vec2  sUV  = vWorldPos.xz / uTerrainSize;
        vec4  splt = texture(uSplatmap, sUV);

        // --- Height-based blending ---
        // Sample all albedos first; use luminance as height hint so material
        // borders snap rather than linearly crossfade.
        vec3 a0 = (uMatCount > 0) ? TriAlbedo(uAlbedo0, vWorldPos, geoN, uTile0) : vec3(0);
        vec3 a1 = (uMatCount > 1) ? TriAlbedo(uAlbedo1, vWorldPos, geoN, uTile1) : vec3(0);
        vec3 a2 = (uMatCount > 2) ? TriAlbedo(uAlbedo2, vWorldPos, geoN, uTile2) : vec3(0);
        vec3 a3 = (uMatCount > 3) ? TriAlbedo(uAlbedo3, vWorldPos, geoN, uTile3) : vec3(0);

        // UE5 LB_HeightBlend: convert weight to [-1,1], add luminance height hint,
        // clamp to [0.0001,1], renormalize. A bright albedo can win even at low
        // splatmap weight — produces sharp, geologically natural transitions.
        vec3 lum = vec3(0.299, 0.587, 0.114);
        float w0 = (uMatCount > 0) ? clamp((splt.r * 2.0 - 1.0) + dot(a0, lum), 0.0001, 1.0) : 0.0;
        float w1 = (uMatCount > 1) ? clamp((splt.g * 2.0 - 1.0) + dot(a1, lum), 0.0001, 1.0) : 0.0;
        float w2 = (uMatCount > 2) ? clamp((splt.b * 2.0 - 1.0) + dot(a2, lum), 0.0001, 1.0) : 0.0;
        float w3 = (uMatCount > 3) ? clamp((splt.a * 2.0 - 1.0) + dot(a3, lum), 0.0001, 1.0) : 0.0;
        float wSum = w0 + w1 + w2 + w3;
        if (wSum > 0.001) { w0/=wSum; w1/=wSum; w2/=wSum; w3/=wSum; }
        else { w0 = 1.0; w1 = w2 = w3 = 0.0; }

        base = a0*w0 + a1*w1 + a2*w2 + a3*w3;

        // Normal map blend
        vec3 blendedN = vec3(0.0);
        if (uMatCount > 0) blendedN += TriNormal(uNormal0, vWorldPos, geoN, uTile0) * w0;
        if (uMatCount > 1) blendedN += TriNormal(uNormal1, vWorldPos, geoN, uTile1) * w1;
        if (uMatCount > 2) blendedN += TriNormal(uNormal2, vWorldPos, geoN, uTile2) * w2;
        if (uMatCount > 3) blendedN += TriNormal(uNormal3, vWorldPos, geoN, uTile3) * w3;
        N = normalize(blendedN + geoN * 0.001); // safe normalize guard

        // Roughness blend
        roughness  = 0.0;
        if (uMatCount > 0) roughness += TriRoughness(uRoughness0, vWorldPos, geoN, uTile0) * w0;
        if (uMatCount > 1) roughness += TriRoughness(uRoughness1, vWorldPos, geoN, uTile1) * w1;
        if (uMatCount > 2) roughness += TriRoughness(uRoughness2, vWorldPos, geoN, uTile2) * w2;
        if (uMatCount > 3) roughness += TriRoughness(uRoughness3, vWorldPos, geoN, uTile3) * w3;
        roughness = clamp(roughness, 0.04, 1.0);
    }

    // Cook-Torrance PBR (matches client terrain.frag)
    const float PI = 3.14159265359;
    vec3  L     = normalize(uSunDir);
    vec3  V     = normalize(uCamPos - vWorldPos);
    vec3  H     = normalize(L + V);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.001);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    float a2 = pow(roughness * roughness, 2.0);
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * denom * denom);
    float r = roughness + 1.0; float k = (r*r)/8.0;
    float G = (NdotL/(NdotL*(1.0-k)+k)) * (NdotV/(NdotV*(1.0-k)+k));
    vec3  F0 = vec3(0.04);
    vec3  F  = F0 + (1.0-F0) * pow(clamp(1.0-VdotH,0.0,1.0), 5.0);

    vec3 specular = (D*G*F) / max(4.0*NdotL*NdotV, 0.001);
    vec3 diffuse  = (1.0-F) * base / PI;
    vec3 color    = (diffuse + specular) * uSunColor * NdotL + base * uAmbient;

    // Reinhard tone map (matches client)
    color = color / (color + vec3(1.0));

    // Brush ring
    if (uBrushActive != 0) {
        float d    = length(vec2(vWorldPos.x - uBrushPos.x, vWorldPos.z - uBrushPos.z));
        float ring = 1.0 - smoothstep(0.0, 1.4, abs(d - uBrushRadius));
        color = mix(color, vec3(1.0, 0.85, 0.1), ring * 0.85);
    }

    // Distance fog
    float fog = clamp((length(uCamPos - vWorldPos) - 300.0) / 600.0, 0.0, 1.0);
    color = mix(color, vec3(0.52, 0.68, 0.84), fog);

    FragColor = vec4(color, 1.0);
}
)glsl";

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static float  g_brushRadius = 10.f;
static bool   g_flyMode     = false;
static double g_lastMouseX  = 0.0, g_lastMouseY = 0.0;
static Camera g_cam;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static GLuint BuildShader() {
    auto compile = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[2048]; glGetShaderInfoLog(s,sizeof(log),nullptr,log); fprintf(stderr,"[shader] %s\n",log); }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    GLuint p  = glCreateProgram();
    glAttachShader(p,vs); glAttachShader(p,fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

static glm::vec3 ScreenRay(float mx, float my, int W, int H,
                             const glm::mat4& proj, const glm::mat4& view) {
    float nx = (2.f*mx/W)-1.f, ny = 1.f-(2.f*my/H);
    glm::vec4 eye = glm::inverse(proj)*glm::vec4(nx,ny,-1.f,1.f);
    eye = glm::vec4(eye.x,eye.y,-1.f,0.f);
    return glm::normalize(glm::vec3(glm::inverse(view)*eye));
}

static bool RaycastTerrain(const glm::vec3& origin, const glm::vec3& dir,
                            const Heightmap& hmap, glm::vec3& hit) {
    float step=1.5f, prevT=0.f;
    for (float t=0.f; t<4000.f; t+=step) {
        glm::vec3 p=origin+dir*t;
        if (p.x<0||p.z<0||p.x>=hmap.WorldW()||p.z>=hmap.WorldH()) {
            prevT=t; step=std::min(step*1.05f,20.f); continue;
        }
        if (p.y<=hmap.SampleWorld(p.x,p.z)) {
            float lo=prevT, hi=t;
            for (int i=0;i<20;i++) {
                float mid=(lo+hi)*.5f; glm::vec3 mp=origin+dir*mid;
                if (mp.y<=hmap.SampleWorld(mp.x,mp.z)) hi=mid; else lo=mid;
            }
            glm::vec3 hp=origin+dir*((lo+hi)*.5f);
            hit={hp.x, hmap.SampleWorld(hp.x,hp.z), hp.z};
            return true;
        }
        prevT=t; step=std::min(step*1.02f,20.f);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Bind one material's textures into slots [base, base+1, base+2]
// Falls back to defaults if the material slot doesn't exist
// ---------------------------------------------------------------------------
static void BindMaterialSlot(GLuint shader, int matSlot,
                              const std::vector<Material>& mats,
                              GLuint defNormal, GLuint defRough) {
    static const char* albedoU[]    = {"uAlbedo0",    "uAlbedo1",    "uAlbedo2",    "uAlbedo3"};
    static const char* normalU[]    = {"uNormal0",    "uNormal1",    "uNormal2",    "uNormal3"};
    static const char* roughnessU[] = {"uRoughness0", "uRoughness1", "uRoughness2", "uRoughness3"};
    static const char* tileU[]      = {"uTile0",      "uTile1",      "uTile2",      "uTile3"};

    int base = matSlot * 3;
    bool has  = matSlot < (int)mats.size();

    glActiveTexture(GL_TEXTURE0 + base);
    glBindTexture(GL_TEXTURE_2D, has ? mats[matSlot].albedo : 0);
    glUniform1i(glGetUniformLocation(shader, albedoU[matSlot]), base);

    glActiveTexture(GL_TEXTURE0 + base + 1);
    glBindTexture(GL_TEXTURE_2D, has ? mats[matSlot].normal : defNormal);
    glUniform1i(glGetUniformLocation(shader, normalU[matSlot]), base+1);

    glActiveTexture(GL_TEXTURE0 + base + 2);
    glBindTexture(GL_TEXTURE_2D, has ? mats[matSlot].roughness : defRough);
    glUniform1i(glGetUniformLocation(shader, roughnessU[matSlot]), base+2);

    glUniform1f(glGetUniformLocation(shader, tileU[matSlot]),
                has ? mats[matSlot].tiling : 4.f);
}

// ---------------------------------------------------------------------------
// Resize helper
// ---------------------------------------------------------------------------
static void ResizeTerrain(Heightmap& hmap, TerrainRenderer& tr, Splatmap& smap,
                           int w, int h, float cs=2.f) {
    tr.Destroy(); hmap.Resize(w,h,cs); tr.Init(hmap); tr.MarkDirtyAll();
    smap.Resize(w,h);
    g_cam.pos   = {hmap.WorldW()*.5f, 80.f, hmap.WorldH()*.3f};
    g_cam.pitch = 30.f; g_cam.yaw = 0.f;
}

enum class EditorMode { Sculpt, Paint };

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    SetCwdToExeDir();

    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,6);
    glfwWindowHint(GLFW_OPENGL_PROFILE,GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* win = glfwCreateWindow(1600,900,"RCO Terrain Editor",nullptr,nullptr);
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    gladLoadGLLoader((GLADloadproc)glfwGetProcAddress);

    glfwSetScrollCallback(win,[](GLFWwindow*,double,double dy){
        if (!g_flyMode) g_brushRadius=std::clamp(g_brushRadius+(float)dy*.8f,.5f,120.f);
        else            g_cam.speed  =std::clamp(g_cam.speed  +(float)dy*5.f ,2.f,500.f);
    });

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::GetIO().IniFilename = nullptr; // don't litter cwd with imgui.ini
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(win,true);
    ImGui_ImplOpenGL3_Init("#version 460");

    // Default placeholder textures
    // Flat normal:  (128,128,255) → decoded as vec3(0,0,1) = no perturbation
    // Grey rough:   (180,180,180) → ~0.7 roughness via G channel
    GLuint defNormal = MakeSolidTex(128,128,255);
    GLuint defRough  = MakeSolidTex(180,180,180);

    // Paths are resolved relative to the terrain editor's own exe dir
    // (dist/tools/). The client's runtime assets live in the sibling folder
    // dist/client/, so we reach them via "../client/data/...".
    static const std::string kMatDir  = "../client/data/terrain/materials/";
    static const std::string kDataDir = "../client/data/areas/";

    Heightmap       hmap;
    TerrainRenderer terrain;
    Splatmap        splatmap;
    ResizeTerrain(hmap, terrain, splatmap, 512, 512);

    GLuint shader = BuildShader();

    std::vector<Material> materials;
    auto ReloadMats = [&](){
        for (auto& m : materials) m.Unload();
        materials = ScanMaterials(kMatDir, defNormal, defRough);
    };
    ReloadMats();

    EditorMode  mode          = EditorMode::Sculpt;
    BrushMode   brushMode     = BrushMode::Raise;
    float       brushStrength = 1.f;
    float       flattenH      = 0.f;
    int         selectedMat   = 0;
    bool        wireframe     = false;
    bool        brushOnTerrain = false;
    glm::vec3   brushPos(0.f);
    char        areaName[128] = "default";
    char        msgBuf[64]    = "";
    float       msgTimer      = 0.f;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    double lastTime = glfwGetTime();

    while (!glfwWindowShouldClose(win)) {
        double now=glfwGetTime(); float dt=std::min((float)(now-lastTime),.05f); lastTime=now;
        glfwPollEvents();

        int fbW,fbH; glfwGetFramebufferSize(win,&fbW,&fbH);
        if (!fbW||!fbH){ glfwSwapBuffers(win); continue; }

        float     aspect = (float)fbW/fbH;
        glm::mat4 proj   = glm::perspective(glm::radians(60.f),aspect,.5f,4000.f);
        glm::mat4 view   = g_cam.View();
        ImGuiIO&  io     = ImGui::GetIO();

        // Fly mode
        bool rmbDown = glfwGetMouseButton(win,GLFW_MOUSE_BUTTON_RIGHT)==GLFW_PRESS && !io.WantCaptureMouse;
        if (rmbDown && !g_flyMode) {
            glfwSetInputMode(win,GLFW_CURSOR,GLFW_CURSOR_DISABLED);
            glfwGetCursorPos(win,&g_lastMouseX,&g_lastMouseY);
            g_flyMode=true;
        } else if (!rmbDown && g_flyMode) {
            glfwSetInputMode(win,GLFW_CURSOR,GLFW_CURSOR_NORMAL);
            g_flyMode=false;
        }
        if (g_flyMode) {
            double mx,my; glfwGetCursorPos(win,&mx,&my);
            g_cam.ApplyMouseDelta((float)(mx-g_lastMouseX),(float)(my-g_lastMouseY));
            g_lastMouseX=mx; g_lastMouseY=my;
            g_cam.Move(win,dt);
        }

        // Brush raycast & action
        brushOnTerrain=false;
        if (!io.WantCaptureMouse && !g_flyMode) {
            double mx,my; glfwGetCursorPos(win,&mx,&my);
            glm::vec3 ray=ScreenRay((float)mx,(float)my,fbW,fbH,proj,view);
            if (RaycastTerrain(g_cam.pos,ray,hmap,brushPos)) {
                brushOnTerrain=true;
                if (glfwGetMouseButton(win,GLFW_MOUSE_BUTTON_LEFT)==GLFW_PRESS) {
                    if (mode==EditorMode::Sculpt) {
                        ApplyBrush(hmap,brushPos.x,brushPos.z,g_brushRadius,brushStrength,dt,brushMode,flattenH);
                        terrain.MarkDirtyRegion(brushPos.x,brushPos.z,g_brushRadius+hmap.cell_size*2.f,hmap);
                    } else if (mode==EditorMode::Paint && !materials.empty()) {
                        PaintSplatmap(splatmap,brushPos.x,brushPos.z,g_brushRadius,brushStrength,dt,
                                      std::min(selectedMat,3),hmap.WorldW(),hmap.WorldH());
                    }
                }
            }
        }

        terrain.Update(hmap);
        splatmap.Upload();

        // 3D render
        glViewport(0,0,fbW,fbH);
        glClearColor(.52f,.68f,.84f,1.f);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glPolygonMode(GL_FRONT_AND_BACK, wireframe?GL_LINE:GL_FILL);

        glm::mat4 model(1.f);
        glm::vec3 sunDir=glm::normalize(glm::vec3(.6f,1.f,.4f));

        glUseProgram(shader);
        glUniformMatrix4fv(glGetUniformLocation(shader,"uModel"),1,GL_FALSE,glm::value_ptr(model));
        glUniformMatrix4fv(glGetUniformLocation(shader,"uView"), 1,GL_FALSE,glm::value_ptr(view));
        glUniformMatrix4fv(glGetUniformLocation(shader,"uProj"), 1,GL_FALSE,glm::value_ptr(proj));
        glUniform3fv(glGetUniformLocation(shader,"uSunDir"),   1,glm::value_ptr(sunDir));
        glUniform3f (glGetUniformLocation(shader,"uSunColor"), 1.f,.95f,.80f);
        glUniform3f (glGetUniformLocation(shader,"uAmbient"),  .18f,.20f,.26f);
        glUniform3fv(glGetUniformLocation(shader,"uCamPos"),   1,glm::value_ptr(g_cam.pos));
        glUniform3fv(glGetUniformLocation(shader,"uBrushPos"), 1,glm::value_ptr(brushPos));
        glUniform1f (glGetUniformLocation(shader,"uBrushRadius"),g_brushRadius);
        glUniform1i (glGetUniformLocation(shader,"uBrushActive"),brushOnTerrain?1:0);
        glUniform1i (glGetUniformLocation(shader,"uWireframe"),  wireframe?1:0);

        int matCount=std::min((int)materials.size(),4);
        glUniform1i(glGetUniformLocation(shader,"uMatCount"),matCount);
        glUniform2f(glGetUniformLocation(shader,"uTerrainSize"),hmap.WorldW(),hmap.WorldH());

        // Bind 4 material slots (albedo=slot*3, normal=slot*3+1, rough=slot*3+2)
        for (int i=0;i<4;i++) BindMaterialSlot(shader,i,materials,defNormal,defRough);

        // Splatmap at slot 12
        glActiveTexture(GL_TEXTURE12);
        glBindTexture(GL_TEXTURE_2D, splatmap.tex);
        glUniform1i(glGetUniformLocation(shader,"uSplatmap"),12);

        terrain.Render();
        glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);

        // ImGui
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();

        const float kPW=280.f;
        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize({kPW,(float)fbH});
        ImGui::Begin("##panel",nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoBringToFrontOnFocus|
            ImGuiWindowFlags_NoScrollbar);

        ImGui::TextColored({.4f,.9f,.5f,1.f},"RCO Terrain Editor");
        ImGui::Separator();

        // Area
        ImGui::Text("Area");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputText("##area",areaName,sizeof(areaName));
        if (ImGui::Button("512"))  ResizeTerrain(hmap,terrain,splatmap, 512, 512);
        ImGui::SameLine();
        if (ImGui::Button("1024")) ResizeTerrain(hmap,terrain,splatmap,1024,1024);
        ImGui::SameLine();
        if (ImGui::Button("2048")) ResizeTerrain(hmap,terrain,splatmap,2048,2048);
        ImGui::SameLine();
        if (ImGui::Button("4096")) ResizeTerrain(hmap,terrain,splatmap,4096,4096);

        std::string dataDir = kDataDir + areaName + "/";
        if (ImGui::Button("Save")) {
            std::filesystem::create_directories(dataDir);
            bool ok = hmap.Save(dataDir+"heightmap.bin") && splatmap.Save(dataDir+"splatmap.bin");
            // Write materials.txt — "name tiling" per line (max 4)
            if (ok) {
                std::ofstream mf(dataDir+"materials.txt");
                int cnt = std::min((int)materials.size(), 4);
                for (int i = 0; i < cnt; i++)
                    mf << materials[i].name << " " << materials[i].tiling << "\n";
            }
            snprintf(msgBuf, sizeof(msgBuf), ok ? "Saved!" : "Save failed!");
            msgTimer = 2.f;
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            Heightmap tmp;
            if (tmp.Load(dataDir+"heightmap.bin")) {
                terrain.Destroy(); hmap=tmp; terrain.Init(hmap); terrain.MarkDirtyAll();
                splatmap.Load(dataDir+"splatmap.bin");
                snprintf(msgBuf, sizeof(msgBuf), "Loaded %dx%d", hmap.W, hmap.H);
            } else snprintf(msgBuf, sizeof(msgBuf), "Load failed!");
            msgTimer = 2.f;
        }
        if (msgTimer>0.f){ msgTimer-=dt; ImGui::TextColored({.5f,1.f,.5f,1.f},"%s",msgBuf); }

        ImGui::Spacing(); ImGui::Separator();

        // Mode tabs
        {
            bool sculpt=(mode==EditorMode::Sculpt);
            bool paint =(mode==EditorMode::Paint);
            if (sculpt) ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.2f,.5f,.2f,1.f));
            if (ImGui::Button("  Sculpt  ")) mode=EditorMode::Sculpt;
            if (sculpt) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (paint)  ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.2f,.3f,.6f,1.f));
            if (ImGui::Button("  Paint   ")) mode=EditorMode::Paint;
            if (paint)  ImGui::PopStyleColor();
        }

        ImGui::Spacing();

        // Sculpt tools
        if (mode==EditorMode::Sculpt) {
            const char* ml[]={"Raise","Lower","Smooth","Flatten"};
            for (int i=0;i<4;i++){
                bool sel=((int)brushMode==i);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.25f,.55f,.25f,1.f));
                if (ImGui::Button(ml[i])) brushMode=(BrushMode)i;
                if (sel) ImGui::PopStyleColor();
                if (i<3) ImGui::SameLine();
            }
            if (brushMode==BrushMode::Flatten){
                ImGui::SetNextItemWidth(-1.f);
                ImGui::DragFloat("##fh",&flattenH,.2f,-100.f,500.f,"Target H: %.1f");
            }
        }

        // Paint palette
        if (mode==EditorMode::Paint) {
            if (ImGui::Button("Reload")) ReloadMats();
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", kMatDir.c_str());

            if (materials.empty()) {
                ImGui::Spacing();
                ImGui::TextWrapped("Drop subfolders into:\ndist/client/data/terrain/materials/");
                ImGui::TextWrapped("Each folder = 1 material.\nFiles: albedo.png, normal.png, roughness.png");
            } else {
                ImGui::Text("Materials  (max 4 in splatmap):");
                ImGui::Spacing();
                for (int i=0;i<(int)materials.size()&&i<4;i++){
                    bool sel=(selectedMat==i);
                    if (sel) ImGui::PushStyleColor(ImGuiCol_Header,ImVec4(.2f,.3f,.7f,1.f));

                    ImGui::Image((ImTextureID)(intptr_t)materials[i].albedo,{32.f,32.f});
                    ImGui::SameLine();

                    // Show icons for which maps are available
                    bool hasN = (materials[i].normal    != defNormal);
                    bool hasR = (materials[i].roughness != defRough);
                    std::string label = materials[i].name;
                    if (hasN) label += " [N]";
                    if (hasR) label += " [R]";

                    if (ImGui::Selectable(label.c_str(), sel, 0, ImVec2(0,32.f)))
                        selectedMat=i;
                    if (sel) ImGui::PopStyleColor();

                    // Tiling slider for selected
                    if (sel){
                        ImGui::SetNextItemWidth(-1.f);
                        ImGui::SliderFloat("##tile",&materials[i].tiling,.5f,32.f,"Tiling: %.1f");
                    }
                }
                if ((int)materials.size()>4)
                    ImGui::TextDisabled("(+%d not used — max 4 per splatmap)",(int)materials.size()-4);
            }
        }

        ImGui::Spacing(); ImGui::Separator();

        // Brush (shared)
        ImGui::Text("Brush");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::SliderFloat("##r",&g_brushRadius,.5f,120.f,"Radius: %.1f");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::SliderFloat("##s",&brushStrength,.1f,5.f,"Strength: %.2f");

        ImGui::Spacing(); ImGui::Separator();

        ImGui::Checkbox("Wireframe",&wireframe);

        ImGui::Spacing(); ImGui::Separator();

        // Camera
        ImGui::Text("Camera");
        ImGui::SetNextItemWidth(-1.f);
        ImGui::SliderFloat("##spd",&g_cam.speed,2.f,500.f,"Speed: %.0f");
        ImGui::TextDisabled("%.0f / %.0f / %.0f",g_cam.pos.x,g_cam.pos.y,g_cam.pos.z);

        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("Grid: %dx%d  %.0fx%.0f m",hmap.W,hmap.H,hmap.WorldW(),hmap.WorldH());
        if (brushOnTerrain) ImGui::TextDisabled("@ %.1f/%.1f/%.1f",brushPos.x,brushPos.y,brushPos.z);
        ImGui::Spacing();
        ImGui::TextDisabled("RMB drag = fly");
        ImGui::TextDisabled("WASD/QE  = move");
        ImGui::TextDisabled("Scroll   = brush / speed");
        ImGui::TextDisabled("LMB      = sculpt / paint");

        ImGui::End();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }

    for (auto& m : materials) m.Unload();
    splatmap.Destroy();
    terrain.Destroy();
    glDeleteTextures(1,&defNormal);
    glDeleteTextures(1,&defRough);
    glDeleteProgram(shader);

    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext();
    glfwDestroyWindow(win); glfwTerminate();
    return 0;
}
