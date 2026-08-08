// d3d12_testapp.cpp
//
// A deliberately small D3D12 application that mirrors the part of Unreal's
// CustomDepth pass we care about: it renders a grid of quads into a
// depth-stencil target, each quad writing a distinct stencil value.
//
// Why this exists: the capture layer (queue acquisition, descriptor->resource
// mapping, ResourceBarrier shadow tracking, PlaneSlice=1 readback) needs to be
// developed against ground truth. Here every stencil value is known by
// construction, so a wrong readback is obvious. If the capture layer cannot
// pull correct stencil out of this, it has no business being injected into a
// shipped game.
//
// The grid is laid out so a validator can assert exact expectations:
//   quad (col, row) occupies a known screen rect and writes stencil = index + 1
//
// Usage:
//   d3d12_testapp.exe [--frames N] [--width W] [--height H]
//
//   --frames N   render N frames then exit cleanly (0 = run until closed).
//                Lets the fixture run unattended in a test script.

// WIN32_LEAN_AND_MEAN / NOMINMAX come from the build system (see CMakeLists.txt)
// so they apply uniformly across every translation unit, including the injected
// DLL where a stray min/max macro would be an unpleasant surprise.
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- config

namespace {

constexpr UINT kFrameCount = 3;

// 4x4 grid => 16 objects, stencil values 1..16. Deliberately well under the
// 8-bit ceiling: the point here is correctness of readback, not exercising the
// slot allocator. Slot recycling is tested separately against the real game.
constexpr int kGridCols = 4;
constexpr int kGridRows = 4;
constexpr int kQuadCount = kGridCols * kGridRows;

struct Options {
    UINT frames = 0;   // 0 = until the window is closed
    UINT width = 1280;
    UINT height = 720;
    // Vsync is on by default so the window looks normal, but it caps frame rate
    // and would mask any cost the capture layer adds. Timing runs must disable
    // it, otherwise "no measurable overhead" only means "both were 60fps".
    bool vsync = true;
};

void ThrowIfFailed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what,
                      static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

// The shader is compiled at runtime so the fixture stays a single .cpp with no
// build-time shader step.
constexpr char kShaderSource[] = R"(
cbuffer Constants : register(b0)
{
    float4 gColor;
    float2 gOffset;
    float2 gScale;
};

struct VSOut { float4 pos : SV_POSITION; };

VSOut VSMain(float2 p : POSITION)
{
    VSOut o;
    o.pos = float4(p * gScale + gOffset, 0.5f, 1.0f);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    return gColor;
}
)";

struct Vertex { float x, y; };

// Unit quad as a triangle strip, centred on the origin.
constexpr Vertex kQuadVerts[4] = {
    {-1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
};

struct Constants {
    float color[4];
    float offset[2];
    float scale[2];
};
static_assert(sizeof(Constants) == 32, "root constant count must match");

}  // namespace

// ---------------------------------------------------------------- app

class TestApp {
public:
    explicit TestApp(const Options& opt) : opt_(opt) {}

    void Run() {
        CreateWindowAndShow();
        InitD3D();
        MainLoop();
        WaitForGpu();
    }

private:
    // ---- window ----

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hwnd, msg, w, l);
    }

    void CreateWindowAndShow() {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SegcapD3D12TestApp";
        RegisterClassExW(&wc);

        RECT r = {0, 0, static_cast<LONG>(opt_.width),
                  static_cast<LONG>(opt_.height)};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

        hwnd_ = CreateWindowExW(0, wc.lpszClassName,
                                L"segcap D3D12 test fixture",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr,
                                wc.hInstance, nullptr);
        if (!hwnd_) throw std::runtime_error("CreateWindowExW failed");
        ShowWindow(hwnd_, SW_SHOW);

        std::printf("[testapp] pid=%lu hwnd=%p %ux%u\n",
                    GetCurrentProcessId(), static_cast<void*>(hwnd_),
                    opt_.width, opt_.height);
        std::fflush(stdout);
    }

    // ---- device ----

    void InitD3D() {
        UINT factoryFlags = 0;
#ifdef _DEBUG
        // The debug layer is what turns a wrong ResourceBarrier from a silent
        // GPU hang into a readable error message. Worth every millisecond.
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif
        ComPtr<IDXGIFactory4> factory;
        ThrowIfFailed(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&factory)),
                      "CreateDXGIFactory2");

        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device_)),
                      "D3D12CreateDevice");

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)),
                      "CreateCommandQueue");

        DXGI_SWAP_CHAIN_DESC1 scd = {};
        scd.BufferCount = kFrameCount;
        scd.Width = opt_.width;
        scd.Height = opt_.height;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> sc1;
        ThrowIfFailed(factory->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &scd,
                                                      nullptr, nullptr, &sc1),
                      "CreateSwapChainForHwnd");
        ThrowIfFailed(sc1.As(&swapChain_), "IDXGISwapChain3 QI");
        frameIndex_ = swapChain_->GetCurrentBackBufferIndex();

        CreateDescriptorHeaps();
        CreateRenderTargets();
        CreateDepthStencil();
        CreatePipeline();
        CreateVertexBuffer();

        ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                           IID_PPV_ARGS(&fence_)),
                      "CreateFence");
        fenceValue_ = 1;
        fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent_) throw std::runtime_error("CreateEvent failed");
    }

    void CreateDescriptorHeaps() {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = kFrameCount;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&rtvDesc,
                                                    IID_PPV_ARGS(&rtvHeap_)),
                      "CreateDescriptorHeap(RTV)");
        rtvSize_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // Two DSVs: the scene depth we actually render into, and a decoy that is
        // cleared every frame but never bound. The decoy exists to make the
        // election logic discriminate -- it mirrors ResourceId::1932 in Stray,
        // which is full-res D32S8, cleared each frame, never used as a depth
        // target. With only one depth target the election has nothing to get
        // wrong, so the test would prove nothing.
        D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
        dsvDesc.NumDescriptors = 2;
        dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        ThrowIfFailed(device_->CreateDescriptorHeap(&dsvDesc,
                                                    IID_PPV_ARGS(&dsvHeap_)),
                      "CreateDescriptorHeap(DSV)");
    }

    void CreateRenderTargets() {
        D3D12_CPU_DESCRIPTOR_HANDLE h =
            rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrameCount; ++i) {
            ThrowIfFailed(swapChain_->GetBuffer(i, IID_PPV_ARGS(&renderTargets_[i])),
                          "GetBuffer");
            device_->CreateRenderTargetView(renderTargets_[i].Get(), nullptr, h);
            h.ptr += rtvSize_;
        }
        for (UINT i = 0; i < kFrameCount; ++i) {
            ThrowIfFailed(device_->CreateCommandAllocator(
                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                              IID_PPV_ARGS(&allocators_[i])),
                          "CreateCommandAllocator");
        }
        ThrowIfFailed(device_->CreateCommandList(
                          0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                          allocators_[0].Get(), pso_.Get(), IID_PPV_ARGS(&cmdList_)),
                      "CreateCommandList");
        ThrowIfFailed(cmdList_->Close(), "Close initial command list");
    }

    // Created as R24G8_TYPELESS rather than D24_UNORM_S8_UINT specifically so
    // the stencil plane can be addressed as its own subresource later. A
    // typed depth format would block the PlaneSlice=1 copy we ultimately need,
    // and this mirrors how Unreal allocates its depth targets.
    void CreateDepthStencil() {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = opt_.width;
        rd.Height = opt_.height;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        clear.DepthStencil.Depth = 1.0f;
        clear.DepthStencil.Stencil = 0;

        ThrowIfFailed(device_->CreateCommittedResource(
                          &hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                          IID_PPV_ARGS(&depthStencil_)),
                      "CreateCommittedResource(depth)");

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

        dsvSize_ = device_->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        D3D12_CPU_DESCRIPTOR_HANDLE h =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateDepthStencilView(depthStencil_.Get(), &dsv, h);

        // The decoy, identical in every respect the election can observe except
        // that nothing ever renders into it.
        ThrowIfFailed(device_->CreateCommittedResource(
                          &hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                          IID_PPV_ARGS(&decoyDepth_)),
                      "CreateCommittedResource(decoy depth)");
        h.ptr += dsvSize_;
        device_->CreateDepthStencilView(decoyDepth_.Get(), &dsv, h);
    }

    void CreatePipeline() {
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        param.Constants.Num32BitValues = sizeof(Constants) / 4;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 1;
        rsd.pParameters = &param;
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        ThrowIfFailed(D3D12SerializeRootSignature(
                          &rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err),
                      "D3D12SerializeRootSignature");
        ThrowIfFailed(device_->CreateRootSignature(0, sig->GetBufferPointer(),
                                                   sig->GetBufferSize(),
                                                   IID_PPV_ARGS(&rootSig_)),
                      "CreateRootSignature");

        ComPtr<ID3DBlob> vs = Compile("VSMain", "vs_5_0");
        ComPtr<ID3DBlob> ps = Compile("PSMain", "ps_5_0");

        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.InputLayout = {layout, _countof(layout)};
        pd.pRootSignature = rootSig_.Get();
        pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;

        pd.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;

        // This is the whole point of the fixture: stencil writes are baked into
        // the PSO, exactly as they are in a real D3D12 title. A capture layer
        // that wants to know "does this draw write stencil?" cannot ask the
        // command list -- it has to have recorded this desc at PSO creation.
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = 0xFF;
        pd.DepthStencilState.StencilWriteMask = 0xFF;
        const D3D12_DEPTH_STENCILOP_DESC op = {
            D3D12_STENCIL_OP_REPLACE, D3D12_STENCIL_OP_REPLACE,
            D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS};
        pd.DepthStencilState.FrontFace = op;
        pd.DepthStencilState.BackFace = op;

        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pd.SampleDesc.Count = 1;

        ThrowIfFailed(device_->CreateGraphicsPipelineState(&pd,
                                                           IID_PPV_ARGS(&pso_)),
                      "CreateGraphicsPipelineState");

        // Depth-only PSO for the CustomDepth pass. UE renders CustomDepth
        // without a colour target, and the distinct PSO matters: it is the only
        // place the stencil configuration for that pass exists, which is
        // precisely the "baked into the PSO, not on the command list" property
        // the capture layer has to cope with.
        D3D12_GRAPHICS_PIPELINE_STATE_DESC depthOnly = pd;
        depthOnly.PS = {nullptr, 0};
        depthOnly.NumRenderTargets = 0;
        for (UINT i = 0; i < 8; ++i) depthOnly.RTVFormats[i] = DXGI_FORMAT_UNKNOWN;
        ThrowIfFailed(device_->CreateGraphicsPipelineState(&depthOnly,
                                                           IID_PPV_ARGS(&psoDepthOnly_)),
                      "CreateGraphicsPipelineState(depth-only)");
    }

    ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
        ComPtr<ID3DBlob> code, err;
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1,
                                nullptr, nullptr, nullptr, entry, target, flags,
                                0, &code, &err);
        if (FAILED(hr)) {
            std::string msg = "shader compile failed: ";
            if (err) msg += static_cast<const char*>(err->GetBufferPointer());
            throw std::runtime_error(msg);
        }
        return code;
    }

    void CreateVertexBuffer() {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = sizeof(kQuadVerts);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(device_->CreateCommittedResource(
                          &hp, D3D12_HEAP_FLAG_NONE, &rd,
                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                          IID_PPV_ARGS(&vertexBuffer_)),
                      "CreateCommittedResource(vb)");

        void* mapped = nullptr;
        D3D12_RANGE noRead = {0, 0};
        ThrowIfFailed(vertexBuffer_->Map(0, &noRead, &mapped), "Map(vb)");
        std::memcpy(mapped, kQuadVerts, sizeof(kQuadVerts));
        vertexBuffer_->Unmap(0, nullptr);

        vbView_.BufferLocation = vertexBuffer_->GetGPUVirtualAddress();
        vbView_.StrideInBytes = sizeof(Vertex);
        vbView_.SizeInBytes = sizeof(kQuadVerts);
    }

    // ---- frame ----

    void MainLoop() {
        MSG msg = {};
        UINT rendered = 0;

        LARGE_INTEGER freq = {}, prev = {};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&prev);
        std::vector<double> frameMs;
        if (opt_.frames) frameMs.reserve(opt_.frames);

        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                continue;
            }
            RenderFrame();
            ++rendered;

            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            frameMs.push_back(1000.0 * static_cast<double>(now.QuadPart - prev.QuadPart) /
                              static_cast<double>(freq.QuadPart));
            prev = now;

            if (opt_.frames && rendered >= opt_.frames) break;
        }

        ReportTiming(frameMs);
    }

    // Reports percentiles, not just the mean. A capture layer that stalls the
    // render thread every Nth frame can leave the mean almost untouched while
    // producing visible hitching -- p99 and max are where that shows up.
    static void ReportTiming(std::vector<double> ms) {
        if (ms.size() < 20) {
            std::printf("[testapp] too few frames for timing\n");
            return;
        }
        // Discard the first frames: device warm-up and shader compilation are
        // not what we are measuring.
        ms.erase(ms.begin(), ms.begin() + 10);
        std::sort(ms.begin(), ms.end());

        double sum = 0.0;
        for (double v : ms) sum += v;
        const auto pct = [&](double p) { return ms[static_cast<size_t>(p * (ms.size() - 1))]; };

        std::printf("[testapp] frames=%zu mean=%.3fms p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
                    ms.size(), sum / ms.size(), pct(0.50), pct(0.95), pct(0.99), ms.back());
        std::fflush(stdout);
    }

    void RenderFrame() {
        ThrowIfFailed(allocators_[frameIndex_]->Reset(), "allocator Reset");
        ThrowIfFailed(cmdList_->Reset(allocators_[frameIndex_].Get(), pso_.Get()),
                      "cmdList Reset");

        Transition(renderTargets_[frameIndex_].Get(),
                   D3D12_RESOURCE_STATE_PRESENT,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(frameIndex_) * rtvSize_;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();

        cmdList_->SetGraphicsRootSignature(rootSig_.Get());
        cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

        const float bg[4] = {0.05f, 0.05f, 0.08f, 1.0f};
        cmdList_->ClearRenderTargetView(rtv, bg, 0, nullptr);
        cmdList_->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0,
            nullptr);

        // Clear the decoy too, and then deliberately never bind it. This is the
        // exact shape of Stray's ResourceId::1932.
        D3D12_CPU_DESCRIPTOR_HANDLE decoyDsv =
            dsvHeap_->GetCPUDescriptorHandleForHeapStart();
        decoyDsv.ptr += dsvSize_;
        cmdList_->ClearDepthStencilView(
            decoyDsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0,
            0, nullptr);

        D3D12_VIEWPORT vp = {0.0f, 0.0f, static_cast<float>(opt_.width),
                             static_cast<float>(opt_.height), 0.0f, 1.0f};
        D3D12_RECT sr = {0, 0, static_cast<LONG>(opt_.width),
                         static_cast<LONG>(opt_.height)};
        cmdList_->RSSetViewports(1, &vp);
        cmdList_->RSSetScissorRects(1, &sr);
        cmdList_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmdList_->IASetVertexBuffers(0, 1, &vbView_);

        // Grid of quads. Each writes stencil = index + 1, so a validator can
        // assert an exact value at an exact pixel.
        const float cellW = 2.0f / kGridCols;
        const float cellH = 2.0f / kGridRows;
        const float halfW = cellW * 0.35f;
        const float halfH = cellH * 0.35f;

        auto quadConstants = [&](int row, int col) {
            const int index = row * kGridCols + col;
            Constants c = {};
            // Colour encodes the stencil id, so the colour image and the mask
            // can be eyeballed against each other directly.
            c.color[0] = static_cast<float>(index + 1) / kQuadCount;
            c.color[1] = 0.35f + 0.5f * static_cast<float>(col) / kGridCols;
            c.color[2] = 0.35f + 0.5f * static_cast<float>(row) / kGridRows;
            c.color[3] = 1.0f;
            c.offset[0] = -1.0f + cellW * (col + 0.5f);
            c.offset[1] = -1.0f + cellH * (row + 0.5f);
            c.scale[0] = halfW;
            c.scale[1] = halfH;
            return c;
        };

        // ---- scene pass -------------------------------------------------
        // Rebinding per draw is what makes this look like a real scene depth
        // buffer to the election logic. Engines bind per pass and there are many
        // passes; Stray's scene depth took 1365 binds in one frame. Binding once
        // would leave the "most-bound target is scene depth" rule untested.
        for (int row = 0; row < kGridRows; ++row) {
            for (int col = 0; col < kGridCols; ++col) {
                cmdList_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                const Constants c = quadConstants(row, col);
                // Scene stencil carries engine-ish semantics, not object IDs --
                // mirroring UE4, where these bits are lighting channels and the
                // receive-decal flag. Deliberately NOT the values we later
                // assert on.
                cmdList_->OMSetStencilRef(0x80);
                cmdList_->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
                cmdList_->DrawInstanced(4, 1, 0, 0);
            }
        }

        // ---- CustomDepth pass -------------------------------------------
        // Depth-only, bound once, carrying the per-object IDs 1..16. This is the
        // buffer the capture layer must elect and read back.
        cmdList_->SetPipelineState(psoDepthOnly_.Get());
        cmdList_->OMSetRenderTargets(0, nullptr, FALSE, &decoyDsv);
        for (int row = 0; row < kGridRows; ++row) {
            for (int col = 0; col < kGridCols; ++col) {
                const int index = row * kGridCols + col;
                const Constants c = quadConstants(row, col);
                cmdList_->OMSetStencilRef(static_cast<UINT>(index + 1));
                cmdList_->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
                cmdList_->DrawInstanced(4, 1, 0, 0);
            }
        }
        cmdList_->SetPipelineState(pso_.Get());

        Transition(renderTargets_[frameIndex_].Get(),
                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_PRESENT);

        ThrowIfFailed(cmdList_->Close(), "cmdList Close");
        ID3D12CommandList* lists[] = {cmdList_.Get()};
        queue_->ExecuteCommandLists(1, lists);

        ThrowIfFailed(swapChain_->Present(opt_.vsync ? 1 : 0, 0), "Present");
        MoveToNextFrame();
    }

    void Transition(ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                    D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = res;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList_->ResourceBarrier(1, &b);
    }

    void MoveToNextFrame() {
        const UINT64 target = fenceValue_;
        ThrowIfFailed(queue_->Signal(fence_.Get(), target), "Signal");
        ++fenceValue_;
        if (fence_->GetCompletedValue() < target) {
            ThrowIfFailed(fence_->SetEventOnCompletion(target, fenceEvent_),
                          "SetEventOnCompletion");
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
        frameIndex_ = swapChain_->GetCurrentBackBufferIndex();
    }

    void WaitForGpu() {
        if (!queue_ || !fence_) return;
        const UINT64 target = fenceValue_;
        if (FAILED(queue_->Signal(fence_.Get(), target))) return;
        ++fenceValue_;
        if (fence_->GetCompletedValue() < target) {
            if (SUCCEEDED(fence_->SetEventOnCompletion(target, fenceEvent_)))
                WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    Options opt_;
    HWND hwnd_ = nullptr;

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapChain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_, dsvHeap_;
    ComPtr<ID3D12Resource> renderTargets_[kFrameCount];
    ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> cmdList_;
    ComPtr<ID3D12Resource> depthStencil_;
    ComPtr<ID3D12Resource> decoyDepth_;
    UINT dsvSize_ = 0;
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso_;
    ComPtr<ID3D12PipelineState> psoDepthOnly_;
    ComPtr<ID3D12Resource> vertexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW vbView_ = {};

    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    UINT rtvSize_ = 0;
    UINT frameIndex_ = 0;
};

// ---------------------------------------------------------------- entry

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> UINT {
            return (i + 1 < argc) ? static_cast<UINT>(std::atoi(argv[++i])) : 0;
        };
        if (a == "--frames") opt.frames = next();
        else if (a == "--width") opt.width = next();
        else if (a == "--height") opt.height = next();
        else if (a == "--novsync") opt.vsync = false;
        else if (a == "--help") {
            std::printf("usage: %s [--frames N] [--width W] [--height H]\n",
                        argv[0]);
            return 0;
        }
    }

    try {
        TestApp app(opt);
        app.Run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[testapp] FATAL: %s\n", e.what());
        return 1;
    }
    return 0;
}
