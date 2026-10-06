#include "capture.h"
#include "../third_party/nvof/nvOpticalFlowD3D11.h"
#include <compressapi.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <execution>
#include <numeric>

const char* kActionNames[A_COUNT] = {"idle", "ataque 1", "ataque 2", "inspecionar", "sacar", "recarregar", "pular"};
const float kMaxDur[A_COUNT] = {0, 1.2f, 1.5f, 7.0f, 1.6f, 3.5f, 1.3f};

const MaskFrame* Take::At(float t) const {
  if (frames.empty() || t < 0 || t > Duration()) return nullptr;
  auto it = std::upper_bound(frames.begin(), frames.end(), t, [](float v, const MaskFrame& f) { return v < f.t; });
  return it == frames.begin() ? &frames.front() : &*(it - 1);
}

uint8_t Take::StateAt(float t) const {
  if (frames.empty()) return S_UNKNOWN;
  auto it = std::upper_bound(frames.begin(), frames.end(), t, [](float v, const MaskFrame& f) { return v < f.t; });
  return (it == frames.begin() ? frames.front() : *(it - 1)).state;
}

bool Take::OrInto(float t0, float t1, std::vector<uint8_t>& bits) const {
  if (frames.empty() || t0 > Duration()) return false;
  auto orf = [&](const MaskFrame& f) { for (size_t k = 0; k < bits.size() && k < f.bits.size(); k++) bits[k] |= f.bits[k]; };
  if (const MaskFrame* a = At(std::max(t0, 0.f))) orf(*a);
  for (const MaskFrame& f : frames) if (f.t > t0 && f.t <= t1) orf(f);
  return true;
}

// ---- Windows compression API (cabinet.dll): XPRESS for frames while recording, XPRESS + Huffman for files
static bool Squeeze(const void* src, size_t n, std::vector<uint8_t>& out, DWORD algo) {
  COMPRESSOR_HANDLE c = nullptr;
  if (!CreateCompressor(algo, nullptr, &c)) return false;
  SIZE_T need = 0;
  Compress(c, src, n, nullptr, 0, &need);  // reports the size it needs
  out.resize(need);
  bool ok = need && Compress(c, src, n, out.data(), need, &need);
  out.resize(ok ? need : 0);
  CloseCompressor(c);
  return ok;
}

static bool Unsqueeze(const uint8_t* src, size_t n, void* dst, size_t want, DWORD algo) {
  DECOMPRESSOR_HANDLE d = nullptr;
  if (!CreateDecompressor(algo, nullptr, &d)) return false;
  SIZE_T got = 0;
  bool ok = Decompress(d, src, n, dst, want, &got) && got == want;
  CloseDecompressor(d);
  return ok;
}

void PackFrame(const std::vector<uint8_t>& bits, const std::vector<uint8_t>& luma, std::vector<uint8_t>& out) {
  std::vector<uint8_t> raw(bits);
  raw.insert(raw.end(), luma.begin(), luma.end());
  if (!Squeeze(raw.data(), raw.size(), out, COMPRESS_ALGORITHM_XPRESS)) out.clear();
}

bool UnpackFrame(const std::vector<uint8_t>& in, size_t nb, size_t nl, std::vector<uint8_t>& bits, std::vector<uint8_t>& luma) {
  std::vector<uint8_t> raw(nb + nl);
  if (in.empty() || !Unsqueeze(in.data(), in.size(), raw.data(), raw.size(), COMPRESS_ALGORITHM_XPRESS)) return false;
  bits.assign(raw.begin(), raw.begin() + nb);
  luma.assign(raw.begin() + nb, raw.end());
  return true;
}

// ---- library file: "VMLB" v7 | raw size | XPRESS+Huffman of: w h roi slot | idle variants | takes -> frames
namespace {
struct Writer {
  std::vector<uint8_t> b;
  void Put(const void* p, size_t n) { auto c = (const uint8_t*)p; b.insert(b.end(), c, c + n); }
  template <class T> void V(T v) { Put(&v, sizeof v); }
  void Fixed(const std::vector<uint8_t>& v, size_t n) {  // exactly n bytes, zero padded
    size_t k = std::min(n, v.size());
    Put(v.data(), k);
    b.resize(b.size() + n - k, 0);
  }
};
struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  void Get(void* d, size_t k) {
    if (!ok || at + k > n) { ok = false; memset(d, 0, k); return; }
    memcpy(d, p + at, k);
    at += k;
  }
  template <class T> T V() { T v{}; Get(&v, sizeof v); return v; }
  std::vector<uint8_t> Bytes(size_t k) { std::vector<uint8_t> v(ok && at + k <= n ? k : 0); if (v.size() == k) Get(v.data(), k); else ok = false; return v; }
};
}  // namespace

bool Library::Save(const std::string& path) const {
  Writer o;
  for (int v : {w, h, rx, ry, rw, rh}) o.V(v);
  o.V(slot);
  o.V((char)left);
  size_t nb = ((size_t)rw * rh + 7) / 8, nl = (size_t)LW() * LH();
  o.Fixed(idle, nb);
  o.Fixed(move.size() == nb ? move : idle, nb);
  o.Fixed(idleRight.size() == nb ? idleRight : idle, nb);
  o.Fixed(moveRight.size() == nb ? moveRight : move, nb);
  for (int a = 0; a < A_COUNT; a++) {
    o.V((uint32_t)takes[a].size());
    for (const Take& t : takes[a]) {
      o.V(t.prev); o.V(t.gap); o.V(t.combo); o.V((char)t.cut);
      o.V((uint32_t)t.frames.size());
      for (const MaskFrame& m : t.frames) { o.V(m.t); o.V(m.state); o.Fixed(m.bits, nb); o.Fixed(m.luma, nl); }
    }
  }
  std::vector<uint8_t> z;
  if (!Squeeze(o.b.data(), o.b.size(), z, COMPRESS_ALGORITHM_XPRESS_HUFF)) return false;
  FILE* f = _wfopen(Widen(path).c_str(), L"wb");
  if (!f) return false;
  uint32_t ver = 7;
  uint64_t raw = o.b.size();
  fwrite("VMLB", 1, 4, f);
  fwrite(&ver, 4, 1, f);
  fwrite(&raw, 8, 1, f);
  fwrite(z.data(), 1, z.size(), f);
  bool ok = !ferror(f);
  fclose(f);
  return ok;
}

bool Library::Load(const std::string& path) {
  FILE* f = _wfopen(Widen(path).c_str(), L"rb");
  if (!f) return false;
  char magic[4];
  uint32_t ver = 0;
  uint64_t raw = 0;
  bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "VMLB", 4) && fread(&ver, 4, 1, f) == 1 && (ver == 6 || ver == 7) &&
            fread(&raw, 8, 1, f) == 1 && raw > 0 && raw < (1ull << 34);
  std::vector<uint8_t> z;
  uint8_t buf[1 << 16];
  for (size_t k; ok && (k = fread(buf, 1, sizeof buf, f)) > 0;) z.insert(z.end(), buf, buf + k);
  fclose(f);
  if (!ok) return false;  // older versions have no press context: record again
  std::vector<uint8_t> b((size_t)raw);
  if (!Unsqueeze(z.data(), z.size(), b.data(), b.size(), COMPRESS_ALGORITHM_XPRESS_HUFF)) return false;
  Reader r{b.data(), b.size()};
  Library L;
  L.w = r.V<int>(); L.h = r.V<int>(); L.rx = r.V<int>(); L.ry = r.V<int>(); L.rw = r.V<int>(); L.rh = r.V<int>();
  L.slot = r.V<char>();
  L.left = r.V<char>() != 0;
  if (!r.ok || L.w <= 0 || L.h <= 0 || L.rw <= 0 || L.rh <= 0 || L.rx < 0 || L.ry < 0 || L.rx + L.rw > L.w || L.ry + L.rh > L.h) return false;
  size_t nb = ((size_t)L.rw * L.rh + 7) / 8, nl = (size_t)L.LW() * L.LH();
  L.idle = r.Bytes(nb);
  L.move = r.Bytes(nb);
  if (ver >= 7) { L.idleRight = r.Bytes(nb); L.moveRight = r.Bytes(nb); }
  else { L.idleRight = L.idle; L.moveRight = L.move; }
  for (int a = 0; r.ok && a < A_COUNT; a++) {
    uint32_t nt = r.V<uint32_t>();
    if (nt > 10000) return false;
    for (uint32_t i = 0; r.ok && i < nt; i++) {
      Take t;
      t.prev = r.V<int>(); t.gap = r.V<float>(); t.combo = r.V<int>(); t.cut = r.V<char>() != 0;
      uint32_t nf = r.V<uint32_t>();
      if (nf > 1000000) return false;
      for (uint32_t k = 0; r.ok && k < nf; k++) {
        MaskFrame m;
        m.t = r.V<float>();
        if (ver >= 7) m.state = r.V<uint8_t>();
        m.bits = r.Bytes(nb);
        m.luma = r.Bytes(nl);
        t.frames.push_back(std::move(m));
      }
      L.takes[a].push_back(std::move(t));
    }
  }
  if (!r.ok) return false;
  *this = std::move(L);
  return true;
}

const Take* Library::Pick(int a, int prev, float gap, int combo, float minDur, uint8_t state, float at, const Take* prefer) const {
  const Take* best = nullptr;
  float bestCost = 1e9f;
  for (const Take& t : takes[a]) {
    if (t.frames.empty() || t.Duration() + 0.02f < minDur) continue;
    // the hand matters most, then what came before and how long ago
    float c = ((t.combo & 1) != (combo & 1) ? 8.f : 0) + (t.prev != prev ? 4.f : 0) +
              std::min(fabsf(std::min(t.gap, 3.f) - std::min(gap, 3.f)), 3.f) + (t.cut ? 0.5f : 0);
    uint8_t recorded = t.StateAt(at);
    if (!(state & S_UNKNOWN) && !(recorded & S_UNKNOWN)) {
      uint8_t diff = state ^ recorded;
      if (diff & S_RIGHT) c += 6.f;
      if (diff & S_MOVE) c += 3.f;
      if (diff & S_WALK) c += 1.5f;
      if (diff & S_CROUCH) c += 1.5f;
    }
    if (&t == prefer) c -= 0.35f;
    if (c < bestCost) { bestCost = c; best = &t; }
  }
  return best;
}

PressCtx NextPress(const PressCtx& last, Action a, double t) {
  PressCtx p;
  p.a = a;
  p.t = t;
  p.prev = last.a;
  p.gap = (float)std::min(t - last.t, 99.0);
  p.combo = last.a == a && t - last.t < kMaxDur[a] ? last.combo + 1 : 0;
  return p;
}

// ---- mask helpers
void UnpackBits(const std::vector<uint8_t>& bits, int w, int h, std::vector<uint8_t>& out) {
  out.resize((size_t)w * h);
  for (size_t i = 0; i < out.size(); i++) out[i] = (bits[i >> 3] >> (i & 7)) & 1 ? 255 : 0;
}

void PackBits(const std::vector<uint8_t>& px, std::vector<uint8_t>& bits) {
  bits.assign((px.size() + 7) / 8, 0);
  for (size_t i = 0; i < px.size(); i++) if (px[i]) bits[i >> 3] |= 1 << (i & 7);
}

void Dilate(std::vector<uint8_t>& px, int w, int h, int r) {  // separable square max filter
  if (r <= 0) return;
  std::vector<uint8_t> tmp(px.size());
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t m = 0;
      for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r) && !m; k++) m = px[y * w + k];
      tmp[y * w + x] = m;
    }
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint8_t m = 0;
      for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r) && !m; k++) m = tmp[k * w + x];
      px[y * w + x] = m;
    }
}

void Half(const std::vector<uint8_t>& src, int w, int h, std::vector<uint8_t>& dst) {
  int hw = (w + 1) / 2, hh = (h + 1) / 2;
  dst.resize((size_t)hw * hh);
  for (int y = 0; y < hh; y++)
    for (int x = 0; x < hw; x++) {
      int s = 0, n = 0;
      for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
          int sx = x * 2 + dx, sy = y * 2 + dy;
          if (sx < w && sy < h) { s += src[(size_t)sy * w + sx]; n++; }
        }
      dst[(size_t)y * hw + x] = (uint8_t)(s / n);
    }
}

void CleanMask(std::vector<uint8_t>& bits, int w, int h, const std::vector<uint8_t>* previous) {
  std::vector<uint8_t> px;
  UnpackBits(bits, w, h, px);
  // 1) 8-connected pieces; arms enter from the bottom (or the lower side edges)
  std::vector<int> comp(px.size(), -1), stack;
  std::vector<char> keep;
  for (int i = 0; i < w * h; i++) {
    if (!px[i] || comp[i] >= 0) continue;
    int id = (int)keep.size();
    bool anchored = false;
    comp[i] = id;
    stack.push_back(i);
    while (!stack.empty()) {
      int j = stack.back();
      stack.pop_back();
      int x = j % w, y = j / w;
      if (y == h - 1 || ((x == 0 || x == w - 1) && y > h * 2 / 5)) anchored = true;
      for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
          int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          int k = ny * w + nx;
          if (px[k] && comp[k] < 0) { comp[k] = id; stack.push_back(k); }
        }
    }
    keep.push_back(anchored);
  }
  // 2) pieces a few pixels from the arms stay as well (knife leaving the hand mid-flip)
  std::vector<uint8_t> reach(px.size());
  for (size_t i = 0; i < px.size(); i++) reach[i] = comp[i] >= 0 && keep[comp[i]] ? 255 : 0;
  Dilate(reach, w, h, 8);
  if (previous && previous->size() == px.size()) {
    std::vector<uint8_t> tracked = *previous;
    Dilate(tracked, w, h, 12);  // recover a detached piece near its position in the preceding green-screen frame
    for (size_t i = 0; i < px.size(); i++) reach[i] |= tracked[i];
  }
  std::vector<char> keep2 = keep;
  for (size_t i = 0; i < px.size(); i++) if (comp[i] >= 0 && reach[i]) keep2[comp[i]] = 1;
  for (size_t i = 0; i < px.size(); i++) px[i] = comp[i] >= 0 && keep2[comp[i]] ? 255 : 0;
  // 3) holes (green reflected on metal) = background the border cannot reach
  std::vector<uint8_t> outside(px.size(), 0);
  for (int x = 0; x < w; x++) for (int y : {0, h - 1}) { int i = y * w + x; if (!px[i] && !outside[i]) { outside[i] = 1; stack.push_back(i); } }
  for (int y = 0; y < h; y++) for (int x : {0, w - 1}) { int i = y * w + x; if (!px[i] && !outside[i]) { outside[i] = 1; stack.push_back(i); } }
  while (!stack.empty()) {
    int j = stack.back();
    stack.pop_back();
    int x = j % w, y = j / w;
    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (auto& d : nb) {
      int nx = x + d[0], ny = y + d[1];
      if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
      int k = ny * w + nx;
      if (!px[k] && !outside[k]) { outside[k] = 1; stack.push_back(k); }
    }
  }
  for (size_t i = 0; i < px.size(); i++) if (!px[i] && !outside[i]) px[i] = 255;
  PackBits(px, bits);
}

// pixels present in at least minFrac of the frames: one-off junk does not grow the fallback masks
static std::vector<uint8_t> Frequent(const std::vector<const std::vector<uint8_t>*>& fs, int w, int h, float minFrac) {
  std::vector<uint16_t> count((size_t)w * h, 0);
  for (const auto* b : fs)
    for (size_t i = 0; i < count.size(); i++) count[i] += ((*b)[i >> 3] >> (i & 7)) & 1;
  uint16_t need = (uint16_t)std::max(1.f, ceilf(minFrac * fs.size()));
  std::vector<uint8_t> px(count.size());
  for (size_t i = 0; i < px.size(); i++) px[i] = !fs.empty() && count[i] >= need ? 255 : 0;
  std::vector<uint8_t> bits;
  PackBits(px, bits);
  return bits;
}

bool Recorder::InTake(double t) const {
  for (const RawInput& in : inputs) if (t >= in.t && t < in.t + kMaxDur[in.a]) return true;
  return false;
}

// Takes: each input owns the active frames until the next input or its max length; takes cut short by another
// input only count when nothing clean exists for that action. Idle: an even spread of the frames outside every take.
// Every used frame is cleaned, then all of them are cropped to the region they cover.
Library Recorder::Build(int w, int h) const {
  Library L;
  L.w = w; L.h = h; L.slot = slot; L.left = left;
  const size_t nb = ((size_t)w * h + 7) / 8;
  const int fw = (w + 1) / 2, fh = (h + 1) / 2;
  const size_t nl = (size_t)fw * fh;
  // each press owns the frames until the next press on the same track (weapon or jump) or its max length
  struct Plan { int a; double t0; std::vector<int> idx; PressCtx ctx; bool cut; };
  std::vector<Plan> plans;
  PressCtx last;
  for (size_t i = 0; i < inputs.size(); i++) {
    double t = inputs[i].t;
    Action a = inputs[i].a;
    bool jump = a == A_JUMP;
    double next = 1e30;
    for (size_t j = i + 1; j < inputs.size(); j++)
      if ((inputs[j].a == A_JUMP) == jump) { next = inputs[j].t; break; }
    Plan p{a, t, {}, {}, next < t + kMaxDur[a]};
    if (!jump) last = p.ctx = NextPress(last, a, t);
    double end = std::min(t + kMaxDur[a], next);
    for (int k = 0; k < (int)frames.size(); k++)
      if (frames[k].active && frames[k].t >= t && frames[k].t < end) p.idx.push_back(k);
    if (!p.idx.empty()) plans.push_back(std::move(p));
  }
  std::vector<const Plan*> chosen;
  for (const Plan& p : plans) chosen.push_back(&p);
  std::array<std::vector<int>, 4> idleBuckets;
  std::vector<int> samples;
  for (int k = 0; k < (int)frames.size(); k++) if (frames[k].active && !InTake(frames[k].t)) {
    int bucket = (frames[k].moving ? 1 : 0) | ((frames[k].state & S_RIGHT) ? 2 : 0);
    idleBuckets[bucket].push_back(k);
  }
  for (const auto& bucket : idleBuckets) {
    size_t n = std::min<size_t>(bucket.size(), 64);
    for (size_t i = 0; i < n; i++) samples.push_back(bucket[i * bucket.size() / n]);
  }
  std::sort(samples.begin(), samples.end());

  std::vector<int> used = samples;
  for (const Plan* p : chosen) used.insert(used.end(), p->idx.begin(), p->idx.end());
  std::sort(used.begin(), used.end());
  used.erase(std::unique(used.begin(), used.end()), used.end());
  if (used.empty()) return L;  // nothing recorded: stays Empty()
  std::vector<std::vector<uint8_t>> cleaned(frames.size());
  std::vector<std::array<int, 4>> box(frames.size());
  std::for_each(std::execution::par, used.begin(), used.end(), [&](int k) {
    std::vector<uint8_t> b, l;
    if (!UnpackFrame(frames[k].packed, nb, nl, b, l)) { b.assign(nb, 0); l.assign(nl, 0); }
    CleanMask(b, w, h);
    std::array<int, 4> bb{w, h, 0, 0};
    for (size_t i = 0; i < (size_t)w * h; i++)
      if ((b[i >> 3] >> (i & 7)) & 1) {
        int x = (int)(i % w), y = (int)(i / w);
        bb = {std::min(bb[0], x), std::min(bb[1], y), std::max(bb[2], x + 1), std::max(bb[3], y + 1)};
      }
    box[k] = bb;
    PackFrame(b, l, cleaned[k]);
  });
  // The chroma pass can split a fast knife/hand away from the bottom-anchored component.
  // Revisit sudden dropouts using only foreground pixels close to the previous clean frame.
  std::vector<uint8_t> previousBits;
  int previousCount = 0, previousIndex = -1;
  auto pixels = [](const std::vector<uint8_t>& b) {
    int n = 0; for (uint8_t v : b) n += std::popcount((unsigned)v); return n;
  };
  for (int k : used) {
    std::vector<uint8_t> b, l;
    if (!UnpackFrame(cleaned[k], nb, nl, b, l)) continue;
    int count = pixels(b);
    if (previousIndex >= 0 && frames[k].t - frames[previousIndex].t < 0.08 && count < previousCount * 0.9f) {
      std::vector<uint8_t> raw, ignored;
      if (UnpackFrame(frames[k].packed, nb, nl, raw, ignored)) {
        std::vector<uint8_t> previous;
        UnpackBits(previousBits, w, h, previous);
        CleanMask(raw, w, h, &previous);
        if (pixels(raw) > count) {
          b.swap(raw);
          PackFrame(b, l, cleaned[k]);
          std::array<int, 4> bb{w, h, 0, 0};
          for (size_t i = 0; i < (size_t)w * h; i++) if ((b[i >> 3] >> (i & 7)) & 1) {
            int x = (int)(i % w), y = (int)(i / w);
            bb = {std::min(bb[0], x), std::min(bb[1], y), std::max(bb[2], x + 1), std::max(bb[3], y + 1)};
          }
          box[k] = bb;
          count = pixels(b);
        }
      }
    }
    previousBits = std::move(b);
    previousCount = count; previousIndex = k;
  }
  int x0 = w, y0 = h, x1 = 0, y1 = 0;
  for (int k : used) { x0 = std::min(x0, box[k][0]); y0 = std::min(y0, box[k][1]); x1 = std::max(x1, box[k][2]); y1 = std::max(y1, box[k][3]); }
  if (x0 >= x1 || y0 >= y1) { x0 = y0 = 0; x1 = w; y1 = h; }
  x0 = std::max(0, x0 - 4) & ~1;
  y0 = std::max(0, y0 - 4) & ~1;
  L.rx = x0; L.ry = y0;
  L.rw = std::min((std::min(w, x1 + 4) - x0 + 1) & ~1, (w - x0) & ~1);
  L.rh = std::min((std::min(h, y1 + 4) - y0 + 1) & ~1, (h - y0) & ~1);

  auto crop = [&](int k, MaskFrame& m) {
    m.state = frames[k].state;
    std::vector<uint8_t> b, l, px((size_t)L.rw * L.rh);
    if (!UnpackFrame(cleaned[k], nb, nl, b, l)) { b.assign(nb, 0); l.assign(nl, 0); }
    for (int y = 0; y < L.rh; y++)
      for (int x = 0; x < L.rw; x++) {
        size_t i = (size_t)(L.ry + y) * w + L.rx + x;
        px[(size_t)y * L.rw + x] = (b[i >> 3] >> (i & 7)) & 1 ? 255 : 0;
      }
    PackBits(px, m.bits);
    m.luma.resize((size_t)L.LW() * L.LH());
    for (int y = 0; y < L.LH(); y++)
      for (int x = 0; x < L.LW(); x++) m.luma[(size_t)y * L.LW() + x] = l[(size_t)(L.ry / 2 + y) * fw + L.rx / 2 + x];
  };
  for (const Plan* p : chosen) {
    Take t;
    t.prev = p->ctx.prev; t.gap = p->ctx.gap; t.combo = p->ctx.combo; t.cut = p->cut;
    for (int k : p->idx) {
      MaskFrame m;
      m.t = (float)(frames[k].t - p->t0);
      crop(k, m);
      t.frames.push_back(std::move(m));
    }
    L.takes[p->a].push_back(std::move(t));
  }
  Take st;
  for (int k : samples) { MaskFrame m; crop(k, m); st.frames.push_back(std::move(m)); }
  std::vector<const std::vector<uint8_t>*> still, walking, stillRight, walkingRight;
  for (size_t i = 0; i < samples.size(); i++) {
    const auto& f = frames[samples[i]];
    (f.moving ? walking : still).push_back(&st.frames[i].bits);
    if (f.state & S_RIGHT) (f.moving ? walkingRight : stillRight).push_back(&st.frames[i].bits);
  }
  L.idle = Frequent(still.empty() ? walking : still, L.rw, L.rh, 0.03f);
  L.move = Frequent(walking.empty() ? still : walking, L.rw, L.rh, 0.03f);
  L.idleRight = stillRight.empty() ? (walkingRight.empty() ? L.idle : Frequent(walkingRight, L.rw, L.rh, 0.03f))
                                   : Frequent(stillRight, L.rw, L.rh, 0.03f);
  L.moveRight = walkingRight.empty() ? L.move : Frequent(walkingRight, L.rw, L.rh, 0.03f);
  if (walkingRight.empty())
    for (size_t i = 0; i < L.moveRight.size(); ++i) L.moveRight[i] |= L.idleRight[i];
  if (!st.frames.empty()) L.takes[A_IDLE].push_back(std::move(st));
  return L;
}

// ---- camera tracking
// HUD bands (timer/score on top, health/ammo at the bottom) and the crosshair box, as fractions of the screen.
// ponytail: fixed for the default CS2 HUD; make them settings if a custom HUD scale breaks tracking
const float kHudTop = 0.07f, kHudBottom = 0.09f, kCrossFrac = 0.02f;

void WorldValid(const std::vector<uint8_t>& mask, int w, int h, std::vector<uint8_t>& valid) {
  std::vector<uint8_t> m = mask;
  if (m.size() != (size_t)w * h) m.assign((size_t)w * h, 0);
  Dilate(m, w, h, 2);  // viewmodel shading spills a little past the mask
  valid.assign((size_t)w * h, 0);
  int y0 = (int)ceilf(h * kHudTop), y1 = (int)(h * (1 - kHudBottom)), cr = (int)ceilf(w * kCrossFrac);
  for (int y = y0; y < y1; y++)
    for (int x = 0; x < w; x++) {
      size_t i = (size_t)y * w + x;
      if (!m[i] && !(abs(x - w / 2) <= cr && abs(y - h / 2) <= cr)) valid[i] = 255;
    }
}

void RotVec(const float w[3], float R[9]) {
  float th = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
  if (th < 1e-12f) { const float I[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(R, I, sizeof I); return; }
  float x = w[0] / th, y = w[1] / th, z = w[2] / th, c = cosf(th), s = sinf(th), v = 1 - c;
  const float M[9] = {c + x * x * v, x * y * v - z * s, x * z * v + y * s,
                      y * x * v + z * s, c + y * y * v, y * z * v - x * s,
                      z * x * v - y * s, z * y * v + x * s, c + z * z * v};
  memcpy(R, M, sizeof M);
}

static void Mul3(const float A[9], const float B[9], float C[9]) {
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) C[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
}

static void Orthonormalize(float R[9]) {  // rows: x, y re-orthogonalized, z = x cross y
  float* a = R; float* b = R + 3; float* c = R + 6;
  float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  for (int i = 0; i < 3; i++) a[i] /= n;
  float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  for (int i = 0; i < 3; i++) b[i] -= d * a[i];
  n = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
  for (int i = 0; i < 3; i++) b[i] /= n;
  c[0] = a[1] * b[2] - a[2] * b[1]; c[1] = a[2] * b[0] - a[0] * b[2]; c[2] = a[0] * b[1] - a[1] * b[0];
}

using Level = CamTracker::Level;

static void Down(const std::vector<float>& I, const std::vector<uint8_t>& v, int w, int h, int d, Level& L) {
  L.w = w / 2; L.h = h / 2; L.d = d;
  L.I.resize((size_t)L.w * L.h);
  L.v.resize(L.I.size());
  for (int y = 0; y < L.h; y++)
    for (int x = 0; x < L.w; x++) {
      size_t i = (size_t)y * 2 * w + x * 2, o = (size_t)y * L.w + x;
      L.I[o] = (I[i] + I[i + 1] + I[i + w] + I[i + w + 1]) * 0.25f;
      L.v[o] = v[i] && v[i + 1] && v[i + w] && v[i + w + 1];
    }
  L.gx.resize(L.I.size());
  L.gy.resize(L.I.size());
  for (int y = 0; y < L.h; y++)
    for (int x = 0; x < L.w; x++) {
      auto at = [&](int xx, int yy) { return L.I[(size_t)std::clamp(yy, 0, L.h - 1) * L.w + std::clamp(xx, 0, L.w - 1)]; };
      L.gx[(size_t)y * L.w + x] = (at(x + 1, y) - at(x - 1, y)) * 0.5f;
      L.gy[(size_t)y * L.w + x] = (at(x, y + 1) - at(x, y - 1)) * 0.5f;
    }
}

// pixel (x, y) of a level with scale d, seen from the previous camera: d_prev = Q d_cur
static bool Warp(const float Q[9], const float K[4], int d, float x, float y, float& u, float& v) {
  float X = ((x + 0.5f) * d - K[2]) / K[0], Y = -((y + 0.5f) * d - K[3]) / K[1];
  float a = Q[0] * X + Q[1] * Y + Q[2], b = Q[3] * X + Q[4] * Y + Q[5], c = Q[6] * X + Q[7] * Y + Q[8];
  if (c < 0.1f) return false;
  u = (K[2] + K[0] * a / c) / d - 0.5f;
  v = (K[3] - K[1] * b / c) / d - 0.5f;
  return true;
}

static bool Sample(const Level& L, float u, float v, float& I, float& gx, float& gy) {
  if (!(u >= 1 && v >= 1 && u < L.w - 2 && v < L.h - 2)) return false;
  int x = (int)u, y = (int)v;
  float fx = u - x, fy = v - y;
  size_t i = (size_t)y * L.w + x;
  if (!L.v[i] || !L.v[i + 1] || !L.v[i + L.w] || !L.v[i + L.w + 1]) return false;
  auto bl = [&](const std::vector<float>& a) { return (a[i] * (1 - fx) + a[i + 1] * fx) * (1 - fy) + (a[i + L.w] * (1 - fx) + a[i + L.w + 1] * fx) * fy; };
  I = bl(L.I); gx = bl(L.gx); gy = bl(L.gy);
  return true;
}

static bool Solve3(const double A[9], const double b[3], double x[3]) {
  auto det = [](const double M[9]) { return M[0] * (M[4] * M[8] - M[5] * M[7]) - M[1] * (M[3] * M[8] - M[5] * M[6]) + M[2] * (M[3] * M[7] - M[4] * M[6]); };
  double d = det(A);
  if (fabs(d) < 1e-20) return false;
  for (int c = 0; c < 3; c++) {
    double M[9];
    memcpy(M, A, sizeof M);
    for (int r = 0; r < 3; r++) M[r * 3 + c] = b[r];
    x[c] = det(M) / d;
  }
  return true;
}

// Gauss-Newton on the rotation (numeric Jacobian: 3 extra warps per sample), Huber weights so moving players
// and leftover HUD do not drag it. Returns the mean absolute luma residual (huge when it could not run).
static float GaussNewton(const Level& P, const Level& C, const float K[4], float w[3], int iters, int maxSamples) {
  int nv = 0;
  for (uint8_t b : C.v) nv += b;
  int step = std::max(1, (int)sqrtf((float)nv / maxSamples));
  const float eps = 1e-3f;
  float err = 1e9f;
  for (int it = 0; it < iters; it++) {
    float Q[9], Qd[3][9];
    RotVec(w, Q);
    for (int j = 0; j < 3; j++) { float wj[3] = {w[0], w[1], w[2]}; wj[j] += eps; RotVec(wj, Qd[j]); }
    double H[9] = {}, g[3] = {}, sum = 0;
    int n = 0;
    for (int y = 0; y < C.h; y += step)
      for (int x = 0; x < C.w; x += step) {
        size_t i = (size_t)y * C.w + x;
        float u, v, I, gx, gy, J[3];
        if (!C.v[i] || !Warp(Q, K, C.d, (float)x, (float)y, u, v) || !Sample(P, u, v, I, gx, gy)) continue;
        bool bad = false;
        for (int j = 0; j < 3 && !bad; j++) {
          float uj, vj;
          bad = !Warp(Qd[j], K, C.d, (float)x, (float)y, uj, vj);
          J[j] = (gx * (uj - u) + gy * (vj - v)) / eps;
        }
        if (bad) continue;
        float r = I - C.I[i], a = fabsf(r), wt = a < 0.04f ? 1 : 0.04f / a;
        for (int p = 0; p < 3; p++) {
          g[p] += wt * J[p] * r;
          for (int q = 0; q < 3; q++) H[p * 3 + q] += wt * J[p] * J[q];
        }
        sum += a;
        n++;
      }
    if (n < 50) return 1e9f;
    err = (float)(sum / n);
    double tr = H[0] + H[4] + H[8], dl[3];
    for (int p = 0; p < 3; p++) H[p * 4] += 1e-4 * tr + 1e-12;  // light damping: roll is weakly seen on flat walls
    if (!Solve3(H, g, dl)) return 1e9f;
    for (int p = 0; p < 3; p++) w[p] -= (float)dl[p];
    if (fabs(dl[0]) + fabs(dl[1]) + fabs(dl[2]) < 1e-5) break;
  }
  return err;
}

void CamTracker::Reset(double now) {
  havePrev = false;
  lost = now;
  ok = false;
  omega[0] = omega[1] = omega[2] = 0;
}

bool CamTracker::Update(const std::vector<uint8_t>& luma, const std::vector<uint8_t>& valid, int w, int h, const float K[4], double now) {
  std::vector<float> I((size_t)w * h);
  std::vector<uint8_t> v((size_t)w * h, 1);
  for (size_t i = 0; i < I.size(); i++) I[i] = luma[i] * (1 / 255.f);
  if (valid.size() == v.size()) for (size_t i = 0; i < v.size(); i++) v[i] = valid[i] != 0;
  Down(I, v, w, h, 8, cur[0]);
  Down(cur[0].I, cur[0].v, cur[0].w, cur[0].h, 16, cur[1]);
  Down(cur[1].I, cur[1].v, cur[1].w, cur[1].h, 32, cur[2]);
  float wv[3] = {0, 0, 0};
  if (!havePrev) {
    ok = true;
    err = 0;
  } else {
    // 1) brute-force translation at 1/32 (fast flicks), read as a rotation at the screen center
    const Level &P = prev[2], &C = cur[2];
    int nvalid = 0;
    for (uint8_t b : C.v) nvalid += b;
    float bestCost = 1e9f;
    int btx = 0, bty = 0;
    const int R = 10;
    for (int ty = -R; ty <= R; ty++)
      for (int tx = -R; tx <= R; tx++) {
        float sum = 0;
        int n = 0;
        for (int y = 0; y < C.h; y++) {
          int py = y + ty;
          if (py < 0 || py >= P.h) continue;
          for (int x = 0; x < C.w; x++) {
            int px = x + tx;
            size_t i = (size_t)y * C.w + x, j = (size_t)py * P.w + px;
            if (px < 0 || px >= P.w || !C.v[i] || !P.v[j]) continue;
            sum += fabsf(P.I[j] - C.I[i]);
            n++;
          }
        }
        if (n > 20 && n > nvalid * 3 / 10 && sum / n < bestCost) { bestCost = sum / n; btx = tx; bty = ty; }
      }
    wv[0] = atanf(bty * 32.f / K[1]);
    wv[1] = atanf(btx * 32.f / K[0]);
    // 2) the full rotation model, coarse to fine
    GaussNewton(prev[1], cur[1], K, wv, 6, 4000);
    err = GaussNewton(prev[0], cur[0], K, wv, 4, 3000);
    ok = err < 0.08f && sqrtf(wv[0] * wv[0] + wv[1] * wv[1] + wv[2] * wv[2]) < 0.8f;
  }
  if (ok) {
    float Q[9], N[9];
    RotVec(wv, Q);
    Mul3(R, Q, N);
    memcpy(R, N, sizeof N);
    Orthonormalize(R);
    memcpy(omega, wv, sizeof wv);
  } else {
    lost = now;  // the memory no longer lines up with this camera
    omega[0] = omega[1] = omega[2] = 0;
  }
  for (int k = 0; k < 3; k++) std::swap(prev[k], cur[k]);
  havePrev = true;
  return ok;
}

// ---- desktop duplication
bool ScreenCapture::Init() {
  ok = false;
  dup.Reset();
  ComPtr<IDXGIDevice> dd;
  ComPtr<IDXGIAdapter> ad;
  ComPtr<IDXGIOutput> out;
  ComPtr<IDXGIOutput1> out1;
  if (FAILED(gDev->QueryInterface(IID_PPV_ARGS(&dd))) || FAILED(dd->GetAdapter(&ad)) || FAILED(ad->EnumOutputs(0, &out)) || FAILED(out.As(&out1))) {
    Log("captura: nenhum monitor na GPU do overlay");
    return false;
  }
  DXGI_OUTPUT_DESC od;
  out->GetDesc(&od);
  x = od.DesktopCoordinates.left; y = od.DesktopCoordinates.top;
  w = od.DesktopCoordinates.right - x; h = od.DesktopCoordinates.bottom - y;
  HRESULT hr = out1->DuplicateOutput(gDev, &dup);
  if (FAILED(hr)) {
    if (hr != lastErr) Log("captura: DuplicateOutput falhou 0x%08X", (unsigned)hr);
    lastErr = hr;
    return false;
  }
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (!screen) {
    HR(gDev->CreateTexture2D(&d, nullptr, &screen));
    HR(gDev->CreateShaderResourceView(screen.Get(), nullptr, &screenSRV));
  }
  return ok = true;
}

bool ScreenCapture::Acquire(double* presentTime, UINT timeoutMs) {
  if (!dup && !Init()) return false;
  DXGI_OUTDUPL_FRAME_INFO fi;
  ComPtr<IDXGIResource> res;
  HRESULT hr = dup->AcquireNextFrame(timeoutMs, &fi, &res);
  if (hr == DXGI_ERROR_WAIT_TIMEOUT) { waits++; return false; }
  if (FAILED(hr)) {  // mode change, UAC, exclusive fullscreen switch...: start over
    if (hr != lastErr) Log("captura: AcquireNextFrame 0x%08X", (unsigned)hr);
    lastErr = hr;
    losts++;
    Init();
    return false;
  }
  bool isFresh = fi.LastPresentTime.QuadPart != 0;  // 0 = only the cursor moved
  if (isFresh) {
    ComPtr<ID3D11Texture2D> t;
    if (SUCCEEDED(res.As(&t))) gCtx->CopyResource(screen.Get(), t.Get());
    *presentTime = QpcToSec(fi.LastPresentTime.QuadPart);
    fresh++;
  } else {
    stale++;
  }
  dup->ReleaseFrame();
  return isFresh;
}

static const char* kHideHLSL = R"(
Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2); Texture2D t3 : register(t3);
Texture2D t4 : register(t4); Texture2D t5 : register(t5); Texture2D t6 : register(t6);
Texture2D<int2> flow1 : register(t7); Texture2D<uint> cost1 : register(t8);
Texture2D mask1 : register(t9); Texture2D image1 : register(t10);
Texture2D<int2> flow2 : register(t11); Texture2D<uint> cost2 : register(t12);
Texture2D mask2 : register(t13); Texture2D image2 : register(t14);
Texture2D<int2> flow3 : register(t15); Texture2D<uint> cost3 : register(t16);
Texture2D mask3 : register(t17); Texture2D image3 : register(t18);
Texture2D<int2> flow4 : register(t19); Texture2D<uint> cost4 : register(t20);
Texture2D mask4 : register(t21); Texture2D image4 : register(t22);
SamplerState sLin : register(s0); SamplerState sPoint : register(s1);
cbuffer P : register(b0) { float2 srcTexel; float tol; float levels; };
// rows of camera->world for writing (r) and reading (s) the background memory; K = fx fy cx cy;
// scr = screen w h, memory w h; tm = now, max age, lost, on; hud = top, bottom, crosshair half-size px; mtex = mask texel
cbuffer Pano : register(b1) { float4 r0, r1, r2, s0, s1, s2, K, scr, tm, hud, mtex, fp, fp2, optical; };  // fp: texture, reach, sharp, grain; fp2.x: patch
static const float PI = 3.14159265;

float3 CamDir(float2 s) { return normalize(float3((s.x - K.z) / K.x, -(s.y - K.w) / K.y, 1)); }
float2 ToPano(float3 d) { return float2(atan2(d.x, d.z) / (2 * PI) + 0.5, 0.5 - asin(clamp(d.y, -1, 1)) / PI); }

// where this display pixel was in the captured frame: the camera kept turning since the capture
float2 Captured(float2 uv) {
  if (tm.w <= 0) return uv;
  float3 c = CamDir(uv * scr.xy);
  float3 d = float3(dot(s0.xyz, c), dot(s1.xyz, c), dot(s2.xyz, c));
  float3 q = d.x * r0.xyz + d.y * r1.xyz + d.z * r2.xyz;
  return q.z > 0.05 ? (K.zw + float2(K.x * q.x, -K.y * q.y) / q.z) / scr.xy : uv;
}

float4 PSOpticalInput(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  return t0.SampleLevel(sLin, uv, 0);
}

bool HistorySample(Texture2D<int2> flow, Texture2D<uint> cost, Texture2D oldMask,
                   Texture2D oldImage, float2 pixel, float2 flowPixel, out float3 color) {
  uint w, h; flow.GetDimensions(w, h);
  if (w == 0 || h == 0) return false;
  if (any(flowPixel < 0) || any(flowPixel >= scr.xy) || t1.SampleLevel(sPoint, flowPixel / scr.xy, 0).r > 0.01) return false;
  int2 p = clamp(int2(flowPixel * float2(w, h) / scr.xy), int2(0, 0), int2(w - 1, h - 1));
  uint confidence = cost.Load(int3(p, 0));
  if (confidence > 96) return false;
  int2 v = flow.Load(int3(p, 0));  // signed S10.5, in half-resolution input pixels
  float2 source = (pixel + float2(v) * scr.xy / float2(w * 4, h * 4) / 32.0) / scr.xy;
  if (any(source < 0.002) || any(source > 0.998) || oldMask.SampleLevel(sPoint, source, 0).r > 0.01) return false;
  color = oldImage.SampleLevel(sLin, source, 0).rgb;
  return true;
}

// 4 bilinear taps = 4x4 box of the full-res screen per quarter-res texel, reprojected to now; the viewmodel
// (it does not turn with the camera: same place in the capture) becomes holes (alpha 0)
float4 PSHole(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float2 cu = Captured(uv);
  float k = t1.Sample(sLin, cu).r > 0.01 || any(cu < 0) || any(cu > 1) ? 0 : 1;
  float3 c = 0;
  [unroll] for (int i = 0; i < 4; i++) c += t0.SampleLevel(sLin, cu + float2(i & 1 ? 1 : -1, i & 2 ? 1 : -1) * srcTexel, 0).rgb;
  return float4(c * 0.25 * k, k);
}

// pull-push: the mip chain of the holed image (premultiplied) is composited coarse -> fine, so holes take the
// smooth average of what surrounds them (level 0 skipped on purpose: slight blur). Where the camera has seen
// the real background behind the viewmodel recently, that wins.
float4 PSFill(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float m = smoothstep(0.0, 0.8, t1.Sample(sLin, uv).r);  // feathered edge (see UploadMask)
  if (m < 0.004) discard;
  float3 historyColor;
  float2 nearOffset = t4.SampleLevel(sLin, uv, 0).rg * 512;
  float2 flowPixel = pos.xy + nearOffset * 1.1;
  if (length(nearOffset) < 256 &&
      ((optical.x > 0 && HistorySample(flow1, cost1, mask1, image1, pos.xy, flowPixel, historyColor)) ||
       (optical.y > 0 && HistorySample(flow2, cost2, mask2, image2, pos.xy, flowPixel, historyColor)) ||
       (optical.z > 0 && HistorySample(flow3, cost3, mask3, image3, pos.xy, flowPixel, historyColor)) ||
       (optical.w > 0 && HistorySample(flow4, cost4, mask4, image4, pos.xy, flowPixel, historyColor))))
    return float4(historyColor * m, m);
  float4 s = t0.SampleLevel(sLin, uv, levels - 1);
  float3 rec = s.rgb / max(s.a, 1e-4), prev = rec;
  float stop = clamp(3 - fp.z, 0, levels - 2);  // finest level used; fractional = blend of two
  for (int l = (int)levels - 2; l >= (int)floor(stop); l--) {
    prev = rec;
    s = t0.SampleLevel(sLin, uv, l);
    rec = s.rgb + (1 - s.a) * rec;
  }
  rec = lerp(rec, prev, frac(stop));
  float4 ls = t0.SampleLevel(sLin, uv, levels - 1);  // (not "s2": that name is the read rotation's last row)
  float3 lowHere = ls.rgb / max(ls.a, 1e-4);
  for (int l2 = (int)levels - 2; l2 >= (int)fp.w; l2--) { ls = t0.SampleLevel(sLin, uv, l2); lowHere = ls.rgb + (1 - ls.a) * lowHere; }
  // texture: the screen mirrored across the nearest mask edge, minus its own blur, rides on the smooth fill,
  // so walls stay walls and floors stay floors instead of a flat smudge (fades deep inside the mask)
  float3 nr = t4.SampleLevel(sLin, uv, 0).rgb;
  float2 off = nr.rg * 512;  // screen pixels to the closest pixel outside; b: how safe mirroring is here
  float dist = length(off);
  if (dist > 0.5 && fp.x > 0) {
    // mirror across the edge; when that lands off screen or on another hidden part, look closer to the edge
    const float tries[4] = {2, 1.6, 1.3, 1.1};
    [unroll] for (int i = 0; i < 4; i++) {
      float2 muv = (pos.xy + tries[i] * off) / scr.xy;
      float4 lo = t0.SampleLevel(sLin, muv, fp.w), at = t0.SampleLevel(sLin, muv, 0);
      if (lo.a > 0.3 && at.a > 0.5 && all(muv > 0) && all(muv < 1)) {
        float3 detail = t5.SampleLevel(sLin, Captured(muv), 0).rgb - lo.rgb / lo.a;
        rec = saturate(rec + detail * exp(-dist / fp.y) * nr.b * fp.x);
        break;
      }
    }
  }
  if (fp2.x > 0) {  // content-aware: every nearby match votes with the full-res pixels it points at
    float2 dims = 1 / mtex.xy, tp = pos.xy / 4 - 0.5, b = round(tp);
    float3 acc = 0;
    float wsum = 0;
    [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {
      float2 q = b + float2(x, y);
      float4 mm = t6.Load(int3(clamp(q, 0, dims - 1), 0));
      if (mm.z > 0) {
        float2 d = tp - q;
        float w = exp(-dot(d, d) / 0.8) / (mm.w + 0.004);  // close neighbours and good matches weigh more
        float2 suv = (mm.xy + d + 0.5) * 4 / scr.xy;
        float3 c = t5.SampleLevel(sLin, Captured(suv), 0).rgb;
        if (fp2.w > 0) {  // keep the colour of this spot, take only the texture from there
          float4 lo = t0.SampleLevel(sLin, suv, fp.w);
          c += lowHere - lo.rgb / max(lo.a, 1e-3);
        }
        acc += c * w;
        wsum += w;
      }
    }
    if (wsum > 1e-6) rec = lerp(rec, saturate(acc / wsum), fp2.x);
  }
  if (tm.w > 0) {
    float3 c = CamDir(pos.xy);
    float2 p = ToPano(float3(dot(s0.xyz, c), dot(s1.xyz, c), dot(s2.xyz, c)));
    float t = t3.SampleLevel(sPoint, p, 0).r;
    float4 pc = t2.SampleLevel(sLin, p, 0);
    float k = t >= tm.z && pc.a > 0.5 ? saturate((tm.y - (tm.x - t)) / (0.3 * tm.y)) : 0;
    rec = lerp(rec, pc.rgb / max(pc.a, 1e-3), k);
  }
  return float4(rec * m, m);
}

// ---- PatchMatch (Photoshop's content-aware fill), real time at 1/4 res. For every hole pixel: the spot outside
// the hole whose 5x5 neighbourhood looks the same. Kept from frame to frame and refined a few steps per frame
// (propagation from neighbours at shrinking jumps + random search), so it converges while you play.
// In these passes srcTexel holds the 1/4-res size, tol the jump, levels the random seed / mip count.
bool InHole(float2 t) { return t1.SampleLevel(sPoint, (t + 0.5) / srcTexel, 0).r > 0.01; }
bool Source(float2 t) {  // the whole patch around t must be real screen (higher quality checks its corners too)
  bool ok = all(t >= 3) && all(t <= srcTexel - 4) && !InHole(t);
  if (fp2.y > 1.5) ok = ok && !InHole(t + float2(2, 2)) && !InHole(t - float2(2, 2)) && !InHole(t + float2(2, -2)) && !InHole(t - float2(2, -2));
  return ok;
}
float4 Guide(float2 t) { return t0.SampleLevel(sLin, (t + 0.5) / srcTexel, 0); }  // bilinear: sub-pixel matches; a = how sure
float PatchCost(float2 p, float2 s) {  // mean squared difference, pixels we are sure about weigh more
  float c = 0, ws = 0;
  if (fp2.y > 2.5) {
    [unroll] for (int y = -2; y <= 2; y++) [unroll] for (int x = -2; x <= 2; x++) {
      float4 a = Guide(p + float2(x, y)), b = Guide(s + float2(x, y));
      float3 d = a.rgb - b.rgb;
      c += a.a * dot(d, d);
      ws += a.a;
    }
  } else {
    [unroll] for (int y = -2; y <= 2; y += 2) [unroll] for (int x = -2; x <= 2; x += 2) {
      float4 a = Guide(p + float2(x, y)), b = Guide(s + float2(x, y));
      float3 d = a.rgb - b.rgb;
      c += a.a * dot(d, d);
      ws += a.a;
    }
  }
  return c / max(ws, 1e-3);
}
float2 Hash2(float2 p, float s) {
  float3 q = frac(p.xyx * float3(0.1031, 0.1030, 0.0973) + s * 0.6180339);
  q += dot(q, q.yzx + 33.33);
  return frac((q.xx + q.yz) * q.zy);
}

// guide image: known pixels as they are (sure), hole pixels as last frame's matches paint them (half sure),
// else the smooth fill (barely sure)
float4 PSGuide(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float2 p = floor(pos.xy);
  float4 h = t0.SampleLevel(sPoint, uv, 0);
  if (h.a > 0.5) return float4(h.rgb, 1);
  float3 sum = 0;
  float n = 0;
  [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {
    float2 q = p + float2(x, y);
    float4 mm = t2.Load(int3(clamp(q, 0, srcTexel - 1), 0));
    if (mm.z > 0) {
      float4 c = t0.SampleLevel(sLin, (mm.xy + p - q + 0.5) / srcTexel, 0);
      if (c.a > 0.5) { float w = 1 / (mm.w + 0.004); sum += c.rgb / c.a * w; n += w; }
    }
  }
  if (n > 0) return float4(sum / n, 0.4);
  float4 s = t0.SampleLevel(sLin, uv, levels - 1);
  float3 rec = s.rgb / max(s.a, 1e-4);
  for (int l = (int)levels - 2; l >= 1; l--) { s = t0.SampleLevel(sLin, uv, l); rec = s.rgb + (1 - s.a) * rec; }
  return float4(rec, 0.15);
}

// one refinement step: keep the best of last frame's match, the mirror across the edge, neighbours' matches
// (shifted; the 1-pixel ones favoured so copies stay in one piece) and a random search at shrinking radius
float4 PSPatch(float4 pos : SV_Position) : SV_Target {
  float2 p = floor(pos.xy);
  if (!InHole(p)) return float4(p, 0, 0);
  float2 best = p;
  float bc = 1e30;
#define TRY(c, bias) { float2 cc = round(c); if (Source(cc)) { float kc = PatchCost(p, cc) * (bias); if (kc < bc) { bc = kc; best = cc; } } }
  float4 cur = t2.Load(int3(p, 0));
  if (cur.z > 0) TRY(cur.xy, 0.95)                                    // last frame's match: sticky, no flicker
  float2 off = t3.SampleLevel(sLin, (p + 0.5) / srcTexel, 0).rg * 128;  // to the nearest outside pixel, 1/4 px
  TRY(round(p + 2 * off), 1)                                          // mirrored across the edge
  TRY(round(p + off + sign(off) * 3), 1)                              // just outside
  [unroll] for (int i = 0; i < 8; i++) {
    float j = i < 4 ? tol : 1;
    int dir = i & 3;
    float2 q = p + float2(dir == 0 ? j : dir == 1 ? -j : 0, dir == 2 ? j : dir == 3 ? -j : 0);
    float4 nn = t2.Load(int3(clamp(q, 0, srcTexel - 1), 0));
    if (nn.z > 0) TRY(nn.xy + p - q, j <= 1 ? 0.92 : 1)
  }
  float r = 64;
  [loop] for (int k = 0; k < (int)fp2.z; k++) { TRY(best + (Hash2(p, levels * 13 + k) * 2 - 1) * r, 1) r = max(r * 0.55, 0.5); }
#undef TRY
  return float4(best, bc < 1e29 ? 1 : 0, bc < 1e29 ? bc : 1);
}

// background memory: every direction the camera sees outside the viewmodel/HUD is stored with its time
struct PanoOut { float4 c : SV_Target0; float t : SV_Target1; };
PanoOut PSPano(float4 pos : SV_Position) {
  float lon = (pos.x / scr.z - 0.5) * 2 * PI, lat = (0.5 - pos.y / scr.w) * PI;
  float3 d = float3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));
  float3 c = d.x * r0.xyz + d.y * r1.xyz + d.z * r2.xyz;  // world -> camera: R transposed
  if (c.z < 0.05) discard;
  float2 s = K.zw + float2(K.x * c.x, -K.y * c.y) / c.z;
  if (s.x < 0 || s.x >= scr.x || s.y < hud.x * scr.y || s.y >= (1 - hud.y) * scr.y || all(abs(s - 0.5 * scr.xy) < hud.z)) discard;
  float2 uv = s / scr.xy;
  float m = 0;  // stay clear of the viewmodel and a margin: a missed sliver would come back later in the fill
  [unroll] for (int i = 0; i < 9; i++) m = max(m, t1.SampleLevel(sLin, uv + float2(i % 3 - 1, i / 3 - 1) * 2 * mtex.xy, 0).r);
  if (m > 0.004) discard;
  PanoOut o;
  o.c = float4(t0.SampleLevel(sLin, uv, 0).rgb, 1);
  o.t = tm.x;
  return o;
}


// green by chromaticity, not brightness: shadows on the green floor stay green, black gloves and metal do not
float NonGreen(float3 c) { return (c.g > 0.04 && c.g > max(c.r, c.b) * (1 + tol * 4) + 0.01) ? 0 : 1; }
// R: non-green coverage of the 4x4 block, G: its average luma
float4 PSChroma(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float v = 0, l = 0;
  [unroll] for (int y = 0; y < 4; y++) [unroll] for (int x = 0; x < 4; x++) {
    float3 c = t0.SampleLevel(sPoint, uv + (float2(x, y) - 1.5) * srcTexel, 0).rgb;
    v += NonGreen(c);
    l += dot(c, float3(0.299, 0.587, 0.114));
  }
  return float4(v / 16, l / 16, 0, 0);
}

float4 PSPreview(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float m = t1.Sample(sLin, uv).r * 0.45;
  return float4(m, 0, 0, m);
}
)";

struct HideCB { float srcTexel[2]; float tol; float levels; };
struct PanoCB { float r[3][4], s[3][4], K[4], scr[4], tm[4], hud[4], mtex[4], fp[4], fp2[4], optical[4]; };

static ComPtr<ID3D11Texture2D> Staging(int w, int h, DXGI_FORMAT fmt) {
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt;
  d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> t;
  HR(gDev->CreateTexture2D(&d, nullptr, &t));
  return t;
}

struct Hider::OpticalHistory {
  static constexpr int kFrames = 5;
  HMODULE dll = nullptr;
  NvOFHandle of = nullptr;
  NV_OF_D3D11_API_FUNCTION_LIST api{};
  int head = -1, count = 0, fw = 0, fh = 0, ow = 0, oh = 0, sw = 0, sh = 0;
  bool valid[kFrames - 1]{};
  RT input[kFrames];
  ComPtr<ID3D11Texture2D> frame[kFrames], oldMask[kFrames], vectors[kFrames - 1], costs[kFrames - 1];
  ComPtr<ID3D11ShaderResourceView> frameView[kFrames], maskView[kFrames], vectorView[kFrames - 1], costView[kFrames - 1];
  NvOFGPUBufferHandle inputHandle[kFrames]{}, vectorHandle[kFrames - 1]{}, costHandle[kFrames - 1]{};
  ComPtr<ID3D11PixelShader> downsample;

  ~OpticalHistory() {
    if (of) {
      for (auto& h : inputHandle) if (h) api.nvOFUnregisterResourceD3D11(h);
      for (auto& h : vectorHandle) if (h) api.nvOFUnregisterResourceD3D11(h);
      for (auto& h : costHandle) if (h) api.nvOFUnregisterResourceD3D11(h);
      api.nvOFDestroy(of);
    }
    if (dll) FreeLibrary(dll);
  }

  bool Texture(int w, int h, DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& tex,
               ComPtr<ID3D11ShaderResourceView>& view) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1;
    d.Format = format; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(gDev->CreateTexture2D(&d, nullptr, &tex)) &&
           SUCCEEDED(gDev->CreateShaderResourceView(tex.Get(), nullptr, &view));
  }

  bool Supports(NV_OF_BUFFER_USAGE usage, DXGI_FORMAT wanted) {
    uint32_t n = 0;
    if (api.nvOFGetSurfaceFormatCountD3D11(of, usage, NV_OF_MODE_OPTICALFLOW, &n) != NV_OF_SUCCESS || !n) return false;
    std::vector<DXGI_FORMAT> formats(n);
    if (api.nvOFGetSurfaceFormatD3D11(of, usage, NV_OF_MODE_OPTICALFLOW, formats.data()) != NV_OF_SUCCESS) return false;
    bool found = std::find(formats.begin(), formats.end(), wanted) != formats.end();
    if (!found) {
      std::string actual;
      for (auto f : formats) actual += std::to_string((int)f) + " ";
      Log("NVOFA: formato %d indisponivel para uso %d (suportados: %s)", (int)wanted, (int)usage, actual.c_str());
    }
    return found;
  }

  bool Init(int w, int h, ID3D11Texture2D* mask) {
    sw = w; sh = h;
    fw = ((w + 7) / 8) * 4; fh = ((h + 7) / 8) * 4; // half resolution, aligned to 4x4 flow grid
    ow = fw / 4; oh = fh / 4;
    dll = LoadLibraryW(L"nvofapi64.dll");
    if (!dll) { Log("NVOFA: nvofapi64.dll ausente"); return false; }
    auto create = (decltype(&NvOFAPICreateInstanceD3D11))GetProcAddress(dll, "NvOFAPICreateInstanceD3D11");
    if (!create || create(NV_OF_API_VERSION, &api) != NV_OF_SUCCESS ||
        api.nvCreateOpticalFlowD3D11(gDev, gCtx, &of) != NV_OF_SUCCESS) {
      Log("NVOFA: falha ao criar sessao D3D11"); return false;
    }
    uint32_t n = 0;
    if (api.nvOFGetCaps(of, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &n) != NV_OF_SUCCESS || !n) {
      Log("NVOFA: falha ao consultar grade"); return false;
    }
    std::vector<uint32_t> grids(n);
    if (api.nvOFGetCaps(of, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids.data(), &n) != NV_OF_SUCCESS ||
        std::find(grids.begin(), grids.end(), 4u) == grids.end()) {
      Log("NVOFA: grade 4x4 indisponivel"); return false;
    }
    if (!Supports(NV_OF_BUFFER_USAGE_INPUT, DXGI_FORMAT_B8G8R8A8_UNORM) ||
        !Supports(NV_OF_BUFFER_USAGE_OUTPUT, DXGI_FORMAT_R16G16_SINT) ||
        !Supports(NV_OF_BUFFER_USAGE_COST, DXGI_FORMAT_R8_UINT)) return false;
    NV_OF_INIT_PARAMS p{};
    p.width = fw; p.height = fh; p.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
    p.mode = NV_OF_MODE_OPTICALFLOW; p.perfLevel = NV_OF_PERF_LEVEL_FAST;
    p.enableOutputCost = NV_OF_TRUE;
    if (api.nvOFInit(of, &p) != NV_OF_SUCCESS) { Log("NVOFA: nvOFInit falhou"); return false; }
    D3D11_TEXTURE2D_DESC md{}; mask->GetDesc(&md);
    for (int i = 0; i < kFrames; ++i) {
      input[i] = MakeRT(fw, fh, DXGI_FORMAT_B8G8R8A8_UNORM);
      if (!Texture(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, frame[i], frameView[i]) ||
          !Texture(md.Width, md.Height, md.Format, oldMask[i], maskView[i]) ||
          api.nvOFRegisterResourceD3D11(of, input[i].tex.Get(), &inputHandle[i]) != NV_OF_SUCCESS) return false;
    }
    for (int i = 0; i < kFrames - 1; ++i) {
      if (!Texture(ow, oh, DXGI_FORMAT_R16G16_SINT, vectors[i], vectorView[i]) ||
          !Texture(ow, oh, DXGI_FORMAT_R8_UINT, costs[i], costView[i]) ||
          api.nvOFRegisterResourceD3D11(of, vectors[i].Get(), &vectorHandle[i]) != NV_OF_SUCCESS ||
          api.nvOFRegisterResourceD3D11(of, costs[i].Get(), &costHandle[i]) != NV_OF_SUCCESS) return false;
    }
    downsample = MakePS(kHideHLSL, "PSOpticalInput");
    return true;
  }

  void Record(ID3D11ShaderResourceView* screen, ID3D11Texture2D* mask, bool fresh, int historyFrames) {
    if (!fresh) return;
    ComPtr<ID3D11RenderTargetView> savedRT;
    ComPtr<ID3D11DepthStencilView> savedDS;
    D3D11_VIEWPORT savedVP{}; UINT nvp = 1;
    gCtx->OMGetRenderTargets(1, &savedRT, &savedDS);
    gCtx->RSGetViewports(&nvp, &savedVP);
    head = (head + 1) % kFrames;
    std::fill(std::begin(valid), std::end(valid), false);
    ComPtr<ID3D11Resource> source;
    screen->GetResource(&source);
    gCtx->CopyResource(frame[head].Get(), source.Get());
    gCtx->CopyResource(oldMask[head].Get(), mask);
    ID3D11RenderTargetView* target = input[head].rtv.Get();
    gCtx->OMSetRenderTargets(1, &target, nullptr);
    SetViewport(0, 0, (float)fw, (float)fh);
    gPass.Draw(downsample.Get(), {screen}, gPass.opaque.Get());
    gCtx->OMSetRenderTargets(1, savedRT.GetAddressOf(), savedDS.Get());
    if (nvp) gCtx->RSSetViewports(1, &savedVP);
    count = std::min(count + 1, kFrames);
    if (!historyFrames) return;
    for (int age = 1; age < count && age <= historyFrames; ++age) {
      int previous = (head + kFrames - age) % kFrames;
      NV_OF_EXECUTE_INPUT_PARAMS in{};
      in.inputFrame = inputHandle[head];
      in.referenceFrame = inputHandle[previous];
      in.disableTemporalHints = age > 1 ? NV_OF_TRUE : NV_OF_FALSE;
      NV_OF_EXECUTE_OUTPUT_PARAMS out{};
      out.outputBuffer = vectorHandle[age - 1];
      out.outputCostBuffer = costHandle[age - 1];
      valid[age - 1] = api.nvOFExecute(of, &in, &out) == NV_OF_SUCCESS;
      if (!valid[age - 1]) Log("NVOFA: nvOFExecute falhou (idade %d)", age);
    }
  }

  ID3D11ShaderResourceView* View(int age, int kind) const {
    if (age < 1 || age >= kFrames || !valid[age - 1]) return nullptr;
    int previous = (head + kFrames - age) % kFrames;
    switch (kind) {
      case 0: return vectorView[age - 1].Get();
      case 1: return costView[age - 1].Get();
      case 2: return maskView[previous].Get();
      default: return frameView[previous].Get();
    }
  }
};

Hider::~Hider() { delete optical; }

void Hider::RecordHistory(ID3D11ShaderResourceView* screen, bool fresh) {
  if (!optical || !fresh) return;
  optical->Record(screen, maskTex.Get(), true, fp.optical &&
                   std::any_of(scratch.begin(), scratch.end(), [](uint8_t v) { return v != 0; })
                   ? std::clamp(fp.opticalFrames, 1, 4) : 0);
}

void Hider::Init(int w, int h) {
  delete optical; optical = nullptr;
  sw = w; sh = h;
  mw = (w + 3) / 4; mh = (h + 3) / 4;
  holed = MakeRT(mw, mh, DXGI_FORMAT_R8G8B8A8_UNORM, true);
  maskRT = MakeRT(mw, mh, DXGI_FORMAT_R8G8_UNORM);
  panoC = MakeRT(kPanoW, kPanoH, DXGI_FORMAT_R8G8B8A8_UNORM);
  panoT = MakeRT(kPanoW, kPanoH, DXGI_FORMAT_R32_FLOAT);
  guide = MakeRT(mw, mh, DXGI_FORMAT_R8G8B8A8_UNORM);
  for (RT& n : nnf) {
    n = MakeRT(mw, mh, DXGI_FORMAT_R32G32B32A32_FLOAT);
    float none[4] = {0, 0, 0, 0};
    gCtx->ClearRenderTargetView(n.rtv.Get(), none);
  }
  float zero[4] = {0, 0, 0, 0}, never[4] = {-1e9f, 0, 0, 0};
  gCtx->ClearRenderTargetView(panoC.rtv.Get(), zero);
  gCtx->ClearRenderTargetView(panoT.rtv.Get(), never);
  D3D11_TEXTURE2D_DESC d{};
  d.Width = mw; d.Height = mh; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8_UNORM;
  d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  HR(gDev->CreateTexture2D(&d, nullptr, &maskTex));
  HR(gDev->CreateShaderResourceView(maskTex.Get(), nullptr, &maskSRV));
  d.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
  HR(gDev->CreateTexture2D(&d, nullptr, &nearTex));
  HR(gDev->CreateShaderResourceView(nearTex.Get(), nullptr, &nearSRV));
  for (auto& r : ring) r = Staging(mw, mh, DXGI_FORMAT_R8G8_UNORM);
  liveStage = Staging(mw, mh, DXGI_FORMAT_R8G8_UNORM);
  psHole = MakePS(kHideHLSL, "PSHole");
  psFill = MakePS(kHideHLSL, "PSFill");
  psChroma = MakePS(kHideHLSL, "PSChroma");
  psPreview = MakePS(kHideHLSL, "PSPreview");
  psPano = MakePS(kHideHLSL, "PSPano");
  psGuide = MakePS(kHideHLSL, "PSGuide");
  psPatch = MakePS(kHideHLSL, "PSPatch");
  D3D11_BUFFER_DESC bd{sizeof(HideCB), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
  HR(gDev->CreateBuffer(&bd, nullptr, &cb));
  bd.ByteWidth = sizeof(PanoCB);
  HR(gDev->CreateBuffer(&bd, nullptr, &cbPano));
  UploadMask(nullptr, 0, 0, 0, 0, 0, 0, 0);
  optical = new OpticalHistory;
  if (!optical->Init(w, h, maskTex.Get())) {
    Log("NVOFA indisponivel: usando preenchimento atual");
    delete optical; optical = nullptr;
  } else Log("NVOFA D3D11 inicializado (%dx%d)", optical->fw, optical->fh);
}

static void SetCB(ID3D11Buffer* cb, float tx, float ty, float tol, float levels) {
  D3D11_MAPPED_SUBRESOURCE ms;
  HR(gCtx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms));
  *(HideCB*)ms.pData = {{tx, ty}, tol, levels};
  gCtx->Unmap(cb, 0);
  gCtx->PSSetConstantBuffers(0, 1, &cb);
}

static void DrawChroma(ID3D11ShaderResourceView* screen, RT& target, ID3D11Buffer* cb, ID3D11PixelShader* ps, float tol) {
  ComPtr<ID3D11Resource> r;
  screen->GetResource(&r);
  D3D11_TEXTURE2D_DESC sd;
  ((ID3D11Texture2D*)r.Get())->GetDesc(&sd);
  ID3D11RenderTargetView* rtv = target.rtv.Get();
  gCtx->OMSetRenderTargets(1, &rtv, nullptr);
  SetViewport(0, 0, (float)target.w, (float)target.h);
  SetCB(cb, 1.f / sd.Width, 1.f / sd.Height, tol, 0);
  gPass.Draw(ps, {screen}, gPass.opaque.Get());
}

void Hider::ChromaQueue(ID3D11ShaderResourceView* screen, float tolerance, const Recorder::RawFrame& meta) {
  if ((int)pending.size() == kRing) ChromaPoll(ready, true);  // ring full: land what is in flight, hand it out on the next poll
  DrawChroma(screen, maskRT, cb.Get(), psChroma.Get(), tolerance);
  gCtx->CopyResource(ring[head].Get(), maskRT.tex.Get());
  pending.push_back({head, meta});
  head = (head + 1) % kRing;
}

void Hider::ChromaPoll(std::vector<Recorder::RawFrame>& out, bool flush) {
  if (&out != &ready && !ready.empty()) {
    for (auto& f : ready) out.push_back(std::move(f));
    ready.clear();
  }
  const int lw = (mw + 1) / 2, lh = (mh + 1) / 2;
  std::vector<uint8_t> sup;
  while (!pending.empty()) {
    Pending& p = pending.front();
    D3D11_MAPPED_SUBRESOURCE ms;
    HRESULT hr = gCtx->Map(ring[p.slot].Get(), 0, D3D11_MAP_READ, flush ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    HR(hr);
    Recorder::RawFrame f = std::move(p.meta);
    bitsTmp.assign((size_t)(mw * mh + 7) / 8, 0);
    lumaFull.resize((size_t)mw * mh);
    sup.assign((size_t)lw * lh, 0);
    for (int y = 0; y < mh; y++) {
      const uint8_t* row = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch;
      for (int x = 0; x < mw; x++) {
        size_t i = (size_t)y * mw + x;
        if (row[x * 2] >= 64) { bitsTmp[i >> 3] |= 1 << (i & 7); sup[(size_t)(y / 2) * lw + x / 2] = 255; }  // a quarter of the block
        lumaFull[i] = row[x * 2 + 1];
      }
    }
    gCtx->Unmap(ring[p.slot].Get(), 0);
    // only the thumbnail around the viewmodel is ever used: zero the rest so the frame compresses well
    Half(lumaFull, mw, mh, thumbTmp);
    Dilate(sup, lw, lh, 3);
    for (size_t i = 0; i < sup.size(); i++) if (!sup[i]) thumbTmp[i] = 0;
    PackFrame(bitsTmp, thumbTmp, f.packed);
    out.push_back(std::move(f));
    pending.erase(pending.begin());
  }
}

void Hider::Analyze(ID3D11ShaderResourceView* screen, std::vector<uint8_t>& luma) {
  DrawChroma(screen, maskRT, cb.Get(), psChroma.Get(), 0);
  gCtx->CopyResource(liveStage.Get(), maskRT.tex.Get());
  D3D11_MAPPED_SUBRESOURCE ms;
  HR(gCtx->Map(liveStage.Get(), 0, D3D11_MAP_READ, 0, &ms));
  luma.resize((size_t)mw * mh);
  for (int y = 0; y < mh; y++) {
    const uint8_t* row = (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch;
    for (int x = 0; x < mw; x++) luma[(size_t)y * mw + x] = row[x * 2 + 1];
  }
  gCtx->Unmap(liveStage.Get(), 0);
}

void Hider::UploadMask(const std::vector<uint8_t>* bits, int rx, int ry, int rw, int rh, int dx, int dy, int dilate, bool mirror) {
  scratch.assign((size_t)mw * mh, 0);
  if (bits) {
    for (int y = 0; y < rh; y++) {
      int Y = ry + y + dy;
      if (Y < 0 || Y >= mh) continue;
      for (int x = 0; x < rw; x++) {
        size_t i = (size_t)y * rw + x;
        int X = rx + x + dx;
        if (mirror) X = mw - 1 - X;  // left hand: the game mirrors the whole viewmodel about the screen center
        if (X >= 0 && X < mw && i / 8 < bits->size() && ((*bits)[i >> 3] >> (i & 7)) & 1) scratch[(size_t)Y * mw + X] = 255;
      }
    }
    // soft edge: one pixel wider, then a 5x5 box blur -> fades over ~20 screen pixels instead of a 4 px step
    int r = std::clamp(fp.feather, 0, 6);
    Dilate(scratch, mw, mh, dilate + (r + 1) / 2);
    std::vector<uint8_t> tmp(scratch.size());
    for (int pass = 0; pass < 2 && r > 0; pass++) {  // horizontal, then vertical
      for (int y = 0; y < mh; y++)
        for (int x = 0; x < mw; x++) {
          int s = 0;
          for (int k = -r; k <= r; k++)
            s += pass ? scratch[(size_t)std::clamp(y + k, 0, mh - 1) * mw + x] : scratch[(size_t)y * mw + std::clamp(x + k, 0, mw - 1)];
          tmp[(size_t)y * mw + x] = (uint8_t)(s / (2 * r + 1));
        }
      scratch.swap(tmp);
    }
  }
  gCtx->UpdateSubresource(maskTex.Get(), 0, nullptr, scratch.data(), mw, 0);
  // closest outside pixel for every mask pixel: two chamfer sweeps carrying the nearest seed (8-neighbours)
  std::vector<int> seed(scratch.size());
  for (size_t i = 0; i < seed.size(); i++) seed[i] = scratch[i] ? -1 : (int)i;
  auto d2 = [&](int i, int s) { int dx = i % mw - s % mw, dy = i / mw - s / mw; return dx * dx + dy * dy; };
  auto relax = [&](int x, int y, int nx, int ny) {
    if (nx < 0 || ny < 0 || nx >= mw || ny >= mh) return;
    int i = y * mw + x, s = seed[ny * mw + nx];
    if (s >= 0 && (seed[i] < 0 || d2(i, s) < d2(i, seed[i]))) seed[i] = s;
  };
  for (int y = 0; y < mh; y++)
    for (int x = 0; x < mw; x++) { relax(x, y, x - 1, y); relax(x, y, x - 1, y - 1); relax(x, y, x, y - 1); relax(x, y, x + 1, y - 1); }
  for (int y = mh - 1; y >= 0; y--)
    for (int x = mw - 1; x >= 0; x--) { relax(x, y, x + 1, y); relax(x, y, x + 1, y + 1); relax(x, y, x, y + 1); relax(x, y, x - 1, y + 1); }
  // where two edges are equally close the mirror flips direction: fade the texture there (no hard seam)
  std::vector<float> ux(seed.size(), 0), uy(seed.size(), 0);
  for (size_t i = 0; i < seed.size(); i++) {
    if (seed[i] < 0 || seed[i] == (int)i) continue;
    float ox = (float)(seed[i] % mw - (int)(i % mw)), oy = (float)(seed[i] / mw - (int)(i / mw)), n = sqrtf(ox * ox + oy * oy);
    ux[i] = ox / n; uy[i] = oy / n;
  }
  nearBuf.assign(scratch.size() * 4, 0);
  for (int y = 0; y < mh; y++)
    for (int x = 0; x < mw; x++) {
      size_t i = (size_t)y * mw + x;
      if (seed[i] < 0 || seed[i] == (int)i) continue;
      float sx = 0, sy = 0;
      int n = 0;
      for (int ky = std::max(0, y - 2); ky <= std::min(mh - 1, y + 2); ky++)
        for (int kx = std::max(0, x - 2); kx <= std::min(mw - 1, x + 2); kx++) { sx += ux[(size_t)ky * mw + kx]; sy += uy[(size_t)ky * mw + kx]; n++; }
      float agree = sqrtf(sx * sx + sy * sy) / n;  // 1 = all neighbours mirror the same way
      // mask pixels are 4 screen pixels; offsets stored /512 screen pixels so they fit SNORM
      float ox = (seed[i] % mw - (int)(i % mw)) * 4.f, oy = (seed[i] / mw - (int)(i / mw)) * 4.f;
      nearBuf[i * 4] = (int16_t)std::clamp(ox / 512 * 32767, -32767.f, 32767.f);
      nearBuf[i * 4 + 1] = (int16_t)std::clamp(oy / 512 * 32767, -32767.f, 32767.f);
      nearBuf[i * 4 + 2] = (int16_t)(std::clamp((agree - fp.seam) / std::max(0.98f - fp.seam, 0.02f), 0.f, 1.f) * 32767);
    }
  gCtx->UpdateSubresource(nearTex.Get(), 0, nullptr, nearBuf.data(), mw * 8, 0);
}


// the part of the background memory (latitude/longitude) the current view can touch: rows y0..y1 and
// columns x0..x1, which may wrap past the right edge
static void PanoBand(const float R[9], const float K[4], int sw, int sh, int pw, int ph, int& y0, int& y1, int& x0, int& x1) {
  float lo = 1e9f, hi = -1e9f, lmin = 1e9f, lmax = -1e9f;
  float lon0 = atan2f(R[2], R[8]);  // where the camera looks
  auto lat = [&](float sx, float sy) {
    float c[3] = {(sx - K[2]) / K[0], -(sy - K[3]) / K[1], 1};
    float n = sqrtf(c[0] * c[0] + c[1] * c[1] + 1);
    float l = asinf(std::clamp((R[3] * c[0] + R[4] * c[1] + R[5]) / n, -1.f, 1.f));
    lo = std::min(lo, l); hi = std::max(hi, l);
    float d = atan2f(R[0] * c[0] + R[1] * c[1] + R[2], R[6] * c[0] + R[7] * c[1] + R[8]) - lon0;
    d = remainderf(d, XM_2PI);  // relative to the view direction, -pi..pi
    lmin = std::min(lmin, d); lmax = std::max(lmax, d);
  };
  for (int i = 0; i <= 16; i++) {
    float t = i / 16.f;
    lat(t * sw, 0); lat(t * sw, (float)sh); lat(0, t * sh); lat((float)sw, t * sh);
  }
  for (float sg : {1.f, -1.f}) {  // a pole on screen: the band runs all the way to it
    float c[3] = {R[3] * sg, R[4] * sg, R[5] * sg};  // world up in camera space
    if (c[2] <= 0.01f) continue;
    float sx = K[2] + K[0] * c[0] / c[2], sy = K[3] - K[1] * c[1] / c[2];
    if (sx >= 0 && sx < sw && sy >= 0 && sy < sh) { (sg > 0 ? hi : lo) = sg * XM_PIDIV2; lmin = -XM_PI; lmax = XM_PI; }
  }
  y0 = std::clamp((int)floorf((0.5f - hi / XM_PI) * ph) - 2, 0, ph);
  y1 = std::clamp((int)ceilf((0.5f - lo / XM_PI) * ph) + 2, 0, ph);
  if (lmax - lmin > XM_2PI * 0.9f || hi > 1.3f || lo < -1.3f) { x0 = 0; x1 = pw; return; }  // near a pole: all of it
  x0 = (int)floorf(((lon0 + lmin) / XM_2PI + 0.5f) * pw) - 4;
  x1 = (int)ceilf(((lon0 + lmax) / XM_2PI + 0.5f) * pw) + 4;
}

void Hider::Fill(ID3D11ShaderResourceView* screen, const PanoCam* cam, float now, float maxAge, float lost) {
  ComPtr<ID3D11RenderTargetView> outRTV;
  ComPtr<ID3D11DepthStencilView> outDSV;
  D3D11_VIEWPORT outVP;
  UINT nvp = 1;
  gCtx->OMGetRenderTargets(1, &outRTV, &outDSV);
  gCtx->RSGetViewports(&nvp, &outVP);

  PanoCB pc{};
  if (cam) {
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) { pc.r[i][j] = cam->R[i * 3 + j]; pc.s[i][j] = cam->show[i * 3 + j]; }
    memcpy(pc.K, cam->K, sizeof pc.K);
    pc.tm[0] = now; pc.tm[1] = maxAge; pc.tm[2] = lost; pc.tm[3] = 1;
  }
  pc.scr[0] = (float)sw; pc.scr[1] = (float)sh; pc.scr[2] = kPanoW; pc.scr[3] = kPanoH;
  pc.hud[0] = kHudTop; pc.hud[1] = kHudBottom; pc.hud[2] = kCrossFrac * sw;
  pc.mtex[0] = 1.f / mw; pc.mtex[1] = 1.f / mh;
  int q = std::clamp(fp.quality, 1, 4);
  const float randoms[4] = {4, 6, 9, 14};
  pc.fp2[0] = std::clamp(fp.patch, 0.f, 1.f); pc.fp2[1] = (float)q; pc.fp2[2] = randoms[q - 1]; pc.fp2[3] = fp.colorFix ? 1.f : 0.f;
  pc.fp[0] = fp.texture; pc.fp[1] = std::max(fp.reach, 1.f); pc.fp[2] = std::clamp(fp.sharp, 0.f, 3.f); pc.fp[3] = std::clamp(fp.grain, 0.f, 5.f);
  for (int age = 1; age <= 4; ++age)
    pc.optical[age - 1] = fp.optical && optical && optical->valid[age - 1] ? 1.f : 0.f;
  D3D11_MAPPED_SUBRESOURCE ms;
  HR(gCtx->Map(cbPano.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms));
  *(PanoCB*)ms.pData = pc;
  gCtx->Unmap(cbPano.Get(), 0);
  gCtx->PSSetConstantBuffers(1, 1, cbPano.GetAddressOf());

  if (cam) {
    int y0, y1, x0, x1;
    PanoBand(cam->R, cam->K, sw, sh, kPanoW, kPanoH, y0, y1, x0, x1);
    ID3D11RenderTargetView* rt[2] = {panoC.rtv.Get(), panoT.rtv.Get()};
    gCtx->OMSetRenderTargets(2, rt, nullptr);
    auto strip = [&](int a, int b) {  // only the columns in view (one or two pieces when it wraps)
      a = std::clamp(a, 0, kPanoW); b = std::clamp(b, 0, kPanoW);
      if (b <= a || y1 <= y0) return;
      SetViewport((float)a, (float)y0, (float)(b - a), (float)(y1 - y0));
      gPass.Draw(psPano.Get(), {screen, maskSRV.Get()}, gPass.opaque.Get());
    };
    strip(x0, x1);
    if (x0 < 0) strip(x0 + kPanoW, kPanoW);
    if (x1 > kPanoW) strip(0, x1 - kPanoW);
  }

  ComPtr<ID3D11Resource> r;
  screen->GetResource(&r);
  D3D11_TEXTURE2D_DESC sd;
  ((ID3D11Texture2D*)r.Get())->GetDesc(&sd);
  ID3D11RenderTargetView* rtv = holed.rtv.Get();
  gCtx->OMSetRenderTargets(1, &rtv, nullptr);
  SetViewport(0, 0, (float)holed.w, (float)holed.h);
  SetCB(cb.Get(), 1.f / sd.Width, 1.f / sd.Height, 0, 0);
  gPass.Draw(psHole.Get(), {screen, maskSRV.Get()}, gPass.opaque.Get());
  gCtx->GenerateMips(holed.srv.Get());
  if (fp.patch > 0) {  // content-aware: guide from last frame's matches, then 4 refinement steps (jumps 8, 4, 2, 1)
    ID3D11RenderTargetView* g = guide.rtv.Get();
    gCtx->OMSetRenderTargets(1, &g, nullptr);
    SetViewport(0, 0, (float)mw, (float)mh);
    SetCB(cb.Get(), (float)mw, (float)mh, 0, (float)holed.mips);
    gPass.Draw(psGuide.Get(), {holed.srv.Get(), maskSRV.Get(), nnf[nnfCur].srv.Get()}, gPass.opaque.Get());
    const int steps[4] = {4, 6, 10, 16};  // refinement steps per frame by quality
    for (int st = 0; st < steps[std::clamp(fp.quality, 1, 4) - 1]; st++) {
      float jump = (float)(8 >> (st % 4));
      int nx = nnfCur ^ 1;
      ID3D11RenderTargetView* o = nnf[nx].rtv.Get();
      gCtx->OMSetRenderTargets(1, &o, nullptr);
      SetCB(cb.Get(), (float)mw, (float)mh, jump, (float)(pmFrame++ % 9973));
      gPass.Draw(psPatch.Get(), {guide.srv.Get(), maskSRV.Get(), nnf[nnfCur].srv.Get(), nearSRV.Get()}, gPass.opaque.Get());
      nnfCur = nx;
    }
  }

  gCtx->OMSetRenderTargets(1, outRTV.GetAddressOf(), outDSV.Get());
  gCtx->RSSetViewports(1, &outVP);
  SetCB(cb.Get(), 0, 0, 0, (float)holed.mips);
  gPass.Draw(psFill.Get(), {holed.srv.Get(), maskSRV.Get(), panoC.srv.Get(), panoT.srv.Get(), nearSRV.Get(), screen, nnf[nnfCur].srv.Get(),
             optical ? optical->View(1, 0) : nullptr, optical ? optical->View(1, 1) : nullptr,
             optical ? optical->View(1, 2) : nullptr, optical ? optical->View(1, 3) : nullptr,
             optical ? optical->View(2, 0) : nullptr, optical ? optical->View(2, 1) : nullptr,
             optical ? optical->View(2, 2) : nullptr, optical ? optical->View(2, 3) : nullptr,
             optical ? optical->View(3, 0) : nullptr, optical ? optical->View(3, 1) : nullptr,
             optical ? optical->View(3, 2) : nullptr, optical ? optical->View(3, 3) : nullptr,
             optical ? optical->View(4, 0) : nullptr, optical ? optical->View(4, 1) : nullptr,
             optical ? optical->View(4, 2) : nullptr, optical ? optical->View(4, 3) : nullptr},
             gPass.premulOver.Get());
}

void Hider::Preview() {
  gPass.Draw(psPreview.Get(), {nullptr, maskSRV.Get()}, gPass.premulOver.Get());
}

