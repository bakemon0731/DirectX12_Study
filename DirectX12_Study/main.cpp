#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl.h>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

constexpr UINT kWidth = 1280;
constexpr UINT kHeight = 720;
constexpr UINT kFrameCount = 2;   // バックバッファ枚数

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr)) throw std::runtime_error("HRESULT failed");
}

// ---------------------------------------------------------
// ウィンドウプロシージャ
// ---------------------------------------------------------
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
        // デバッグレイヤー（開発中は必ず有効に。エラー内容が出力ウィンドウに出る）
        UINT dxgiFlags = 0;
#ifdef _DEBUG
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        // ファクトリとデバイス
        ComPtr<IDXGIFactory4> factory;
        ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory)));
        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)));

        // コマンドキュー
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)));

        // スワップチェーン
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
        ThrowIfFailed(sc1.As(&m_swapChain));   // IDXGISwapChain3 へ変換
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

        // RTV用ディスクリプタヒープ
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = kFrameCount;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        // バックバッファごとにRTVを作る
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrameCount; ++i) {
            ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, handle);
            handle.ptr += m_rtvSize;
        }

        // コマンドアロケータとコマンドリスト
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_allocator)));
        ThrowIfFailed(m_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr,
            IID_PPV_ARGS(&m_cmdList)));
        ThrowIfFailed(m_cmdList->Close());   // 作成直後は記録状態なので一旦閉じる

        // フェンス
        ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&m_fence)));
        m_fenceValue = 1;
        m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    }

    void Render()
    {
        // 1. 記録の準備（前フレームのGPU処理は完了済みなのでリセットできる）
        ThrowIfFailed(m_allocator->Reset());
        ThrowIfFailed(m_cmdList->Reset(m_allocator.Get(), nullptr));

        // 2. PRESENT → RENDER_TARGET
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_backBuffers[m_frameIndex].Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        m_cmdList->ResourceBarrier(1, &barrier);

        // 3. 描画先を設定してクリア
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(m_frameIndex) * m_rtvSize;
        m_cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

        const float clearColor[4] = { 0.1f, 0.3f, 0.6f, 1.0f };
        m_cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

        // 4. RENDER_TARGET → PRESENT
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_cmdList->ResourceBarrier(1, &barrier);

        // 5. 記録終了 → 提出 → 表示
        ThrowIfFailed(m_cmdList->Close());
        ID3D12CommandList* lists[] = { m_cmdList.Get() };
        m_queue->ExecuteCommandLists(1, lists);
        ThrowIfFailed(m_swapChain->Present(1, 0));   // 1 = VSync有効

        // 6. GPU完了待ち（第10回で改良する）
        WaitForGpu();
        m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    }

    ~Renderer()
    {
        if (m_queue && m_fence) WaitForGpu();
        if (m_fenceEvent) CloseHandle(m_fenceEvent);
    }

private:
    void WaitForGpu()
    {
        const UINT64 value = m_fenceValue;
        ThrowIfFailed(m_queue->Signal(m_fence.Get(), value));   // GPUが到達したら値を書く
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

    // クライアント領域が 1280x720 になるようウィンドウサイズを補正
    RECT rc = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindow(wc.lpszClassName, TEXT("DX12 Portfolio"),
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,  // リサイズ非対応の間は固定
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