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
