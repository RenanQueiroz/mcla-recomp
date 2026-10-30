// msaa_line_pso.cpp : self-contained D3D12 test (no game data) for the sample-position problem of AMD's D3D12 driver with a LINE-topology
// pipeline state (RasterizerState.MultisampleEnable = FALSE, "aliased lines") on a 2x or 4x multisampled render target (fork issue #28).
//
// What it does: every test case records one command list on a 1280x720 2x (default) or 4x MSAA R16G16B16A16F target + D32_S8 depth, binds PSOs in
// some order (and optionally draws a line list), then draws two slanted-edge quads with a plain triangle PSO and measures the effective hardware
// sample positions from the coverage of those quads (printed in 1/16 pixel from the pixel's corner; the D3D standard patterns are 2x (12,12) (4,4)
// and 4x (6,2) (14,6) (2,10) (10,14); "all samples at the pixel centre" is (8,8)). It also counts the samples a short line draw covers ("line px").
//
// RESULTS (2026-10-30, Windows 11 26300, AMD Radeon(TM) Graphics driver 32.00.21045.16010; RTX 5080 and WARP: standard pattern in every case,
//   WARP except where it keeps explicit sample positions from case 18 for the later lists, so 18 runs last):
//   1. 2x and 4x alike, the Radeon leaves all samples at the pixel centre for the triangles that follow an aliased line PSO (or a wireframe triangle PSO,
//      W1) when that PSO is bound while the previous graphics PSO of the list has another sample count, or there is none (cases 1-4, 7, 8, 11, 12, 16, 17,
//      19-23, 26, E1-E3, E6, E8, F0-F2, F5-F9, F11, G1-G5, L1, W1). It does not matter whether a line is drawn, what the line PSO's other state is (blend, depth,
//      stencil, cull, a clip() in the pixel shader, SV_Position), or what happens in between (barriers, clears, re-binding the targets, a compute PSO,
//      a triangle PSO with MultisampleEnable). A 1-sample line PSO and a point PSO do not do it (G6, G7).
//   2. It does not happen when the previous graphics PSO has the line PSO's sample count (cases 24, 25, 27, E4, E7, E11, F3, F4, F10, V1-V5): the
//      driver seems to set the sample positions only when the sample count changes between two PSOs, and sets the centre for an aliased line PSO.
//      A later PSO of the same count does not undo it (23); a PSO of another count does (E5, E10). A list that starts with a compute
//      PSO has no graphics PSO yet (26).
//   3. Other cures: MultisampleEnable = TRUE on the line PSO (5, 6, 9, 10); SetSamplePositions(0,0,nullptr) after it (14, 15).
//   4. The guard (cases H*, V*): a triangle PSO with the line PSO's sample count, bound right before it, whatever came before: every sequence is standard.
//      The guard needs one render target format (R8G8B8A8_UNORM alone, R16G16B16A16_FLOAT alone, a depth format alone or both all work), no pixel shader,
//      and a sample count equal to the line PSO's; a PSO without outputs (sample count 0) or with 1 sample does not do (F1, F2). The line pixels
//      ("line px") are the same with the guard as in the sequence that was always fine (L2, H13), and differ from the broken one (L1) by a few
//      samples at 4x only.
//
// Build (llvm-mingw):  x86_64-w64-mingw32-clang++ -O1 -std=c++17 msaa_line_pso.cpp -o msaa_line_pso.exe -ld3d12 -ldxgi -ld3dcompiler -luuid -static
// Run:                 msaa_line_pso.exe [adapter name substring] [2|4]      (default: first hardware adapter, 2 samples; "Basic" selects WARP;
//                      REPRO_DEBUG=1 turns the D3D12 debug layer on and prints its messages)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>
using Microsoft::WRL::ComPtr;

static void DumpLayerMessages();
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED 0x%08X at line %d: %s\n", (unsigned)hr_, __LINE__, #x); DumpLayerMessages(); exit(1); } } while (0)

static const UINT W = 1280, H = 720;
static UINT g_ns = 2;   // samples per pixel of the target: 2 (default) or 4, argv[2]
static ComPtr<ID3D12Device> dev;
static ComPtr<ID3D12CommandQueue> q;
static ComPtr<ID3D12Fence> fence;
static UINT64 fenceVal = 0;
static ComPtr<ID3D12Resource> rt, ds, uavTex, readback;
static ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap, cbvHeap;
static ComPtr<ID3D12RootSignature> rsEmpty, rsOther, rsCompute;
static ComPtr<ID3D12PipelineState> g_start, g_start2;
// The per-line-PSO guard (what the SDK patch does): when g_guard is on, PreLine binds the guard PSO of a line PSO's sample count right before it.
struct LineGuard { ID3D12PipelineState* line; ID3D12PipelineState* guard; };
static std::vector<LineGuard> g_lineGuards; static bool g_guard = false;
static void BindPso(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* p)
{
    if (g_guard) for (auto& g : g_lineGuards) if (g.line == p) cl->SetPipelineState(g.guard);
    cl->SetPipelineState(p);
}   // when set, bound as the very first command of every test list (the fix)
static ComPtr<ID3D12PipelineState> psoDump;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp; static UINT64 rowSize, total; static UINT rows;
static void DumpLayerMessages()
{
    if (!getenv("REPRO_DEBUG") || !dev) return;
    ComPtr<ID3D12InfoQueue> iq; if (FAILED(dev.As(&iq))) return;
    UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && i < 40; ++i) { SIZE_T len = 0; iq->GetMessage(i, nullptr, &len); std::vector<char> m(len); D3D12_MESSAGE* msg = (D3D12_MESSAGE*)m.data(); iq->GetMessage(i, msg, &len); printf("layer: [sev %d] %s\n", (int)msg->Severity, msg->pDescription); }
}

static ComPtr<ID3DBlob> Compile(const char* src, const char* target)
{
    ComPtr<ID3DBlob> b, e;
    HRESULT hr = D3DCompile(src, strlen(src), "x", nullptr, nullptr, "main", target, 0, 0, &b, &e);
    if (FAILED(hr)) { printf("shader compile failed: %s\n", e ? (const char*)e->GetBufferPointer() : "?"); exit(1); }
    return b;
}

static const char* VS_QUADS =
    "static const float2 P[12] = { float2(100,0), float2(111.25,720), float2(400,720), float2(100,0), float2(400,720), float2(400,0),\n"
    "  float2(600,300), float2(1200,309.375), float2(1200,719), float2(600,300), float2(1200,719), float2(600,719) };\n"
    "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = P[id]; return float4(p.x / 640.0 - 1.0, 1.0 - p.y / 360.0, 0.5, 1.0); }\n";
static const char* VS_LINES =
    "float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2(450 + 100 * (id & 1), 100.3 + 10 * (id >> 1) + 7 * (id & 1)); return float4(p.x / 640.0 - 1.0, 1.0 - p.y / 360.0, 0.5, 1.0); }\n";
static const char* PS_CONST = "float4 main(float4 pos : SV_Position) : SV_Target { return float4(10, 10, 10, 10); }\n";
static const char* PS_DISCARD = "float4 main(float4 pos : SV_Position) : SV_Target { clip(pos.x - 0.5); return float4(10, 10, 10, 10); }\n";
static const char* PS_USEPOS = "float4 main(float4 pos : SV_Position) : SV_Target { return float4(10, 10, 10, 10) * (1.0 + 0.0 * pos.x); }\n";

struct PsoOpt { D3D12_PRIMITIVE_TOPOLOGY_TYPE topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; bool msaaEnable = false; bool usePos = false; bool depth = false; D3D12_CULL_MODE cull = D3D12_CULL_MODE_NONE; bool ccw = false; bool lines = false; bool aaLine = false;
                  bool blend = false; bool discardPs = false;                       // fix-shape variants and the line PSOs of the game that did not trigger
                  bool dummy = false; int samples = -1;   /* -1: g_ns */ bool dummyRt = false; bool dummyRtOnly = false; bool otherRs = false;
                  bool wire = false; DXGI_FORMAT rtfOverride = DXGI_FORMAT_UNKNOWN, dsfOverride = DXGI_FORMAT_UNKNOWN; };   // dummy: no PS, no render target, no depth (the list-start PSO of the fix)

static ComPtr<ID3D12PipelineState> MakePSO(const PsoOpt& o)
{
    auto vs = Compile(o.lines ? VS_LINES : VS_QUADS, "vs_5_0");
    auto ps = Compile(o.usePos ? PS_USEPOS : (o.discardPs ? PS_DISCARD : PS_CONST), "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = (o.otherRs ? rsOther : rsEmpty).Get();
    d.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    if (!o.dummy) d.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = 0xF;
    if (o.blend) { auto& b = d.BlendState.RenderTarget[0]; b.BlendEnable = TRUE; b.SrcBlend = D3D12_BLEND_SRC_ALPHA; b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; b.BlendOp = D3D12_BLEND_OP_ADD; b.SrcBlendAlpha = D3D12_BLEND_ONE; b.DestBlendAlpha = D3D12_BLEND_ZERO; b.BlendOpAlpha = D3D12_BLEND_OP_ADD; }
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = o.wire ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID; d.RasterizerState.CullMode = o.cull; d.RasterizerState.FrontCounterClockwise = o.ccw ? TRUE : FALSE;
    d.RasterizerState.DepthClipEnable = TRUE; d.RasterizerState.MultisampleEnable = o.msaaEnable ? TRUE : FALSE; d.RasterizerState.AntialiasedLineEnable = o.aaLine ? TRUE : FALSE;
    d.DepthStencilState.DepthEnable = o.depth ? TRUE : FALSE; d.DepthStencilState.DepthWriteMask = o.depth ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO; d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
    d.DepthStencilState.StencilEnable = o.depth ? TRUE : FALSE; d.DepthStencilState.StencilReadMask = 255; d.DepthStencilState.StencilWriteMask = 255;
    d.DepthStencilState.FrontFace = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS }; d.DepthStencilState.BackFace = d.DepthStencilState.FrontFace;
    d.PrimitiveTopologyType = o.topo;
    bool hasRt = !o.dummy || o.dummyRt || o.dummyRtOnly || o.rtfOverride != DXGI_FORMAT_UNKNOWN;
    bool hasDs = !o.dummy || o.dummyRt || o.dsfOverride != DXGI_FORMAT_UNKNOWN;
    if (hasRt) { d.NumRenderTargets = 1; d.RTVFormats[0] = o.rtfOverride != DXGI_FORMAT_UNKNOWN ? o.rtfOverride : DXGI_FORMAT_R16G16B16A16_FLOAT; }
    if (hasDs) d.DSVFormat = o.dsfOverride != DXGI_FORMAT_UNKNOWN ? o.dsfOverride : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    d.SampleDesc = { (UINT)(o.samples < 0 ? (int)g_ns : o.samples), 0 };
    ComPtr<ID3D12PipelineState> p; CK(dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&p)));
    return p;
}

static void Wait()
{
    CK(q->Signal(fence.Get(), ++fenceVal));
    if (fence->GetCompletedValue() < fenceVal) { HANDLE e = CreateEvent(nullptr, FALSE, FALSE, nullptr); CK(fence->SetEventOnCompletion(fenceVal, e)); WaitForSingleObject(e, INFINITE); CloseHandle(e); }
}

static D3D12_RESOURCE_BARRIER Tr(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x = {}; x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource = r; x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; x.Transition.StateBefore = a; x.Transition.StateAfter = b; return x;
}

static void Setup(IDXGIAdapter* ad)
{
    CK(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
    D3D12_COMMAND_QUEUE_DESC qd = {}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT; CK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    CK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = W; rd.Height = 1024; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; rd.SampleDesc = { g_ns, 0 };
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {}; cv.Format = rd.Format;
    CK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt)));
    D3D12_RESOURCE_DESC dd = rd; dd.Format = DXGI_FORMAT_R32G8X24_TYPELESS; dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE dcv = {}; dcv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dcv.DepthStencil = { 0.0f, 0 };
    CK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_PPV_ARGS(&ds)));
    D3D12_DESCRIPTOR_HEAP_DESC h1 = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 }; CK(dev->CreateDescriptorHeap(&h1, IID_PPV_ARGS(&rtvHeap)));
    D3D12_DESCRIPTOR_HEAP_DESC h2 = { D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 }; CK(dev->CreateDescriptorHeap(&h2, IID_PPV_ARGS(&dsvHeap)));
    D3D12_RENDER_TARGET_VIEW_DESC rv = {}; rv.Format = rd.Format; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS; dev->CreateRenderTargetView(rt.Get(), &rv, rtvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {}; dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS; dev->CreateDepthStencilView(ds.Get(), &dv, dsvHeap->GetCPUDescriptorHandleForHeapStart());
    // graphics root signature (empty)
    D3D12_ROOT_SIGNATURE_DESC rs0 = { 0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE }; ComPtr<ID3DBlob> sb, eb;
    CK(D3D12SerializeRootSignature(&rs0, D3D_ROOT_SIGNATURE_VERSION_1, &sb, &eb)); CK(dev->CreateRootSignature(0, sb->GetBufferPointer(), sb->GetBufferSize(), IID_PPV_ARGS(&rsEmpty)));
    CK(dev->CreateRootSignature(0, sb->GetBufferPointer(), sb->GetBufferSize(), IID_PPV_ARGS(&rsOther)));   // a second, distinct root signature object (the dummy PSO's)
    // dump compute: SRV (MS texture) + UAV (2560 x 720 float4)
    D3D12_DESCRIPTOR_RANGE r0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 }, r1 = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER rp[2] = {}; rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable = { 1, &r0 }; rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable = { 1, &r1 };
    D3D12_ROOT_SIGNATURE_DESC rs1 = { 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    CK(D3D12SerializeRootSignature(&rs1, D3D_ROOT_SIGNATURE_VERSION_1, &sb, &eb)); CK(dev->CreateRootSignature(0, sb->GetBufferPointer(), sb->GetBufferSize(), IID_PPV_ARGS(&rsCompute)));
    std::string ns = std::to_string(g_ns);
    std::string csSrc = "Texture2DMS<float4," + ns + "> src : register(t0); RWTexture2D<float4> dst : register(u0);\n"
                        "[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) { for (int s = 0; s < " + ns + "; ++s) dst[uint2(id.x*" + ns + "+s,id.y)] = src.Load(int2(id.xy),s); }\n";
    auto cs = Compile(csSrc.c_str(), "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {}; cd.pRootSignature = rsCompute.Get(); cd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() }; CK(dev->CreateComputePipelineState(&cd, IID_PPV_ARGS(&psoDump)));
    D3D12_DESCRIPTOR_HEAP_DESC h3 = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 }; CK(dev->CreateDescriptorHeap(&h3, IID_PPV_ARGS(&cbvHeap)));
    D3D12_RESOURCE_DESC ud = {}; ud.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; ud.Width = W * g_ns; ud.Height = 720; ud.DepthOrArraySize = 1; ud.MipLevels = 1; ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; ud.SampleDesc = { 1, 0 }; ud.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&uavTex)));
    dev->GetCopyableFootprints(&ud, 0, 1, 0, &fp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES rhp = {}; rhp.Type = D3D12_HEAP_TYPE_READBACK; D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CK(dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); D3D12_CPU_DESCRIPTOR_HANDLE h = cbvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {}; sv.Format = rd.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; dev->CreateShaderResourceView(rt.Get(), &sv, h);
    h.ptr += inc; D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {}; uv.Format = ud.Format; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; dev->CreateUnorderedAccessView(uavTex.Get(), nullptr, &uv, h);
}

static const float half2float(unsigned short h) { unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float f; if (e == 0) f = ldexpf((float)m, -24); else if (e == 31) f = m ? NAN : INFINITY; else f = ldexpf((float)(m + 1024), (int)e - 25); return s ? -f : f; }

// Runs one test case. 'pre' records the commands that precede the probe draw (PSO sets, line draws, ...); then two slanted-edge quads are drawn with 'tri' and the sample positions measured.
static void Test(const char* name, ID3D12PipelineState* tri, void (*pre)(ID3D12GraphicsCommandList*, void*), void* ctx)
{
    ComPtr<ID3D12CommandAllocator> al; CK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)));
    ComPtr<ID3D12GraphicsCommandList> cl; CK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl)));
    if (g_start) cl->SetPipelineState(g_start.Get());   // the fix: a plain triangle PSO before anything else
    if (g_start2) cl->SetPipelineState(g_start2.Get());
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeap->GetCPUDescriptorHandleForHeapStart(), dsvH = dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cl->OMSetRenderTargets(1, &rtvH, TRUE, &dsvH);
    D3D12_RECT clr = { 0, 0, (LONG)W, (LONG)H }; float c0[4] = {};
    cl->ClearDepthStencilView(dsvH, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 1, &clr);
    cl->ClearRenderTargetView(rtvH, c0, 1, &clr);
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0.0f, 0.5f }; cl->RSSetViewports(1, &vp);
    D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H }; cl->RSSetScissorRects(1, &sc);
    cl->SetGraphicsRootSignature(rsEmpty.Get());
    if (pre) pre(cl.Get(), ctx);
    cl->SetPipelineState(tri);
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->DrawInstanced(12, 1, 0, 0);
    D3D12_RESOURCE_BARRIER b1 = Tr(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); cl->ResourceBarrier(1, &b1);
    ID3D12DescriptorHeap* hs[] = { cbvHeap.Get() }; cl->SetDescriptorHeaps(1, hs);
    cl->SetComputeRootSignature(rsCompute.Get()); cl->SetPipelineState(psoDump.Get());
    UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV); D3D12_GPU_DESCRIPTOR_HANDLE g = cbvHeap->GetGPUDescriptorHandleForHeapStart(), g2 = g; g2.ptr += inc;
    cl->SetComputeRootDescriptorTable(0, g); cl->SetComputeRootDescriptorTable(1, g2); cl->Dispatch(W / 8, H / 8, 1);
    D3D12_RESOURCE_BARRIER b2[2] = { Tr(rt.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET), Tr(uavTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE) };
    cl->ResourceBarrier(2, b2);
    D3D12_TEXTURE_COPY_LOCATION cd = {}; cd.pResource = readback.Get(); cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; cd.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION cs = {}; cs.pResource = uavTex.Get(); cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&cd, 0, 0, 0, &cs, nullptr);
    D3D12_RESOURCE_BARRIER b3 = Tr(uavTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); cl->ResourceBarrier(1, &b3);
    CK(cl->Close()); ID3D12CommandList* ls[] = { cl.Get() }; q->ExecuteCommandLists(1, ls); Wait();
    unsigned char* p = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)total }; CK(readback->Map(0, &rr, (void**)&p));
    auto covered = [&](int s, int x, int y) { const unsigned short* px = (const unsigned short*)(p + (size_t)y * fp.Footprint.RowPitch + (size_t)(x * g_ns + s) * 8); return half2float(px[0]) > 5.0f; };
    double U[4], V[4];
    for (int s = 0; s < (int)g_ns; ++s)
    {
        double su = 0; int nu = 0;
        for (int c = 102; c < 110; ++c) { int r = -1; for (int y = 0; y < (int)H; ++y) if (!covered(s, c, y)) { r = y; break; } su += r - 64 * (c - 100); ++nu; }
        double sv2 = 0; int nv = 0;
        for (int r = 302; r < 308; ++r) { int cf = -1; for (int x = 600; x < 1200; ++x) if (!covered(s, x, r)) { cf = x - 600; break; } sv2 += cf - 64 * (r - 300); ++nv; }
        U[s] = su / nu; V[s] = sv2 / nv;
    }
    long lc[4] = { 0, 0, 0, 0 };   // covered samples in the area of the line draw (x 440..560, y 95..125, away from the two quads), per sample
    for (int s = 0; s < (int)g_ns; ++s) for (int y = 95; y < 125; ++y) for (int x = 440; x < 560; ++x) lc[s] += covered(s, x, y);
    readback->Unmap(0, nullptr);
    double sx[4], sy[4];
    for (int s = 0; s < (int)g_ns; ++s) { sx[s] = (64 * U[s] + V[s]) / (64.0 * 64.0 - 1) * 16; sy[s] = (64 * V[s] + U[s]) / (64.0 * 64.0 - 1) * 16; }
    // the D3D standard patterns in 1/16 px from the pixel's corner: 2x (12,12) (4,4); 4x (6,2) (14,6) (2,10) (10,14)
    static const double std2[2][2] = { { 12, 12 }, { 4, 4 } }, std4[4][2] = { { 6, 2 }, { 14, 6 }, { 2, 10 }, { 10, 14 } };
    bool standard = true, centre = true;
    for (int s = 0; s < (int)g_ns; ++s)
    {
        const double* e = g_ns == 2 ? std2[s] : std4[s];
        if (!(fabs(sx[s] - e[0]) < 1 && fabs(sy[s] - e[1]) < 1)) standard = false;
        if (!(fabs(sx[s] - 8) < 1 && fabs(sy[s] - 8) < 1)) centre = false;
    }
    char pos[256]; pos[0] = 0;
    for (int s = 0; s < (int)g_ns; ++s) { char t[40]; snprintf(t, sizeof t, "s%d (%4.1f,%4.1f) ", s, sx[s], sy[s]); strcat(pos, t); }
    if (lc[0] + lc[1] > 0) { char t[100]; snprintf(t, sizeof t, "| line px %ld %ld", lc[0], lc[1]); if (g_ns == 4) { char u[40]; snprintf(u, sizeof u, " %ld %ld", lc[2], lc[3]); strcat(t, u); } strcat(pos, t); strcat(pos, " "); }
    printf("%-62s %s/16 px  => %s\n", name, pos, standard ? "standard pattern" : (centre ? "ALL SAMPLES AT PIXEL CENTRE (broken)" : "other"));
}

struct Ctx { ID3D12PipelineState* line; bool draw; ID3D12PipelineState* tri2; int ssp = 0; bool compute = false; int extra = 0; ID3D12PipelineState* triMs = nullptr; ID3D12PipelineState* first = nullptr; bool firstCompute = false;
             std::vector<ID3D12PipelineState*> before, after; };   // before: bound in order before the line PSO; after: in order after it (the sequences of the G/E cases)
static void PreLine(ID3D12GraphicsCommandList* cl, void* c)
{
    Ctx* x = (Ctx*)c;
    ComPtr<ID3D12GraphicsCommandList1> cl1; cl->QueryInterface(IID_PPV_ARGS(&cl1));
    if (x->first) cl->SetPipelineState(x->first);
    if (x->firstCompute) { cl->SetComputeRootSignature(rsCompute.Get()); cl->SetPipelineState(psoDump.Get()); }
    for (auto* p : x->before) BindPso(cl, p);
    BindPso(cl, x->line);
    for (auto* p : x->after) BindPso(cl, p);
    if (x->draw) { cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST); cl->DrawInstanced(4, 1, 0, 0); }
    if (x->compute) { cl->SetComputeRootSignature(rsCompute.Get()); cl->SetPipelineState(psoDump.Get()); }
    if (x->ssp == 1 && cl1) cl1->SetSamplePositions(0, 0, nullptr);
    if (x->ssp == 3 && cl1) { D3D12_SAMPLE_POSITION pos[2] = { { 4, 4 }, { -4, -4 } }; cl1->SetSamplePositions(2, 1, pos); }
    if (x->extra == 1)   // a draw with a MultisampleEnable=TRUE triangle PSO, clipped to a 1x1 scissor away from the measurement area
    {
        cl->SetPipelineState(x->triMs); D3D12_RECT sc1 = { 0, 0, 1, 1 }; cl->RSSetScissorRects(1, &sc1); cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST); cl->DrawInstanced(3, 1, 0, 0);
        D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H }; cl->RSSetScissorRects(1, &sc);
    }
    if (x->extra == 2)   // re-bind the render targets
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeap->GetCPUDescriptorHandleForHeapStart(), dsvH = dsvHeap->GetCPUDescriptorHandleForHeapStart(); cl->OMSetRenderTargets(1, &rtvH, TRUE, &dsvH);
    }
    if (x->extra == 3)   // RT RENDER_TARGET -> COPY_SOURCE -> RENDER_TARGET
    {
        D3D12_RESOURCE_BARRIER a = Tr(rt.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE), b = Tr(rt.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET); cl->ResourceBarrier(1, &a); cl->ResourceBarrier(1, &b);
    }
    if (x->extra == 4)   // clear the render target again
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvH = rtvHeap->GetCPUDescriptorHandleForHeapStart(); float c0[4] = {}; D3D12_RECT clr = { 0, 0, (LONG)W, (LONG)H }; cl->ClearRenderTargetView(rtvH, c0, 1, &clr);
    }
    if (x->extra == 5)   // set a different (non-line) triangle PSO without drawing
    {
        cl->SetPipelineState(x->triMs);
    }
    if (x->tri2) cl->SetPipelineState(x->tri2);
    if (x->ssp == 2 && cl1) cl1->SetSamplePositions(0, 0, nullptr);
}

int main(int argc, char** argv)
{
    ComPtr<IDXGIFactory4> f; CK(CreateDXGIFactory1(IID_PPV_ARGS(&f)));
    std::string want = argc > 1 ? argv[1] : "";
    if (argc > 2) g_ns = (UINT)atoi(argv[2]);
    if (g_ns != 2 && g_ns != 4) { printf("samples must be 2 or 4\n"); return 1; }
    ComPtr<IDXGIAdapter1> ad;
    for (UINT i = 0;; ++i)
    {
        ComPtr<IDXGIAdapter1> a; if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d); char name[256]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, 256, nullptr, nullptr);
        bool sw = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        printf("adapter %u: %s%s\n", i, name, sw ? " (software)" : "");
        if (!ad && ((want.empty() && !sw) || (!want.empty() && std::string(name).find(want) != std::string::npos))) ad = a;
    }
    if (!ad) { printf("no adapter selected\n"); return 1; }
    DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d); char name[256]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, 256, nullptr, nullptr); printf("using: %s\n\n", name);
    if (getenv("REPRO_DEBUG")) { ComPtr<ID3D12Debug> dbg; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer(); }
    Setup(ad.Get());
    PsoOpt tri; auto psoTri = MakePSO(tri);
    PsoOpt tri2 = tri; tri2.msaaEnable = true; auto psoTriMs = MakePSO(tri2);
    // line PSOs
    PsoOpt l; l.topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE; l.lines = true;
    PsoOpt lA = l;                                                  // aliased lines, PS ignores SV_Position
    PsoOpt lB = l; lB.usePos = true;                                // aliased lines, PS reads SV_Position
    PsoOpt lC = l; lC.msaaEnable = true;                            // MultisampleEnable = TRUE
    PsoOpt lD = l; lD.depth = true; lD.cull = D3D12_CULL_MODE_NONE; lD.ccw = false;   // depth+stencil like the game's PSO
    PsoOpt lE = lD; lE.msaaEnable = true;
    PsoOpt lF = l; lF.aaLine = true;
    auto psoA = MakePSO(lA), psoB = MakePSO(lB), psoC = MakePSO(lC), psoD = MakePSO(lD), psoE = MakePSO(lE), psoF = MakePSO(lF);
    Test("0 triangles only (control)", psoTri.Get(), nullptr, nullptr);
    Ctx cA{ psoA.Get(), false, nullptr }, cAd{ psoA.Get(), true, nullptr }, cB{ psoB.Get(), false, nullptr }, cBd{ psoB.Get(), true, nullptr }, cC{ psoC.Get(), false, nullptr }, cCd{ psoC.Get(), true, nullptr };
    Ctx cD{ psoD.Get(), false, nullptr }, cDd{ psoD.Get(), true, nullptr }, cE{ psoE.Get(), false, nullptr }, cEd{ psoE.Get(), true, nullptr }, cF{ psoF.Get(), false, nullptr }, cFd{ psoF.Get(), true, nullptr };
    Test("1 SetPSO(aliased line, PS ignores pos) then triangles", psoTri.Get(), PreLine, &cA);
    Test("2 ... and a line draw first", psoTri.Get(), PreLine, &cAd);
    Test("3 SetPSO(aliased line, PS reads SV_Position) then triangles", psoTri.Get(), PreLine, &cB);
    Test("4 ... and a line draw first", psoTri.Get(), PreLine, &cBd);
    Test("5 SetPSO(line, MultisampleEnable=TRUE) then triangles", psoTri.Get(), PreLine, &cC);
    Test("6 ... and a line draw first", psoTri.Get(), PreLine, &cCd);
    Test("7 SetPSO(aliased line + depth/stencil enabled) then triangles", psoTri.Get(), PreLine, &cD);
    Test("8 ... and a line draw first", psoTri.Get(), PreLine, &cDd);
    Test("9 SetPSO(line + depth/stencil, MultisampleEnable=TRUE) then tri", psoTri.Get(), PreLine, &cE);
    Test("10 ... and a line draw first", psoTri.Get(), PreLine, &cEd);
    Test("11 SetPSO(line, AntialiasedLineEnable=TRUE) then triangles", psoTri.Get(), PreLine, &cF);
    Test("12 ... and a line draw first", psoTri.Get(), PreLine, &cFd);
    Test("13 triangles with MultisampleEnable=TRUE only (control)", psoTriMs.Get(), nullptr, nullptr);
    Ctx c14{ psoA.Get(), false, nullptr, 1 }, c15{ psoA.Get(), false, psoTri.Get(), 2 }, c16{ psoA.Get(), false, nullptr, 0, true }, c17{ psoA.Get(), true, nullptr, 0, true }, c18{ psoA.Get(), false, nullptr, 3 };
    Test("14 SetPSO(aliased line); SetSamplePositions(0,0,null); tri", psoTri.Get(), PreLine, &c14);
    Test("15 SetPSO(aliased line); SetPSO(tri); SetSamplePositions(0,0,null)", psoTri.Get(), PreLine, &c15);
    Test("16 SetPSO(aliased line); SetPSO(compute); then tri", psoTri.Get(), PreLine, &c16);
    Test("17 line draw; SetPSO(compute); then tri", psoTri.Get(), PreLine, &c17);
    Ctx c19{ psoA.Get(), false, nullptr, 0, false, 1, psoTriMs.Get() }, c20{ psoA.Get(), false, nullptr, 0, false, 2 }, c21{ psoA.Get(), false, nullptr, 0, false, 3 }, c22{ psoA.Get(), false, nullptr, 0, false, 4 }, c23{ psoA.Get(), false, nullptr, 0, false, 5, psoTriMs.Get() };
    Test("19 SetPSO(aliased line); small draw with MultisampleEnable=TRUE tri PSO; tri", psoTri.Get(), PreLine, &c19);
    Test("20 SetPSO(aliased line); OMSetRenderTargets again; tri", psoTri.Get(), PreLine, &c20);
    Test("21 SetPSO(aliased line); barrier RT->COPY_SOURCE->RT; tri", psoTri.Get(), PreLine, &c21);
    Test("22 SetPSO(aliased line); ClearRenderTargetView again; tri", psoTri.Get(), PreLine, &c22);
    Test("23 SetPSO(aliased line); SetPSO(MultisampleEnable=TRUE tri), no draw; tri", psoTri.Get(), PreLine, &c23);
    Ctx c24{ psoA.Get(), false, nullptr, 0, false, 0, nullptr, psoTri.Get() }, c25{ psoA.Get(), true, nullptr, 0, false, 0, nullptr, psoTri.Get() }, c26{ psoA.Get(), false, nullptr, 0, false, 0, nullptr, nullptr, true }, c27{ psoA.Get(), false, nullptr, 0, false, 0, nullptr, psoC.Get() };
    Test("24 SetPSO(tri); SetPSO(aliased line); [Test sets tri] draw", psoTri.Get(), PreLine, &c24);
    Test("25 SetPSO(tri); SetPSO(aliased line); line draw; tri", psoTri.Get(), PreLine, &c25);
    Test("26 SetPSO(compute) first; SetPSO(aliased line); tri", psoTri.Get(), PreLine, &c26);
    Test("27 SetPSO(MS line) first; SetPSO(aliased line); tri", psoTri.Get(), PreLine, &c27);

    // ---- the fix: a plain triangle PSO bound as the very first command of the list (g_start), with the same line sequences as above.
    // "dummy" = what the SDK patch binds: vertex shader only, no pixel shader, no render target, no depth, its own empty root signature, never drawn with.
    printf("\n--- list-start PSO (the fix) ---\n");
    PsoOpt dA; dA.dummy = true; dA.samples = 0; dA.otherRs = true;                 // no outputs at all (the runtime wants SampleDesc.Count 0 then)
    PsoOpt dB = dA; dB.samples = 1; dB.dummyRtOnly = true;                       // 1 sample, one RT format, no depth, no PS
    PsoOpt dC = dA; dC.samples = -1; dC.dummyRt = true;                         // as many samples as the target, RT + depth formats of the target, no PS
    PsoOpt dE = dA; dE.samples = g_ns == 2 ? 4 : 2; dE.dummyRt = true;           // the OTHER sample count (2 for a 4x target, 4 for a 2x one)
    PsoOpt dD = tri; dD.otherRs = true;                                          // a full triangle PSO (pixel shader, RT + depth), other root signature
    auto psoDA = MakePSO(dA), psoDB = MakePSO(dB), psoDC = MakePSO(dC), psoDD = MakePSO(dD), psoDE = MakePSO(dE);
    Ctx f1{ psoA.Get(), false, nullptr }, f1d{ psoA.Get(), true, nullptr }, f1c{ psoA.Get(), false, nullptr, 0, false, 0, nullptr, nullptr, true };
    struct { const char* name; ID3D12PipelineState* start; Ctx* c; } fix[] = {
        { "F0 (control, no list-start PSO) SetPSO(aliased line); tri", nullptr, &f1 },
        { "F1 dummy no-output PSO first; SetPSO(aliased line); tri", psoDA.Get(), &f1 },
        { "F2 dummy 1x/RT-only PSO first; SetPSO(aliased line); tri", psoDB.Get(), &f1 },
        { "F3 dummy matching count/RT+depth first; SetPSO(aliased line); tri", psoDC.Get(), &f1 },
        { "F4 full triangle PSO (other root sig) first; SetPSO(aliased line); tri", psoDD.Get(), &f1 },
        { "F5 dummy no-output first; SetPSO(aliased line); line draw; tri", psoDA.Get(), &f1d },
        { "F6 dummy no-output first; compute PSO; SetPSO(aliased line); tri", psoDA.Get(), &f1c },
        { "F7 dummy no-output first; SetPSO(aliased line + depth/stencil); tri", psoDA.Get(), &cD },
        { "F8 dummy no-output first; SetPSO(aliased line, PS reads pos); line draw; tri", psoDA.Get(), &cBd },
    };
    for (auto& t : fix) { g_start = t.start; Test(t.name, psoTri.Get(), PreLine, t.c); }
    g_start = psoDE.Get(); Test("F9 dummy with the OTHER sample count first; SetPSO(aliased line); tri", psoTri.Get(), PreLine, &f1);
    g_start = psoDE.Get(); g_start2 = psoDC.Get(); Test("F10 both counts (other, then matching) first; SetPSO(aliased line); tri", psoTri.Get(), PreLine, &f1);
    g_start = psoDC.Get(); g_start2 = psoDE.Get(); Test("F11 both counts (matching, then other) first; SetPSO(aliased line); tri", psoTri.Get(), PreLine, &f1);
    g_start = nullptr; g_start2 = nullptr;

    // ---- which first PSOs trigger it? (the game's line PSOs 4168, 4170, 4244, 4253 did not; 4103 and 4179 did)
    printf("\n--- other first PSOs, no list-start PSO ---\n");
    PsoOpt lG = l; lG.blend = true;                                                // blending on
    PsoOpt lH = l; lH.discardPs = true;                                            // pixel shader with clip()
    PsoOpt lI = lD; lI.blend = true;                                               // depth+stencil like the game's, blending on
    PsoOpt lJ = lD; lJ.discardPs = true;                                           // depth+stencil like the game's, clip()
    PsoOpt lK = lD; lK.cull = D3D12_CULL_MODE_BACK;                                // depth+stencil, back-face culling
    PsoOpt pt = l; pt.topo = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;                  // points
    PsoOpt lL = l; lL.samples = 1;                                                 // a 1-sample line PSO (target is 2x)
    auto psoG = MakePSO(lG), psoH = MakePSO(lH), psoI = MakePSO(lI), psoJ = MakePSO(lJ), psoK = MakePSO(lK), psoP = MakePSO(pt), psoL = MakePSO(lL);
    Ctx gG{ psoG.Get(), false, nullptr }, gH{ psoH.Get(), false, nullptr }, gI{ psoI.Get(), false, nullptr }, gJ{ psoJ.Get(), false, nullptr }, gK{ psoK.Get(), false, nullptr }, gP{ psoP.Get(), false, nullptr }, gL{ psoL.Get(), false, nullptr };
    Test("G1 SetPSO(aliased line, blend on); tri", psoTri.Get(), PreLine, &gG);
    Test("G2 SetPSO(aliased line, PS with clip()); tri", psoTri.Get(), PreLine, &gH);
    Test("G3 SetPSO(aliased line + depth/stencil, blend on); tri", psoTri.Get(), PreLine, &gI);
    Test("G4 SetPSO(aliased line + depth/stencil, PS with clip()); tri", psoTri.Get(), PreLine, &gJ);
    Test("G5 SetPSO(aliased line + depth/stencil, cull back); tri", psoTri.Get(), PreLine, &gK);
    Test("G6 SetPSO(POINT topology PSO); tri", psoTri.Get(), PreLine, &gP);
    Test("G7 SetPSO(aliased line with SampleDesc 1); tri", psoTri.Get(), PreLine, &gL);

    // ---- sequences of PSOs with different sample counts (only bound, never drawn with, except the final probe draw). "match" = the target's count.
    printf("\n--- PSO sequences with different sample counts ---\n");
    PsoOpt t1 = tri; t1.samples = 1;                                              // a 1-sample triangle PSO
    PsoOpt tO = tri; tO.samples = g_ns == 2 ? 4 : 2;                              // a triangle PSO of the other count
    PsoOpt lO = l; lO.samples = tO.samples;                                       // an aliased line PSO of the other count
    auto psoT1 = MakePSO(t1), psoTO = MakePSO(tO), psoLO = MakePSO(lO);
    ID3D12PipelineState *T = psoTri.Get(), *T1 = psoT1.Get(), *TO = psoTO.Get(), *LM = psoA.Get(), *LO = psoLO.Get();
    Ctx e1{ LM, false, nullptr }; e1.before = { T1 };
    Ctx e2{ LM, false, nullptr }; e2.before = { T, T1 };
    Ctx e3{ LM, false, nullptr }; e3.before = { T }; e3.after = { T1, LM };
    Ctx e4{ LM, false, nullptr }; e4.before = { T1, T };
    Ctx e5{ LM, false, nullptr }; e5.after = { T1, T };
    Ctx e6{ LM, false, nullptr }; e6.before = { TO };
    Ctx e7{ LM, false, nullptr }; e7.before = { TO, T };
    Ctx e8{ LM, false, nullptr }; e8.before = { T }; e8.after = { TO, LM };
    Ctx e10{ LO, false, nullptr };
    Ctx e11{ LM, false, nullptr }; e11.before = { T }; e11.after = { T1, T, LM };
    Test("E1 1x tri; line; tri", psoTri.Get(), PreLine, &e1);
    Test("E2 match tri; 1x tri; line; tri", psoTri.Get(), PreLine, &e2);
    Test("E3 match tri; line; 1x tri; line; tri", psoTri.Get(), PreLine, &e3);
    Test("E4 1x tri; match tri; line; tri", psoTri.Get(), PreLine, &e4);
    Test("E5 line first; 1x tri; match tri; tri", psoTri.Get(), PreLine, &e5);
    Test("E6 other-count tri; line; tri", psoTri.Get(), PreLine, &e6);
    Test("E7 other-count tri; match tri; line; tri", psoTri.Get(), PreLine, &e7);
    Test("E8 match tri; line; other-count tri; line; tri", psoTri.Get(), PreLine, &e8);
    Test("E10 other-count aliased line first; match tri", psoTri.Get(), PreLine, &e10);
    Test("E11 match tri; line; 1x tri; match tri; line; tri", psoTri.Get(), PreLine, &e11);

    // ---- wireframe triangles (FillMode WIREFRAME, MultisampleEnable FALSE) as the first PSO: do they collapse the positions like a line PSO?
    printf("\n--- wireframe ---\n");
    PsoOpt wf = tri; wf.wire = true; auto psoWF = MakePSO(wf);
    Ctx w1{ psoWF.Get(), false, nullptr };
    Test("W1 SetPSO(wireframe triangle PSO) first; tri", psoTri.Get(), PreLine, &w1);

    // ---- which outputs does the guard PSO need? The guard binds right before the line PSO; here as the first PSO of the list (g_start), matching sample count, no pixel shader.
    printf("\n--- guard PSO shapes (matching sample count) ---\n");
    struct { const char* name; DXGI_FORMAT rt, ds; } shapes[] = {
        { "V1 guard: R8G8B8A8_UNORM only", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN },
        { "V2 guard: R16G16B16A16_FLOAT only", DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_UNKNOWN },
        { "V3 guard: D32_FLOAT_S8X24 only", DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT_S8X24_UINT },
        { "V4 guard: D32_FLOAT only", DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_D32_FLOAT },
        { "V5 guard: R8G8B8A8_UNORM + D32_FLOAT_S8X24", DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT },
    };
    for (auto& sh : shapes)
    {
        PsoOpt g; g.dummy = true; g.samples = -1; g.otherRs = true; g.rtfOverride = sh.rt; g.dsfOverride = sh.ds;
        auto pso = MakePSO(g); g_start = pso; Test(sh.name, psoTri.Get(), PreLine, &f1); g_start = nullptr;
    }

    // ---- the guard as the SDK patch uses it: right before every aliased line PSO of a multisampled target, a guard PSO with the line PSO's sample count
    // (R8G8B8A8_UNORM, no depth, no pixel shader, own root signature). The same sequences as E1..E11 and the line draws: every one should now be standard.
    printf("\n--- the guard before each line PSO (g_guard on) ---\n");
    PsoOpt gm; gm.dummy = true; gm.samples = -1; gm.otherRs = true; gm.rtfOverride = DXGI_FORMAT_R8G8B8A8_UNORM;
    PsoOpt go = gm; go.samples = tO.samples;
    auto psoGM = MakePSO(gm), psoGO = MakePSO(go);
    g_lineGuards = { { LM, psoGM.Get() }, { LO, psoGO.Get() } }; g_guard = true;
    Test("H0 guard; line; tri (line first in the list)", psoTri.Get(), PreLine, &f1);
    Test("H1 1x tri; [guard] line; tri", psoTri.Get(), PreLine, &e1);
    Test("H2 match tri; 1x tri; [guard] line; tri", psoTri.Get(), PreLine, &e2);
    Test("H3 match tri; [guard] line; 1x tri; [guard] line; tri", psoTri.Get(), PreLine, &e3);
    Test("H6 other-count tri; [guard] line; tri", psoTri.Get(), PreLine, &e6);
    Test("H8 match tri; [guard] line; other-count tri; [guard] line; tri", psoTri.Get(), PreLine, &e8);
    Test("H10 [guard of the other count] other-count line first; match tri", psoTri.Get(), PreLine, &e10);
    Test("H12 [guard] line first, line draw, tri", psoTri.Get(), PreLine, &f1d);
    Ctx lt{ LM, true, nullptr }; lt.before = { T };
    Test("H13 match tri; [guard] line, line draw, tri", psoTri.Get(), PreLine, &lt);
    g_guard = false;
    Test("L1 (no guard) line first, line draw, tri", psoTri.Get(), PreLine, &f1d);
    Test("L2 (no guard) match tri; line, line draw, tri", psoTri.Get(), PreLine, &lt);
    if (g_ns == 2) Test("18 SetPSO(aliased line); SetSamplePositions(2,1,standard); tri", psoTri.Get(), PreLine, &c18);
    if (getenv("REPRO_DEBUG")) { ComPtr<ID3D12InfoQueue> iq; if (SUCCEEDED(dev.As(&iq))) { UINT64 n = iq->GetNumStoredMessages(); for (UINT64 i = 0; i < n && i < 40; ++i) { SIZE_T len = 0; iq->GetMessage(i, nullptr, &len); std::vector<char> b(len); D3D12_MESSAGE* m = (D3D12_MESSAGE*)b.data(); iq->GetMessage(i, m, &len); printf("layer: [sev %d] %s\n", (int)m->Severity, m->pDescription); } } }
    return 0;
}
