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
// バックバッファ枚数
constexpr UINT kFrameCount = 2;   

inline void ThrowIfFailed(HRESULT hr)
{
	if (FAILED(hr)) throw std::runtime_error("HRESULT failed");
}

//ウィンドウプロシージャ
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	//ボタンを押すなど、WindowがDESTROYされたとき
	if (msg == WM_DESTROY) 
	{
		//OSに対して「このアプリを終了」する合図を送る
		PostQuitMessage(0);
		return 0;
	}
	//自分で処理しなかったその他の大量のメッセージをWindowsのデフォルト処理に丸投げする関数
	return DefWindowProc(hwnd, msg, wp, lp);
}

//レンダラー
class Renderer
{
public:
	void Init(HWND hwnd)
	{
		// デバッグレイヤー
		UINT dxgiFlags = 0;

		//デバッグ時にのみ、この中の処理を行う
#ifdef _DEBUG
		// D3D12のデバッグ機能を管理する「手下（インターフェース）」を指すポインタを用意
		ComPtr<ID3D12Debug> debug;

		// DirectX 12のデバッグ機能を司るインターフェースを取得
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
		{
			//エラー検知機能を「ON」にする
			debug->EnableDebugLayer();
			//次の「ファクトリ生成」の処理でもデバッグ機能を有効にするため、目印を立てる
			dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}

#endif
		// ファクトリとデバイス

		//グラフィックスの管理・生成を行う「ファクトリ」のポインタを用意
		ComPtr<IDXGIFactory4> factory;
		//ファクトリを実際に生成。（デバックのフラグも渡す）
		ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory)));
		//デバイスを生成。GPUを操作操作するため。
		ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&m_device)));


		//コマンドキュー

		//コマンドキューの設定（設計図）を入れる構造体。
		D3D12_COMMAND_QUEUE_DESC qd = {};
		//コマンドの種類を「DIRECT（標準的な描画・計算命令用）」に指定。
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		//設計図（qd）をもとに、デバイスをつかってコマンドをキュー
		ThrowIfFailed(m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue)));

		//スワップチェーン
		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width = kWidth;
		sd.Height = kHeight;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = kFrameCount;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

		
