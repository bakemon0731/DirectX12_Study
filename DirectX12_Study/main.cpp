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
};
