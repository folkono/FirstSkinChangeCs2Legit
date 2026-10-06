#include "render.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>
#include <unordered_map>
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

Passes gPass;

ComPtr<ID3DBlob> CompileHLSL(const char* src, const char* entry, const char* target) {
  ComPtr<ID3DBlob> code, err;
  HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
  if (FAILED(hr)) Fail("shader %s:\n%s", entry, err ? (const char*)err->GetBufferPointer() : "?");
  return code;
}

ComPtr<ID3D11PixelShader> MakePS(const char* src, const char* entry) {
  auto b = CompileHLSL(src, entry, "ps_5_0");
  ComPtr<ID3D11PixelShader> ps;
  HR(gDev->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &ps));
  return ps;
}

RT MakeRT(int w, int h, DXGI_FORMAT fmt, bool mips, UINT samples) {
  RT r;
  r.w = w; r.h = h;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w; d.Height = h; d.MipLevels = mips ? 0 : 1; d.ArraySize = 1; d.Format = fmt;
  d.SampleDesc.Count = samples; d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
  HR(gDev->CreateTexture2D(&d, nullptr, &r.tex));
  HR(gDev->CreateRenderTargetView(r.tex.Get(), nullptr, &r.rtv));
  HR(gDev->CreateShaderResourceView(r.tex.Get(), nullptr, &r.srv));
  r.tex->GetDesc(&d);
  r.mips = d.MipLevels;
  return r;
}

void SetViewport(float x, float y, float w, float h) {
  D3D11_VIEWPORT vp{x, y, w, h, 0, 1};
  gCtx->RSSetViewports(1, &vp);
}

static const char* kFullscreenVS = R"(
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO VS(uint id : SV_VertexID) {
  VO o; o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o;
})";

void Passes::Init() {
  auto b = CompileHLSL(kFullscreenVS, "VS", "vs_5_0");
  HR(gDev->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs));
  D3D11_SAMPLER_DESC s{};
  s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  s.MaxLOD = D3D11_FLOAT32_MAX;
  HR(gDev->CreateSamplerState(&s, &linClamp));
  s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  HR(gDev->CreateSamplerState(&s, &pointClamp));
  D3D11_BLEND_DESC bd{};
  auto& rt = bd.RenderTarget[0];
  rt.BlendEnable = TRUE;
  rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
  rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
  rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  HR(gDev->CreateBlendState(&bd, &premulOver));
  rt.BlendEnable = FALSE;
  HR(gDev->CreateBlendState(&bd, &opaque));
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
  HR(gDev->CreateRasterizerState(&rd, &noCull));
}

void Passes::Draw(ID3D11PixelShader* ps, std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11BlendState* blend) {
  gCtx->IASetInputLayout(nullptr);
  gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  gCtx->VSSetShader(vs.Get(), nullptr, 0);
  gCtx->PSSetShader(ps, nullptr, 0);
  ID3D11SamplerState* ss[2] = {linClamp.Get(), pointClamp.Get()};
  gCtx->PSSetSamplers(0, 2, ss);
  std::vector<ID3D11ShaderResourceView*> v(srvs);
  gCtx->PSSetShaderResources(0, (UINT)v.size(), v.data());
  gCtx->OMSetBlendState(blend, nullptr, 0xffffffff);
  gCtx->OMSetDepthStencilState(nullptr, 0);
  gCtx->RSSetState(noCull.Get());
  gCtx->Draw(3, 0);
  std::vector<ID3D11ShaderResourceView*> none(v.size(), nullptr);
  gCtx->PSSetShaderResources(0, (UINT)none.size(), none.data());
}

// ---- textures
static std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> gTexCache;

static ID3D11ShaderResourceView* LoadTextureMemory(const std::string& key, const void* bytes, size_t size, bool srgb) {
  auto it = gTexCache.find(key);
  if (it != gTexCache.end()) return it->second.Get();
  static ComPtr<IWICImagingFactory> wic;
  if (!wic) HR(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)));
  ComPtr<IWICStream> st;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> fr;
  ComPtr<IWICFormatConverter> cv;
  HR(wic->CreateStream(&st));
  HR(st->InitializeFromMemory((BYTE*)bytes, (DWORD)size));
  if (FAILED(wic->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) {
    Log("textura ilegivel: %s", key.c_str());
    return gTexCache[key].Get();
  }
  HR(dec->GetFrame(0, &fr));
  HR(wic->CreateFormatConverter(&cv));
  HR(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
  UINT w, h;
  HR(cv->GetSize(&w, &h));
  std::vector<uint8_t> px((size_t)w * h * 4);
  HR(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()));
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w; d.Height = h; d.MipLevels = 0; d.ArraySize = 1;
  d.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
  ComPtr<ID3D11Texture2D> tex;
  HR(gDev->CreateTexture2D(&d, nullptr, &tex));
  gCtx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), w * 4, 0);
  ComPtr<ID3D11ShaderResourceView> srv;
  HR(gDev->CreateShaderResourceView(tex.Get(), nullptr, &srv));
  gCtx->GenerateMips(srv.Get());
  return (gTexCache[key] = srv).Get();
}

ID3D11ShaderResourceView* LoadTextureFile(const std::string& path, bool srgb) {
  std::string key = path + (srgb ? "|s" : "|l");
  auto it = gTexCache.find(key);
  if (it != gTexCache.end()) return it->second.Get();
  HANDLE f = CreateFileW(Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) { Log("textura nao achada: %s", path.c_str()); return gTexCache[key].Get(); }
  std::vector<uint8_t> buf(GetFileSize(f, nullptr));
  DWORD got = 0;
  ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr);
  CloseHandle(f);
  return LoadTextureMemory(key, buf.data(), buf.size(), srgb);
}

// ---- glTF
int Model::Find(const std::string& name) const {
  for (size_t i = 0; i < nodes.size(); i++) if (nodes[i].name == name) return (int)i;
  return -1;
}
int Model::FindClip(const std::string& name) const {
  for (size_t i = 0; i < clips.size(); i++) if (clips[i].name == name) return (int)i;
  return -1;
}
void Model::Reorder() {
  order.clear();
  std::vector<std::vector<int>> kids(nodes.size());
  for (size_t i = 0; i < nodes.size(); i++) if (nodes[i].parent >= 0) kids[nodes[i].parent].push_back((int)i);
  for (size_t i = 0; i < nodes.size(); i++) if (nodes[i].parent < 0) order.push_back((int)i);
  for (size_t k = 0; k < order.size(); k++) for (int c : kids[order[k]]) order.push_back(c);
}
void Model::ResetPose() {
  for (auto& n : nodes) { n.t = n.t0; n.r = n.r0; n.s = n.s0; }
}
void Model::UpdateWorld(const std::vector<int>* follow, const Model* src) {
  for (int i : order) {
    Node& n = nodes[i];
    if (follow && (*follow)[i] >= 0) { n.world = src->nodes[(*follow)[i]].world; continue; }
    XMMATRIX local = XMMatrixScalingFromVector(XMLoadFloat3(&n.s)) * XMMatrixRotationQuaternion(XMLoadFloat4(&n.r)) * XMMatrixTranslationFromVector(XMLoadFloat3(&n.t));
    if (n.parent >= 0) local = local * XMLoadFloat4x4(&nodes[n.parent].world);
    XMStoreFloat4x4(&n.world, local);
  }
}

static ID3D11ShaderResourceView* GltfTexture(const cgltf_data* d, const cgltf_texture* t, const std::string& path, bool srgb) {
  if (!t) return nullptr;
  const cgltf_image* img = t->has_webp && t->webp_image ? t->webp_image : t->image;
  if (!img || !img->buffer_view) return nullptr;
  std::string key = path + "#" + std::to_string(img - d->images) + (srgb ? "|s" : "|l");
  return LoadTextureMemory(key, cgltf_buffer_view_data(img->buffer_view), img->buffer_view->size, srgb);
}

struct Vtx { float p[3], n[3], uv[2]; uint16_t j[4]; float w[4]; };

bool LoadGLB(const std::string& path, Model& m, bool geometry) {
  cgltf_options opt{};
  cgltf_data* d = nullptr;
  if (cgltf_parse_file(&opt, path.c_str(), &d) != cgltf_result_success) return false;
  if (cgltf_load_buffers(&opt, d, path.c_str()) != cgltf_result_success) { cgltf_free(d); return false; }

  m.nodes.resize(d->nodes_count);
  for (size_t i = 0; i < d->nodes_count; i++) {
    const cgltf_node& cn = d->nodes[i];
    Node& n = m.nodes[i];
    n.name = cn.name ? cn.name : "";
    n.parent = cn.parent ? (int)cgltf_node_index(d, cn.parent) : -1;
    if (cn.has_matrix) {
      XMVECTOR s, r, t;
      XMMatrixDecompose(&s, &r, &t, XMLoadFloat4x4((const XMFLOAT4X4*)cn.matrix));
      XMStoreFloat3(&n.s, s); XMStoreFloat4(&n.r, r); XMStoreFloat3(&n.t, t);
    } else {
      if (cn.has_translation) n.t = {cn.translation[0], cn.translation[1], cn.translation[2]};
      if (cn.has_rotation) n.r = {cn.rotation[0], cn.rotation[1], cn.rotation[2], cn.rotation[3]};
      if (cn.has_scale) n.s = {cn.scale[0], cn.scale[1], cn.scale[2]};
    }
    n.t0 = n.t; n.r0 = n.r; n.s0 = n.s;
  }
  m.Reorder();

  for (size_t i = 0; i < d->skins_count; i++) {
    const cgltf_skin& cs = d->skins[i];
    Skin s;
    for (size_t j = 0; j < cs.joints_count; j++) s.joints.push_back((int)cgltf_node_index(d, cs.joints[j]));
    s.ibm.resize(cs.joints_count);
    for (size_t j = 0; j < cs.joints_count; j++) {
      XMStoreFloat4x4(&s.ibm[j], XMMatrixIdentity());
      if (cs.inverse_bind_matrices) cgltf_accessor_read_float(cs.inverse_bind_matrices, j, &s.ibm[j]._11, 16);
    }
    m.skins.push_back(std::move(s));
  }

  if (geometry) {
    for (size_t i = 0; i < d->materials_count; i++) {
      const cgltf_material& cm = d->materials[i];
      Material mt;
      if (cm.has_pbr_metallic_roughness) {
        auto& p = cm.pbr_metallic_roughness;
        mt.base = {p.base_color_factor[0], p.base_color_factor[1], p.base_color_factor[2], p.base_color_factor[3]};
        mt.metal = p.metallic_factor; mt.rough = p.roughness_factor;
        mt.tBase = GltfTexture(d, p.base_color_texture.texture, path, true);
        mt.tMR = GltfTexture(d, p.metallic_roughness_texture.texture, path, false);
        mt.ao = cm.occlusion_texture.texture && cm.occlusion_texture.texture == p.metallic_roughness_texture.texture;
      }
      mt.tNormal = GltfTexture(d, cm.normal_texture.texture, path, false);
      m.mats.push_back(mt);
    }
    m.mats.push_back(Material{});  // fallback for primitives without material
    for (size_t ni = 0; ni < d->nodes_count; ni++) {
      const cgltf_node& cn = d->nodes[ni];
      if (!cn.mesh) continue;
      for (size_t pi = 0; pi < cn.mesh->primitives_count; pi++) {
        const cgltf_primitive& cp = cn.mesh->primitives[pi];
        if (cp.type != cgltf_primitive_type_triangles) continue;
        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *jnt = nullptr, *wgt = nullptr;
        for (size_t a = 0; a < cp.attributes_count; a++) {
          const auto& at = cp.attributes[a];
          if (at.type == cgltf_attribute_type_position) pos = at.data;
          else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
          else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
          else if (at.type == cgltf_attribute_type_joints && at.index == 0) jnt = at.data;
          else if (at.type == cgltf_attribute_type_weights && at.index == 0) wgt = at.data;
        }
        if (!pos) continue;
        bool skinned = cn.skin && jnt && wgt;
        std::vector<Vtx> v(pos->count);
        for (size_t k = 0; k < pos->count; k++) {
          Vtx& x = v[k];
          cgltf_accessor_read_float(pos, k, x.p, 3);
          if (nrm) cgltf_accessor_read_float(nrm, k, x.n, 3); else x.n[0] = x.n[2] = 0, x.n[1] = 1;
          if (uv) cgltf_accessor_read_float(uv, k, x.uv, 2); else x.uv[0] = x.uv[1] = 0;
          cgltf_uint j[4] = {0, 0, 0, 0};
          float w[4] = {1, 0, 0, 0};
          if (skinned) { cgltf_accessor_read_uint(jnt, k, j, 4); cgltf_accessor_read_float(wgt, k, w, 4); }
          for (int c = 0; c < 4; c++) { x.j[c] = (uint16_t)j[c]; x.w[c] = w[c]; }
        }
        std::vector<uint32_t> idx;
        if (cp.indices) { idx.resize(cp.indices->count); for (size_t k = 0; k < idx.size(); k++) idx[k] = (uint32_t)cgltf_accessor_read_index(cp.indices, k); }
        else { idx.resize(v.size()); for (size_t k = 0; k < idx.size(); k++) idx[k] = (uint32_t)k; }
        Prim pr;
        D3D11_BUFFER_DESC bd{(UINT)(v.size() * sizeof(Vtx)), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER};
        D3D11_SUBRESOURCE_DATA sd{v.data()};
        HR(gDev->CreateBuffer(&bd, &sd, &pr.vb));
        bd = {(UINT)(idx.size() * 4), D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER};
        sd = {idx.data()};
        HR(gDev->CreateBuffer(&bd, &sd, &pr.ib));
        pr.count = (UINT)idx.size();
        pr.mat = cp.material ? (int)cgltf_material_index(d, cp.material) : (int)m.mats.size() - 1;
        pr.skin = skinned ? (int)cgltf_skin_index(d, cn.skin) : -1;
        pr.node = (int)ni;
        pr.mesh = cn.mesh->name ? cn.mesh->name : (cn.name ? cn.name : "");
        m.prims.push_back(std::move(pr));
      }
    }
  }

  for (size_t ai = 0; ai < d->animations_count; ai++) {
    const cgltf_animation& ca = d->animations[ai];
    Clip c;
    c.name = ca.name ? ca.name : "";
    for (size_t ci = 0; ci < ca.channels_count; ci++) {
      const cgltf_animation_channel& ch = ca.channels[ci];
      if (!ch.target_node || !ch.sampler) continue;
      int path = ch.target_path == cgltf_animation_path_type_translation ? 0 : ch.target_path == cgltf_animation_path_type_rotation ? 1 : ch.target_path == cgltf_animation_path_type_scale ? 2 : -1;
      if (path < 0) continue;
      Channel o;
      o.node = (int)cgltf_node_index(d, ch.target_node);
      o.path = path;
      o.step = ch.sampler->interpolation == cgltf_interpolation_type_step;
      const cgltf_accessor* in = ch.sampler->input;
      const cgltf_accessor* out = ch.sampler->output;
      int comps = path == 1 ? 4 : 3;
      bool cubic = ch.sampler->interpolation == cgltf_interpolation_type_cubic_spline;
      o.times.resize(in->count);
      for (size_t k = 0; k < in->count; k++) cgltf_accessor_read_float(in, k, &o.times[k], 1);
      o.values.resize(in->count * comps);
      for (size_t k = 0; k < in->count; k++) cgltf_accessor_read_float(out, cubic ? k * 3 + 1 : k, &o.values[k * comps], comps);
      if (!o.times.empty()) c.duration = std::max(c.duration, o.times.back());
      c.ch.push_back(std::move(o));
    }
    if (c.duration <= 0) c.duration = 1;  // single-pose clips (idle) loop as a 1s hold
    m.clips.push_back(std::move(c));
  }
  cgltf_free(d);
  return true;
}

Clip Rebind(const Clip& c, const Model& from, const Model& to) {
  Clip r;
  r.name = c.name;
  r.duration = c.duration;
  for (const auto& ch : c.ch) {
    int n = to.Find(from.nodes[ch.node].name);
    if (n < 0) continue;
    Channel x = ch;
    x.node = n;
    r.ch.push_back(std::move(x));
  }
  return r;
}

void SampleClip(Model& m, const Clip& c, float t) {
  for (const auto& ch : c.ch) {
    size_t n = ch.times.size();
    if (!n) continue;
    size_t k1 = std::upper_bound(ch.times.begin(), ch.times.end(), t) - ch.times.begin();
    size_t k0 = k1 ? k1 - 1 : 0;
    if (k1 >= n) k1 = n - 1;
    float u = 0;
    if (!ch.step && k1 != k0) u = std::clamp((t - ch.times[k0]) / (ch.times[k1] - ch.times[k0]), 0.f, 1.f);
    Node& nd = m.nodes[ch.node];
    if (ch.path == 1) {
      XMVECTOR a = XMLoadFloat4((const XMFLOAT4*)&ch.values[k0 * 4]), b = XMLoadFloat4((const XMFLOAT4*)&ch.values[k1 * 4]);
      XMStoreFloat4(&nd.r, XMQuaternionNormalize(XMQuaternionSlerp(a, b, u)));
    } else {
      XMVECTOR a = XMLoadFloat3((const XMFLOAT3*)&ch.values[k0 * 3]), b = XMLoadFloat3((const XMFLOAT3*)&ch.values[k1 * 3]);
      XMStoreFloat3(ch.path == 0 ? &nd.t : &nd.s, XMVectorLerp(a, b, u));
    }
  }
}

// ---- environment from the game frame
static const char* kEnvHLSL = R"(
Texture2D t0 : register(t0); SamplerState sLin : register(s0);
float4 PS(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {  // 4x4 taps per texel, sRGB -> linear
  float3 c = 0;
  [unroll] for (int i = 0; i < 16; i++) c += t0.SampleLevel(sLin, uv + (float2(i % 4, i / 4) - 1.5) / float2(1024, 576), 0).rgb;
  return float4(pow(c / 16, 2.2), 1);
}
)";

ID3D11ShaderResourceView* BuildEnv(ID3D11ShaderResourceView* screen) {
  static RT env;
  static ComPtr<ID3D11PixelShader> ps;
  if (!ps) { env = MakeRT(256, 144, DXGI_FORMAT_R16G16B16A16_FLOAT, true); ps = MakePS(kEnvHLSL, "PS"); }
  ID3D11ShaderResourceView* none = nullptr;
  gCtx->PSSetShaderResources(5, 1, &none);  // the model pass may still have it bound
  gCtx->OMSetRenderTargets(1, env.rtv.GetAddressOf(), nullptr);
  SetViewport(0, 0, 256, 144);
  gPass.Draw(ps.Get(), {screen}, gPass.opaque.Get());
  gCtx->GenerateMips(env.srv.Get());
  return env.srv.Get();
}

// ---- model pass
static const char* kModelHLSL = R"(
cbuffer Frame : register(b0) {
  row_major float4x4 viewProj;
  float3 sunDir; float sunI;
  float3 sky; float hemiI;
  float3 ground; float envI;
  float4 envInfo;  // on, mip count, strength, mirrored
};
cbuffer Bones : register(b1) { row_major float4x4 bones[128]; };
cbuffer Mat : register(b2) {
  float4 baseFactor;
  float metalFactor, roughFactor, hasBase, hasMR;
  float hasNormal, hasAO, skinMode, paintMode;
  float pScale, pRot, pRough, tintMode;
  float2 pOff; float2 pad;
  float4 pc[4];
};
Texture2D tBase : register(t0); Texture2D tMR : register(t1); Texture2D tNrm : register(t2);
Texture2D tPat : register(t3); Texture2D tMask : register(t4); Texture2D tEnv : register(t5);
SamplerState sWrap : register(s0); SamplerState sClamp : register(s1);

struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; uint4 j : BLENDINDICES; float4 w : BLENDWEIGHT; };
struct VO { float4 pos : SV_Position; float3 wp : TEXCOORD1; float3 n : TEXCOORD2; float2 uv : TEXCOORD0; };

VO VS(VI i) {
  float4 w = i.w / max(dot(i.w, 1), 1e-5);
  float4x4 m = bones[i.j.x] * w.x + bones[i.j.y] * w.y + bones[i.j.z] * w.z + bones[i.j.w] * w.w;
  VO o;
  float4 wp = mul(float4(i.p, 1), m);
  o.wp = wp.xyz; o.pos = mul(wp, viewProj); o.n = mul(i.n, (float3x3)m); o.uv = i.uv;
  return o;
}

static const float PI = 3.14159265;
// cotangent frame from screen derivatives (meshes ship without tangents); ddy flipped to match GL conventions
float3 Perturb(float3 N, float3 p, float2 uv, float3 tn) {
  float3 dp1 = ddx(p), dp2 = -ddy(p);
  float2 du1 = ddx(uv), du2 = -ddy(uv);
  float3 d2 = cross(dp2, N), d1 = cross(N, dp1);
  float3 T = d2 * du1.x + d1 * du2.x;
  float3 B = d2 * du1.y + d1 * du2.y;
  float inv = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
  return normalize(T * inv * tn.x + B * inv * tn.y + N * tn.z);
}

// direction (viewmodel space: +X camera-left, +Y up, +Z ahead) -> where the game screen shows that way
float2 EnvUV(float3 d) {
  float2 uv = 0.5 - 0.5 * normalize(d).xy;
  if (envInfo.w > 0) uv.x = 1 - uv.x;
  return uv;
}

float4 PS(VO i) : SV_Target {
  float3 N = normalize(i.n);
  float4 base = baseFactor;
  if (hasBase > 0) base *= tBase.Sample(sWrap, i.uv);
  float rough = roughFactor, metal = metalFactor, ao = 1;
  if (hasMR > 0) { float4 mr = tMR.Sample(sWrap, i.uv); rough *= mr.g; metal *= mr.b; if (hasAO > 0) ao = mr.r; }
  if (hasNormal > 0) { float3 tn = tNrm.Sample(sWrap, i.uv).xyz * 2 - 1; tn.y = -tn.y; N = Perturb(N, i.wp, i.uv, tn); }

  if (skinMode == 1) {  // knife: paint inside the red-channel blade mask
    float cov = tMask.Sample(sClamp, i.uv).r;
    float2 q = (i.uv - 0.5) * pScale;
    float cs = cos(pRot), sn = sin(pRot);
    q = float2(cs * q.x + sn * q.y, -sn * q.x + cs * q.y) + 0.5 + pOff;
    float4 kp = tPat.Sample(sWrap, q);
    float3 paint = paintMode == 0 ? pc[0].rgb : paintMode == 1 ? lerp(lerp(lerp(pc[0].rgb, pc[1].rgb, kp.r), pc[2].rgb, kp.g), pc[3].rgb, kp.b) : kp.rgb;
    base.rgb = lerp(base.rgb, paint, cov);
    rough = lerp(rough, pRough, cov);
    metal = lerp(metal, 0.9, cov);
  } else if (skinMode == 2) {
    base = tPat.Sample(sWrap, i.uv);
  } else if (skinMode == 3) {
    float4 p = tPat.Sample(sWrap, i.uv * pScale);
    base.rgb = baseFactor.rgb * (tintMode == 0 ? lerp(pc[0].rgb, pc[1].rgb, p.r) : p.rgb);
  } else if (skinMode == 4) {
    base.rgb *= pc[0].rgb / max(baseFactor.rgb, 1e-4);
  }

  float3 V = normalize(-i.wp), L = sunDir, H = normalize(L + V);
  float NdL = saturate(dot(N, L)), NdV = saturate(dot(N, V)) + 1e-4, NdH = saturate(dot(N, H)), VdH = saturate(dot(V, H));
  rough = clamp(rough, 0.04, 1);
  float a = rough * rough, a2 = a * a;
  float3 F0 = lerp(0.04, base.rgb, metal);
  float dd = NdH * NdH * (a2 - 1) + 1;
  float D = a2 / (PI * dd * dd);
  float k = a * 0.5;
  float G = (NdL / (NdL * (1 - k) + k)) * (NdV / (NdV * (1 - k) + k));
  float3 F = F0 + (1 - F0) * pow(1 - VdH, 5);
  float3 spec = D * G * F / (4 * NdL * NdV + 1e-4);
  float3 diff = base.rgb * (1 - metal);
  float3 direct = ((1 - F) * diff / PI + spec) * sunI * NdL;
  float3 hemi = lerp(ground, sky, N.y * 0.5 + 0.5) * hemiI;
  float3 R = reflect(-V, N);
  float3 envSpec = lerp(ground, sky, R.y * 0.5 + 0.5) * envI * 2.5;
  float3 Fr = F0 + (max(1 - rough, F0) - F0) * pow(1 - NdV, 5);
  float3 ambient = (hemi / PI * diff + envI * 0.8 * diff + envSpec * Fr * (1 - rough * 0.6)) * ao;
  if (envInfo.x > 0) {  // the map itself lights the model and shows up in its reflections
    float3 irr = tEnv.SampleLevel(sClamp, EnvUV(N), envInfo.y - 3).rgb * envInfo.z;
    float3 rad = tEnv.SampleLevel(sClamp, EnvUV(R), rough * (envInfo.y - 2)).rgb * envInfo.z * 2;
    ambient = (irr * diff + rad * Fr) * ao;
  }
  return float4(direct + ambient, 1);
}
)";

static ComPtr<ID3D11VertexShader> gModelVS;
static ComPtr<ID3D11PixelShader> gModelPS;
static ComPtr<ID3D11InputLayout> gModelIL;
static ComPtr<ID3D11Buffer> gFrameCB, gBonesCB, gMatCB;
static ComPtr<ID3D11SamplerState> gWrap, gClamp;
static ComPtr<ID3D11DepthStencilState> gDepth;
static ComPtr<ID3D11RasterizerState> gRaster;
static ComPtr<ID3D11BlendState> gOpaque;

struct FrameCB { XMFLOAT4X4 viewProj; XMFLOAT3 sunDir; float sunI; XMFLOAT3 sky; float hemiI; XMFLOAT3 ground; float envI; XMFLOAT4 env; };
struct MatCB {
  XMFLOAT4 base;
  float metal, rough, hasBase, hasMR;
  float hasNormal, hasAO, skinMode, paintMode;
  float pScale, pRot, pRough, tintMode;
  float pOff[2], pad[2];
  float pc[4][4];
};

static ComPtr<ID3D11Buffer> MakeCB(UINT size) {
  D3D11_BUFFER_DESC d{(size + 15) & ~15u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE};
  ComPtr<ID3D11Buffer> b;
  HR(gDev->CreateBuffer(&d, nullptr, &b));
  return b;
}
static void Upload(ID3D11Buffer* b, const void* data, size_t size) {
  D3D11_MAPPED_SUBRESOURCE ms;
  HR(gCtx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms));
  memcpy(ms.pData, data, size);
  gCtx->Unmap(b, 0);
}

void ModelPassInit() {
  auto vsb = CompileHLSL(kModelHLSL, "VS", "vs_5_0");
  HR(gDev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &gModelVS));
  gModelPS = MakePS(kModelHLSL, "PS");
  D3D11_INPUT_ELEMENT_DESC il[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vtx, p), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vtx, n), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vtx, uv), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, offsetof(Vtx, j), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vtx, w), D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  HR(gDev->CreateInputLayout(il, 5, vsb->GetBufferPointer(), vsb->GetBufferSize(), &gModelIL));
  gFrameCB = MakeCB(sizeof(FrameCB));
  gBonesCB = MakeCB(sizeof(XMFLOAT4X4) * 128);
  gMatCB = MakeCB(sizeof(MatCB));
  D3D11_SAMPLER_DESC s{};
  s.Filter = D3D11_FILTER_ANISOTROPIC; s.MaxAnisotropy = 8;
  s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  s.MaxLOD = D3D11_FLOAT32_MAX;
  HR(gDev->CreateSamplerState(&s, &gWrap));
  s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  HR(gDev->CreateSamplerState(&s, &gClamp));
  D3D11_DEPTH_STENCIL_DESC dd{};
  dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
  HR(gDev->CreateDepthStencilState(&dd, &gDepth));
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
  HR(gDev->CreateRasterizerState(&rd, &gRaster));
  D3D11_BLEND_DESC bd{};
  bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  HR(gDev->CreateBlendState(&bd, &gOpaque));
}

static XMFLOAT3 SrgbToLinear(float r, float g, float b) {
  auto f = [](float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); };
  return {f(r), f(g), f(b)};
}

void ModelPassBegin(const XMFLOAT4X4& viewProj, const Light& light) {
  FrameCB f{};
  f.viewProj = viewProj;
  XMStoreFloat3(&f.sunDir, XMVector3Normalize(XMLoadFloat3(&light.dir)));
  f.sunI = light.sun;
  f.sky = {1, 1, 1};
  f.ground = SrgbToLinear(0x66 / 255.f, 0x67 / 255.f, 0x5b / 255.f);
  f.hemiI = light.hemi;
  f.envI = light.env;
  if (light.map) {
    ComPtr<ID3D11Resource> r;
    light.map->GetResource(&r);
    D3D11_TEXTURE2D_DESC d;
    ((ID3D11Texture2D*)r.Get())->GetDesc(&d);
    f.env = {1, (float)d.MipLevels, light.mapStrength, light.mapFlip ? 1.f : 0.f};
  }
  Upload(gFrameCB.Get(), &f, sizeof f);
  gCtx->PSSetShaderResources(5, 1, &light.map);
  gCtx->IASetInputLayout(gModelIL.Get());
  gCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  gCtx->VSSetShader(gModelVS.Get(), nullptr, 0);
  gCtx->PSSetShader(gModelPS.Get(), nullptr, 0);
  ID3D11Buffer* cbs[3] = {gFrameCB.Get(), gBonesCB.Get(), gMatCB.Get()};
  gCtx->VSSetConstantBuffers(0, 3, cbs);
  gCtx->PSSetConstantBuffers(0, 3, cbs);
  ID3D11SamplerState* ss[2] = {gWrap.Get(), gClamp.Get()};
  gCtx->PSSetSamplers(0, 2, ss);
  gCtx->OMSetDepthStencilState(gDepth.Get(), 0);
  gCtx->OMSetBlendState(gOpaque.Get(), nullptr, 0xffffffff);
  gCtx->RSSetState(gRaster.Get());
}

void DrawModel(const Model& m, const Model& skel, const std::vector<int>* remap, const Paint* paint) {
  static XMFLOAT4X4 bones[128];
  for (const Prim& p : m.prims) {
    if (!p.visible) continue;
    if (p.skin >= 0) {
      const Skin& s = m.skins[p.skin];
      size_t n = std::min<size_t>(s.joints.size(), 128);
      for (size_t k = 0; k < n; k++) {
        int jn = s.joints[k];
        const XMFLOAT4X4* w = remap && (*remap)[jn] >= 0 ? &skel.nodes[(*remap)[jn]].world : &m.nodes[jn].world;
        XMStoreFloat4x4(&bones[k], XMLoadFloat4x4(&s.ibm[k]) * XMLoadFloat4x4(w));
      }
    } else {
      bones[0] = m.nodes[p.node].world;
    }
    Upload(gBonesCB.Get(), bones, sizeof bones);

    const Material& mt = m.mats[p.mat];
    MatCB c{};
    c.base = mt.base; c.metal = mt.metal; c.rough = mt.rough;
    c.hasBase = mt.tBase ? 1.f : 0.f; c.hasMR = mt.tMR ? 1.f : 0.f; c.hasNormal = mt.tNormal ? 1.f : 0.f; c.hasAO = mt.ao ? 1.f : 0.f;
    ID3D11ShaderResourceView* srv[5] = {mt.tBase, mt.tMR, mt.tNormal, nullptr, nullptr};
    if (paint && paint->mode && p.paint) {
      c.skinMode = (float)paint->mode; c.paintMode = (float)paint->paintMode; c.tintMode = (float)paint->tintMode;
      c.pScale = paint->scale; c.pRot = paint->rot; c.pRough = paint->rough; c.pOff[0] = paint->offX; c.pOff[1] = paint->offY;
      memcpy(c.pc, paint->c, sizeof c.pc);
      srv[3] = paint->pattern ? paint->pattern : paint->mask;
      srv[4] = paint->mask;
      if (paint->mode == 2) { c.base = {1, 1, 1, 1}; }
    }
    Upload(gMatCB.Get(), &c, sizeof c);
    gCtx->PSSetShaderResources(0, 5, srv);
    UINT stride = sizeof(Vtx), off = 0;
    gCtx->IASetVertexBuffers(0, 1, p.vb.GetAddressOf(), &stride, &off);
    gCtx->IASetIndexBuffer(p.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    gCtx->DrawIndexed(p.count, 0, 0);
  }
}
