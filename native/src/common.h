#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

extern ID3D11Device* gDev;
extern ID3D11DeviceContext* gCtx;
extern std::string gRoot;  // folder holding models/, textures/, native/

[[noreturn]] void Fail(const char* fmt, ...);
void Log(const char* fmt, ...);
#define HR(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) Fail("%s\nfalhou: 0x%08X\n%s:%d", #x, (unsigned)hr_, __FILE__, __LINE__); } while (0)

double Now();                 // seconds on the QPC timeline
double QpcToSec(int64_t qpc);
std::wstring Widen(const std::string& s);
