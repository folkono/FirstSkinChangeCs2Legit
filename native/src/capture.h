#pragma once
#include "render.h"

enum Action { A_IDLE, A_ATTACK1, A_ATTACK2, A_INSPECT, A_DRAW, A_RELOAD, A_JUMP, A_COUNT };
enum FrameState : uint8_t { S_MOVE = 1, S_RIGHT = 2, S_WALK = 4, S_CROUCH = 8, S_UNKNOWN = 128 };
extern const char* kActionNames[A_COUNT];
extern const float kMaxDur[A_COUNT];  // longest a take of each action may run

// ---- mask library. Masks are 1 bit at 1/4 of the screen, grayscale thumbnails at 1/8, both cropped to the
// region the viewmodel ever covered (roi, in mask pixels, even-aligned so the thumbnail crop lines up)
struct MaskFrame { float t = 0; std::vector<uint8_t> bits, luma; uint8_t state = S_UNKNOWN; };
// a take = what one key press made the viewmodel do, plus the context it was pressed in: the game picks a
// different animation for the same key after another action, mid-swing, or as the 2nd hit of a chain (other hand)
struct Take {
  std::vector<MaskFrame> frames;
  int prev = -1;      // action pressed before it (-1: none)
  float gap = 99;     // seconds since that press
  int combo = 0;      // presses of this same action in a row before it (odd/even = which hand swings)
  bool cut = false;   // the next press interrupted it
  float Duration() const { return frames.empty() ? 0 : frames.back().t; }
  const MaskFrame* At(float t) const;  // last frame at or before t
  uint8_t StateAt(float t) const;
  // OR every frame shown during [t0, t1] into bits; false once t0 is past the take
  bool OrInto(float t0, float t1, std::vector<uint8_t>& bits) const;
};
struct Library {
  int w = 0, h = 0;                    // full mask size (screen / 4)
  int rx = 0, ry = 0, rw = 0, rh = 0;  // roi
  char slot = '3';                     // weapon slot key the masks belong to
  bool left = false;                   // recorded with the viewmodel mirrored (left hand)
  std::vector<uint8_t> idle, move, idleRight, moveRight; // fallback masks by movement and held secondary button
  std::vector<Take> takes[A_COUNT];    // takes[A_IDLE]: idle samples
  int LW() const { return rw / 2; }
  int LH() const { return rh / 2; }
  bool Empty() const { return idle.empty(); }
  bool Save(const std::string& path) const;
  bool Load(const std::string& path);
  // take of action a closest to this context, at least minDur long (nullptr: none)
  const Take* Pick(int a, int prev, float gap, int combo, float minDur = 0, uint8_t state = S_UNKNOWN,
                   float at = 0, const Take* prefer = nullptr) const;
};

// context of a press from the one before it (jumps do not count: they never interrupt the weapon)
struct PressCtx { int prev = -1; float gap = 99; int combo = 0; double t = -1e9; int a = -1; };
PressCtx NextPress(const PressCtx& last, Action a, double t);

// recorded frames are kept compressed (mask bits + thumbnail): minutes of footage fit in a few hundred MB
void PackFrame(const std::vector<uint8_t>& bits, const std::vector<uint8_t>& luma, std::vector<uint8_t>& out);
bool UnpackFrame(const std::vector<uint8_t>& in, size_t nb, size_t nl, std::vector<uint8_t>& bits, std::vector<uint8_t>& luma);

struct Recorder {
  struct RawFrame { double t = 0; bool active = false, moving = false; std::vector<uint8_t> packed; uint8_t state = S_UNKNOWN; };
  struct RawInput { double t; Action a; };
  bool left = false;
  std::vector<RawFrame> frames;
  std::vector<RawInput> inputs;
  char slot = '3';
  void Clear() { frames.clear(); inputs.clear(); }
  bool InTake(double t) const;  // some input's take is still running at t
  Library Build(int w, int h) const;
};

// keeps only what hangs off the arms (pieces touching the bottom/lower sides, or close to them) and fills holes
void CleanMask(std::vector<uint8_t>& bits, int w, int h, const std::vector<uint8_t>* previous = nullptr);

// ---- camera tracking from the screen alone: frame-to-frame rotation (3 axes) by coarse search + Gauss-Newton
// on a pyramid of the luma, pixels of the viewmodel and HUD left out
extern const float kHudTop, kHudBottom, kCrossFrac;  // screen bands that are not the world
void WorldValid(const std::vector<uint8_t>& mask, int w, int h, std::vector<uint8_t>& valid);
void RotVec(const float w[3], float R[9]);  // axis-angle -> rotation matrix (row major)
struct CamTracker {
  float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // camera -> world; world = the camera when tracking started
  float omega[3] = {};                        // last frame-to-frame rotation, camera axes (x right, y up, z forward)
  double lost = -1e9;                         // background memory from before this is invalid
  bool ok = false;
  float err = 0;  // mean luma residual of the last update
  void Reset(double now);
  // luma/valid: whole screen at 1/4 (w x h). K: fx fy cx cy in screen pixels
  bool Update(const std::vector<uint8_t>& luma, const std::vector<uint8_t>& valid, int w, int h, const float K[4], double now);
  struct Level { int w = 0, h = 0, d = 1; std::vector<float> I, gx, gy; std::vector<uint8_t> v; };
 private:
  Level prev[3], cur[3];  // screen / 8, / 16, / 32
  bool havePrev = false;
};

// ---- DXGI desktop duplication (GPU copy, present timestamps on the QPC clock)
struct ScreenCapture {
  bool ok = false;
  int x = 0, y = 0, w = 0, h = 0;  // output rect in desktop coordinates
  int fresh = 0, stale = 0, waits = 0, losts = 0;  // Acquire outcomes (diagnostics)
  HRESULT lastErr = S_OK;
  bool Init();
  bool Acquire(double* presentTime, UINT timeoutMs = 0);  // true when a new desktop image landed in Screen()
  ID3D11ShaderResourceView* Screen() const { return screenSRV.Get(); }
 private:
  ComPtr<IDXGIOutputDuplication> dup;
  ComPtr<ID3D11Texture2D> screen;
  ComPtr<ID3D11ShaderResourceView> screenSRV;
};

// ---- GPU passes that hide the real viewmodel
struct PanoCam {
  float R[9];     // camera -> world of the captured frame (writes the background memory)
  float show[9];  // camera -> world of the frame on screen when drawn (reads it)
  float K[4];     // fx fy cx cy, screen pixels
};
struct FillParams {        // how the hidden area is rebuilt (menu sliders)
  bool optical = true;      // NVOFA history reconstruction before the existing fill
  int opticalFrames = 2;   // previous frames consulted by NVOFA (1..4)
  float texture = 1;       // strength of the texture carried in from the edge (0 = plain blur)
  float reach = 140;       // screen pixels into the hole before that texture fades out
  float sharp = 2;         // blur base: 0 smoothest .. 3 sharpest (finest pull-push level used)
  float grain = 2;         // texture scale: blur level removed from it (1 fine .. 4 coarse)
  int feather = 2;         // soft edge, mask pixels (x4 screen pixels)
  float seam = 0.55f;      // where two edges meet: higher fades the texture more (no hard seam)
  float patch = 1;         // PatchMatch (content-aware fill): 0 off .. 1 full
  int quality = 2;         // 1 light .. 4 heavy: patch size, steps and random tries per frame
  bool colorFix = true;    // copies keep the local colour, only bring texture
};
struct Hider {
  ~Hider();
  FillParams fp;
  int mw = 0, mh = 0;  // mask resolution (screen / 4); thumbnails are half of it
  void Init(int screenW, int screenH);
  // recording: chroma key (non-green -> 1) + thumbnail on the GPU, read back a few frames later without stalling
  void ChromaQueue(ID3D11ShaderResourceView* screen, float tolerance, const Recorder::RawFrame& meta);
  void ChromaPoll(std::vector<Recorder::RawFrame>& out, bool flush);
  // live: luma of this very frame at mask resolution (waits for the GPU, ~0.2 ms)
  void Analyze(ID3D11ShaderResourceView* screen, std::vector<uint8_t>& luma);
  // bits: roi-sized mask placed at (rx + dx, ry + dy), mirrored left-right when asked; nullptr clears
  void UploadMask(const std::vector<uint8_t>* bits, int rx, int ry, int rw, int rh, int dx, int dy, int dilate, bool mirror = false);
  const std::vector<uint8_t>& MaskPixels() const { return scratch; }  // what was uploaded, 0/255 per mask pixel
  // into the bound target, where the mask is: the remembered real background (cam != null) over a pull-push fill.
  // Times are seconds on any clock shared by the calls.
  void Fill(ID3D11ShaderResourceView* screen, const PanoCam* cam, float now, float maxAge, float lost);
  void RecordHistory(ID3D11ShaderResourceView* screen, bool fresh);
  bool OpticalAvailable() const { return optical != nullptr; }
  void Preview();  // red tint of the live mask
 private:
  struct OpticalHistory;
  OpticalHistory* optical = nullptr;
  static constexpr int kRing = 6;
  static constexpr int kPanoW = 8192, kPanoH = 4096;  // equirectangular background memory (~1.5x screen detail)
  struct Pending { int slot; Recorder::RawFrame meta; };
  RT holed, maskRT, panoC, panoT, guide, nnf[2];  // nnf: per hole pixel, the matching spot outside (x, y, valid)
  int nnfCur = 0, pmFrame = 0;
  ComPtr<ID3D11Texture2D> maskTex, ring[kRing], liveStage;
  std::vector<Pending> pending;           // oldest first
  std::vector<Recorder::RawFrame> ready;  // read back early because the ring was full
  int head = 0, sw = 0, sh = 0;
  ComPtr<ID3D11ShaderResourceView> maskSRV, nearSRV;
  ComPtr<ID3D11Texture2D> nearTex;  // per mask pixel: offset to the closest pixel outside the mask
  std::vector<int16_t> nearBuf;
  ComPtr<ID3D11PixelShader> psHole, psFill, psChroma, psPreview, psPano, psGuide, psPatch;
  ComPtr<ID3D11Buffer> cb, cbPano;
  std::vector<uint8_t> scratch, lumaFull, bitsTmp, thumbTmp;
};

void UnpackBits(const std::vector<uint8_t>& bits, int w, int h, std::vector<uint8_t>& out);
void PackBits(const std::vector<uint8_t>& px, std::vector<uint8_t>& bits);
void Dilate(std::vector<uint8_t>& px, int w, int h, int r);
void Half(const std::vector<uint8_t>& src, int w, int h, std::vector<uint8_t>& dst);  // 2x2 average, rounds up
