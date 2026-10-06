#pragma once
#include "common.h"

// ---- GPU helpers
ComPtr<ID3DBlob> CompileHLSL(const char* src, const char* entry, const char* target);
ComPtr<ID3D11PixelShader> MakePS(const char* src, const char* entry);

struct RT {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11ShaderResourceView> srv;
  int w = 0, h = 0, mips = 1;
};
RT MakeRT(int w, int h, DXGI_FORMAT fmt, bool mips = false, UINT samples = 1);
void SetViewport(float x, float y, float w, float h);

// fullscreen triangle + shared states for the 2D passes
struct Passes {
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11SamplerState> linClamp, pointClamp;
  ComPtr<ID3D11BlendState> premulOver, opaque;
  ComPtr<ID3D11RasterizerState> noCull;
  void Init();
  void Draw(ID3D11PixelShader* ps, std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11BlendState* blend);
};
extern Passes gPass;

// WIC decode (png/jpg/webp), cached, mipmapped. srgb picks the _SRGB view.
ID3D11ShaderResourceView* LoadTextureFile(const std::string& path, bool srgb);

// ---- glTF scene
struct Node {
  std::string name;
  int parent = -1;
  XMFLOAT3 t{}, s{1, 1, 1}, t0{}, s0{1, 1, 1};
  XMFLOAT4 r{0, 0, 0, 1}, r0{0, 0, 0, 1};
  XMFLOAT4X4 world{};
};
struct Channel { int node = -1; int path = 0; bool step = false; std::vector<float> times, values; };  // path 0 T, 1 R, 2 S
struct Clip { std::string name; float duration = 0; std::vector<Channel> ch; };
struct Material {
  XMFLOAT4 base{1, 1, 1, 1};
  float metal = 1, rough = 1;
  ID3D11ShaderResourceView *tBase = nullptr, *tMR = nullptr, *tNormal = nullptr;
  bool ao = false;
};
struct Prim {
  ComPtr<ID3D11Buffer> vb, ib;
  UINT count = 0;
  int mat = 0, skin = -1, node = -1;
  std::string mesh;
  bool visible = true, paint = false;
};
struct Skin { std::vector<int> joints; std::vector<XMFLOAT4X4> ibm; };

struct Model {
  std::vector<Node> nodes;
  std::vector<int> order;  // parents before children
  std::vector<Prim> prims;
  std::vector<Material> mats;
  std::vector<Skin> skins;
  std::vector<Clip> clips;
  int Find(const std::string& name) const;
  int FindClip(const std::string& name) const;
  void Reorder();
  void ResetPose();
  void UpdateWorld(const std::vector<int>* follow = nullptr, const Model* src = nullptr);  // follow[i] >= 0: copy src world
};
bool LoadGLB(const std::string& path, Model& m, bool geometry);
Clip Rebind(const Clip& c, const Model& from, const Model& to);
void SampleClip(Model& m, const Clip& c, float t);

// ---- model pass
struct Paint {  // skin compositing, ported from the web viewer
  int mode = 0;      // 0 none, 1 knife mask paint, 2 gun albedo, 3 gun tinted pattern, 4 gun solid color
  int paintMode = 0; // knife: 0 c0, 1 channel mix, 2 pattern rgb
  int tintMode = 0;  // gun pattern: 0 mix(c0,c1,p.r), 1 p.rgb
  float scale = 1, rot = 0, rough = 0.3f, offX = 0, offY = 0;
  float c[4][4]{};
  ID3D11ShaderResourceView *pattern = nullptr, *mask = nullptr;
};
struct Light {
  XMFLOAT3 dir{0.4f, 0.8f, -0.4f};
  float sun = 2, hemi = 1.4f, env = 0.32f;
  ID3D11ShaderResourceView* map = nullptr;  // game image as environment: reflections + ambient (BuildEnv)
  float mapStrength = 1.5f;
  bool mapFlip = false;                     // viewmodel drawn mirrored: the environment flips with it
};
// small blurred, linear copy of the game frame with mips, used as the model's environment
ID3D11ShaderResourceView* BuildEnv(ID3D11ShaderResourceView* screen);
void ModelPassInit();
void ModelPassBegin(const XMFLOAT4X4& viewProj, const Light& light);
// draws meshes of `m`; joint matrices come from `skel` (through remap when given, for rebound glove meshes)
void DrawModel(const Model& m, const Model& skel, const std::vector<int>* remap, const Paint* paint);
