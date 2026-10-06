// Viewmodel overlay: hides the in-game viewmodel and draws ours on top.
// Never touches game memory: DXGI desktop duplication for pixels, passive Raw Input for keys/buttons.
#include "capture.h"
#include "mask_ai.h"
#include <dcomp.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

ID3D11Device* gDev;
ID3D11DeviceContext* gCtx;
std::string gRoot;
static FILE* gLogFile;
static bool gSmoke, gTestOverlay;  // --test-overlay: menu open, visible to screenshots, quits after 4s

void Log(const char* fmt, ...) {
  char b[2048];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  OutputDebugStringA(b);
  OutputDebugStringA("\n");
  if (gLogFile) { fprintf(gLogFile, "%s\n", b); fflush(gLogFile); }
}
void Fail(const char* fmt, ...) {
  char b[2048];
  va_list a;
  va_start(a, fmt);
  vsnprintf(b, sizeof b, fmt, a);
  va_end(a);
  Log("ERRO: %s", b);
  if (!gSmoke) MessageBoxA(nullptr, b, "vmoverlay", MB_ICONERROR);
  ExitProcess(1);
}
static double gQpcFreq = 1;
double QpcToSec(int64_t q) { return (double)q / gQpcFreq; }
double Now() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return QpcToSec(c.QuadPart); }
std::wstring Widen(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  return w;
}
static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static bool Has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
static bool EndsWith(const std::string& s, const char* suf) { size_t n = strlen(suf); return s.size() >= n && s.compare(s.size() - n, n, suf) == 0; }

// ================= catalog (same as the web viewer)
struct WeaponDef {
  std::string key, label, group, file, skinKey;
  bool knife = false, autoFire = false, hidden = false;
  std::string rig;  // astrastrike clip-set suffix for "golpes completos" knives
};
struct SkinDef {
  std::string id, asset, label, map, mask;
  int style = -1, ncol = 0;
  float scale = 1, rot = 0, offX = 0, offY = 0, rough = 0.3f;
  float col[4][3]{};
};
static std::vector<WeaponDef> gDefs;
static std::vector<SkinDef> gSkins;
static const char* kGloves[] = {"bloodhound-charred", "brokenfang-jade", "driver-crimson", "hydra-case-hardened",
                                "moto-spearmint", "specialist-kimono", "sport-superconductor", "wraps-slaughter"};

static std::string Title(std::string k) {
  static const std::map<std::string, std::string> names = {
      {"ak47", "AK-47"}, {"m4a4", "M4A4"}, {"m4a1s", "M4A1-S"}, {"famas", "FAMAS"}, {"galil", "Galil AR"}, {"aug", "AUG"},
      {"sg553", "SG 553"}, {"awp", "AWP"}, {"ssg08", "SSG 08"}, {"scar20", "SCAR-20"}, {"g3sg1", "G3SG1"}, {"mp9", "MP9"},
      {"mac10", "MAC-10"}, {"mp7", "MP7"}, {"mp5sd", "MP5-SD"}, {"ump45", "UMP-45"}, {"p90", "P90"}, {"bizon", "PP-Bizon"},
      {"m249", "M249"}, {"negev", "Negev"}, {"glock", "Glock-18"}, {"usp", "USP-S"}, {"hkp2000", "P2000"}, {"p250", "P250"},
      {"fiveseven", "Five-SeveN"}, {"tec9", "Tec-9"}, {"cz75a", "CZ75-Auto"}, {"deagle", "Desert Eagle"},
      {"revolver", "R8 Revolver"}, {"elite", "Dual Berettas"}, {"knife", "Faca CT"}, {"knife-default-t", "Faca T"}};
  auto it = names.find(k);
  if (it != names.end()) return it->second;
  if (k.rfind("knife-", 0) == 0) k = k.substr(6);
  std::string o;
  bool up = true;
  for (char c : k) {
    if (c == '-') { o += ' '; up = true; }
    else { o += up ? (char)toupper((unsigned char)c) : c; up = false; }
  }
  return o;
}

static void BuildCatalog() {
  auto add = [](const char* group, const char* keys, bool knife, bool autoFire) {
    std::istringstream ss(keys);
    std::string k;
    while (ss >> k) gDefs.push_back({k, Title(k), group, "models/view/view-" + k + ".glb", k, knife, autoFire});
  };
  add("Rifles", "ak47 m4a4 m4a1s famas galil aug sg553", false, true);
  add("Snipers", "awp ssg08 scar20 g3sg1", false, false);
  add("SMGs", "mp9 mac10 mp7 mp5sd ump45 p90 bizon", false, true);
  add("Pesadas", "m249 negev", false, true);
  add("Pistolas", "glock usp hkp2000 p250 fiveseven tec9 cz75a deagle revolver elite", false, false);
  add("Facas", "knife knife-default-t knife-karambit knife-butterfly knife-m9-bayonet knife-bayonet knife-flip knife-gut "
               "knife-huntsman knife-falchion knife-bowie knife-shadow-daggers knife-navaja knife-stiletto knife-talon "
               "knife-ursus knife-classic knife-paracord knife-survival knife-nomad knife-skeleton knife-kukri", true, false);
  const char* rig[][2] = {{"karambit", "knife-karambit"}, {"butterfly", "knife-butterfly"}, {"knife_ct", "knife"}, {"knife_t", "knife-default-t"}};
  for (auto& r : rig)
    gDefs.push_back({r[0], Title(r[1]) + " (golpes)", "Facas - golpes completos", std::string("models/view/view-") + r[1] + ".glb", r[1], true, false, false, r[0]});
  // legacy meshes: only loaded when a skin painted for the old UVs asks for them
  std::istringstream ss("sg553 aug mp7 mp5sd ump45 bizon m249 negev cz75a hkp2000 elite fiveseven revolver g3sg1 scar20");
  std::string k;
  while (ss >> k) {
    bool autoFire = false;
    for (auto& d : gDefs) if (d.key == k) autoFire = d.autoFire;
    gDefs.push_back({k + "-legacy", Title(k), "legacy", "models/view/view-" + k + "-legacy.glb", k, false, autoFire, true});
  }
}

static void LoadSkins() {
  std::ifstream f(Widen(gRoot + "/native/skins.tsv"));
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> c;
    std::stringstream ss(line);
    std::string cell;
    while (std::getline(ss, cell, '\t')) c.push_back(cell);
    if (c.size() < 24) { Log("skins.tsv: linha curta (%zu campos): %.60s", c.size(), line.c_str()); continue; }
    SkinDef s;
    s.id = c[0]; s.asset = c[1]; s.map = c[3]; s.mask = c[4];
    size_t bar = c[2].find(" | ");
    s.label = bar == std::string::npos ? c[2] : c[2].substr(bar + 3);
    s.style = atoi(c[5].c_str()); s.scale = (float)atof(c[6].c_str()); s.rot = (float)atof(c[7].c_str());
    s.offX = (float)atof(c[8].c_str()); s.offY = (float)atof(c[9].c_str()); s.rough = (float)atof(c[10].c_str());
    s.ncol = atoi(c[11].c_str());
    for (int i = 0; i < 12; i++) s.col[i / 3][i % 3] = (float)atof(c[12 + i].c_str());
    gSkins.push_back(std::move(s));
  }
  std::sort(gSkins.begin(), gSkins.end(), [](const SkinDef& a, const SkinDef& b) { return a.label < b.label; });
  Log("%zu skins", gSkins.size());
}

static const WeaponDef* FindDef(const std::string& k) { for (auto& d : gDefs) if (d.key == k) return &d; return nullptr; }
static const SkinDef* FindSkin(const std::string& id) { for (auto& s : gSkins) if (s.id == id) return &s; return nullptr; }
static std::string BaseAsset(const std::string& a) { return EndsWith(a, "-legacy") ? a.substr(0, a.size() - 7) : a; }

// ================= input codes + binds
enum { K_M1 = 0x1001, K_M2, K_M3, K_M4, K_M5, K_WHEEL_UP = 0x1010, K_WHEEL_DOWN };
enum Bind { B_ATTACK1, B_ATTACK2, B_INSPECT, B_RELOAD, B_SLOT1, B_SLOT2, B_SLOT3, B_SLOT4, B_SLOT5, B_LAST,
            B_FWD, B_BACK, B_LEFT, B_RIGHT, B_WALK, B_CROUCH, B_JUMP, B_HAND, B_MENU, B_LIVE, B_RECORD, B_COUNT };
static const char* kBindId[B_COUNT] = {"attack1", "attack2", "inspect", "reload", "slot1", "slot2", "slot3", "slot4", "slot5",
                                       "lastweapon", "forward", "back", "left", "right", "walk", "crouch", "jump", "hand", "menu", "live", "record"};
static const char* kBindLabel[B_COUNT] = {"Ataque 1", "Ataque 2", "Inspecionar", "Recarregar", "Arma 1 (primaria)", "Arma 2 (pistola)",
                                          "Arma 3 (faca)", "Arma 4 (granada)", "Arma 5 (C4)", "Ultima arma", "Frente", "Tras", "Esquerda",
                                          "Direita", "Andar devagar", "Agachar", "Pular", "Trocar mao (espelho)", "Menu do overlay", "Ao vivo (liga/desliga)", "Gravar (liga/desliga)"};
static const int kBindDefault[B_COUNT][2] = {{K_M1, 0}, {K_M2, 0}, {'F', 0}, {'R', 0}, {'1', 0}, {VK_INSERT, 0}, {'3', 0}, {'4', 0}, {'5', 0},
                                             {'Q', 0}, {'W', 0}, {'S', 0}, {'A', 0}, {'D', 0}, {VK_SHIFT, 0}, {VK_CONTROL, 0}, {VK_SPACE, 0}, {'H', 0},
                                             {VK_HOME, 0}, {VK_F8, 0}, {VK_F9, 0}};
static int gBind[B_COUNT][2];

static std::string KeyName(int code) {
  switch (code) {
    case 0: return "-";
    case K_M1: return "Mouse 1";
    case K_M2: return "Mouse 2";
    case K_M3: return "Mouse 3";
    case K_M4: return "Mouse 4";
    case K_M5: return "Mouse 5";
    case K_WHEEL_UP: return "Roda p/ cima";
    case K_WHEEL_DOWN: return "Roda p/ baixo";
  }
  LONG lp = (LONG)MapVirtualKeyW(code, MAPVK_VK_TO_VSC) << 16;
  if (code == VK_INSERT || code == VK_DELETE || code == VK_HOME || code == VK_END || code == VK_PRIOR || code == VK_NEXT ||
      (code >= VK_LEFT && code <= VK_DOWN) || code == VK_DIVIDE || code == VK_NUMLOCK)
    lp |= 1 << 24;  // extended keys
  wchar_t buf[64];
  if (GetKeyNameTextW(lp, buf, 64) > 0) {
    char out[128];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
    return out;
  }
  char hex[16];
  snprintf(hex, sizeof hex, "0x%02X", code);
  return hex;
}

// ================= config
struct Config {
  std::string weapon = "karambit", glove, lib = "faca";
  std::map<std::string, std::string> skin;
  float fov = 68, ox = 2.5f, oy = 0, oz = -1.5f, lightAz = 135, lightEl = 55;
  int res = 0, customW = 1280, customH = 960, customAspect = 0, dilate = 1;  // dilation in mask pixels (screen / 4)
  bool customBars = false;
  bool live = false, preview = false;
  float syncMs = 0, greenTol = 0.12f;
  bool visualSync = true;   // refine the take phase against the captured viewmodel
  bool aiMask = true;       // ONNX segmentation when a trained model is available
  float trailMs = 20;  // overlay mode: the mask also covers where the take goes this much further (fast swings)
  int moveDilate = 1;  // extra dilation while walking, when the screen gave no match
  float bob = 1, sway = 1, forwardRetreat = 0.35f, backwardRetreat = 0.35f;  // walk bob / mouse lag / directional movement (inches)
  bool mirror = false;      // left hand: the game mirrors the viewmodel (switch hands key)
  bool pano = true;         // fill with the real background remembered while the camera turned
  float panoAge = 1.5f;     // seconds that memory stays valid
  bool onlyGame = true;     // live only while the CS2 window has focus
  bool autoLight = true;    // light from above, side and strength from the game image
  float envStrength = 1.6f; // how much the game image lights / reflects on the model
  bool mousePredict = true; // background memory follows the mouse up to the instant it is shown
  FillParams fill;          // how the hidden area is rebuilt
} cfg;

struct ResPreset { const char* name; int w, h; bool bars; };
static const ResPreset kRes[] = {
    {"Nativa", 0, 0, false}, {"1280x960 4:3 esticado", 1280, 960, false}, {"1024x768 4:3 esticado", 1024, 768, false},
    {"1440x1080 4:3 esticado", 1440, 1080, false}, {"800x600 4:3 esticado", 800, 600, false},
    {"1280x1024 5:4 esticado", 1280, 1024, false}, {"1680x1050 16:10 esticado", 1680, 1050, false},
    {"1280x960 4:3 barras pretas", 1280, 960, true}, {"1920x1080 16:9", 1920, 1080, false}};
static const int kResCount = (int)(sizeof kRes / sizeof kRes[0]);

static void ApplyCustomAspect() {
  if (!cfg.customAspect) return;
  static const int ratio[][2] = {{0, 0}, {4, 3}, {16, 10}, {16, 9}};
  int x = ratio[cfg.customAspect][0], y = ratio[cfg.customAspect][1];
  cfg.customW = std::clamp(cfg.customW, std::max(320, (240 * x + y - 1) / y), std::min(3840, 2160 * x / y));
  cfg.customH = (cfg.customW * y + x / 2) / x;
}

static std::string ConfigPath() { return gRoot + "/native/config.ini"; }
static void SaveConfig() {
  std::ofstream f(Widen(ConfigPath()));
  f << "weapon=" << cfg.weapon << "\nglove=" << cfg.glove << "\nlib=" << cfg.lib << "\nfov=" << cfg.fov << "\nox=" << cfg.ox
    << "\noy=" << cfg.oy << "\noz=" << cfg.oz << "\nlightAz=" << cfg.lightAz << "\nlightEl=" << cfg.lightEl << "\nres=" << cfg.res
    << "\ncustomW=" << cfg.customW << "\ncustomH=" << cfg.customH << "\ncustomAspect=" << cfg.customAspect << "\ncustomBars=" << cfg.customBars
    << "\ndilate=" << cfg.dilate << "\nlive=" << cfg.live << "\npreview=" << cfg.preview << "\nsyncMs=" << cfg.syncMs << "\nvisualSync=" << cfg.visualSync << "\naiMask=" << cfg.aiMask
    << "\ngreenTol=" << cfg.greenTol << "\ntrailMs=" << cfg.trailMs << "\nmoveDilate=" << cfg.moveDilate << "\nbob=" << cfg.bob
    << "\nsway=" << cfg.sway << "\nforwardRetreat=" << cfg.forwardRetreat << "\nbackwardRetreat=" << cfg.backwardRetreat
    << "\nmirror=" << cfg.mirror << "\npano=" << cfg.pano << "\npanoAge=" << cfg.panoAge
    << "\nonlyGame=" << cfg.onlyGame << "\nautoLight=" << cfg.autoLight << "\nenvStrength=" << cfg.envStrength << "\nmousePredict=" << cfg.mousePredict << "\nfillTexture=" << cfg.fill.texture
    << "\nfillReach=" << cfg.fill.reach << "\nfillSharp=" << cfg.fill.sharp << "\nfillGrain=" << cfg.fill.grain
    << "\nfillFeather=" << cfg.fill.feather << "\nfillSeam=" << cfg.fill.seam << "\nfillPatch=" << cfg.fill.patch << "\nfillQuality=" << cfg.fill.quality << "\nfillColorFix=" << cfg.fill.colorFix << "\nfillOptical=" << cfg.fill.optical << "\nfillOpticalFrames=" << cfg.fill.opticalFrames << "\n";
  for (int b = 0; b < B_COUNT; b++) f << "bind." << kBindId[b] << "=" << gBind[b][0] << "," << gBind[b][1] << "\n";
  for (auto& [k, v] : cfg.skin) if (!v.empty()) f << "skin." << k << "=" << v << "\n";
}
static void LoadConfig() {
  memcpy(gBind, kBindDefault, sizeof gBind);
  std::ifstream f(Widen(ConfigPath()));
  std::string line;
  while (std::getline(f, line)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    float n = (float)atof(v.c_str());
    if (k == "weapon" && FindDef(v)) cfg.weapon = v;
    else if (k == "glove") cfg.glove = v;
    else if (k == "lib" && !v.empty()) cfg.lib = v;
    else if (k == "fov") cfg.fov = n;
    else if (k == "ox") cfg.ox = n;
    else if (k == "oy") cfg.oy = n;
    else if (k == "oz") cfg.oz = n;
    else if (k == "lightAz") cfg.lightAz = n;
    else if (k == "lightEl") cfg.lightEl = n;
    else if (k == "res") cfg.res = std::clamp((int)n, 0, kResCount);
    else if (k == "customW") cfg.customW = std::clamp((int)n, 320, 3840);
    else if (k == "customH") cfg.customH = std::clamp((int)n, 240, 2160);
    else if (k == "customAspect") cfg.customAspect = std::clamp((int)n, 0, 3);
    else if (k == "customBars") cfg.customBars = n != 0;
    else if (k == "dilate") cfg.dilate = std::clamp((int)n, 0, 8);
    else if (k == "live") cfg.live = n != 0;
    else if (k == "preview") cfg.preview = n != 0;
    else if (k == "syncMs") cfg.syncMs = n;
    else if (k == "visualSync") cfg.visualSync = n != 0;
    else if (k == "aiMask") cfg.aiMask = n != 0;
    else if (k == "greenTol") cfg.greenTol = n;
    else if (k == "trailMs") cfg.trailMs = n;
    else if (k == "moveDilate") cfg.moveDilate = std::clamp((int)n, 0, 8);
    else if (k == "mirror") cfg.mirror = n != 0;
    else if (k == "pano") cfg.pano = n != 0;
    else if (k == "panoAge") cfg.panoAge = std::clamp(n, 0.2f, 5.f);
    else if (k == "onlyGame") cfg.onlyGame = n != 0;
    else if (k == "autoLight") cfg.autoLight = n != 0;
    else if (k == "envStrength") cfg.envStrength = std::clamp(n, 0.f, 4.f);
    else if (k == "mousePredict") cfg.mousePredict = n != 0;
    else if (k == "fillTexture") cfg.fill.texture = std::clamp(n, 0.f, 2.f);
    else if (k == "fillReach") cfg.fill.reach = std::clamp(n, 20.f, 600.f);
    else if (k == "fillSharp") cfg.fill.sharp = std::clamp(n, 0.f, 3.f);
    else if (k == "fillGrain") cfg.fill.grain = std::clamp(n, 1.f, 4.f);
    else if (k == "fillFeather") cfg.fill.feather = std::clamp((int)n, 0, 6);
    else if (k == "fillSeam") cfg.fill.seam = std::clamp(n, 0.f, 0.95f);
    else if (k == "fillPatch") cfg.fill.patch = std::clamp(n, 0.f, 1.f);
    else if (k == "fillQuality") cfg.fill.quality = std::clamp((int)n, 1, 4);
    else if (k == "fillColorFix") cfg.fill.colorFix = n != 0;
    else if (k == "fillOptical") cfg.fill.optical = n != 0;
    else if (k == "fillOpticalFrames") cfg.fill.opticalFrames = std::clamp((int)n, 1, 4);
    else if (k == "bob") cfg.bob = n;
    else if (k == "sway") cfg.sway = n;
    else if (k == "forwardRetreat") cfg.forwardRetreat = std::clamp(n, -10.f, 10.f);
    else if (k == "backwardRetreat") cfg.backwardRetreat = std::clamp(n, -10.f, 10.f);
    else if (k.rfind("bind.", 0) == 0) {
      for (int b = 0; b < B_COUNT; b++) {
        if (k.substr(5) != kBindId[b]) continue;
        size_t c = v.find(',');
        gBind[b][0] = atoi(v.c_str());
        gBind[b][1] = c == std::string::npos ? 0 : atoi(v.c_str() + c + 1);
      }
    }
    else if (k.rfind("skin.", 0) == 0) cfg.skin[k.substr(5)] = v;
  }
}

// ================= weapons at runtime
struct TRS { XMFLOAT3 t; XMFLOAT4 r; XMFLOAT3 s; };
struct Anim { int cur = -1, prev = -1; float t = 0, tp = 0, fade = 1; bool loop = true, prevLoop = true; };
struct WeaponRT {
  const WeaponDef* def = nullptr;
  Model model, rig;
  bool rigged = false;
  std::vector<int> follow;  // model node -> rig node driving it
  Anim anim;
  std::string skinId = "\x01", gloveId = "\x01";
  Paint paint;
  std::unique_ptr<Model> glove;
  std::vector<int> gloveRemap;
  std::vector<TRS> tmp;
  Model& Skel() { return rigged ? rig : model; }
};
static std::map<std::string, std::unique_ptr<WeaponRT>> gWeapons;
static WeaponRT* gShown = nullptr;

static void Play(WeaponRT& w, const std::string& name) {
  Model& s = w.Skel();
  int i = s.FindClip(name);
  if (i < 0) return;
  // Alternative draws/inspects are occasional; fire-alt remains the separate secondary attack.
  if (name != "fire" && (name.size() < 4 || name.compare(name.size() - 4, 4, "-alt") != 0)) {
    int alt = s.FindClip(name + "-alt");
    if (alt >= 0) {
      static std::minstd_rand rng(GetTickCount());
      if (std::uniform_int_distribution<int>(0, 4)(rng) == 0) i = alt;
    }
  }
  Anim& a = w.anim;
  if (a.cur >= 0 && a.cur != i && name.rfind("draw", 0) != 0) { a.prev = a.cur; a.tp = a.t; a.prevLoop = a.loop; a.fade = 0; }
  else { a.prev = -1; a.fade = 1; }
  a.cur = i;
  a.t = 0;
  a.loop = name.rfind("idle", 0) == 0;
}

static void Tick(WeaponRT& w, float dt) {
  Anim& a = w.anim;
  Model& s = w.Skel();
  if (a.cur < 0) return;
  a.t += dt; a.tp += dt;
  if (a.fade < 1) a.fade = std::min(1.f, a.fade + dt / 0.1f);
  const Clip& c = s.clips[a.cur];
  if (a.loop) a.t = fmodf(a.t, c.duration);
  else if (a.t >= c.duration) { a.t = c.duration; Play(w, "idle"); }
  s.ResetPose();
  if (a.prev >= 0 && a.fade < 1) {
    const Clip& p = s.clips[a.prev];
    SampleClip(s, p, a.prevLoop ? fmodf(a.tp, p.duration) : std::min(a.tp, p.duration));
    w.tmp.resize(s.nodes.size());
    for (size_t i = 0; i < s.nodes.size(); i++) w.tmp[i] = {s.nodes[i].t, s.nodes[i].r, s.nodes[i].s};
    s.ResetPose();
    SampleClip(s, s.clips[a.cur], a.t);
    for (size_t i = 0; i < s.nodes.size(); i++) {
      Node& n = s.nodes[i];
      XMStoreFloat3(&n.t, XMVectorLerp(XMLoadFloat3(&w.tmp[i].t), XMLoadFloat3(&n.t), a.fade));
      XMStoreFloat3(&n.s, XMVectorLerp(XMLoadFloat3(&w.tmp[i].s), XMLoadFloat3(&n.s), a.fade));
      XMStoreFloat4(&n.r, XMQuaternionSlerp(XMLoadFloat4(&w.tmp[i].r), XMLoadFloat4(&n.r), a.fade));
    }
  } else {
    SampleClip(s, s.clips[a.cur], a.t);
  }
  s.UpdateWorld();
  if (w.rigged) w.model.UpdateWorld(&w.follow, &w.rig);
}

static bool Act(WeaponRT& w, Action a, int combo) {
  Model& s = w.Skel();
  auto first = [&](std::initializer_list<const char*> names) -> const char* {
    for (auto n : names) if (s.FindClip(n) >= 0) return n;
    return nullptr;
  };
  const char* n = nullptr;
  bool odd = combo % 2 == 0;  // 1st, 3rd... hit of a chain: one hand; 2nd, 4th: the other
  switch (a) {
    case A_ATTACK1: n = first({"fire", odd ? "fire-left" : "fire-right", odd ? "light_miss1" : "light_miss2"}); break;
    case A_ATTACK2: n = first({"fire-alt", "heavy_miss1", "fire-scoped", "inspect"}); break;
    case A_INSPECT: n = "inspect"; break;
    case A_DRAW: n = "draw"; break;
    case A_RELOAD: n = "reload"; break;
    default: break;
  }
  if (n) Play(w, n);
  return n != nullptr;
}

static XMFLOAT3 Srgb(const float* c) {
  auto f = [](float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); };
  return {f(c[0]), f(c[1]), f(c[2])};
}

static void ApplySkin(WeaponRT& w, const SkinDef* s) {
  Paint p{};
  if (s) {
    bool albedo = s->style == -1 || s->style == 7 || s->style == 9;
    p.scale = s->scale; p.rot = s->rot * XM_PI / 180; p.offX = s->offX; p.offY = s->offY; p.rough = s->rough;
    if (w.def->knife) {
      bool dataPattern = s->ncol && !albedo;  // pattern channels are weights, not colors
      p.mask = s->mask.empty() ? nullptr : LoadTextureFile(gRoot + s->mask, false);
      p.pattern = s->map.empty() ? nullptr : LoadTextureFile(gRoot + s->map, !dataPattern);
      p.paintMode = !p.pattern || s->style == 1 ? 0 : dataPattern ? 1 : 2;
      for (int i = 0; i < 4; i++) { XMFLOAT3 l = Srgb(s->col[i]); p.c[i][0] = l.x; p.c[i][1] = l.y; p.c[i][2] = l.z; p.c[i][3] = 1; }
      p.mode = p.mask ? 1 : 0;
    } else {
      p.pattern = s->map.empty() ? nullptr : LoadTextureFile(gRoot + s->map, true);
      for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) p.c[i][k] = s->col[i][k];
      if (albedo && p.pattern) p.mode = 2;
      else if (!p.pattern) p.mode = s->ncol ? 4 : 0;
      else { p.mode = 3; p.tintMode = s->ncol ? 0 : 1; }
    }
  }
  w.paint = p;
}

static void ApplyGlove(WeaponRT& w, const std::string& id) {
  w.glove.reset();
  w.gloveRemap.clear();
  for (auto& p : w.model.prims) {
    std::string n = Lower(p.mesh);
    if (Has(n, "firstperson") && (Has(n, "default_gloves") || Has(n, "gloves_arms"))) p.visible = id.empty();
  }
  if (id.empty()) return;
  auto g = std::make_unique<Model>();
  if (!LoadGLB(gRoot + "/models/gloves/gloves-" + id + ".glb", *g, true)) { Log("luva nao carregou: %s", id.c_str()); return; }
  g->UpdateWorld();
  w.gloveRemap.resize(g->nodes.size());
  for (size_t i = 0; i < g->nodes.size(); i++) w.gloveRemap[i] = w.model.Find(g->nodes[i].name);
  w.glove = std::move(g);
}

static WeaponRT* GetWeapon(const std::string& key) {
  auto it = gWeapons.find(key);
  if (it != gWeapons.end()) return it->second.get();
  auto& slot = gWeapons[key];  // stays null on failure so we do not retry every frame
  const WeaponDef* d = FindDef(key);
  if (!d) return nullptr;
  auto w = std::make_unique<WeaponRT>();
  w->def = d;
  if (!LoadGLB(gRoot + "/" + d->file, w->model, true)) { Log("modelo nao carregou: %s", d->file.c_str()); return nullptr; }
  for (auto& p : w->model.prims) {
    std::string n = Lower(p.mesh);
    p.paint = (Has(n, "weapon") || Has(n, "body_hd") || Has(n, "body_legacy")) && !Has(n, "firstperson") && !Has(n, "glove") && !Has(n, "sleeve");
  }
  if (!d->rig.empty()) {
    // astrastrike clip set drives a shared CS2 viewmodel rig; the view GLB copies its bones by name in world space
    static const char* clips[][2] = {{"draw", "draw"}, {"idle", "idle1"}, {"inspect", "lookat01"}, {"light_miss1", "light_miss1"},
                                     {"light_miss2", "light_miss2"}, {"light_hit1", "light_hit1"}, {"light_hit2", "light_hit2"},
                                     {"light_backstab", "light_backstab"}, {"heavy_miss1", "heavy_miss1"}, {"heavy_hit1", "heavy_hit1"},
                                     {"heavy_backstab", "heavy_backstab"}};
    for (auto& c : clips) {
      Model a;
      if (!LoadGLB(gRoot + "/models/cs2vm/" + c[1] + "_" + d->rig + ".glb", a, false) || a.clips.empty()) continue;
      if (w->rig.nodes.empty()) {
        w->rig.nodes = a.nodes;
        int weapon = w->rig.Find("weapon"), wpn = w->rig.Find("wpn");
        if (weapon >= 0 && wpn >= 0) {
          // weapon skeleton rides the vm "wpn" bone; undo its own root conversion so it lands in wpn space
          Node& root = w->rig.nodes[w->rig.nodes[weapon].parent >= 0 ? w->rig.nodes[weapon].parent : weapon];
          root.parent = wpn;
          root.r = root.r0 = {0.5f, 0.5f, 0.5f, 0.5f};
          root.t = root.t0 = {0, 0, 0};
        }
        w->rig.Reorder();
      }
      Clip clip = Rebind(a.clips[0], a, w->rig);
      clip.name = c[0];
      w->rig.clips.push_back(std::move(clip));
    }
    if (w->rig.clips.empty()) { Log("anims nao carregaram: %s", d->rig.c_str()); return nullptr; }
    w->follow.resize(w->model.nodes.size());
    for (size_t i = 0; i < w->model.nodes.size(); i++) w->follow[i] = w->rig.Find(w->model.nodes[i].name);
    w->rigged = true;
  }
  if (w->Skel().clips.empty()) { Log("sem animacoes: %s", d->file.c_str()); return nullptr; }
  Play(*w, "draw");
  slot = std::move(w);
  return slot.get();
}

// weapon + skin + gloves currently configured; a legacy skin swaps in the legacy mesh
static WeaponRT* Current() {
  auto it = cfg.skin.find(cfg.weapon);
  const SkinDef* s = it == cfg.skin.end() ? nullptr : FindSkin(it->second);
  std::string key = s && EndsWith(s->asset, "-legacy") ? s->asset : cfg.weapon;
  WeaponRT* w = GetWeapon(key);
  if (!w) return nullptr;
  std::string sid = s ? s->id : "";
  if (w->skinId != sid) { ApplySkin(*w, s); w->skinId = sid; }
  if (w->gloveId != cfg.glove) { ApplyGlove(*w, cfg.glove); w->gloveId = cfg.glove; }
  if (w != gShown) { gShown = w; Play(*w, "draw"); }
  return w;
}

// ================= viewmodel rendering
static RT gViewMS, gViewRes;
static struct { float bright = 0.3f, x = 0, y = 0.5f; } gScene;  // mean luma; center of the brightest spot (-1..1, +x right, +y up)
static ID3D11ShaderResourceView* gEnv;  // game frame as environment (auto light)  // game image: mean luma, right-vs-left balance (-1..1)
static ComPtr<ID3D11DepthStencilView> gDSV;
static ComPtr<ID3D11PixelShader> gPSView, gPSViewFlip;

static const char* kViewHLSL = R"(
Texture2D t0 : register(t0); SamplerState sLin : register(s0);
// three.js ACESFilmicToneMapping at exposure 1.08, then sRGB encode; premultiplied out
float3 ACES(float3 c) {
  c *= 1.08 / 0.6;
  c = float3(0.59719, 0.07600, 0.02840) * c.r + float3(0.35458, 0.90834, 0.13383) * c.g + float3(0.04823, 0.01566, 0.83777) * c.b;
  float3 a = c * (c + 0.0245786) - 0.000090537, b = c * (0.983729 * c + 0.4329510) + 0.238081;
  c = a / b;
  c = float3(1.60475, -0.10208, -0.00327) * c.r + float3(-0.53108, 1.10813, -0.07276) * c.g + float3(-0.07367, -0.00605, 1.07602) * c.b;
  return saturate(c);
}
float3 Encode(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055; }
float4 PSView(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float4 s = t0.Sample(sLin, uv);
  if (s.a < 0.002) discard;
  return float4(Encode(ACES(s.rgb / s.a)) * s.a, s.a);
}
float4 PSViewFlip(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return PSView(pos, float2(1 - uv.x, uv.y)); }
)";

static void EnsureViewRT(int w, int h) {
  if (gViewRes.w == w && gViewRes.h == h) return;
  gViewMS = MakeRT(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, false, 4);
  gViewRes = MakeRT(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_D32_FLOAT;
  d.SampleDesc.Count = 4; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ComPtr<ID3D11Texture2D> t;
  HR(gDev->CreateTexture2D(&d, nullptr, &t));
  gDSV.Reset();
  HR(gDev->CreateDepthStencilView(t.Get(), nullptr, &gDSV));
}

// ================= procedural motion: CS2 bobs and lags the viewmodel in code, there are no walk clips
static std::set<int> gHeld;
static bool Held(int b) { return (gBind[b][0] && gHeld.count(gBind[b][0])) || (gBind[b][1] && gHeld.count(gBind[b][1])); }
static bool Moving() { return Held(B_FWD) || Held(B_BACK) || Held(B_LEFT) || Held(B_RIGHT); }
static struct { float speed = 0, phase = 0, strafe = 0, forward = 0, backward = 0, yaw = 0, pitch = 0, crouch = 0, jumpY = 0; double jumpT0 = -10; } gMo;
static const float kAirTime = 0.75f;  // CS2 jump: ~57 units up at 800 gravity
static std::atomic<long> gMouseDX{0}, gMouseDY{0};

// raw mouse counts with their time, so the camera turn since a capture is known exactly (not guessed)
static std::mutex gMouseMu;
static std::deque<std::tuple<double, int, int>> gMouseLog;  // (t, dx, dy), last second
static void MouseSum(double t0, double t1, double& sx, double& sy) {
  sx = sy = 0;
  std::lock_guard<std::mutex> lk(gMouseMu);
  for (auto& [t, dx, dy] : gMouseLog)
    if (t > t0 && t <= t1) { sx += dx; sy += dy; }
}
// radians of camera turn per mouse count, learned by comparing counts with the turn the screen tracker measured
// (no need to type the in-game sensitivity; signs come out right by themselves)
struct Sens {
  double xx = 0, xw = 0, yy = 0, yw = 0;
  float kx = 0, ky = 0;
  bool ok = false;
  void Add(double cx, double cy, float yaw, float pitch) {
    xx *= 0.995; xw *= 0.995; yy *= 0.995; yw *= 0.995;  // slowly forget: sensitivity/zoom changes are followed
    if (fabs(cx) >= 3) { xx += cx * cx; xw += cx * yaw; }
    if (fabs(cy) >= 3) { yy += cy * cy; yw += cy * pitch; }
    if (xx > 0) kx = (float)(xw / xx);
    if (yy > 0) ky = (float)(yw / yy);
    ok = xx > 5000;
  }
};
static Sens gSens;
static const double kInputLag = 0.006;  // ponytail: game reads input ~this long before its frame is presented

static void UpdateMotion(float dt, bool menu) {
  long dx = gMouseDX.exchange(0), dy = gMouseDY.exchange(0);
  if (menu) dx = dy = 0;
  float target = Moving() ? (Held(B_WALK) || Held(B_CROUCH) ? 0.45f : 1.f) : 0.f;
  gMo.speed += (target - gMo.speed) * std::min(1.f, dt * 6);
  if (gMo.speed > 0.02f) gMo.phase = fmodf(gMo.phase + dt * XM_2PI * (0.9f + 0.9f * gMo.speed), XM_2PI * 2);
  float strafe = (Held(B_LEFT) ? 1.f : 0.f) - (Held(B_RIGHT) ? 1.f : 0.f);
  gMo.strafe += (strafe - gMo.strafe) * std::min(1.f, dt * 5);
  float forward = Held(B_FWD) && !Held(B_BACK) ? (Held(B_WALK) || Held(B_CROUCH) ? 0.45f : 1.f) : 0.f;
  gMo.forward += (forward - gMo.forward) * std::min(1.f, dt * 6);
  float backward = Held(B_BACK) && !Held(B_FWD) ? (Held(B_WALK) || Held(B_CROUCH) ? 0.45f : 1.f) : 0.f;
  gMo.backward += (backward - gMo.backward) * std::min(1.f, dt * 6);
  gMo.crouch += ((Held(B_CROUCH) ? 1.f : 0.f) - gMo.crouch) * std::min(1.f, dt * 8);
  // turning right drags the model left (+X here), looking down drags it up
  float k = 0.0004f * cfg.sway, decay = expf(-dt * 10);
  gMo.yaw = std::clamp(gMo.yaw + dx * k, -0.07f, 0.07f) * decay;
  gMo.pitch = std::clamp(gMo.pitch - dy * k, -0.05f, 0.05f) * decay;
  float t = (float)(Now() - gMo.jumpT0);
  if (t < kAirTime) gMo.jumpY = -0.010f * (1 - 2 * t / kAirTime) * std::min(1.f, t / 0.08f);  // down going up, up coming down
  else if (t < kAirTime + 0.6f) {  // landing: from the top straight into a short dip
    float u = t - kAirTime;
    gMo.jumpY = (0.010f - 0.022f * sinf(std::min(u / 0.1f, 1.f) * XM_PIDIV2)) * expf(-u * 6);
  } else gMo.jumpY = 0;
}

static XMMATRIX MotionMatrix() {
  float s = gMo.speed * cfg.bob;
  float bx = sinf(gMo.phase * 0.5f) * 0.008f * s - gMo.strafe * 0.005f * cfg.bob;  // side sway, lags behind strafes
  float by = -fabsf(sinf(gMo.phase * 0.5f)) * 0.006f * s - gMo.crouch * 0.004f + gMo.jumpY * cfg.bob;  // dip on each step
  float roll = gMo.strafe * XMConvertToRadians(2.5f) * cfg.bob;
  float retreat = gMo.forward * cfg.forwardRetreat + gMo.backward * cfg.backwardRetreat;
  return XMMatrixRotationRollPitchYaw(gMo.pitch, gMo.yaw, roll) * XMMatrixTranslation(bx, by, -retreat * 0.0254f);
}

static void RenderViewmodel(WeaponRT& w, int rw, int rh) {
  EnsureViewRT(rw, rh);
  float clear[4] = {0, 0, 0, 0};
  gCtx->ClearRenderTargetView(gViewMS.rtv.Get(), clear);
  gCtx->ClearDepthStencilView(gDSV.Get(), D3D11_CLEAR_DEPTH, 1, 0);
  gCtx->OMSetRenderTargets(1, gViewMS.rtv.GetAddressOf(), gDSV.Get());
  SetViewport(0, 0, (float)rw, (float)rh);
  float aspect = (float)rw / rh;
  // viewmodel_fov is horizontal at 4:3: vertical stays fixed when wider (hor+), horizontal when narrower
  float vfov = 2 * atanf(tanf(XMConvertToRadians(cfg.fov) / 2) / std::max(aspect, 4.f / 3));
  // viewmodel_offset in inches, Source axes x right / y forward / z up; +X is camera-left in this space
  XMMATRIX vp = XMMatrixTranslation(-cfg.ox * 0.0254f, cfg.oz * 0.0254f, cfg.oy * 0.0254f) * MotionMatrix() *
                XMMatrixLookToRH(XMVectorZero(), XMVectorSet(0, 0, 1, 0), XMVectorSet(0, 1, 0, 0)) *
                XMMatrixPerspectiveFovRH(vfov, aspect, 0.01f, 10.f);
  XMFLOAT4X4 vpf;
  XMStoreFloat4x4(&vpf, vp);
  Light L;
  float az = XMConvertToRadians(cfg.lightAz), el = XMConvertToRadians(cfg.lightEl);
  L.dir = {cosf(el) * sinf(az), sinf(el), cosf(el) * cosf(az)};
  if (cfg.autoLight) {  // the light comes from the brightest spot of the game image; dark map = darker model
    float x = cfg.mirror ? -gScene.x : gScene.x;  // the mirrored image flips sides back
    L.dir = {-x, 0.35f + 0.65f * std::max(gScene.y, -0.2f), -0.3f};  // +X is camera-left here; a bit from behind lights the front
    L.sun *= std::clamp(gScene.bright / 0.25f, 0.15f, 2.5f);
    L.map = gEnv;
    L.mapStrength = cfg.envStrength;
    L.mapFlip = cfg.mirror;
  }
  ModelPassBegin(vpf, L);
  DrawModel(w.model, w.model, nullptr, &w.paint);
  if (w.glove) DrawModel(*w.glove, w.model, &w.gloveRemap, nullptr);
  gCtx->ResolveSubresource(gViewRes.tex.Get(), 0, gViewMS.tex.Get(), 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
  ID3D11RenderTargetView* none = nullptr;
  gCtx->OMSetRenderTargets(1, &none, nullptr);
}

static void ViewRect(int sw, int sh, int* rw, int* rh, D3D11_VIEWPORT* rect) {
  bool custom = cfg.res == kResCount;
  const ResPreset& r = kRes[custom ? 0 : cfg.res];
  *rw = custom ? cfg.customW : r.w ? r.w : sw;
  *rh = custom ? cfg.customH : r.h ? r.h : sh;
  *rect = {0, 0, (float)sw, (float)sh, 0, 1};
  if (custom ? cfg.customBars : r.bars) {
    float fit = std::min((float)sw / *rw, (float)sh / *rh);
    rect->Width = *rw * fit; rect->Height = *rh * fit;
    rect->TopLeftX = (sw - rect->Width) / 2; rect->TopLeftY = (sh - rect->Height) / 2;
  }
}

// the game's own camera in screen pixels: fov 90 horizontal at 4:3, Hor+ when wider, stretched like the viewmodel
static void GameIntrinsics(int sw, int sh, float K[4]) {
  int rw, rh;
  D3D11_VIEWPORT r;
  ViewRect(sw, sh, &rw, &rh, &r);
  float ar = (float)rw / rh, tv = ar >= 4.f / 3 ? 0.75f : 1 / ar, th = tv * ar;
  K[0] = r.Width / 2 / th;
  K[1] = r.Height / 2 / tv;
  K[2] = r.TopLeftX + r.Width / 2;
  K[3] = r.TopLeftY + r.Height / 2;
}

// CS2's window by its title: no handle to the game process is ever opened
static bool GameFocused() {
  static double checked = -1;
  static bool focused;
  double t = Now();
  if (t - checked < 0.25) return focused;
  checked = t;
  wchar_t title[128] = L"";
  GetWindowTextW(GetForegroundWindow(), title, 128);
  return focused = wcsstr(title, L"Counter-Strike") != nullptr;
}

static void SleepUntil(double t) {  // sub-millisecond, unlike Sleep()
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  double d = t - Now();
  if (d <= 0) return;
  LARGE_INTEGER due;
  due.QuadPart = -(LONGLONG)(d * 1e7);
  if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 100);
  else Sleep((DWORD)(d * 1000));
}

// ================= input (own thread: accurate timestamps, never blocks or alters the game's input)
struct InputEvent { double t; int code; bool down; };
static std::mutex gInMu;
static std::vector<InputEvent> gInQ;

static LRESULT CALLBACK InputProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (m == WM_INPUT) {
    double t = Now();
    alignas(8) BYTE buf[128];
    UINT sz = sizeof buf;
    if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
      const RAWINPUT* ri = (const RAWINPUT*)buf;
      std::lock_guard<std::mutex> lk(gInMu);
      if (ri->header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD& k = ri->data.keyboard;
        if (k.VKey && k.VKey != 0xFF) gInQ.push_back({t, k.VKey, !(k.Flags & RI_KEY_BREAK)});
      } else if (ri->header.dwType == RIM_TYPEMOUSE) {
        const RAWMOUSE& mo = ri->data.mouse;
        USHORT f = mo.usButtonFlags;
        static const USHORT downs[5] = {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
        static const USHORT ups[5] = {RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP, RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP};
        for (int b = 0; b < 5; b++) {
          if (f & downs[b]) gInQ.push_back({t, K_M1 + b, true});
          if (f & ups[b]) gInQ.push_back({t, K_M1 + b, false});
        }
        if (f & RI_MOUSE_WHEEL) {  // a notch is a press + release
          int code = (SHORT)mo.usButtonData > 0 ? K_WHEEL_UP : K_WHEEL_DOWN;
          gInQ.push_back({t, code, true});
          gInQ.push_back({t, code, false});
        }
        if (!(mo.usFlags & MOUSE_MOVE_ABSOLUTE) && (mo.lLastX || mo.lLastY)) {
          gMouseDX += mo.lLastX;
          gMouseDY += mo.lLastY;
          std::lock_guard<std::mutex> ml(gMouseMu);
          gMouseLog.emplace_back(t, mo.lLastX, mo.lLastY);
          while (!gMouseLog.empty() && std::get<0>(gMouseLog.front()) < t - 1) gMouseLog.pop_front();
        }
      }
    }
  }
  return DefWindowProcW(h, m, wp, lp);
}

static void InputThread() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = InputProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"vmoverlay_input";
  RegisterClassW(&wc);
  HWND h = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  RAWINPUTDEVICE rid[2] = {{0x01, 0x06, RIDEV_INPUTSINK, h}, {0x01, 0x02, RIDEV_INPUTSINK, h}};
  if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) Log("RegisterRawInputDevices falhou %lu", GetLastError());
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
}

// ================= live / record state
static ScreenCapture gCap;
static Hider gHider;
static Library gLib;
static MaskAI gMaskAI;
static std::vector<uint8_t> gAIMaskBits;
static double gAIMaskTime = -1;
static Recorder gRec;
static bool gRecording, gMenu, gQuit, gHaveFrame;
static char gSlot = '3', gLastSlot = '1';
static struct { Action a = A_IDLE; const Take* take = nullptr; double t0 = 0; PressCtx ctx; uint8_t state = S_UNKNOWN; } gCur;
static struct { int b = -1, slot = 0; } gBinding;  // menu waiting for a key to bind
static std::vector<uint8_t> gLiveBits;
static PressCtx gPress;                              // last weapon press (context for the next one)
static std::vector<uint8_t> gLuma4, gValid;          // live frame luma at 1/4, world pixels for camera tracking
static struct { double takeStart = -1; float phase = 0; } gVisual;
static CamTracker gCam;
static bool gTracking;
static double gStart, gFrameT = -1, gLastRec = -1, gCapDt = 1 / 60.0;
static struct { double t0 = 0; int fresh = 0; float fps = 0; } gCapRate;  // capture health shown in the UI
static std::string gStatus = "Pronto.";
static bool gMaskDirty = true;  // library replaced: take pointers and the uploaded mask are stale
static std::deque<std::pair<double, uint8_t>> gStateLog;

static uint8_t StateNow() {
  return (Moving() ? S_MOVE : 0) | (Held(B_ATTACK2) ? S_RIGHT : 0) |
         (Held(B_WALK) ? S_WALK : 0) | (Held(B_CROUCH) ? S_CROUCH : 0);
}
static void RememberState(double t) {
  uint8_t s = StateNow();
  if (gStateLog.empty() || gStateLog.back().second != s) gStateLog.emplace_back(t, s);
  while (gStateLog.size() > 1 && gStateLog[1].first < t - 3) gStateLog.pop_front();
}
static uint8_t StateAtCapture(double t) {
  for (auto it = gStateLog.rbegin(); it != gStateLog.rend(); ++it) if (it->first <= t) return it->second;
  return gStateLog.empty() ? StateNow() : gStateLog.front().second;
}

static std::string LibPath(const std::string& name) { return gRoot + "/native/masks/" + name + ".vmlib"; }
static char ActiveSlot() { return gRecording ? gRec.slot : gLib.Empty() ? '3' : gLib.slot; }
static std::string TakeCount(const Library& L) {
  size_t n = 0;
  for (int a = A_ATTACK1; a < A_COUNT; a++) n += L.takes[a].size();
  return std::to_string(n) + " takes";
}

static void LoadLibrary() {
  Library L;
  if (L.Load(LibPath(cfg.lib))) {
    if (L.w != gHider.mw || L.h != gHider.mh) { gStatus = "Biblioteca gravada em outra resolucao de tela. Grave de novo."; return; }
    gLib = std::move(L);
    gAIMaskBits.clear(); gAIMaskTime = -1;
    std::string aiModel = gRoot + "/native/ai/models/" + cfg.lib;
    if (cfg.lib == "tr_knife20" || cfg.lib == "tr_knife21" || cfg.lib == "tr_knife22") {
      std::string combined = gRoot + "/native/ai/models/tr_knife20_21_22_refined";
      if (GetFileAttributesW(Widen(combined + "/model.onnx").c_str()) == INVALID_FILE_ATTRIBUTES)
        combined = gRoot + "/native/ai/models/tr_knife20_21";
      if (GetFileAttributesW(Widen(combined + "/model.onnx").c_str()) != INVALID_FILE_ATTRIBUTES) aiModel = combined;
    }
    if (!gMaskAI.Start(aiModel, gLib) && aiModel != gRoot + "/native/ai/models/" + cfg.lib)
      gMaskAI.Start(gRoot + "/native/ai/models/" + cfg.lib, gLib);
    gCur = {};
    gMaskDirty = true;
    gStatus = "Biblioteca '" + cfg.lib + "' carregada (" + TakeCount(gLib) + ").";
  } else if (GetFileAttributesW(Widen(LibPath(cfg.lib)).c_str()) != INVALID_FILE_ATTRIBUTES) {
    gStatus = "Biblioteca '" + cfg.lib + "' e do formato antigo (ou estragou): grave de novo.";
  } else {
    gStatus = "Biblioteca '" + cfg.lib + "' nao existe ainda: grave com F9.";
  }
}

static void ToggleRecord() {
  if (!gCap.ok && !gCap.Init()) { gStatus = "Captura de tela indisponivel (veja vmoverlay.log)."; return; }
  if (!gRecording) {
    gRec.Clear();
    gRec.slot = gSlot;
    gRec.left = cfg.mirror;
    gRecording = true;
    gLastRec = -1;
    gCap.fresh = gCap.stale = gCap.waits = gCap.losts = 0;
    RememberState(Now());
    gStatus = "Gravando... inclua M2 segurado, movimento durante inspecao e acoes em sequencia. F9 para parar.";
    return;
  }
  gRecording = false;
  gHider.ChromaPoll(gRec.frames, true);  // land the frames still in flight
  Log("gravacao: %zu frames, %zu inputs | captura: %d novos, %d sem mudanca, %d esperas, %d perdas (0x%08X)", gRec.frames.size(),
      gRec.inputs.size(), gCap.fresh, gCap.stale, gCap.waits, gCap.losts, (unsigned)gCap.lastErr);
  if (gRec.frames.empty()) {
    gStatus = "Nada gravado: a captura nao recebeu imagens do jogo. Deixe o CS2 em 'Tela cheia em janela' e com foco.";
    return;
  }
  double t0 = Now();
  Library L = gRec.Build(gHider.mw, gHider.mh);
  size_t poses = 0;
  for (auto& ts : L.takes) for (auto& t : ts) poses += t.frames.size();
  Log("biblioteca montada em %.1fs: %zu poses, roi %dx%d", Now() - t0, poses, L.rw, L.rh);
  if (L.Empty()) { gStatus = "Nada da arma na gravacao (estava no slot " + std::string(1, gRec.slot) + "?)."; return; }
  CreateDirectoryW(Widen(gRoot + "/native/masks").c_str(), nullptr);
  if (L.Save(LibPath(cfg.lib))) {
    gLib = std::move(L);
    gMaskAI.Stop(); gAIMaskBits.clear(); gAIMaskTime = -1;
    gCur = {};
    gMaskDirty = true;
    gStatus = "Biblioteca '" + cfg.lib + "' salva (" + TakeCount(gLib) + ").";
  } else gStatus = "Falha ao salvar a biblioteca.";
  gRec.Clear();
}

static void Jump(double t) {
  if (t - gMo.jumpT0 < kAirTime * 0.9) return;  // already in the air
  gMo.jumpT0 = t;
  if (gSlot != ActiveSlot()) return;
  if (gRecording) gRec.inputs.push_back({t, A_JUMP});
}

static void Trigger(Action a, double t) {
  if (gSlot != ActiveSlot()) return;
  if (gRecording) gRec.inputs.push_back({t, a});
  gPress = NextPress(gPress, a, t);
  if (const Take* tk = gLib.Pick(a, gPress.prev, gPress.gap, gPress.combo, 0, StateNow())) gCur = {a, tk, t, gPress, StateNow()};
  if (WeaponRT* w = Current()) Act(*w, a, gPress.combo);
}

static void SwitchSlot(char s, double t) {
  if (s == gSlot) { if (s == ActiveSlot()) Trigger(A_DRAW, t); return; }
  gLastSlot = gSlot;
  gSlot = s;
  gCur = {};
  Trigger(A_DRAW, t);
}

static void ToggleMenu(HWND hwnd);

static void ProcessInput(HWND hwnd) {
  std::vector<InputEvent> evs;
  {
    std::lock_guard<std::mutex> lk(gInMu);
    evs.swap(gInQ);
  }
  for (const InputEvent& e : evs) {
    if (!e.down) { gHeld.erase(e.code); RememberState(e.t); continue; }
    if (!gHeld.insert(e.code).second) continue;  // key repeat
    RememberState(e.t);
    if (gBinding.b >= 0) {  // menu asked for a key: take the next press (Esc cancels)
      if (e.code != VK_ESCAPE) gBind[gBinding.b][gBinding.slot] = e.code;
      gBinding.b = -1;
      SaveConfig();
      continue;
    }
    auto is = [&](int b) { return gBind[b][0] == e.code || gBind[b][1] == e.code; };
    if (is(B_MENU)) { ToggleMenu(hwnd); continue; }
    if (is(B_LIVE)) { cfg.live = !cfg.live; continue; }
    if (is(B_RECORD)) { ToggleRecord(); continue; }
    if (is(B_HAND)) { cfg.mirror = !cfg.mirror; Trigger(A_DRAW, e.t); continue; }  // switchhands works in the menu too
    if (gMenu) continue;  // clicks on the menu are not game input
    for (int k = 0; k < 5; k++) if (is(B_SLOT1 + k)) SwitchSlot((char)('1' + k), e.t);
    if (is(B_LAST)) SwitchSlot(gLastSlot, e.t);
    if (is(B_ATTACK1)) Trigger(A_ATTACK1, e.t);
    if (is(B_ATTACK2)) Trigger(A_ATTACK2, e.t);
    if (is(B_INSPECT)) Trigger(A_INSPECT, e.t);
    if (is(B_RELOAD)) Trigger(A_RELOAD, e.t);
    if (is(B_JUMP)) Jump(e.t);
  }
}

// Mask for the frame on screen at displayTime: the take of the last press (+-trailMs so fast swings stay covered),
// else the standing or walking idle union; a jump in progress adds its bob on top
static const std::vector<uint8_t>* LiveMask(double displayTime) {
  if (gLib.Empty()) return nullptr;
  if (gCur.take && gVisual.takeStart != gCur.t0) { gVisual.takeStart = gCur.t0; gVisual.phase = 0; }
  uint8_t state = StateNow();
  const std::vector<uint8_t>& base = state & S_RIGHT ? (state & S_MOVE ? gLib.moveRight : gLib.idleRight)
                                                      : (state & S_MOVE ? gLib.move : gLib.idle);
  float win = cfg.trailMs / 1000.f, sync = cfg.syncMs / 1000.f + (cfg.visualSync ? gVisual.phase : 0.f);
  const std::vector<uint8_t>* m = &base;
  if (gCur.take) {
    float el = (float)(displayTime - gCur.t0) + sync;
    if (state != gCur.state && el >= 0) {
      const PressCtx& c = gCur.ctx;
      if (const Take* variant = gLib.Pick(gCur.a, c.prev, c.gap, c.combo, el, state, el, gCur.take)) gCur.take = variant;
      gCur.state = state;
    }
    gLiveBits.assign(base.size(), 0);
    bool any = gCur.take->OrInto(el - win, el + win, gLiveBits);
    if (!any && gCur.take->cut && el < kMaxDur[gCur.a]) {  // recorded take was interrupted early: go on in one that ran longer
      const PressCtx& c = gCur.ctx;
      if (const Take* longer = gLib.Pick(gCur.a, c.prev, c.gap, c.combo, el, state, el, gCur.take)) {
        gCur.take = longer; any = longer->OrInto(el - win, el + win, gLiveBits);
      }
    }
    if (any) m = &gLiveBits;
    else gCur = {};
  }
  float je = (float)(displayTime - gMo.jumpT0) + sync;
  if (je >= 0 && je < kAirTime + 0.6f && !gLib.takes[A_JUMP].empty()) {
    if (m != &gLiveBits) gLiveBits = *m;
    for (const Take& t : gLib.takes[A_JUMP]) t.OrInto(je - win, je + win, gLiveBits);
    m = &gLiveBits;
  }
  return m;
}

// Compare the recorded weapon's inner pixels with the captured screen at 1/8 resolution.
// Brightness/contrast are fitted per candidate, so map lighting does not drive the phase.
static void TrackMaskPhase(double captureTime) {
  if (!gCur.take || gLuma4.size() != (size_t)gHider.mw * gHider.mh || gLib.LW() <= 0) return;
  if (gVisual.takeStart != gCur.t0) { gVisual.takeStart = gCur.t0; gVisual.phase = 0; }
  const float base = (float)(captureTime - gCur.t0) + cfg.syncMs / 1000.f;
  const float expected = base + gVisual.phase;
  float best = 1e9f, bestTime = expected;
  int lw = gLib.LW(), lh = gLib.LH(), mw = gHider.mw;
  for (const MaskFrame& f : gCur.take->frames) {
    if (fabsf(f.t - expected) > 0.12f || f.luma.size() != (size_t)lw * lh) continue;
    double n = 0, a = 0, b = 0, aa = 0, bb = 0, ab = 0;
    for (int y = 1; y + 1 < lh; y += 3) for (int x = 1; x + 1 < lw; x += 3) {
      int bx = x * 2, by = y * 2;
      if (by + 1 >= gLib.rh || bx + 1 >= gLib.rw) continue;
      size_t i = (size_t)by * gLib.rw + bx;
      if (i / 8 >= f.bits.size() || !((f.bits[i >> 3] >> (i & 7)) & 1) ||
          !((f.bits[(i + 1) >> 3] >> ((i + 1) & 7)) & 1) ||
          !((f.bits[(i + gLib.rw) >> 3] >> ((i + gLib.rw) & 7)) & 1) ||
          !((f.bits[(i + gLib.rw + 1) >> 3] >> ((i + gLib.rw + 1) & 7)) & 1)) continue;
      int sx = gLib.rx + bx, sy = gLib.ry + by;
      if (cfg.mirror != gLib.left) sx = mw - sx - 2;
      if (sx < 0 || sy < 0 || sx + 1 >= mw || sy + 1 >= gHider.mh) continue;
      float recorded = f.luma[(size_t)y * lw + x];
      float live = (gLuma4[(size_t)sy * mw + sx] + gLuma4[(size_t)sy * mw + sx + 1] +
                    gLuma4[(size_t)(sy + 1) * mw + sx] + gLuma4[(size_t)(sy + 1) * mw + sx + 1]) * 0.25f;
      n++; a += recorded; b += live; aa += recorded * recorded; bb += live * live; ab += recorded * live;
    }
    if (n < 32) continue;
    double va = aa - a * a / n, vb = bb - b * b / n;
    if (va < n * 80 || vb < n * 80) continue;
    float correlation = (float)((ab - a * b / n) / sqrt(va * vb));
    float score = 1.f - correlation + fabsf(f.t - expected) * 0.35f;
    if (score < best) { best = score; bestTime = f.t; }
  }
  if (best < 0.65f) gVisual.phase = std::clamp(gVisual.phase * 0.7f + (bestTime - base) * 0.3f, -0.12f, 0.12f);
}

// ================= window
static void ToggleMenu(HWND hwnd) {
  gMenu = !gMenu;
  LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (gMenu) ex &= ~(LONG_PTR)(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
  else { ex |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE; SaveConfig(); }
  SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
  if (gMenu && ImGui_ImplWin32_WndProcHandler(h, m, wp, lp)) return 1;
  if (m == WM_MOUSEACTIVATE && !gMenu) return MA_NOACTIVATE;
  if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(h, m, wp, lp);
}

static void Menu(WeaponRT* w) {
  ImGui::SetNextWindowPos({40, 40}, ImGuiCond_FirstUseEver);
  ImGui::Begin("Viewmodel Overlay", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::TextDisabled("%s: menu   %s: ao vivo   %s: gravar   (ESC no jogo solta o mouse)", KeyName(gBind[B_MENU][0]).c_str(),
                      KeyName(gBind[B_LIVE][0]).c_str(), KeyName(gBind[B_RECORD][0]).c_str());
  const WeaponDef* def = FindDef(cfg.weapon);
  if (ImGui::CollapsingHeader("Arma", ImGuiTreeNodeFlags_DefaultOpen)) {
    // type first, then the weapon: one long list needed scrolling to reach the knives
    std::string group = def ? def->group : "";
    ImGui::SetNextItemWidth(280);
    if (ImGui::BeginCombo("Tipo", group.c_str())) {
      std::string seen;
      for (auto& d : gDefs) {
        if (d.hidden || d.group == seen) continue;
        seen = d.group;
        if (ImGui::Selectable(d.group.c_str(), d.group == group)) cfg.weapon = d.key;  // first weapon of that type
      }
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(280);
    if (ImGui::BeginCombo("Arma##lista", def ? def->label.c_str() : "?", ImGuiComboFlags_HeightLargest)) {
      for (auto& d : gDefs)
        if (!d.hidden && d.group == group && ImGui::Selectable((d.label + "##" + d.key).c_str(), d.key == cfg.weapon)) cfg.weapon = d.key;
      ImGui::EndCombo();
    }
    if (def) {
      std::string& sid = cfg.skin[def->key];
      const SkinDef* cur = FindSkin(sid);
      int count = 0;
      for (auto& s : gSkins) count += BaseAsset(s.asset) == def->skinKey;
      ImGui::SetNextItemWidth(280);
      std::string none = count ? "Sem skin (" + std::to_string(count) + ")" : "Sem skins";
      if (ImGui::BeginCombo("Skin", cur ? cur->label.c_str() : none.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(none.c_str(), !cur)) sid.clear();
        for (auto& s : gSkins)
          if (BaseAsset(s.asset) == def->skinKey && ImGui::Selectable((s.label + "##" + s.id).c_str(), cur == &s)) sid = s.id;
        ImGui::EndCombo();
      }
    }
    ImGui::SetNextItemWidth(280);
    if (ImGui::BeginCombo("Luvas", cfg.glove.empty() ? "Padrao" : Title(cfg.glove).c_str())) {
      if (ImGui::Selectable("Padrao", cfg.glove.empty())) cfg.glove.clear();
      for (auto g : kGloves) if (ImGui::Selectable(Title(g).c_str(), cfg.glove == g)) cfg.glove = g;
      ImGui::EndCombo();
    }
    if (w) {
      int n = 0;
      for (auto& c : w->Skel().clips) {
        if (n++ % 4) ImGui::SameLine();
        if (ImGui::Button(c.name.c_str())) Play(*w, c.name);
      }
    }
  }
  if (ImGui::CollapsingHeader("POV", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::SliderFloat("FOV (viewmodel_fov)", &cfg.fov, 40, 110, "%.0f");
    ImGui::SliderFloat("X (viewmodel_offset_x)", &cfg.ox, -2.5f, 2.5f, "%.1f");
    ImGui::SliderFloat("Y (viewmodel_offset_y)", &cfg.oy, -2.f, 2.f, "%.1f");
    ImGui::SliderFloat("Z (viewmodel_offset_z)", &cfg.oz, -2.f, 2.f, "%.1f");
    if (ImGui::BeginCombo("Resolucao", cfg.res == kResCount ? "Personalizada" : kRes[cfg.res].name)) {
      for (int i = 0; i < kResCount; i++) if (ImGui::Selectable(kRes[i].name, cfg.res == i)) cfg.res = i;
      if (ImGui::Selectable("Personalizada", cfg.res == kResCount)) cfg.res = kResCount;
      ImGui::EndCombo();
    }
    if (cfg.res == kResCount) {
      if (ImGui::InputInt("Largura", &cfg.customW, 0, 0)) {
        cfg.customW = std::clamp(cfg.customW, 320, 3840);
        ApplyCustomAspect();
      }
      if (ImGui::InputInt("Altura", &cfg.customH, 0, 0)) {
        cfg.customH = std::clamp(cfg.customH, 240, 2160);
        cfg.customAspect = 0;
      }
      static const char* aspects[] = {"Livre", "4:3", "16:10", "16:9"};
      if (ImGui::BeginCombo("Proporcao", aspects[cfg.customAspect])) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(aspects[i], cfg.customAspect == i)) {
          cfg.customAspect = i;
          ApplyCustomAspect();
        }
        ImGui::EndCombo();
      }
      ImGui::Checkbox("Barras pretas (desmarcado: esticado)", &cfg.customBars);
    }
    ImGui::SliderFloat("Balanco ao andar", &cfg.bob, 0, 2, "%.2f");
    ImGui::SliderFloat("Recuo ao andar para frente", &cfg.forwardRetreat, -10, 10, "%.2f pol");
    ImGui::SliderFloat("Recuo ao andar para tras", &cfg.backwardRetreat, -10, 10, "%.2f pol");
    ImGui::SliderFloat("Atraso do mouse", &cfg.sway, 0, 2, "%.2f");
    ImGui::SliderFloat("Luz: direcao", &cfg.lightAz, -180, 180, "%.0f");
    ImGui::SliderFloat("Luz: altura", &cfg.lightEl, -85, 85, "%.0f");
    ImGui::Checkbox("Luz automatica (pela imagem do jogo)", &cfg.autoLight);
    if (cfg.autoLight) ImGui::SliderFloat("Reflexo / luz do mapa", &cfg.envStrength, 0, 4, "%.1f");
    if (cfg.autoLight) { ImGui::SameLine(); ImGui::TextDisabled("brilho %.2f, luz em %+.2f %+.2f", gScene.bright, gScene.x, gScene.y); }
  }
  if (ImGui::CollapsingHeader("Captura e ao vivo", ImGuiTreeNodeFlags_DefaultOpen)) {
    static char lib[64];
    if (!lib[0]) strncpy_s(lib, cfg.lib.c_str(), _TRUNCATE);
    ImGui::SetNextItemWidth(160);
    if (ImGui::InputText("Biblioteca", lib, sizeof lib) && lib[0]) cfg.lib = lib;
    ImGui::SameLine();
    if (ImGui::Button("Carregar")) LoadLibrary();
    if (ImGui::Button(gRecording ? "Parar gravacao" : "Gravar no mapa verde")) ToggleRecord();
    if (gRecording) { ImGui::SameLine(); ImGui::Text("%zu frames, %zu acoes", gRec.frames.size(), gRec.inputs.size()); }
    ImGui::Checkbox("Ao vivo", &cfg.live);
    ImGui::Checkbox("Mascara por IA (ONNX)", &cfg.aiMask);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Usa o modelo treinado da biblioteca atual. Sem modelo ou sem resultado recente, usa a mascara gravada.");
    ImGui::SameLine(); ImGui::TextDisabled("%s", gMaskAI.Ready() ? "modelo carregado" : "treine a biblioteca atual");
    ImGui::SameLine();
    ImGui::Checkbox("Mostrar mascara", &cfg.preview);
    ImGui::SameLine();
    ImGui::Checkbox("So com o CS2 em foco", &cfg.onlyGame);
    ImGui::Checkbox(("Mao esquerda / espelho (" + KeyName(gBind[B_HAND][0]) + ")").c_str(), &cfg.mirror);
    ImGui::Checkbox("Fundo real (memoria da camera)", &cfg.pano);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Atras da arma aparece o que a camera viu ali ha pouco (ao girar a mira), em vez de borrao.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::SliderFloat("validade (s)", &cfg.panoAge, 0.2f, 5.f, "%.1f");
    ImGui::Checkbox("Prever o giro pelo mouse", &cfg.mousePredict);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Usa o movimento real do mouse desde a captura: o fundo lembrado acompanha ate flicks.");
    ImGui::SameLine();
    if (gSens.ok) ImGui::TextDisabled("calibrado (%.5f %.5f rad/contagem)", gSens.kx, gSens.ky);
    else ImGui::TextDisabled("calibrando: gire a mira no jogo...");
    ImGui::Text("Slot atual: %c   slot da biblioteca: %c   captura: %s, %.0f quadros/s", gSlot, ActiveSlot(), gCap.ok ? "ok" : "parada", gCapRate.fps);
    if (gTracking)
      ImGui::Text("Camera: %s  (giro %.2f %.2f %.2f graus, erro %.3f)", gCam.ok ? "rastreando" : "perdida", gCam.omega[0] * 57.3f,
                  gCam.omega[1] * 57.3f, gCam.omega[2] * 57.3f, gCam.err);
    if (!gLib.Empty()) {
      ImGui::Text("  gravada com a mao %s", gLib.left ? "esquerda" : "direita");
      for (int a = A_ATTACK1; a < A_COUNT; a++) ImGui::Text("  %-18s %zu take(s)", kActionNames[a], gLib.takes[a].size());
    }
    ImGui::SliderInt("Dilatacao da mascara", &cfg.dilate, 0, 8);
    if (ImGui::TreeNodeEx("Preenchimento (o que aparece no lugar da arma)", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Checkbox("NVIDIA Optical Flow (historico)", &cfg.fill.optical);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reconstrói a area da arma usando quadros anteriores. Desligue para comparar com o preenchimento atual.");
      if (cfg.fill.optical) {
        ImGui::SliderInt("Frames anteriores (NVOFA)", &cfg.fill.opticalFrames, 1, 4);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mais frames podem revelar partes antes escondidas, mas aumentam o trabalho da GPU. Padrao: 2.");
      }
      if (!gHider.OpticalAvailable()) ImGui::TextDisabled("NVOFA indisponivel nesta GPU/driver");
      ImGui::SliderFloat("Preenchimento inteligente (PatchMatch)", &cfg.fill.patch, 0, 1, "%.2f");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Como o 'preenchimento sensivel ao conteudo' do Photoshop: copia pedacos parecidos da tela.\nMelhora em poucos quadros e segue melhorando enquanto joga. 0 = desligado.");
      if (cfg.fill.patch > 0) {
        static const char* q[] = {"leve", "media", "alta", "maxima"};
        ImGui::SliderInt("Qualidade do inteligente", &cfg.fill.quality, 1, 4, q[std::clamp(cfg.fill.quality, 1, 4) - 1]);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mais alta: pedacos maiores comparados, mais passos e tentativas por quadro.\nMais imperceptivel, mais GPU (alta/maxima ainda ficam em ~1 ms numa 4060).");
        ImGui::Checkbox("Manter a cor do lugar", &cfg.fill.colorFix);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("O pedaco copiado traz so a textura; a cor/brilho fica a da regiao (sem manchas de outra cor).");
      }
      ImGui::SliderFloat("Textura da borda", &cfg.fill.texture, 0, 2, "%.2f");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quanto da textura em volta (parede, chao) continua para dentro. 0 = so borrao.");
      ImGui::SliderFloat("Alcance da textura (px)", &cfg.fill.reach, 20, 600, "%.0f");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ate quantos pixels para dentro a textura vai antes de sumir no borrao.");
      ImGui::SliderFloat("Tamanho do detalhe", &cfg.fill.grain, 1, 4, "%.1f");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("1 = so detalhe fino (ruido, grao), 4 = formas maiores (tijolos, faixas).");
      ImGui::SliderFloat("Nitidez do borrao", &cfg.fill.sharp, 0, 3, "%.1f");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Base por baixo da textura: 0 bem liso, 3 mais definido (pode mostrar blocos).");
      ImGui::SliderInt("Suavidade da borda", &cfg.fill.feather, 0, 6);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Largura da transicao entre o jogo e a area preenchida (x4 px).");
      ImGui::SliderFloat("Esconder emendas", &cfg.fill.seam, 0, 0.95f, "%.2f");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Onde duas bordas se encontram o espelho troca de lado: mais alto apaga mais a textura ali.");
      if (ImGui::Button("Padrao##fill")) cfg.fill = FillParams{};
      ImGui::TreePop();
    }
    if (gCur.take)
      ImGui::Text("Agora: %s, golpe %d da sequencia, depois de %s (%.2fs)", kActionNames[gCur.a], gCur.ctx.combo + 1,
                  gCur.ctx.prev >= 0 ? kActionNames[gCur.ctx.prev] : "nada", std::min(gCur.ctx.gap, 9.f));
    ImGui::SliderFloat("Sync (ms)", &cfg.syncMs, -150, 150, "%.0f");
    ImGui::Checkbox("Sincronizar mascara pela imagem", &cfg.visualSync);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Compara a arma capturada com as poses gravadas e corrige pequenos atrasos automaticamente.");
    ImGui::SliderFloat("Rastro (ms)", &cfg.trailMs, 0, 80, "%.0f");
    ImGui::SliderInt("Dilatacao extra andando", &cfg.moveDilate, 0, 6);
    ImGui::SeparatorText("Gravacao");
    ImGui::SliderFloat("Tolerancia do verde", &cfg.greenTol, 0.02f, 0.5f, "%.2f");
    ImGui::TextWrapped("%s", gStatus.c_str());
  }
  if (ImGui::CollapsingHeader("Teclas (as mesmas binds do seu CS2)")) {
    ImGui::TextDisabled("Clique e aperte a tecla ou botao. Esc cancela, botao direito limpa.");
    if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_SizingFixedFit)) {
      for (int b = 0; b < B_COUNT; b++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kBindLabel[b]);
        for (int k = 0; k < 2; k++) {
          ImGui::TableNextColumn();
          bool waiting = gBinding.b == b && gBinding.slot == k;
          std::string label = (waiting ? std::string("aperte...") : KeyName(gBind[b][k])) + "##" + std::to_string(b * 2 + k);
          if (ImGui::Button(label.c_str(), {150, 0})) { gBinding.b = b; gBinding.slot = k; }
          if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { gBind[b][k] = 0; SaveConfig(); }
        }
      }
      ImGui::EndTable();
    }
    if (ImGui::Button("Restaurar padrao")) { memcpy(gBind, kBindDefault, sizeof gBind); SaveConfig(); }
  }
  if (ImGui::Button("Sair")) gQuit = true;
  ImGui::End();
}

static void RecBadge() {
  ImGui::SetNextWindowPos({20, 20});
  ImGui::SetNextWindowBgAlpha(0.65f);
  ImGui::Begin("rec", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::TextColored({1, 0.35f, 0.35f, 1}, "GRAVANDO  %zu frames   captura %.0f quadros/s   (%s para parar)", gRec.frames.size(),
                     gCapRate.fps, KeyName(gBind[B_RECORD][0]).c_str());
  int n[A_COUNT] = {};
  for (auto& in : gRec.inputs) n[in.a]++;
  ImGui::Text("M1 %d   M2 %d   inspecionar %d   sacar %d   pular %d    (meta: 4 de cada, com pausa entre elas)", n[A_ATTACK1],
              n[A_ATTACK2], n[A_INSPECT], n[A_DRAW], n[A_JUMP]);
  ImGui::TextDisabled("Tambem: sequencias (M1 M1 M1, M1 M2, M2 M1...), uma acao cortando outra, ~10 s parado e ~10 s andando.");
  ImGui::TextDisabled("Inclua M2 segurado e inspecao com movimento/agachamento para gravar essas variantes.");
  if (gCapRate.fps < 1) ImGui::TextColored({1, 0.8f, 0.2f, 1}, "Sem imagem do jogo! Deixe o CS2 em 'Tela cheia em janela'.");
  else if (gSlot != gRec.slot) ImGui::TextColored({1, 0.8f, 0.2f, 1}, "Pegue a arma do slot %c: os outros quadros nao entram.", gRec.slot);
  ImGui::End();
}

// ================= smoke test: renders + checks the fill math offscreen, no window
static void SavePNG(ID3D11Texture2D* tex, const std::string& path) {
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.MipLevels = 1;
  ComPtr<ID3D11Texture2D> st;
  HR(gDev->CreateTexture2D(&d, nullptr, &st));
  gCtx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, tex, 0, nullptr);
  D3D11_MAPPED_SUBRESOURCE ms;
  HR(gCtx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms));
  ComPtr<IWICImagingFactory> wic;
  HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
  ComPtr<IWICStream> s;
  ComPtr<IWICBitmapEncoder> enc;
  ComPtr<IWICBitmapFrameEncode> fr;
  HR(wic->CreateStream(&s));
  HR(s->InitializeFromFilename(Widen(path).c_str(), GENERIC_WRITE));
  HR(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc));
  HR(enc->Initialize(s.Get(), WICBitmapEncoderNoCache));
  HR(enc->CreateNewFrame(&fr, nullptr));
  HR(fr->Initialize(nullptr));
  HR(fr->SetSize(d.Width, d.Height));
  WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
  HR(fr->SetPixelFormat(&fmt));
  HR(fr->WritePixels(d.Height, ms.RowPitch, ms.RowPitch * d.Height, (BYTE*)ms.pData));
  HR(fr->Commit());
  HR(enc->Commit());
  gCtx->Unmap(st.Get(), 0);
}

static void ReadPixel(ID3D11Texture2D* tex, int x, int y, float out[4]) {
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  d.Width = 1; d.Height = 1; d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.MipLevels = 1;
  ComPtr<ID3D11Texture2D> st;
  HR(gDev->CreateTexture2D(&d, nullptr, &st));
  D3D11_BOX box{(UINT)x, (UINT)y, 0, (UINT)x + 1, (UINT)y + 1, 1};
  gCtx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, tex, 0, &box);
  D3D11_MAPPED_SUBRESOURCE ms;
  HR(gCtx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &ms));
  const uint8_t* p = (const uint8_t*)ms.pData;  // BGRA
  out[0] = p[2] / 255.f; out[1] = p[1] / 255.f; out[2] = p[0] / 255.f; out[3] = p[3] / 255.f;
  gCtx->Unmap(st.Get(), 0);
}

// a busy, smooth world around the camera for the tracking/memory tests (direction -> rgb)
static void World(const float d[3], float c[3]) {
  float a = sinf(d[0] * 23 + d[1] * 7) * cosf(d[2] * 17 - d[1] * 11), b = sinf(d[1] * 31 + d[2] * 5 + d[0] * 3), e = cosf(d[0] * 13 - d[2] * 29);
  c[0] = 0.5f + 0.25f * a + 0.15f * e;
  c[1] = 0.5f + 0.25f * b;
  c[2] = 0.45f + 0.2f * a * b + 0.2f * e;
}
static void WorldAt(const float R[9], const float K[4], float sx, float sy, float c[3]) {  // what screen point (sx, sy) sees
  float v[3] = {(sx - K[2]) / K[0], -(sy - K[3]) / K[1], 1}, d[3];
  for (int i = 0; i < 3; i++) d[i] = R[i * 3] * v[0] + R[i * 3 + 1] * v[1] + R[i * 3 + 2] * v[2];
  float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  for (float& x : d) x /= n;
  World(d, c);
}

static int Smoke() {
  int failures = 0;
  auto check = [&](bool ok, const char* what) { Log("%s %s", ok ? "OK  " : "FAIL", what); failures += !ok; };

  // 0) every knife in the menu loads and becomes the one shown
  {
    int bad = 0;
    for (auto& d : gDefs) {
      if (!d.knife || d.hidden) continue;
      cfg.weapon = d.key;
      WeaponRT* k = Current();
      if (!k || k->def != &d) { Log("faca nao troca: %s", d.key.c_str()); bad++; }
    }
    check(!bad, "todas as facas do menu carregam e trocam");
  }
  // 1) viewmodel render with skin + gloves
  cfg = Config{};
  cfg.weapon = "karambit";
  cfg.skin["karambit"] = "knife-karambit-aa_fade";
  cfg.glove = "specialist-kimono";
  WeaponRT* w = Current();
  check(w != nullptr, "karambit (golpes) carregou");
  if (w) {
    check(w->paint.mode == 1 && w->paint.mask && w->paint.pattern, "skin Fade aplicada (mascara + padrao)");
    check(w->glove != nullptr, "luva Specialist Kimono religada ao esqueleto");
    Play(*w, "idle");
    for (int i = 0; i < 30; i++) Tick(*w, 1 / 60.f);
    RenderViewmodel(*w, 1280, 720);
    RT out = MakeRT(1280, 720, DXGI_FORMAT_B8G8R8A8_UNORM);
    float bg[4] = {0.18f, 0.2f, 0.24f, 1};
    gCtx->ClearRenderTargetView(out.rtv.Get(), bg);
    gCtx->OMSetRenderTargets(1, out.rtv.GetAddressOf(), nullptr);
    SetViewport(0, 0, 1280, 720);
    gPass.Draw(gPSView.Get(), {gViewRes.srv.Get()}, gPass.premulOver.Get());
    SavePNG(out.tex.Get(), gRoot + "/native/smoke_viewmodel.png");
    // something opaque must land in the lower half (hands + knife), the top-left corner stays background
    int hits = 0;
    for (int x = 100; x < 1280; x += 60) {
      float p[4];
      ReadPixel(out.tex.Get(), x, 650, p);
      hits += fabsf(p[0] - 0.18f) + fabsf(p[1] - 0.2f) + fabsf(p[2] - 0.24f) > 0.06f;
    }
    float corner[4];
    ReadPixel(out.tex.Get(), 5, 5, corner);
    check(hits >= 3, "viewmodel aparece na parte de baixo da tela");
    check(fabsf(corner[2] - 0.24f) < 0.02f, "fundo intacto fora do modelo");
    // the game image lights the model: a bright red map makes it redder and brighter than a dark one
    auto litWith = [&](uint32_t bgra, float bright) {
      std::vector<uint32_t> img((size_t)640 * 360, bgra);
      D3D11_TEXTURE2D_DESC d{};
      d.Width = 640; d.Height = 360; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA sd{img.data(), 640 * 4};
      ComPtr<ID3D11Texture2D> t;
      ComPtr<ID3D11ShaderResourceView> srv;
      HR(gDev->CreateTexture2D(&d, &sd, &t));
      HR(gDev->CreateShaderResourceView(t.Get(), nullptr, &srv));
      gScene.bright = bright;
      gEnv = BuildEnv(srv.Get());
      RenderViewmodel(*w, 1280, 720);
      gCtx->ClearRenderTargetView(out.rtv.Get(), bg);
      gCtx->OMSetRenderTargets(1, out.rtv.GetAddressOf(), nullptr);
      SetViewport(0, 0, 1280, 720);
      gPass.Draw(gPSView.Get(), {gViewRes.srv.Get()}, gPass.premulOver.Get());
      float sum[3] = {};
      for (int x = 700; x < 1280; x += 40) {
        float p[4];
        ReadPixel(out.tex.Get(), x, 660, p);
        for (int c = 0; c < 3; c++) sum[c] += p[c];
      }
      gEnv = nullptr;
      return std::array<float, 3>{sum[0], sum[1], sum[2]};
    };
    auto red = litWith(0xffff3020u, 0.6f), dark = litWith(0xff101010u, 0.04f);
    Log("luz do mapa: vermelho %.2f %.2f %.2f, escuro %.2f %.2f %.2f", red[0], red[1], red[2], dark[0], dark[1], dark[2]);
    check(red[0] > dark[0] * 1.5f && red[0] - red[2] > dark[0] - dark[2] + 0.5f, "mapa claro/vermelho ilumina e tinge o modelo");
  }

  // 2) pull-push fill: a red block on a gradient must vanish into the gradient
  const int W = 1920, H = 1080;
  std::vector<uint32_t> px((size_t)W * H);
  auto texture = [&](ComPtr<ID3D11ShaderResourceView>& srv) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), W * 4};
    ComPtr<ID3D11Texture2D> t;
    HR(gDev->CreateTexture2D(&d, &sd, &t));
    HR(gDev->CreateShaderResourceView(t.Get(), nullptr, &srv));
  };
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      bool block = x >= 1400 && x < 1800 && y >= 700;
      uint8_t r = block ? 255 : (uint8_t)(255 * x / W), g = block ? 0 : (uint8_t)(255 * y / H), b = block ? 0 : 128;
      px[(size_t)y * W + x] = 0xff000000u | (r << 16) | (g << 8) | b;  // BGRA in memory
    }
  ComPtr<ID3D11ShaderResourceView> screenSRV;
  texture(screenSRV);
  gHider.Init(W, H);
  const int mw = gHider.mw, mh = gHider.mh, lw = (mw + 1) / 2, lh = (mh + 1) / 2;
  auto rect = [&](int x0, int y0, int x1, int y1) {  // full-frame mask bits
    std::vector<uint8_t> p((size_t)mw * mh, 0), bits;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) p[(size_t)y * mw + x] = 255;
    PackBits(p, bits);
    return bits;
  };
  std::vector<uint8_t> blockBits = rect(1400 / 4, 700 / 4, 1800 / 4, mh);
  gHider.UploadMask(&blockBits, 0, 0, mw, mh, 0, 0, 1);
  RT fill = MakeRT(W, H, DXGI_FORMAT_B8G8R8A8_UNORM);
  float zero[4] = {0, 0, 0, 0};
  gCtx->ClearRenderTargetView(fill.rtv.Get(), zero);
  gCtx->OMSetRenderTargets(1, fill.rtv.GetAddressOf(), nullptr);
  SetViewport(0, 0, W, H);
  gHider.Fill(screenSRV.Get(), nullptr, 0, 1.5f, 0);
  SavePNG(fill.tex.Get(), gRoot + "/native/smoke_fill.png");
  float in[4], out[4];
  ReadPixel(fill.tex.Get(), 1600, 900, in);
  ReadPixel(fill.tex.Get(), 300, 300, out);
  Log("fill dentro do bloco: %.2f %.2f %.2f a=%.2f (esperado ~0.83 ~0.83 0.50 a=1)", in[0], in[1], in[2], in[3]);
  check(in[3] > 0.95f && in[1] > 0.5f && fabsf(in[2] - 0.5f) < 0.12f, "bloco vermelho preenchido com o fundo em volta");
  check(out[3] < 0.01f, "fora da mascara nada e desenhado");
  {  // texture continues into the hole: vertical stripes stay stripes just inside the top edge
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) px[(size_t)y * W + x] = x >= 1400 && x < 1800 && y >= 700 ? 0xffff0000u : (x / 8) % 2 ? 0xff202020u : 0xffe0e0e0u;
    ComPtr<ID3D11ShaderResourceView> stripes;
    texture(stripes);
    gHider.fp.patch = 0;  // the edge texture alone first
    gCtx->ClearRenderTargetView(fill.rtv.Get(), zero);
    gCtx->OMSetRenderTargets(1, fill.rtv.GetAddressOf(), nullptr);
    SetViewport(0, 0, W, H);
    gHider.Fill(stripes.Get(), nullptr, 0, 1.5f, 0);
    SavePNG(fill.tex.Get(), gRoot + "/native/smoke_fill_texture.png");
    float a[4], b[4];
    ReadPixel(fill.tex.Get(), 1604, 714, a);
    ReadPixel(fill.tex.Get(), 1612, 714, b);
    Log("listras dentro do buraco: %.2f vs %.2f", a[0], b[0]);
    check(fabsf(a[0] - b[0]) > 0.3f && a[2] > 0.3f, "textura da borda continua para dentro (nao vira mancha)");
    // content-aware fill: after a few frames the stripes also fill the middle of the hole, far from any edge
    auto deep = [&](float patch) {
      gHider.fp.patch = patch;
      for (int i = 0; i < 8; i++) {
        gCtx->ClearRenderTargetView(fill.rtv.Get(), zero);
        gCtx->OMSetRenderTargets(1, fill.rtv.GetAddressOf(), nullptr);
        SetViewport(0, 0, W, H);
        gHider.Fill(stripes.Get(), nullptr, 0, 1.5f, 0);
      }
      float c = 0;  // stripe contrast along a row deep inside
      for (int x = 1500; x < 1700; x += 4) {
        float p0[4], p1[4];
        ReadPixel(fill.tex.Get(), x, 960, p0);
        ReadPixel(fill.tex.Get(), x + 8, 960, p1);
        c += fabsf(p0[0] - p1[0]);
      }
      return c / 50;
    };
    float without = deep(0), with = deep(1);
    for (int q = 1; q <= 4; q++) {  // cost of each quality: 60 frames, then wait for the GPU
      gHider.fp.quality = q;
      double t0 = Now();
      for (int i = 0; i < 60; i++) gHider.Fill(stripes.Get(), nullptr, 0, 1.5f, 0);
      float px4[4];
      ReadPixel(fill.tex.Get(), 0, 0, px4);
      Log("PatchMatch qualidade %d: %.2f ms por quadro (preenchimento inteiro)", q, (Now() - t0) * 1000 / 60);
    }
    gHider.fp.quality = 4;
    float best = deep(1);
    Log("PatchMatch qualidade maxima: contraste %.2f", best);
    SavePNG(fill.tex.Get(), gRoot + "/native/smoke_fill_patch.png");
    gHider.fp = FillParams{};
    Log("PatchMatch: contraste no meio do buraco %.2f (sem: %.2f)", with, without);
    check(with > 0.4f && with > without + 0.2f, "preenchimento inteligente recria a textura longe da borda");
  }

  // 3) recording (compressed frames) -> cleanup -> takes -> roi crop -> compressed file round trip
  auto packed = [&](const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> z;
    PackFrame(bits, std::vector<uint8_t>((size_t)lw * lh, 90), z);
    return z;
  };
  auto at = [&](const Library& Lb, const std::vector<uint8_t>& v, int x, int y) {
    x -= Lb.rx; y -= Lb.ry;
    if (x < 0 || y < 0 || x >= Lb.rw || y >= Lb.rh) return false;
    size_t i = (size_t)y * Lb.rw + x;
    return ((v[i >> 3] >> (i & 7)) & 1) != 0;
  };
  std::vector<uint8_t> pa = packed(rect(10, mh - 30, 40, mh)), pb = packed(rect(150, mh - 30, 200, mh)), pc = packed(rect(80, mh - 40, 120, mh));
  Recorder rec;
  rec.slot = '3';
  for (int i = 0; i < 600; i++) {
    bool walking = i >= 200 && i < 240;  // between the M1 take and the F take
    rec.frames.push_back({i / 60.0, true, walking, walking ? pc : i >= 120 && i < 180 ? pb : pa});
  }
  rec.frames[300].state = S_RIGHT;
  rec.inputs.push_back({2.0, A_ATTACK1});
  rec.inputs.push_back({5.0, A_INSPECT});  // runs to the end of the footage
  Library L = rec.Build(mw, mh);
  check(L.takes[A_ATTACK1].size() == 1 && fabsf(L.takes[A_ATTACK1][0].Duration() - 1.2f) < 1.5f / 60, "take do M1 cortado em 1.2s");
  check(L.takes[A_INSPECT].size() == 1 && L.takes[A_INSPECT][0].frames.size() == 300, "take do F ate o fim da gravacao");
  check(L.takes[A_INSPECT][0].prev == A_ATTACK1 && fabsf(L.takes[A_INSPECT][0].gap - 3.f) < 1e-3f && !L.takes[A_ATTACK1][0].cut,
        "take guarda o contexto (acao anterior, intervalo)");
  check(at(L, L.idle, 20, mh - 5) && !at(L, L.idle, 170, mh - 5) && !at(L, L.idle, 100, mh - 5), "idle so com frames parados fora das acoes");
  check(at(L, L.move, 100, mh - 5) && !at(L, L.move, 20, mh - 5), "mascara andando separada");
  check(!L.takes[A_IDLE].empty() && !L.takes[A_IDLE][0].frames.empty(), "amostras de idle para a deteccao");
  Log("roi: %d,%d %dx%d de %dx%d", L.rx, L.ry, L.rw, L.rh, mw, mh);
  check(L.rx <= 10 && L.rx + L.rw >= 200 && L.rw < mw / 2 && L.ry + L.rh >= mh - 1 && L.rh < 60, "roi cobre so a regiao da arma");
  check(L.takes[A_ATTACK1][0].frames[0].luma.size() == (size_t)L.LW() * L.LH() && L.takes[A_ATTACK1][0].frames[0].luma[5] == 90,
        "miniatura recortada junto");
  std::string path = gRoot + "/native/masks/_smoke.vmlib";
  CreateDirectoryW(Widen(gRoot + "/native/masks").c_str(), nullptr);
  Library R;
  const MaskFrame &f0 = L.takes[A_ATTACK1][0].frames[10];
  check(L.Save(path) && R.Load(path) && R.takes[A_ATTACK1][0].frames[10].bits == f0.bits && R.takes[A_ATTACK1][0].frames[10].luma == f0.luma &&
            R.takes[A_INSPECT][0].frames[0].state == S_RIGHT && R.idleRight == L.idleRight &&
            R.idle == L.idle && R.move == L.move && R.rx == L.rx && R.rw == L.rw && R.rh == L.rh, "biblioteca comprimida salva e lida igual");
  DeleteFileW(Widen(path).c_str());
  const MaskFrame* f = L.takes[A_ATTACK1][0].At(0.5f);
  check(f && fabsf(f->t - 0.5f) < 0.02f && !L.takes[A_ATTACK1][0].At(2.f), "busca de frame por tempo");
  {
    std::vector<uint8_t> p((size_t)mw * mh, 0), bits;
    for (int y = mh - 40; y < mh; y++) for (int x = 50; x < 110; x++) p[(size_t)y * mw + x] = 255;  // hand from the bottom
    for (int y = mh - 25; y < mh - 15; y++) for (int x = 70; x < 90; x++) p[(size_t)y * mw + x] = 0;  // green reflection hole
    for (int y = 20; y < 23; y++) for (int x = 150; x < 153; x++) p[(size_t)y * mw + x] = 255;        // loose speck
    PackBits(p, bits);
    CleanMask(bits, mw, mh);
    auto bit = [&](int x, int y) { size_t i = (size_t)y * mw + x; return ((bits[i >> 3] >> (i & 7)) & 1) != 0; };
    check(bit(80, mh - 20) && bit(60, mh - 5) && !bit(151, 21), "limpeza: tira sujeira solta e fecha buraco");
  }
  {
    std::vector<uint8_t> previous(32 * 32, 0), current(32 * 32, 0), plain, tracked;
    for (int y = 26; y < 32; y++) for (int x = 2; x < 6; x++) previous[y * 32 + x] = current[y * 32 + x] = 255;
    for (int y = 14; y < 17; y++) for (int x = 17; x < 20; x++) previous[y * 32 + x] = 255;
    for (int y = 14; y < 17; y++) for (int x = 19; x < 22; x++) current[y * 32 + x] = 255;
    PackBits(current, plain); tracked = plain;
    CleanMask(plain, 32, 32);
    CleanMask(tracked, 32, 32, &previous);
    size_t blade = 15 * 32 + 20;
    check(!(plain[blade >> 3] & (1 << (blade & 7))) && (tracked[blade >> 3] & (1 << (blade & 7))),
          "tracking temporal recupera parte solta da arma");
  }

  // 4) chroma key through the async readback ring: a gray box on green comes back as a set block of bits
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) px[(size_t)y * W + x] = x >= 800 && x < 1200 && y >= 400 && y < 800 ? 0xff808080u : 0xff10c020u;
  ComPtr<ID3D11ShaderResourceView> greenSRV;
  texture(greenSRV);
  std::vector<Recorder::RawFrame> got;
  for (int i = 0; i < 9; i++) {  // more than the ring holds
    gHider.ChromaQueue(greenSRV.Get(), 0.12f, {i * 0.01, true, false, {}});
    gHider.ChromaPoll(got, false);
  }
  gHider.ChromaPoll(got, true);
  std::vector<uint8_t> gb, gl;
  bool unpacked = !got.empty() && UnpackFrame(got[0].packed, ((size_t)mw * mh + 7) / 8, (size_t)lw * lh, gb, gl);
  auto gbit = [&](int x, int y) { size_t i = (size_t)(y / 4) * mw + x / 4; return (gb[i >> 3] >> (i & 7)) & 1; };
  check(got.size() == 9 && fabs(got[8].t - 0.08) < 1e-9, "anel de leitura entrega todos os frames em ordem");
  check(unpacked && gbit(1000, 600) && !gbit(300, 300) && !gbit(1500, 900), "chroma: arma = 1, verde = 0");
  check(unpacked && gl[(size_t)(600 / 8) * lw + 1000 / 8] > 100 && gl[(size_t)(100 / 8) * lw + 100 / 8] == 0, "miniatura so em volta da arma");
  Log("frame comprimido: %zu bytes (cru %zu)", got.empty() ? 0 : got[0].packed.size(), ((size_t)mw * mh + 7) / 8 + (size_t)lw * lh);
  std::vector<uint8_t> l4;
  gHider.Analyze(greenSRV.Get(), l4);
  check(l4.size() == (size_t)mw * mh && abs(l4[(size_t)(600 / 4) * mw + 1000 / 4] - 128) <= 2, "leitura ao vivo do quadro (luma 1/4)");

  {  // mouse calibration: counts vs. measured turn give the rotation per count, any sign
    Sens sn;
    for (int i = 0; i < 200; i++) {
      double cx = (i % 7 - 3) * 10.0, cy = (i % 5 - 2) * 8.0;
      sn.Add(cx, cy, (float)(0.0004 * cx), (float)(-0.0003 * cy));
    }
    check(sn.ok && fabsf(sn.kx - 0.0004f) < 2e-6f && fabsf(sn.ky + 0.0003f) < 2e-6f, "sensibilidade do mouse aprendida pela tela");
  }
  // 5) presses: a chain alternates hands, an action after another picks the take recorded that way, mirror flips
  {
    Library P;
    P.w = mw; P.h = mh; P.rw = 8; P.rh = 2;
    P.idle = P.move = std::vector<uint8_t>(2, 0);
    auto take = [](int prev, float gap, int combo, float dur) {
      Take t;
      t.prev = prev; t.gap = gap; t.combo = combo;
      t.frames.push_back({0, {1, 0}, {}});
      t.frames.push_back({dur, {1, 0}, {}});
      return t;
    };
    P.takes[A_ATTACK1] = {take(-1, 99, 0, 0.5f), take(A_ATTACK1, 0.4f, 1, 0.5f), take(A_ATTACK2, 0.3f, 0, 0.5f)};
    PressCtx c;
    c = NextPress(c, A_ATTACK1, 10.0);
    const Take* t1 = P.Pick(A_ATTACK1, c.prev, c.gap, c.combo);
    c = NextPress(c, A_ATTACK1, 10.4);
    const Take* t2 = P.Pick(A_ATTACK1, c.prev, c.gap, c.combo);
    PressCtx d = NextPress(NextPress({}, A_ATTACK2, 20.0), A_ATTACK1, 20.3);
    const Take* t3 = P.Pick(A_ATTACK1, d.prev, d.gap, d.combo);
    check(t1 == &P.takes[A_ATTACK1][0] && t2 == &P.takes[A_ATTACK1][1] && c.combo == 1, "M1 M1: o 2o golpe usa a outra mao");
    check(t3 == &P.takes[A_ATTACK1][2], "M2 depois M1: take gravado nesse contexto");
    check(!P.Pick(A_ATTACK1, -1, 99, 0, 2.f), "take curto nao serve para continuar alem do fim");
    Take still = take(-1, 99, 0, 0.5f), heldMoving = still;
    for (auto& f : still.frames) f.state = 0;
    for (auto& f : heldMoving.frames) f.state = S_RIGHT | S_MOVE;
    P.takes[A_INSPECT] = {still, heldMoving};
    check(P.Pick(A_INSPECT, -1, 99, 0, 0.2f, S_RIGHT | S_MOVE, 0.2f) == &P.takes[A_INSPECT][1] &&
          P.Pick(A_INSPECT, -1, 99, 0, 0.2f, 0, 0.2f) == &P.takes[A_INSPECT][0],
          "inspecao escolhe a variante de movimento e M2 segurado");
    std::vector<uint8_t> one = {1};
    gHider.UploadMask(&one, 0, 0, 1, 1, 0, 0, 0, true);
    check(gHider.MaskPixels()[mw - 1] && !gHider.MaskPixels()[0], "mao esquerda espelha a mascara");
  }

  // 6) camera rotation from the screen alone
  float K[4];
  GameIntrinsics(W, H, K);
  const float I3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  auto lumaShot = [&](const float Rm[9], std::vector<uint8_t>& o) {
    o.resize((size_t)mw * mh);
    for (int y = 0; y < mh; y++)
      for (int x = 0; x < mw; x++) {
        float c[3];
        WorldAt(Rm, K, (x + 0.5f) * 4, (y + 0.5f) * 4, c);
        o[(size_t)y * mw + x] = (uint8_t)std::clamp(255 * (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]), 0.f, 255.f);
      }
  };
  {
    std::vector<uint8_t> l0, l1, valid;
    WorldValid(std::vector<uint8_t>((size_t)mw * mh, 0), mw, mh, valid);
    lumaShot(I3, l0);
    const float turns[2][3] = {{0.03f, 0.09f, 0.f}, {0.f, 0.3f, 0.01f}};  // small turn; a 17 degree flick
    for (auto& wt : turns) {
      float R1[9];
      RotVec(wt, R1);
      lumaShot(R1, l1);
      CamTracker ct;
      ct.Reset(0);
      ct.Update(l0, valid, mw, mh, K, 0);
      bool ok = ct.Update(l1, valid, mw, mh, K, 0.01);
      Log("camera: esperado %.4f %.4f %.4f, medido %.4f %.4f %.4f (erro %.3f)", wt[0], wt[1], wt[2], ct.omega[0], ct.omega[1], ct.omega[2], ct.err);
      check(ok && fabsf(ct.omega[0] - wt[0]) < 0.003f && fabsf(ct.omega[1] - wt[1]) < 0.003f && fabsf(ct.omega[2] - wt[2]) < 0.003f,
            "giro da camera medido pela tela");
    }
    for (size_t i = 0; i < l1.size(); i++) l1[i] = (uint8_t)((i * 2654435761u) >> 24);  // a cut to something unrelated
    CamTracker ct;
    ct.Reset(0);
    ct.Update(l0, valid, mw, mh, K, 0);
    check(!ct.Update(l1, valid, mw, mh, K, 0.5) && ct.lost == 0.5, "corte de cena invalida a memoria");
  }

  // 7) background memory: what the camera saw before turning shows up behind the viewmodel
  // (twice: looking ahead, and looking across the seam of the memory where it wraps around)
  for (int pass = 0; pass < 2; pass++) {
    const float base[3] = {0, pass ? 3.05f : 0.f, 0}, wb[3] = {0, 0.15f, 0};
    float RA[9], Rt[9], RB[9];
    RotVec(base, RA);
    RotVec(wb, Rt);
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) RB[i * 3 + j] = RA[i * 3] * Rt[j] + RA[i * 3 + 1] * Rt[3 + j] + RA[i * 3 + 2] * Rt[6 + j];
    float t0 = pass * 20.f;
    auto shot = [&](const float Rm[9], bool block) {
      for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
          if (block && x >= 1300 && x < 1700 && y >= 500 && y < 760) { px[(size_t)y * W + x] = 0xffff0000u; continue; }
          float c[3];
          WorldAt(Rm, K, x + 0.5f, y + 0.5f, c);
          auto q = [](float v) { return (uint32_t)std::clamp(v * 255 + 0.5f, 0.f, 255.f); };
          px[(size_t)y * W + x] = 0xff000000u | (q(c[0]) << 16) | (q(c[1]) << 8) | q(c[2]);
        }
    };
    ComPtr<ID3D11ShaderResourceView> srvA, srvB;
    shot(RA, false);
    texture(srvA);
    shot(RB, true);
    texture(srvB);
    PanoCam ca{}, cb{};
    memcpy(ca.R, RA, sizeof ca.R); memcpy(ca.show, RA, sizeof ca.show); memcpy(ca.K, K, sizeof ca.K);
    memcpy(cb.R, RB, sizeof cb.R); memcpy(cb.show, RB, sizeof cb.show); memcpy(cb.K, K, sizeof cb.K);
    auto run = [&](ID3D11ShaderResourceView* src, const PanoCam* cam, float now) {
      gCtx->ClearRenderTargetView(fill.rtv.Get(), zero);
      gCtx->OMSetRenderTargets(1, fill.rtv.GetAddressOf(), nullptr);
      SetViewport(0, 0, W, H);
      gHider.Fill(src, cam, now, 1.5f, 0);
    };
    gHider.UploadMask(nullptr, 0, 0, 0, 0, 0, 0, 0);
    run(srvA.Get(), &ca, t0 + 1.0f);  // frame A: nothing hidden, everything is remembered
    std::vector<uint8_t> hide = rect(1300 / 4, 500 / 4, 1700 / 4, 760 / 4);
    gHider.UploadMask(&hide, 0, 0, mw, mh, 0, 0, 1);
    const int probes[4][2] = {{1500, 650}, {1350, 560}, {1650, 720}, {1450, 700}};
    auto error = [&]() {
      float e = 0;
      for (auto& pr : probes) {
        float got[4], want[3];
        ReadPixel(fill.tex.Get(), pr[0], pr[1], got);
        WorldAt(RB, K, pr[0] + 0.5f, pr[1] + 0.5f, want);
        e += (fabsf(got[0] - want[0]) + fabsf(got[1] - want[1]) + fabsf(got[2] - want[2])) / 3;
      }
      return e / 4;
    };
    run(srvB.Get(), &cb, t0 + 1.02f);  // frame B: turned right, the red block hides part of what A saw
    SavePNG(fill.tex.Get(), gRoot + "/native/smoke_pano.png");
    float ePano = error();
    run(srvB.Get(), nullptr, t0 + 1.03f);
    float ePull = error();
    run(srvB.Get(), &cb, t0 + 10.f);
    float eOld = error();
    Log("memoria do fundo: erro %.3f, so borrao %.3f, memoria vencida %.3f", ePano, ePull, eOld);
    check(ePano < 0.05f && ePull > ePano * 2, "atras da arma aparece o fundo real lembrado");
    check(fabsf(eOld - ePull) < 0.01f, "memoria vencida volta para o borrao");
  }

  // Exercise the NVOFA path with two GPU-resident frames when the device supports it.
  gHider.RecordHistory(screenSRV.Get(), true);
  gHider.RecordHistory(screenSRV.Get(), true);
  Log(failures ? "SMOKE: %d falha(s)" : "SMOKE: tudo ok", failures);
  return failures ? 1 : 0;
}

// ================= main
static std::string FindRoot() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring dir(exe);
  for (int i = 0; i < 6; i++) {
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    if (GetFileAttributesW((dir + L"\\native\\skins.tsv").c_str()) != INVALID_FILE_ATTRIBUTES) {
      int n = WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
      std::string s(n - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, dir.c_str(), -1, s.data(), n, nullptr, nullptr);
      return s;
    }
  }
  return "";
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
  gSmoke = wcsstr(cmd, L"--smoke") != nullptr;
  gTestOverlay = wcsstr(cmd, L"--test-overlay") != nullptr;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  gQpcFreq = (double)f.QuadPart;
  HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  gRoot = FindRoot();
  if (gRoot.empty()) Fail("Nao achei a pasta do projeto (native\\skins.tsv) acima do .exe.");
  gLogFile = _wfopen(Widen(gRoot + "/native/vmoverlay.log").c_str(), L"w");

  D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2, D3D11_SDK_VERSION, &gDev, nullptr, &gCtx));
  gPass.Init();
  ModelPassInit();
  gPSView = MakePS(kViewHLSL, "PSView");
  gPSViewFlip = MakePS(kViewHLSL, "PSViewFlip");
  BuildCatalog();
  LoadSkins();
  memcpy(gBind, kBindDefault, sizeof gBind);
  if (gSmoke) return Smoke();

  LoadConfig();
  if (!gCap.Init()) Log("captura indisponivel no inicio; tento de novo ao gravar/ao vivo");
  int sx = gCap.w ? gCap.x : 0, sy = gCap.w ? gCap.y : 0;
  int sw = gCap.w ? gCap.w : GetSystemMetrics(SM_CXSCREEN), sh = gCap.w ? gCap.h : GetSystemMetrics(SM_CYSCREEN);
  gHider.Init(sw, sh);
  LoadLibrary();
  std::thread(InputThread).detach();

  // overlay: topmost, click-through, never activated, invisible to screen capture (so it never sees itself)
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"vmoverlay";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
                              wc.lpszClassName, L"Viewmodel Overlay", WS_POPUP, sx, sy, sw, sh, nullptr, nullptr, inst, nullptr);
  if (!hwnd) Fail("CreateWindowEx falhou %lu", GetLastError());
  SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
  if (!gTestOverlay && !SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) Log("WDA_EXCLUDEFROMCAPTURE falhou %lu (Windows 10 2004+?)", GetLastError());

  ComPtr<IDXGIDevice> dxgiDev;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  HR(gDev->QueryInterface(IID_PPV_ARGS(&dxgiDev)));
  HR(dxgiDev->GetAdapter(&adapter));
  HR(adapter->GetParent(IID_PPV_ARGS(&factory)));
  DXGI_SWAP_CHAIN_DESC1 scd{};
  scd.Width = sw; scd.Height = sh; scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; scd.SampleDesc.Count = 1;
  scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; scd.BufferCount = 2; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
  ComPtr<IDXGISwapChain1> swap;
  ComPtr<IDXGISwapChain2> swap2;
  HR(factory->CreateSwapChainForComposition(gDev, &scd, nullptr, &swap));
  HR(swap.As(&swap2));
  HR(swap2->SetMaximumFrameLatency(1));
  HANDLE frameWait = swap2->GetFrameLatencyWaitableObject();
  double refresh = 1 / 60.0;
  DWM_TIMING_INFO ti{sizeof ti};
  if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod) refresh = QpcToSec((int64_t)ti.qpcRefreshPeriod);
  Log("monitor: %dx%d @ %.1f Hz", sw, sh, 1 / refresh);
  ComPtr<IDCompositionDevice> dcomp;
  ComPtr<IDCompositionTarget> target;
  ComPtr<IDCompositionVisual> visual;
  HR(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&dcomp)));
  HR(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target));
  HR(dcomp->CreateVisual(&visual));
  HR(visual->SetContent(swap.Get()));
  HR(target->SetRoot(visual.Get()));
  HR(dcomp->Commit());
  ComPtr<ID3D11Texture2D> back;
  ComPtr<ID3D11RenderTargetView> backRTV;
  HR(swap->GetBuffer(0, IID_PPV_ARGS(&back)));
  HR(gDev->CreateRenderTargetView(back.Get(), nullptr, &backRTV));
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);

  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui::GetIO().FontGlobalScale = sh >= 1400 ? 1.6f : 1.25f;
  ImGui::StyleColorsDark();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(gDev, gCtx);

  if (gTestOverlay) ToggleMenu(hwnd);
  double last = Now(), started = last, lastPresent = 0;
  gStart = last;
  bool uploaded = true;  // the GPU mask may hold something: clear it once when nothing is shown
  while (!gQuit) {
    // recording paces on new desktop frames so none is skipped; everything else paces on the swapchain
    bool dupPace = !gMenu && gCap.ok && gRecording;
    if (!dupPace) WaitForSingleObjectEx(frameWait, 100, TRUE);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) gQuit = true;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    double now = Now();
    if (gTestOverlay && now - started > 4) gQuit = true;
    float dt = (float)std::min(now - last, 0.05);
    last = now;
    ProcessInput(hwnd);
    UpdateMotion(dt, gMenu);
    WeaponRT* w = Current();
    bool inGame = gMenu || !cfg.onlyGame || GameFocused();

    double ft = 0;
    bool fresh = (cfg.live || gRecording) && gCap.Acquire(&ft, dupPace ? 25 : 0);
    now = Now();
    if (fresh) {
      if (gHaveFrame) gCapDt = std::clamp(ft - gFrameT, 1 / 500.0, 0.05);  // omega is per this interval
      gHaveFrame = true;
      gFrameT = ft;
      gCapRate.fresh++;
    }
    if (now - gCapRate.t0 >= 0.5) { gCapRate.fps = (float)(gCapRate.fresh / (now - gCapRate.t0)); gCapRate.fresh = 0; gCapRate.t0 = now; }
    if (gRecording) {
      // takes keep every displayed frame; idle footage is thinned to ~30/s (it only feeds samples and unions)
      if (fresh && gSlot == gRec.slot && ft - gLastRec >= (gRec.InTake(ft) ? refresh * 0.9 : 1 / 30.0)) {
        uint8_t state = StateAtCapture(ft);
        gHider.ChromaQueue(gCap.Screen(), cfg.greenTol, {ft, true, (state & S_MOVE) != 0, {}, state});
        gLastRec = ft;
      }
      gHider.ChromaPoll(gRec.frames, false);
      if (gRec.frames.size() >= 20000) { ToggleRecord(); gStatus = "Parou sozinha no limite de 20000 frames. " + gStatus; }
    }

    // Use the fresh capture to refine the pressed action's phase before uploading its mask.
    bool active = cfg.live && !gRecording && inGame && gSlot == ActiveSlot();
    const std::vector<uint8_t>* mask = active ? LiveMask(now + refresh) : nullptr;  // drawn now, seen one refresh later
    bool useAI = active && cfg.aiMask && gMaskAI.Ready();
    bool analyze = fresh && mask && (cfg.pano || cfg.autoLight || cfg.visualSync || useAI);
    if (analyze) gHider.Analyze(gCap.Screen(), gLuma4);
    if (analyze && cfg.visualSync && gCur.take) {
      TrackMaskPhase(ft);
      mask = LiveMask(now + refresh);
    }
    if (useAI && mask) {
      if (analyze) gMaskAI.Submit(gLuma4, gHider.mw, gHider.mh, gLib, cfg.mirror != gLib.left, ft);
      double resultTime;
      if (gMaskAI.Take(gAIMaskBits, resultTime)) gAIMaskTime = resultTime;
      if (!gAIMaskBits.empty() && now - gAIMaskTime < 0.06) mask = &gAIMaskBits;
    }
    if (mask || uploaded || gMaskDirty) {
      int dilate = cfg.dilate + (Moving() ? cfg.moveDilate : 0);
      gHider.fp = cfg.fill;
      int rx = gLib.rx, ry = gLib.ry, rw = gLib.rw, rh = gLib.rh;
      if (mask == &gAIMaskBits) gMaskAI.Region(rx, ry, rw, rh);
      gHider.UploadMask(mask, rx, ry, rw, rh, 0, 0, dilate, cfg.mirror != gLib.left);
      uploaded = mask != nullptr;
      gMaskDirty = false;
    }
    if (fresh) { gHider.fp = cfg.fill; gHider.RecordHistory(gCap.Screen(), true); }
    if (analyze && cfg.autoLight) {  // scene light: world pixels only (no viewmodel, no HUD bands), eased over ~0.3 s
      const std::vector<uint8_t>& mp = gHider.MaskPixels();
      int mw = gHider.mw, mh = gHider.mh;
      // mean, then the center of the brightest 8% of the pixels = where the light is
      int hist[256] = {}, n = 0;
      double sum = 0;
      for (int y = mh / 10; y < mh * 9 / 10; y += 2)
        for (int x = 0; x < mw; x += 2) {
          size_t i = (size_t)y * mw + x;
          if (mp[i]) continue;
          hist[gLuma4[i]]++;
          sum += gLuma4[i];
          n++;
        }
      int cut = 255;
      for (int acc = 0; cut > 0 && acc + hist[cut] < n * 8 / 100; cut--) acc += hist[cut];
      double cx = 0, cy = 0, cw = 0;
      for (int y = mh / 10; y < mh * 9 / 10; y += 2)
        for (int x = 0; x < mw; x += 2) {
          size_t i = (size_t)y * mw + x;
          if (mp[i] || gLuma4[i] < cut) continue;
          double wgt = gLuma4[i] - cut + 1;
          cx += x * wgt; cy += y * wgt; cw += wgt;
        }
      float e = std::min(1.f, dt * 3);
      if (n) gScene.bright += ((float)(sum / n / 255) - gScene.bright) * e;
      if (cw > 0) {
        gScene.x += ((float)(cx / cw / mw * 2 - 1) - gScene.x) * e;
        gScene.y += ((float)(1 - cy / cw / mh * 2) - gScene.y) * e;
      }
    }
    float K[4];
    GameIntrinsics(sw, sh, K);
    if (!mask || !cfg.pano) gTracking = false;
    else if (analyze) {  // camera rotation from the world pixels around the viewmodel
      static double prevT;
      if (!gTracking) gCam.Reset(ft);
      WorldValid(gHider.MaskPixels(), gHider.mw, gHider.mh, gValid);
      if (gCam.Update(gLuma4, gValid, gHider.mw, gHider.mh, K, ft) && gTracking) {
        double cx, cy;
        MouseSum(prevT - kInputLag, ft - kInputLag, cx, cy);
        gSens.Add(cx, cy, gCam.omega[1], gCam.omega[0]);
      }
      prevT = ft;
      gTracking = true;
    }

    if (w) Tick(*w, dt);
    bool showModel = w && (gMenu || active);
    int rw, rh;
    D3D11_VIEWPORT rect;
    ViewRect(sw, sh, &rw, &rh, &rect);
    gEnv = showModel && cfg.autoLight && gHaveFrame ? BuildEnv(gCap.Screen()) : nullptr;
    if (showModel) RenderViewmodel(*w, rw, rh);

    float clear[4] = {0, 0, 0, 0};
    gCtx->OMSetRenderTargets(1, backRTV.GetAddressOf(), nullptr);
    gCtx->ClearRenderTargetView(backRTV.Get(), clear);
    SetViewport(0, 0, (float)sw, (float)sh);
    bool recent = gHaveFrame && now - gFrameT < 0.25;  // never leave a frozen game image on screen
    if (mask && recent) {
      PanoCam pc;
      if (gTracking) {
        memcpy(pc.R, gCam.R, sizeof pc.R);
        memcpy(pc.K, K, sizeof pc.K);
        // turn since the capture until this reaches the screen: from the mouse counts once calibrated (exact,
        // flicks included), else at the speed the tracker measured
        float wk[3], Q[9];
        if (cfg.mousePredict && gSens.ok) {
          double cx, cy, vx, vy;
          MouseSum(gFrameT - kInputLag, now, cx, cy);
          MouseSum(now - 0.01, now, vx, vy);  // last 10 ms of motion carries on until the refresh
          double ahead = std::max(0.0, refresh - kInputLag) / 0.01;
          cx += vx * ahead; cy += vy * ahead;
          wk[0] = (float)(gSens.ky * cy); wk[1] = (float)(gSens.kx * cx); wk[2] = 0;
        } else {
          float k = (float)std::clamp((now + refresh - gFrameT) / gCapDt, 0.0, 4.0);
          for (int i = 0; i < 3; i++) wk[i] = gCam.omega[i] * k;
        }
        RotVec(wk, Q);
        for (int i = 0; i < 3; i++)
          for (int j = 0; j < 3; j++) pc.show[i * 3 + j] = gCam.R[i * 3] * Q[j] + gCam.R[i * 3 + 1] * Q[3 + j] + gCam.R[i * 3 + 2] * Q[6 + j];
      }
      gHider.fp = cfg.fill;
      float age = cfg.panoAge * (Moving() ? 0.5f : 1.f);  // walking shifts near walls (parallax): trust old memory less
      gHider.Fill(gCap.Screen(), gTracking ? &pc : nullptr, (float)(gFrameT - gStart), age, (float)(gCam.lost - gStart));
    }
    if (cfg.preview && mask) gHider.Preview();
    if (showModel) {
      gCtx->RSSetViewports(1, &rect);
      gPass.Draw((cfg.mirror ? gPSViewFlip : gPSView).Get(), {gViewRes.srv.Get()}, gPass.premulOver.Get());
      SetViewport(0, 0, (float)sw, (float)sh);
    }
    if (gMenu || gRecording) {
      ImGui_ImplDX11_NewFrame();
      ImGui_ImplWin32_NewFrame();
      ImGui::NewFrame();
      if (gMenu) Menu(w);
      else RecBadge();
      ImGui::Render();
      gCtx->OMSetRenderTargets(1, backRTV.GetAddressOf(), nullptr);
      ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    if (!dupPace) swap->Present(1, 0);
    else if (fresh || now - lastPresent > 0.1) {
      swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
      lastPresent = now;
    }
  }
  SaveConfig();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  return 0;
}
