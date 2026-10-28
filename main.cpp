// BedrockClient - overlay client for Minecraft Bedrock (Android, arm64)
// Hooks eglSwapBuffers to draw an ImGui overlay; hooks AInputQueue_getEvent for touch.
#include <android/input.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "dobby.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "BedrockClient", __VA_ARGS__)

static double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- settings
enum { W_FPS, W_CPS, W_FT, W_CLOCK, W_BAT, W_TEMP, W_RAM, W_SESS, W_RES, W_STATS, W_GRAPH, W_MARK, W_N };
struct Widget { const char* name; bool on; float x, y; };
static Widget W[W_N];
static void resetWidgets() {
    static const char* names[W_N] = {"FPS", "CPS", "Frame time", "Clock", "Battery", "Battery temp", "RAM",
                                     "Session time", "Resolution", "FPS stats (avg/min/1% low)", "FPS graph", "Watermark"};
    static const bool on[W_N] = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    for (int i = 0; i < W_N; i++) W[i] = {names[i], on[i], 12.f, 12.f + i * 52.f};
}

static bool crossOn = false, crossOutline = true, crossDot = false;
static int crossStyle = 0;
static float crossSize = 10, crossThick = 2, crossGap = 4;
static float crossCol[4] = {0.3f, 1, 0.4f, 1};

static int theme = 0;
static float accent[4] = {0.2f, 0.8f, 0.4f, 1};
static float txtCol[4] = {1, 1, 1, 1};
static float textScale = 1.0f, bgAlpha = 0.35f, rounding = 8, uiScale = 1.0f, btnAlpha = 0.8f, btnY = 0.25f;
static bool shadow = true, rainbow = false, h24 = true;
static bool menuOpen = false, styleDirty = true, forcePos = false;

static void resetSettings() {
    resetWidgets();
    crossOn = false; crossOutline = true; crossDot = false; crossStyle = 0;
    crossSize = 10; crossThick = 2; crossGap = 4;
    float c[4] = {0.3f, 1, 0.4f, 1}; memcpy(crossCol, c, sizeof c);
    theme = 0;
    float a[4] = {0.2f, 0.8f, 0.4f, 1}; memcpy(accent, a, sizeof a);
    float t[4] = {1, 1, 1, 1}; memcpy(txtCol, t, sizeof t);
    textScale = 1; bgAlpha = 0.35f; rounding = 8; uiScale = 1; btnAlpha = 0.8f; btnY = 0.25f;
    shadow = true; rainbow = false; h24 = true;
    styleDirty = true; forcePos = true;
}

// ---------------------------------------------------------------- config file
struct Ent { const char* k; char t; void* p; };
static std::vector<Ent> ents;
static std::string cfgStatus = "";
static void buildEnts() {
    if (!ents.empty()) return;
    auto B = [](const char* k, bool& v) { ents.push_back({k, 'b', &v}); };
    auto F = [](const char* k, float& v) { ents.push_back({k, 'f', &v}); };
    auto I = [](const char* k, int& v) { ents.push_back({k, 'i', &v}); };
    auto C = [](const char* k, float* v) { ents.push_back({k, 'c', v}); };
    B("crossOn", crossOn); B("crossOutline", crossOutline); B("crossDot", crossDot); I("crossStyle", crossStyle);
    F("crossSize", crossSize); F("crossThick", crossThick); F("crossGap", crossGap); C("crossCol", crossCol);
    I("theme", theme); C("accent", accent); C("txtCol", txtCol);
    F("textScale", textScale); F("bgAlpha", bgAlpha); F("rounding", rounding); F("uiScale", uiScale);
    F("btnAlpha", btnAlpha); F("btnY", btnY);
    B("shadow", shadow); B("rainbow", rainbow); B("h24", h24);
}
static std::string cfgPath() {
    static std::string p;
    if (p.empty()) {
        char b[256] = {0};
        if (FILE* f = fopen("/proc/self/cmdline", "r")) { fread(b, 1, 255, f); fclose(f); }
        p = std::string("/sdcard/Android/data/") + b + "/files/BedrockClient.cfg";
    }
    return p;
}
static void saveCfg() {
    buildEnts();
    FILE* f = fopen(cfgPath().c_str(), "w");
    if (!f) { cfgStatus = "Save failed (cannot write file)"; LOG("save failed: %s", cfgPath().c_str()); return; }
    for (auto& e : ents) {
        if (e.t == 'b') fprintf(f, "%s=%d\n", e.k, *(bool*)e.p);
        else if (e.t == 'i') fprintf(f, "%s=%d\n", e.k, *(int*)e.p);
        else if (e.t == 'f') fprintf(f, "%s=%f\n", e.k, *(float*)e.p);
        else { float* c = (float*)e.p; fprintf(f, "%s=%f %f %f %f\n", e.k, c[0], c[1], c[2], c[3]); }
    }
    for (int i = 0; i < W_N; i++) fprintf(f, "w%d=%d %f %f\n", i, W[i].on, W[i].x, W[i].y);
    fclose(f);
    cfgStatus = "Saved";
}
static void loadCfg() {
    buildEnts();
    FILE* f = fopen(cfgPath().c_str(), "r");
    if (!f) { cfgStatus = "No saved config found"; return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; const char* k = line; const char* v = eq + 1;
        int wi = -1;
        if (k[0] == 'w' && sscanf(k + 1, "%d", &wi) == 1 && wi >= 0 && wi < W_N) {
            int on; float x, y;
            if (sscanf(v, "%d %f %f", &on, &x, &y) == 3) { W[wi].on = on; W[wi].x = x; W[wi].y = y; }
            continue;
        }
        for (auto& e : ents) {
            if (strcmp(e.k, k)) continue;
            if (e.t == 'b') { int t = 0; sscanf(v, "%d", &t); *(bool*)e.p = t; }
            else if (e.t == 'i') sscanf(v, "%d", (int*)e.p);
            else if (e.t == 'f') sscanf(v, "%f", (float*)e.p);
            else { float* c = (float*)e.p; sscanf(v, "%f %f %f %f", c, c + 1, c + 2, c + 3); }
        }
    }
    fclose(f);
    styleDirty = true; forcePos = true; cfgStatus = "Loaded";
}

// ---------------------------------------------------------------- input
struct Ev { int t; float x, y; };
static std::mutex mu;
static std::vector<Ev> evq;
static std::deque<double> clicks;
static std::atomic<int> touchCount{0};

static void onInput(AInputEvent* e) {
    if (!e || AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return;
    int act = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
    std::lock_guard<std::mutex> l(mu);
    touchCount++;
    if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_POINTER_DOWN) clicks.push_back(nowSec());
    int t = act == AMOTION_EVENT_ACTION_DOWN ? 1 : (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL) ? 2 : 0;
    evq.push_back({t, AMotionEvent_getX(e, 0), AMotionEvent_getY(e, 0)});
}
static int32_t (*orig_getEvent)(AInputQueue*, AInputEvent**) = nullptr;
static int32_t hk_getEvent(AInputQueue* q, AInputEvent** out) {
    int32_t r = orig_getEvent(q, out);
    if (r >= 0 && out && *out) onInput(*out);
    return r;
}

// ---------------------------------------------------------------- system info
static int readInt(const char* path) {
    int v = -1;
    if (FILE* f = fopen(path, "r")) { if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); }
    return v;
}
static int ramMB() {
    int kb = -1;
    if (FILE* f = fopen("/proc/self/status", "r")) {
        char l[128];
        while (fgets(l, sizeof l, f)) if (!strncmp(l, "VmRSS:", 6)) { sscanf(l + 6, "%d", &kb); break; }
        fclose(f);
    }
    return kb < 0 ? -1 : kb / 1024;
}

// ---------------------------------------------------------------- stats
static const int FT_N = 120;
static float ft[FT_N]; static int ftIdx = 0, ftCount = 0;
static float fps = 0, avgFps = 0, minFps = 0, lowFps = 0, frameMs = 0;
static int fpsFrames = 0, cps = 0, batPct = -1, batTemp = -1, ram = -1;
static double lastFpsT = 0, lastStatT = 0, lastSysT = 0, startT = 0;

static void updateStats(double t, float dt) {
    ft[ftIdx] = dt * 1000.f; ftIdx = (ftIdx + 1) % FT_N; if (ftCount < FT_N) ftCount++;
    frameMs = dt * 1000.f; fpsFrames++;
    if (t - lastFpsT >= 0.5) { fps = fpsFrames / (float)(t - lastFpsT); fpsFrames = 0; lastFpsT = t; }
    if (t - lastStatT >= 0.5) {
        lastStatT = t;
        std::vector<float> v(ft, ft + ftCount);
        std::sort(v.begin(), v.end());
        double sum = 0; for (float x : v) sum += x;
        float mean = v.empty() ? 16.f : (float)(sum / v.size());
        avgFps = 1000.f / std::max(mean, 0.01f);
        minFps = v.empty() ? 0 : 1000.f / std::max(v.back(), 0.01f);
        size_t li = v.empty() ? 0 : std::min(v.size() - 1, (size_t)(v.size() * 0.99));
        lowFps = v.empty() ? 0 : 1000.f / std::max(v[li], 0.01f);
        std::lock_guard<std::mutex> l(mu);
        while (!clicks.empty() && t - clicks.front() > 1.0) clicks.pop_front();
        cps = (int)clicks.size();
    }
    if (t - lastSysT >= 3.0) {
        lastSysT = t;
        batPct = readInt("/sys/class/power_supply/battery/capacity");
        batTemp = readInt("/sys/class/power_supply/battery/temp");
        ram = ramMB();
    }
}

// ---------------------------------------------------------------- UI helpers
static ImU32 textColor() {
    if (rainbow) {
        float r, g, b; ImGui::ColorConvertHSVtoRGB(fmodf((float)nowSec() * 0.25f, 1.f), 0.8f, 1.f, r, g, b);
        return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, 1));
    }
    return ImGui::ColorConvertFloat4ToU32(ImVec4(txtCol[0], txtCol[1], txtCol[2], txtCol[3]));
}
static void T(const char* fmt, ...) {
    char b[160]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    ImVec2 p = ImGui::GetCursorScreenPos(), sz = ImGui::CalcTextSize(b);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (shadow) { float o = std::max(1.f, ImGui::GetFontSize() * 0.07f); dl->AddText(ImVec2(p.x + o, p.y + o), IM_COL32(0, 0, 0, 200), b); }
    dl->AddText(p, textColor(), b);
    ImGui::Dummy(sz);
}
static bool beginHud(int i) {
    if (!W[i].on) return false;
    char id[16]; snprintf(id, sizeof id, "##w%d", i);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!menuOpen) f |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    ImGui::SetNextWindowPos(ImVec2(W[i].x, W[i].y), (menuOpen && !forcePos) ? ImGuiCond_Appearing : ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(bgAlpha);
    ImGui::Begin(id, nullptr, f);
    ImGui::SetWindowFontScale(textScale);
    if (menuOpen) { ImVec2 p = ImGui::GetWindowPos(); W[i].x = p.x; W[i].y = p.y; }
    return true;
}
static ImVec4 mul(ImVec4 c, float k) { return ImVec4(std::min(c.x * k, 1.f), std::min(c.y * k, 1.f), std::min(c.z * k, 1.f), 1.f); }
static void applyStyle(float h) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    if (theme == 0) ImGui::StyleColorsDark(); else if (theme == 1) ImGui::StyleColorsLight(); else ImGui::StyleColorsClassic();
    s.WindowRounding = s.FrameRounding = s.GrabRounding = s.TabRounding = s.ChildRounding = rounding;
    s.ScrollbarSize = 34; s.GrabMinSize = 30;
    s.FramePadding = ImVec2(12, 10); s.ItemSpacing = ImVec2(12, 14); s.WindowPadding = ImVec2(14, 12);
    s.ScaleAllSizes(uiScale * h / 1080.f);
    ImVec4 a(accent[0], accent[1], accent[2], 1);
    ImVec4* C = s.Colors;
    C[ImGuiCol_Button] = mul(a, .7f); C[ImGuiCol_ButtonHovered] = a; C[ImGuiCol_ButtonActive] = mul(a, 1.2f);
    C[ImGuiCol_SliderGrab] = a; C[ImGuiCol_SliderGrabActive] = mul(a, 1.2f); C[ImGuiCol_CheckMark] = a;
    C[ImGuiCol_Header] = mul(a, .6f); C[ImGuiCol_HeaderHovered] = mul(a, .8f); C[ImGuiCol_HeaderActive] = a;
    C[ImGuiCol_Tab] = mul(a, .45f); C[ImGuiCol_TabHovered] = a; C[ImGuiCol_TabSelected] = mul(a, .8f);
    C[ImGuiCol_TitleBgActive] = mul(a, .5f); C[ImGuiCol_ScrollbarGrab] = mul(a, .7f);
    C[ImGuiCol_FrameBgHovered] = mul(a, .35f); C[ImGuiCol_FrameBgActive] = mul(a, .5f);
    ImGui::GetIO().FontGlobalScale = uiScale * h / 1080.f;
    styleDirty = false;
}

// ---------------------------------------------------------------- HUD
static void drawHud(int w, int h) {
    float sc = h / 1080.f;
    if (beginHud(W_FPS)) { T("FPS: %d", (int)(fps + 0.5f)); ImGui::End(); }
    if (beginHud(W_CPS)) { T("CPS: %d", cps); ImGui::End(); }
    if (beginHud(W_FT)) { T("Frame: %.1f ms", frameMs); ImGui::End(); }
    if (beginHud(W_CLOCK)) {
        time_t tt = time(nullptr); struct tm lt; localtime_r(&tt, &lt); char b[32];
        strftime(b, sizeof b, h24 ? "%H:%M" : "%I:%M %p", &lt); T("%s", b); ImGui::End();
    }
    if (beginHud(W_BAT)) { if (batPct >= 0) T("Battery: %d%%", batPct); else T("Battery: N/A"); ImGui::End(); }
    if (beginHud(W_TEMP)) { if (batTemp >= 0) T("Temp: %.1f C", batTemp / 10.f); else T("Temp: N/A"); ImGui::End(); }
    if (beginHud(W_RAM)) { if (ram >= 0) T("RAM: %d MB", ram); else T("RAM: N/A"); ImGui::End(); }
    if (beginHud(W_SESS)) {
        int s = (int)(nowSec() - startT); T("Session: %02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60); ImGui::End();
    }
    if (beginHud(W_RES)) { T("Res: %dx%d", w, h); ImGui::End(); }
    if (beginHud(W_STATS)) { T("Avg: %d  Min: %d", (int)avgFps, (int)minFps); T("1%% low: %d", (int)lowFps); ImGui::End(); }
    if (beginHud(W_GRAPH)) {
        ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(accent[0], accent[1], accent[2], 1));
        ImGui::PlotLines("##g", ft, FT_N, ftIdx, nullptr, 0.f, 50.f, ImVec2(300 * sc * textScale, 80 * sc * textScale));
        ImGui::PopStyleColor(); ImGui::End();
    }
    if (beginHud(W_MARK)) { T("BedrockClient"); ImGui::End(); }

    if (crossOn) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        float cx = w * 0.5f, cy = h * 0.5f, th = crossThick * sc, sz = crossSize * sc, gp = crossGap * sc;
        ImU32 col = ImGui::ColorConvertFloat4ToU32(ImVec4(crossCol[0], crossCol[1], crossCol[2], crossCol[3]));
        ImU32 blk = IM_COL32(0, 0, 0, 255);
        for (int pass = (crossOutline ? 0 : 1); pass < 2; pass++) {
            ImU32 c = pass == 0 ? blk : col; float t = pass == 0 ? th + 2 * sc : th, e = pass == 0 ? 1 * sc : 0;
            if (crossStyle == 0) {
                dl->AddLine(ImVec2(cx - gp - sz - e, cy), ImVec2(cx - gp + e, cy), c, t);
                dl->AddLine(ImVec2(cx + gp - e, cy), ImVec2(cx + gp + sz + e, cy), c, t);
                dl->AddLine(ImVec2(cx, cy - gp - sz - e), ImVec2(cx, cy - gp + e), c, t);
                dl->AddLine(ImVec2(cx, cy + gp - e), ImVec2(cx, cy + gp + sz + e), c, t);
            } else if (crossStyle == 1) {
                dl->AddCircleFilled(ImVec2(cx, cy), th * 1.5f + (pass == 0 ? 1.5f * sc : 0), c);
            } else if (crossStyle == 2) {
                dl->AddCircle(ImVec2(cx, cy), sz + gp, c, 32, t);
            } else {
                float r = sz + gp; dl->AddRect(ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r), c, 0, 0, t);
            }
        }
        if (crossDot && crossStyle != 1) dl->AddCircleFilled(ImVec2(cx, cy), th, col);
    }
}

// ---------------------------------------------------------------- menu
static void drawMenu(int w, int h) {
    // floating open/close button
    ImGui::SetNextWindowPos(ImVec2((float)w - 12, h * btnY), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0);
    ImGui::Begin("##btn", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, btnAlpha);
    if (ImGui::Button(menuOpen ? " X " : " = ", ImVec2(72 * h / 1080.f, 72 * h / 1080.f))) { menuOpen = !menuOpen; }
    ImGui::PopStyleVar();
    ImGui::End();
    if (!menuOpen) return;

    ImGui::SetNextWindowSize(ImVec2(w * 0.72f, h * 0.82f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(w * 0.5f, h * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::Begin("BedrockClient", &menuOpen, ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("HUD")) {
                for (int i = 0; i < W_N; i++) ImGui::Checkbox(W[i].name, &W[i].on);
                ImGui::Checkbox("24-hour clock", &h24);
                ImGui::SliderFloat("HUD text scale", &textScale, 0.5f, 2.5f);
                ImGui::SliderFloat("HUD background", &bgAlpha, 0.f, 1.f);
                if (ImGui::Button("Reset HUD positions")) { float s = h / 1080.f; for (int i = 0; i < W_N; i++) { W[i].x = 12.f; W[i].y = (12.f + i * 52.f) * s; } forcePos = true; }
                ImGui::TextWrapped("Tip: with this menu open, drag any HUD widget to move it.");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Crosshair")) {
                ImGui::Checkbox("Custom crosshair", &crossOn);
                ImGui::Combo("Style", &crossStyle, "Cross\0Dot\0Circle\0Square\0");
                ImGui::SliderFloat("Size", &crossSize, 2, 40);
                ImGui::SliderFloat("Thickness", &crossThick, 1, 10);
                ImGui::SliderFloat("Gap", &crossGap, 0, 20);
                ImGui::Checkbox("Outline", &crossOutline);
                ImGui::Checkbox("Center dot", &crossDot);
                ImGui::ColorEdit4("Color", crossCol, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_PickerHueWheel);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Style")) {
                if (ImGui::Combo("Menu theme", &theme, "Dark\0Light\0Classic\0")) styleDirty = true;
                static const struct { const char* n; float c[3]; } P[] = {{"Green", {.2f, .8f, .4f}}, {"Blue", {.25f, .55f, 1}}, {"Red", {.95f, .3f, .3f}}, {"Purple", {.65f, .4f, 1}}, {"Orange", {1, .6f, .2f}}};
                for (int i = 0; i < 5; i++) { if (i) ImGui::SameLine(); if (ImGui::Button(P[i].n)) { memcpy(accent, P[i].c, sizeof(float) * 3); styleDirty = true; } }
                if (ImGui::ColorEdit3("Accent color", accent, ImGuiColorEditFlags_NoInputs)) styleDirty = true;
                ImGui::ColorEdit3("HUD text color", txtCol, ImGuiColorEditFlags_NoInputs);
                if (ImGui::SliderFloat("Corner rounding", &rounding, 0, 20)) styleDirty = true;
                if (ImGui::SliderFloat("Menu scale", &uiScale, 0.6f, 1.8f)) styleDirty = true;
                ImGui::SliderFloat("Menu button opacity", &btnAlpha, 0.15f, 1.f);
                ImGui::SliderFloat("Menu button height", &btnY, 0.f, 0.9f);
                ImGui::Checkbox("Text shadow", &shadow);
                ImGui::Checkbox("Rainbow text", &rainbow);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Config")) {
                if (ImGui::Button("Save")) saveCfg();
                ImGui::SameLine(); if (ImGui::Button("Load")) loadCfg();
                ImGui::SameLine(); if (ImGui::Button("Reset all")) resetSettings();
                ImGui::Text("%s", cfgStatus.c_str());
                ImGui::TextWrapped("File: %s", cfgPath().c_str());
                ImGui::Separator();
                ImGui::Text("Touch events seen: %d", touchCount.load());
                ImGui::Text("Screen: %dx%d   FPS: %d", w, h, (int)fps);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- render hook
static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface) = nullptr;
static bool inited = false;
static EGLContext lastCtx = EGL_NO_CONTEXT;
static double lastT = 0;
static bool mouseDown = false;

static void renderOverlay(EGLDisplay d, EGLSurface s) {
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return;
    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w); eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return;

    if (!inited) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImFontConfig fc; fc.SizePixels = 26.f;
        io.Fonts->AddFontDefault(&fc);
        resetSettings(); buildEnts(); loadCfg();
        startT = nowSec(); lastT = startT;
        ImGui_ImplOpenGL3_Init("#version 300 es");
        lastCtx = ctx; inited = true;
        LOG("overlay initialised %dx%d", w, h);
    } else if (ctx != lastCtx) {  // GL context was recreated
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplOpenGL3_Init("#version 300 es");
        lastCtx = ctx;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (styleDirty) applyStyle((float)h);
    double t = nowSec(); float dt = (float)(t - lastT); lastT = t;
    if (dt <= 0 || dt > 1) dt = 1.f / 60.f;
    updateStats(t, dt);

    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = dt;
    {
        std::lock_guard<std::mutex> l(mu);
        for (auto& e : evq) {
            io.AddMousePosEvent(e.x, e.y);
            if (e.t == 1) { io.AddMouseButtonEvent(0, true); mouseDown = true; }
            else if (e.t == 2) { io.AddMouseButtonEvent(0, false); mouseDown = false; }
        }
        evq.clear();
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    drawHud(w, h);
    drawMenu(w, h);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    forcePos = false;
}
static EGLBoolean hk_swap(EGLDisplay d, EGLSurface s) {
    renderOverlay(d, s);
    return orig_swap(d, s);
}

// ---------------------------------------------------------------- entry
static void install() {
    void* sw = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    if (sw) { DobbyHook(sw, (dobby_dummy_func_t)hk_swap, (dobby_dummy_func_t*)&orig_swap); LOG("hooked eglSwapBuffers"); }
    else LOG("eglSwapBuffers NOT FOUND");
    void* ge = dlsym(RTLD_DEFAULT, "AInputQueue_getEvent");
    if (ge) { DobbyHook(ge, (dobby_dummy_func_t)hk_getEvent, (dobby_dummy_func_t*)&orig_getEvent); LOG("hooked AInputQueue_getEvent"); }
    else LOG("AInputQueue_getEvent NOT FOUND");
}
__attribute__((constructor)) static void entry() {
    LOG("BedrockClient loaded");
    std::thread([] { sleep(3); install(); }).detach();
}
