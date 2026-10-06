#include "mask_ai.h"
#include "../third_party/onnxruntime/onnxruntime_c_api.h"
#include "../third_party/onnxruntime/dml_provider_factory.h"
#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>

struct MaskAI::State {
  const OrtApi* api = nullptr;
  OrtEnv* env = nullptr;
  OrtSessionOptions* options = nullptr;
  OrtSession* session = nullptr;
  OrtMemoryInfo* memory = nullptr;
  OrtValue* inputTensor = nullptr;
  OrtValue* outputTensor = nullptr;
  int w = 0, h = 0, rx = 0, ry = 0, rw = 0, rh = 0;
  std::vector<float> input, output, stable;
  std::vector<uint8_t> result;
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  bool quit = false, busy = false, pending = false, ready = false;
  bool hasStable = false;
  double submittedAt = -1, resultAt = -1;

  bool Ok(OrtStatus* status, const char* what) {
    if (!status) return true;
    Log("IA mascara: %s: %s", what, api->GetErrorMessage(status));
    api->ReleaseStatus(status);
    return false;
  }

  ~State() {
    { std::lock_guard<std::mutex> lock(mutex); quit = true; }
    wake.notify_one();
    if (worker.joinable()) worker.join();
    if (api) {
      if (outputTensor) api->ReleaseValue(outputTensor);
      if (inputTensor) api->ReleaseValue(inputTensor);
      if (memory) api->ReleaseMemoryInfo(memory);
      if (session) api->ReleaseSession(session);
      if (options) api->ReleaseSessionOptions(options);
      if (env) api->ReleaseEnv(env);
    }
  }

  bool Init(const std::string& dir, const Library& lib) {
    std::ifstream geometry(Widen(dir + "/geometry.txt"));
    int dims[6]{};
    for (int& d : dims) geometry >> d;
    if (!geometry || dims[0] != lib.w || dims[1] != lib.h || dims[2] > lib.rx ||
        dims[3] > lib.ry || dims[2] + dims[4] < lib.rx + lib.rw ||
        dims[3] + dims[5] < lib.ry + lib.rh || dims[4] <= 0 || dims[5] <= 0 ||
        (dims[4] & 1) || (dims[5] & 1)) {
      Log("IA mascara: modelo nao corresponde a esta biblioteca/resolucao");
      return false;
    }
    rx = dims[2]; ry = dims[3]; rw = dims[4]; rh = dims[5];
    w = rw / 2; h = rh / 2;
    if (w <= 0 || h <= 0) return false;
    const OrtApiBase* base = OrtGetApiBase();
    api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
    if (!api || !Ok(api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "jange-mask", &env), "CreateEnv") ||
        !Ok(api->CreateSessionOptions(&options), "CreateSessionOptions")) return false;
    api->SetSessionExecutionMode(options, ORT_SEQUENTIAL);
    api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
    api->DisableMemPattern(options);
    bool gpu = Ok(OrtSessionOptionsAppendExecutionProvider_DML(options, 0), "DirectML");
    if (!gpu) {
      api->ReleaseSessionOptions(options); options = nullptr;
      if (!Ok(api->CreateSessionOptions(&options), "CPU options")) return false;
      api->SetIntraOpNumThreads(options, 2);
      api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
    }
    if (!Ok(api->CreateSession(env, Widen(dir + "/model.onnx").c_str(), options, &session), "CreateSession")) return false;
    if (!Ok(api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory), "MemoryInfo")) return false;
    size_t pixels = (size_t)w * h;
    input.resize(pixels * 3);
    output.resize(pixels);
    stable.resize(pixels);
    result.resize(pixels);
    int64_t inShape[4] = {1, 3, h, w}, outShape[4] = {1, 1, h, w};
    if (!Ok(api->CreateTensorWithDataAsOrtValue(memory, input.data(), input.size() * sizeof(float), inShape, 4,
                                                 ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensor), "Input tensor") ||
        !Ok(api->CreateTensorWithDataAsOrtValue(memory, output.data(), output.size() * sizeof(float), outShape, 4,
                                                 ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &outputTensor), "Output tensor")) return false;
    worker = std::thread([this] {
      const char* inputs[] = {"input"}, *outputs[] = {"mask"};
      for (;;) {
        double at;
        {
          std::unique_lock<std::mutex> lock(mutex);
          wake.wait(lock, [this] { return pending || quit; });
          if (quit) break;
          pending = false;
          at = submittedAt;
        }
        OrtValue* out = outputTensor;
        bool ok = Ok(api->Run(session, nullptr, inputs, (const OrtValue* const*)&inputTensor, 1,
                              outputs, 1, &out), "Run");
        {
          std::lock_guard<std::mutex> lock(mutex);
          if (ok) {
            double uncertainty = 0;
            size_t relevant = 0;
            for (float p : output) if (p >= 0.05f) { uncertainty += 4.f * p * (1.f - p); ++relevant; }
            float confidence = relevant ? 1.f - (float)(uncertainty / relevant) : 1.f;
            float weight = !hasStable || confidence > 0.90f ? 1.f :
                           confidence > 0.70f ? 0.5f : confidence > 0.40f ? 0.2f : 0.f;
            for (size_t i = 0; i < output.size(); ++i) {
              stable[i] += (output[i] - stable[i]) * weight;
              result[i] = stable[i] >= 0.50f ? 255 : 0;
            }
            hasStable = true;
            resultAt = at;
            ready = true;
          }
          busy = false;
        }
      }
    });
    Log("IA mascara: ONNX %dx%d (%s)", w, h, gpu ? "DirectML" : "CPU");
    return true;
  }
};

MaskAI::MaskAI() = default;
MaskAI::~MaskAI() { Stop(); }
void MaskAI::Stop() { state.reset(); }
void MaskAI::Region(int& rx, int& ry, int& rw, int& rh) const {
  if (!state) { rx = ry = rw = rh = 0; return; }
  rx = state->rx; ry = state->ry; rw = state->rw; rh = state->rh;
}

bool MaskAI::Start(const std::string& modelDir, const Library& lib) {
  Stop();
  if (GetFileAttributesW(Widen(modelDir + "/model.onnx").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
  auto next = std::make_unique<State>();
  if (!next->Init(modelDir, lib)) return false;
  state = std::move(next);
  return true;
}

void MaskAI::Submit(const std::vector<uint8_t>& luma4, int mw, int mh, const Library& lib,
                    bool mirror, double captureTime) {
  State* s = state.get();
  if (!s || luma4.size() != (size_t)mw * mh || lib.w != mw || lib.h != mh) return;
  std::lock_guard<std::mutex> lock(s->mutex);
  if (s->busy || captureTime - s->submittedAt < 1.0 / 45.0) return;
  size_t plane = (size_t)s->w * s->h;
  for (int y = 0; y < s->h; ++y) for (int x = 0; x < s->w; ++x) {
    int sx = s->rx + x * 2, sy = s->ry + y * 2;
    if (mirror) sx = mw - sx - 2;
    if (sx < 0 || sy < 0 || sx + 1 >= mw || sy + 1 >= mh) return;
    size_t p = (size_t)sy * mw + sx, q = (size_t)y * s->w + x;
    float v = (luma4[p] + luma4[p + 1] + luma4[p + mw] + luma4[p + mw + 1]) * (1.f / (4.f * 255.f));
    s->input[q] = s->input[plane + q] = s->input[2 * plane + q] = v;
  }
  s->submittedAt = captureTime;
  s->busy = s->pending = true;
  s->wake.notify_one();
}

bool MaskAI::Take(std::vector<uint8_t>& bits, double& captureTime) {
  State* s = state.get();
  if (!s) return false;
  std::lock_guard<std::mutex> lock(s->mutex);
  if (!s->ready) return false;
  s->ready = false;
  size_t active = std::count_if(s->result.begin(), s->result.end(), [](uint8_t v) { return v != 0; });
  if (active < s->result.size() / 200 || active > s->result.size() * 2 / 3) return false;
  bits.assign(((size_t)s->rw * s->rh + 7) / 8, 0);
  for (int y = 0; y < s->rh; ++y) for (int x = 0; x < s->rw; ++x) {
    if (!s->result[(size_t)(y / 2) * s->w + x / 2]) continue;
    size_t i = (size_t)y * s->rw + x;
    bits[i >> 3] |= (uint8_t)(1u << (i & 7));
  }
  captureTime = s->resultAt;
  return true;
}
