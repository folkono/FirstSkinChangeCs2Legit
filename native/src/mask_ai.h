#pragma once
#include "capture.h"
#include <memory>

// Optional ONNX segmentation of the captured viewmodel. The state-machine mask remains the fallback.
class MaskAI {
  struct State;
  std::unique_ptr<State> state;
public:
  MaskAI();
  ~MaskAI();
  bool Start(const std::string& modelDir, const Library& lib);
  void Stop();
  bool Ready() const { return state != nullptr; }
  void Region(int& rx, int& ry, int& rw, int& rh) const;
  void Submit(const std::vector<uint8_t>& luma4, int mw, int mh, const Library& lib, bool mirror, double captureTime);
  bool Take(std::vector<uint8_t>& bits, double& captureTime);
};
