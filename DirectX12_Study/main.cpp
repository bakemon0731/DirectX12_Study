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
        /*
        * デバッグレイヤー
        * 
        */ 
        UINT dxgiFlags = 0;
#ifdef _DEBUG
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
#endif

        /*
        * ファクトリとデバイス
        * 「ファクトリ（工場）」と「デバイス（グラフィックスカードの本体）」を生成・準備する処理
        */
        
        ComPtr<IDXGIFactory4> factory;
        ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory)));
        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&m_device)));

        /*
        * コマンドキュー
        * CPUからGPU（グラフィックスカード）へ、描画などの命令の束（コマンド）を送り届けるための
        *『専用ポスト（または一方通行の道路）』を作る処理
        */
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)));

        /*
        * スワップチェーン
        * ゲームの描画中に画面がチラつくのを防ぐため、
        * 2枚の画面を裏で交互に切り替えながらスムーズにテレビやモニターに映像を出力するシステム
        */
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

        /*
        * RTV用ディスクリプタヒープ
        * 2枚の画面（バックバッファ）を、DirectX 12に『ここが描画の出力先（レンダーターゲット）だよ』と
        * 教えるための『名札（ビュー）』を並べて置いておくための棚（ヒープ）を作る処理
        */
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.NumDescriptors = kFrameCount;
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtvHeap)));
        m_rtvSize = m_device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        /*
        * バックバッファごとにRTVを作る
        * 「名札を置く棚（ヒープ）」のなかに、2枚の画面（バックバッファ）と紐づいた
        * 「描画の出力先だよという名札（RTV）」を実際に作成して1つずつ並べていく処理
        */
        D3D12_CPU_DESCRIPTOR_HANDLE handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < kFrameCount; ++i) {
            ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])));
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, handle);
            handle.ptr += m_rtvSize;
        }

        /*
        * コマンドアロケータとコマンドリスト
        * 「GPUへの命令を書き込むための『白紙のノート（リスト）』と、
        *  そのノートの紙となる『メモリ領域（アロケータ）』を用意し、
        *  いつでも使えるように一度ノートを閉じる処理」
        */
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

}