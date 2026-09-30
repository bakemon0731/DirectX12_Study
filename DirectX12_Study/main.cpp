#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kFrameCount = 2;   // バックバッファ枚数

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("HRESULT failed");
}

struct Vertex
{
    float pos[3];
    float color[4];
};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------
// レンダラー
// ---------------------------------------------------------
class Renderer
{
public:
    void Init(HWND hwnd)
    {
        // ---- デバッグレイヤー ----
       
        UINT dxgiFlags = 0;
#ifdef _DEBUG
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        // ---- デバイス ----
       
        ComPtr<IDXGIFactory4> factory;
        ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory)));
        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)));

        // ---- コマンドキュー ----
        
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)));

        // ---- スワップチェーン ----
       
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = kWidth;
        sd.Height = kHeight;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kFrameCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        ComPtr<IDXGISwapChain1> sc1;
        ThrowIfFailed(factory->CreateSwapChainForHwnd(
            m_queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1));
        ThrowIfFailed(sc1.As(&m_swapChain));
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

        // ---- RTVヒープ ----
       
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = kFrameCount;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // バックバッファのRTVを作る
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrameCount; ++i) {
            ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, handle);
            handle.ptr += m_rtvSize;
        }

        // ---- アロケータとコマンドリスト ----
       
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator)));
        ThrowIfFailed(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr,
            IID_PPV_ARGS(&m_cmdList)));
        ThrowIfFailed(m_cmdList->Close());

        // ---- フェンス ----
        
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_fence)));
        m_fenceValue = 1;
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

       
        CreatePipeline();
        CreateVertexBuffer();

        // ビューポートとシザー
        m_viewport = { 0.0f, 0.0f, (float)kWidth, (float)kHeight, 0.0f, 1.0f };
        m_scissor = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    }

    void Render()
    {
        // 前フレームのGPU完了を待っているので、アロケータをリセットしてよい
        ThrowIfFailed(m_allocator->Reset());
        ThrowIfFailed(m_cmdList->Reset(m_allocator.Get(), nullptr));

        // ルートシグネチャ / PSO / ビューポート 
       
        m_cmdList->SetGraphicsRootSignature(m_rootSignature.Get());
        m_cmdList->SetPipelineState(m_pso.Get());
        m_cmdList->RSSetViewports(1, &m_viewport);
        m_cmdList->RSSetScissorRects(1, &m_scissor);

        // PRESENT → RENDER_TARGET
        
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_backBuffers[m_frameIndex].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_cmdList->ResourceBarrier(1, &barrier);

        // レンダーターゲット設定とクリア
       
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvSize;
        m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        const float clearColor[4] = { 0.1f, 0.3f, 0.6f, 1.0f };
        m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

        // 三角形の描画 
       
        m_cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_cmdList->IASetVertexBuffers(0, 1, &m_vbView);
        m_cmdList->DrawInstanced(3, 1, 0, 0);  // 頂点数, インスタンス数, 開始頂点, 開始インスタンス

        // RENDER_TARGET → PRESENT
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_cmdList->ResourceBarrier(1, &barrier);

        // 記録終了→提出→表示
        
        ThrowIfFailed(m_cmdList->Close());
        ID3D12CommandList* lists[] = { m_cmdList.Get() };
        m_queue->ExecuteCommandLists(1, lists);
        ThrowIfFailed(m_swapChain->Present(1, 0));

        
        WaitForGpu();
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    ~Renderer()
    {
        if (m_queue && m_fence) WaitForGpu();
        if (m_fenceEvent) CloseHandle(m_fenceEvent);
    }

private:
    // ---------------------------------------------------------
    // ルートシグネチャ / シェーダ / PSO の作成
    // ---------------------------------------------------------
    void CreatePipeline()
    {
        // ---- ルートシグネチャ ----
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, rsError;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &rsError));
        ThrowIfFailed(m_device->CreateRootSignature(
            0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS(&m_rootSignature)));

        // ---- シェーダコンパイル ---
        UINT compileFlags = 0;
#ifdef _DEBUG
        compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> vs, ps, err;

        HRESULT hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr,
            "VSMain", "vs_5_0", compileFlags, 0, &vs, &err);
        if (FAILED(hr)) {
         
            if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
            throw std::runtime_error("VS compile failed");
        }
        hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr,
            "PSMain", "ps_5_0", compileFlags, 0, &ps, &err);
        if (FAILED(hr)) {
            if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
            throw std::runtime_error("PS compile failed");
        }

        // ---- 入力レイアウト ---
        // 頂点構造体のメモリ配置をGPUに教える
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        // ---- PSO ----
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = m_rootSignature.Get();
        pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.InputLayout = { layout, _countof(layout) };

        // ラスタライザ状態
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // 表裏どちらも描く
        pd.RasterizerState.DepthClipEnable = TRUE;

        // ブレンド状態
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // 深度ステンシル
        pd.DepthStencilState.DepthEnable = FALSE;
        pd.DepthStencilState.StencilEnable = FALSE;

        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;  // RTと一致させる必要がある
        pd.SampleDesc.Count = 1;

        ThrowIfFailed(m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pso)));
    }

    // ---------------------------------------------------------
    // 頂点バッファの作成
    // ---------------------------------------------------------
    void CreateVertexBuffer()
    {
        // NDC座標（-1〜1）で直接指定。時計回りが表面（D3Dの既定）
        const Vertex vertices[] = {
            { {  0.0f,  0.5f, 0.0f }, { 1.0f, 0.0f, 0.0f, 1.0f } },  // 上：赤
            { {  0.5f, -0.5f, 0.0f }, { 0.0f, 1.0f, 0.0f, 1.0f } },  // 右下：緑
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } },  // 左下：青
        };
        const UINT size = sizeof(vertices);

       
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = size;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

       
        ThrowIfFailed(m_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_vertexBuffer)));

        // Map→コピー→Unmap。
        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };  
        ThrowIfFailed(m_vertexBuffer->Map(0, &readRange, &mapped));
        memcpy(mapped, vertices, size);
        m_vertexBuffer->Unmap(0, nullptr);

        // 頂点バッファビュー。
        m_vbView.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
        m_vbView.SizeInBytes = size;
        m_vbView.StrideInBytes = sizeof(Vertex);
    }

    void WaitForGpu()
    {
        const UINT64 value = m_fenceValue;
        ThrowIfFailed(m_queue->Signal(m_fence.Get(), value));
        m_fenceValue++;

        if (m_fence->GetCompletedValue() < value) {
            ThrowIfFailed(m_fence->SetEventOnCompletion(value, m_fenceEvent));
            WaitForSingleObject(m_fenceEvent, INFINITE);
        }
    }

    ComPtr<ID3D12Device>              m_device;
    ComPtr<ID3D12CommandQueue>        m_queue;
    ComPtr<IDXGISwapChain3>           m_swapChain;
    ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;
    ComPtr<ID3D12Resource>            m_backBuffers[kFrameCount];
    ComPtr<ID3D12CommandAllocator>    m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    ComPtr<ID3D12Fence>               m_fence;

    ComPtr<ID3D12RootSignature>       m_rootSignature;
    ComPtr<ID3D12PipelineState>       m_pso;
    ComPtr<ID3D12Resource>            m_vertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW          m_vbView = {};
    D3D12_VIEWPORT                    m_viewport = {};
    D3D12_RECT                        m_scissor = {};

    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    UINT   m_frameIndex = 0;
    UINT   m_rtvSize = 0;
};

// ---------------------------------------------------------
// エントリポイント
// ---------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow)
{
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = TEXT("DX12Portfolio");
    RegisterClassEx(&wc);

    RECT rc = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindow(wc.lpszClassName, TEXT("DX12 Portfolio"),
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    try {
        Renderer renderer;
        renderer.Init(hwnd);
        ShowWindow(hwnd, nCmdShow);

     
        MSG msg = {};
        while (msg.message != WM_QUIT) {
            if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            else {
                renderer.Render();
            }
        }
    }
    catch (const std::exception& e) {
        MessageBoxA(hwnd, e.what(), "Error", MB_OK);
        return -1;
    }
    return 0;
}