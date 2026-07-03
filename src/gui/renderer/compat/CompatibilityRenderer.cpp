#include "CompatibilityRenderer.hpp"

#include "config/Config.hpp"
#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/engine/classes/Bones.hpp"
#include "core/engine/types/Weapons.hpp"
#include "gui/frontend/menu/Menu.hpp"

#include <d3d11.h>
#include <imgui/backends/imgui_impl_dx11.h>
#include <imgui/backends/imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
	constexpr COLORREF kOverlayTransparencyKey = RGB(255, 0, 255);
	constexpr char kOverlayClassName[] = "cs2esp_compat_overlay";
	constexpr char kMenuClassName[] = "cs2esp_compat_menu";

	COLORREF ToColorRef(const color_t& color)
	{
		return RGB(
			static_cast<int>(std::clamp(color.r, 0.0f, 1.0f) * 255.0f),
			static_cast<int>(std::clamp(color.g, 0.0f, 1.0f) * 255.0f),
			static_cast<int>(std::clamp(color.b, 0.0f, 1.0f) * 255.0f)
		);
	}

	COLORREF ToColorRef(int r, int g, int b)
	{
		return RGB(r, g, b);
	}

	struct GdiOverlayWindow {
		HWND hwnd = nullptr;
		WNDCLASSEXA wc{};
		HDC buffer_dc = nullptr;
		HBITMAP buffer_bitmap = nullptr;
		HGDIOBJ old_bitmap = nullptr;
		int x = 0;
		int y = 0;
		int width = 1;
		int height = 1;
		bool visible = true;

		bool Init()
		{
			wc.cbSize = sizeof(wc);
			wc.hInstance = GetModuleHandleA(nullptr);
			wc.lpfnWndProc = WndProc;
			wc.lpszClassName = kOverlayClassName;

			if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
				return false;

			hwnd = CreateWindowExA(
				WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
				kOverlayClassName,
				"cs2-external-esp compatibility overlay",
				WS_POPUP,
				0,
				0,
				1,
				1,
				nullptr,
				nullptr,
				wc.hInstance,
				nullptr
			);

			if (!hwnd)
				return false;

			ShowWindow(hwnd, SW_SHOWNOACTIVATE);
			UpdateWindow(hwnd);

			return EnsureBuffer(1, 1);
		}

		void Destroy()
		{
			DestroyBuffer();

			if (hwnd) {
				DestroyWindow(hwnd);
				hwnd = nullptr;
			}

			if (wc.lpszClassName)
				UnregisterClassA(wc.lpszClassName, wc.hInstance);
		}

		bool EnsureBuffer(int new_width, int new_height)
		{
			new_width = std::max(new_width, 1);
			new_height = std::max(new_height, 1);

			if (buffer_dc && width == new_width && height == new_height)
				return true;

			DestroyBuffer();

			BITMAPINFO bmi{};
			bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bmi.bmiHeader.biWidth = new_width;
			bmi.bmiHeader.biHeight = -new_height; // top-down
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;

			void* bits = nullptr;
			HDC screen_dc = GetDC(nullptr);
			buffer_dc = CreateCompatibleDC(screen_dc);
			buffer_bitmap = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
			ReleaseDC(nullptr, screen_dc);

			if (!buffer_dc || !buffer_bitmap)
				return false;

			old_bitmap = SelectObject(buffer_dc, buffer_bitmap);
			width = new_width;
			height = new_height;
			return true;
		}

		void DestroyBuffer()
		{
			if (buffer_dc && old_bitmap) {
				SelectObject(buffer_dc, old_bitmap);
				old_bitmap = nullptr;
			}

			if (buffer_bitmap) {
				DeleteObject(buffer_bitmap);
				buffer_bitmap = nullptr;
			}

			if (buffer_dc) {
				DeleteDC(buffer_dc);
				buffer_dc = nullptr;
			}
		}

		void Clear()
		{
			RECT rect{ 0, 0, width, height };
			HBRUSH brush = CreateSolidBrush(kOverlayTransparencyKey);
			FillRect(buffer_dc, &rect, brush);
			DeleteObject(brush);
		}

		void Present()
		{
			if (!visible || !hwnd || !buffer_dc)
				return;

			POINT dst{ x, y };
			POINT src{ 0, 0 };
			SIZE size{ width, height };
			BLENDFUNCTION blend{};
			blend.BlendOp = AC_SRC_OVER;
			blend.SourceConstantAlpha = 255;

			UpdateLayeredWindow(
				hwnd,
				nullptr,
				&dst,
				&size,
				buffer_dc,
				&src,
				kOverlayTransparencyKey,
				&blend,
				ULW_COLORKEY
			);
		}

		void SetVisible(bool state)
		{
			if (!hwnd || visible == state)
				return;

			visible = state;
			ShowWindow(hwnd, state ? SW_SHOWNOACTIVATE : SW_HIDE);
		}

		bool SetBounds(const RECT& rect)
		{
			const int new_width = static_cast<int>(std::max<LONG>(rect.right - rect.left, 1));
			const int new_height = static_cast<int>(std::max<LONG>(rect.bottom - rect.top, 1));
			x = rect.left;
			y = rect.top;

			if (!EnsureBuffer(new_width, new_height))
				return false;

			return SetWindowPos(
				hwnd,
				HWND_TOPMOST,
				rect.left,
				rect.top,
				new_width,
				new_height,
				SWP_NOACTIVATE | SWP_SHOWWINDOW
			) != FALSE;
		}

		Vec2_t DisplaySize() const
		{
			return { static_cast<float>(width), static_cast<float>(height) };
		}

		static LRESULT CALLBACK WndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			switch (msg) {
			case WM_MOUSEACTIVATE:
				return MA_NOACTIVATE;
			case WM_CLOSE:
				return 0;
			default:
				break;
			}

			return DefWindowProcA(window, msg, wParam, lParam);
		}
	};

	class CompatibilityMenuHost {
	public:
		bool Init()
		{
			wc_.cbSize = sizeof(wc_);
			wc_.hInstance = GetModuleHandleA(nullptr);
			wc_.lpfnWndProc = WndProc;
			wc_.lpszClassName = kMenuClassName;

			if (!RegisterClassExA(&wc_) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
				return false;

			const int width = 640;
			const int height = 420;
			const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
			const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

			self_ = this;

			hwnd_ = CreateWindowExA(
				WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
				kMenuClassName,
				"cs2-external-esp compatibility menu",
				WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
				x,
				y,
				width,
				height,
				nullptr,
				nullptr,
				wc_.hInstance,
				nullptr
			);

			if (!hwnd_)
				return false;

			if (!CreateDevice())
				return false;

			ImGui::CreateContext();
			ImGui::StyleColorsDark();

			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

			if (!ImGui_ImplWin32_Init(hwnd_))
				return false;

			if (!ImGui_ImplDX11_Init(device_, device_context_))
				return false;

			Menu::Init();

			ShowWindow(hwnd_, SW_HIDE);
			UpdateWindow(hwnd_);

			return true;
		}

		void Destroy()
		{
			if (device_) {
				ImGui_ImplDX11_Shutdown();
				ImGui_ImplWin32_Shutdown();
				if (ImGui::GetCurrentContext())
					ImGui::DestroyContext();
				DestroyDevice();
			}

			if (hwnd_) {
				DestroyWindow(hwnd_);
				hwnd_ = nullptr;
			}

			if (wc_.lpszClassName)
				UnregisterClassA(wc_.lpszClassName, wc_.hInstance);

			self_ = nullptr;
		}

		void Show()
		{
			if (!hwnd_)
				return;

			visible_ = true;
			close_requested_ = false;
			ShowWindow(hwnd_, SW_SHOW);
			SetForegroundWindow(hwnd_);
		}

		void Hide()
		{
			if (!hwnd_)
				return;

			visible_ = false;
			ShowWindow(hwnd_, SW_HIDE);
		}

		void Render()
		{
			if (!visible_ || !hwnd_)
				return;

			ImGui_ImplDX11_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();

			Menu::Render();

			ImGui::Render();

			const float clear_color[4]{ 0.04f, 0.04f, 0.05f, 1.0f };
			device_context_->OMSetRenderTargets(1, &render_target_view_, nullptr);
			device_context_->ClearRenderTargetView(render_target_view_, clear_color);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

			swap_chain_->Present(vsync_ ? 1U : 0U, 0U);
		}

		void SetVSync(bool enabled)
		{
			vsync_ = enabled;
		}

		bool ConsumeCloseRequest()
		{
			bool requested = close_requested_;
			close_requested_ = false;
			return requested;
		}

		HWND Hwnd() const
		{
			return hwnd_;
		}

	private:
		bool CreateDevice()
		{
			DXGI_SWAP_CHAIN_DESC sd{};
			sd.BufferCount = 2;
			sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			sd.OutputWindow = hwnd_;
			sd.SampleDesc.Count = 1;
			sd.Windowed = TRUE;
			sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

			const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
			D3D_FEATURE_LEVEL created_level{};

			HRESULT result = D3D11CreateDeviceAndSwapChain(
				nullptr,
				D3D_DRIVER_TYPE_HARDWARE,
				nullptr,
				0U,
				levels,
				2,
				D3D11_SDK_VERSION,
				&sd,
				&swap_chain_,
				&device_,
				&created_level,
				&device_context_
			);

			if (result == DXGI_ERROR_UNSUPPORTED) {
				result = D3D11CreateDeviceAndSwapChain(
					nullptr,
					D3D_DRIVER_TYPE_WARP,
					nullptr,
					0U,
					levels,
					2,
					D3D11_SDK_VERSION,
					&sd,
					&swap_chain_,
					&device_,
					&created_level,
					&device_context_
				);
			}

			if (result != S_OK)
				return false;

			return CreateRenderTarget();
		}

		bool CreateRenderTarget()
		{
			ID3D11Texture2D* back_buffer = nullptr;
			if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
				return false;

			const bool ok = SUCCEEDED(device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_view_));
			back_buffer->Release();
			return ok;
		}

		void DestroyDevice()
		{
			if (render_target_view_) {
				render_target_view_->Release();
				render_target_view_ = nullptr;
			}

			if (swap_chain_) {
				swap_chain_->Release();
				swap_chain_ = nullptr;
			}

			if (device_context_) {
				device_context_->Release();
				device_context_ = nullptr;
			}

			if (device_) {
				device_->Release();
				device_ = nullptr;
			}
		}

		void Resize(UINT width, UINT height)
		{
			if (!device_ || width == 0 || height == 0)
				return;

			if (render_target_view_) {
				render_target_view_->Release();
				render_target_view_ = nullptr;
			}

			swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
		}

		static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
		{
			if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
				return true;

			switch (msg) {
			case WM_CLOSE:
				if (self_) {
					self_->close_requested_ = true;
					self_->Hide();
				}
				return 0;
			case WM_SIZE:
				if (self_ && wParam != SIZE_MINIMIZED)
					self_->Resize(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
				return 0;
			default:
				break;
			}

			return DefWindowProcA(hwnd, msg, wParam, lParam);
		}

	private:
		inline static CompatibilityMenuHost* self_ = nullptr;

		HWND hwnd_ = nullptr;
		WNDCLASSEXA wc_{};
		ID3D11Device* device_ = nullptr;
		ID3D11DeviceContext* device_context_ = nullptr;
		IDXGISwapChain* swap_chain_ = nullptr;
		ID3D11RenderTargetView* render_target_view_ = nullptr;
		bool visible_ = false;
		bool close_requested_ = false;
		bool vsync_ = false;
	};

	struct CompatibilityRendererImpl {
		GdiOverlayWindow overlay;
		CompatibilityMenuHost menu;
		bool running = true;
		bool open = false;
		bool focused = false;
		bool startup_hint_seen = false;
		Vec2_t display_size{};
		view_matrix_t matrix{};
		size_t vel_index = 0;
		std::vector<int> vel_buffer;
		float vel_accumulator = 0.0f;
		std::chrono::steady_clock::time_point last_frame = std::chrono::steady_clock::now();
		float delta_time = 1.0f / 60.0f;
		float fps = 60.0f;
		std::map<std::pair<int, int>, HFONT> fonts;

		HFONT GetFont(int size, bool bold = false)
		{
			const std::pair<int, int> key{ size, bold ? 1 : 0 };
			if (auto found = fonts.find(key); found != fonts.end())
				return found->second;

			HFONT font = CreateFontA(
				-size,
				0,
				0,
				0,
				bold ? FW_BOLD : FW_NORMAL,
				FALSE,
				FALSE,
				FALSE,
				DEFAULT_CHARSET,
				OUT_DEFAULT_PRECIS,
				CLIP_DEFAULT_PRECIS,
				NONANTIALIASED_QUALITY,
				DEFAULT_PITCH | FF_DONTCARE,
				"Arial"
			);

			fonts[key] = font;
			return font;
		}

		void DestroyFonts()
		{
			for (const auto& [_, font] : fonts)
				DeleteObject(font);

			fonts.clear();
		}

		SIZE MeasureText(HDC hdc, const std::string& text, int size, bool bold = false)
		{
			HFONT font = GetFont(size, bold);
			HGDIOBJ previous = SelectObject(hdc, font);
			SIZE result{};
			GetTextExtentPoint32A(hdc, text.c_str(), static_cast<int>(text.size()), &result);
			SelectObject(hdc, previous);
			return result;
		}

		void DrawText(HDC hdc, int x, int y, const std::string& text, COLORREF color, int size, bool bold = false, bool shadow = true)
		{
			if (text.empty())
				return;

			HFONT font = GetFont(size, bold);
			HGDIOBJ previous = SelectObject(hdc, font);
			SetBkMode(hdc, TRANSPARENT);

			if (shadow) {
				SetTextColor(hdc, RGB(0, 0, 0));
				TextOutA(hdc, x + 1, y + 1, text.c_str(), static_cast<int>(text.size()));
			}

			SetTextColor(hdc, color);
			TextOutA(hdc, x, y, text.c_str(), static_cast<int>(text.size()));
			SelectObject(hdc, previous);
		}

		void DrawLine(HDC hdc, int x1, int y1, int x2, int y2, COLORREF color, int thickness = 1)
		{
			HPEN pen = CreatePen(PS_SOLID, thickness, color);
			HGDIOBJ old_pen = SelectObject(hdc, pen);
			MoveToEx(hdc, x1, y1, nullptr);
			LineTo(hdc, x2, y2);
			SelectObject(hdc, old_pen);
			DeleteObject(pen);
		}

		void DrawRect(HDC hdc, int x, int y, int width, int height, COLORREF border, int thickness = 1)
		{
			HPEN pen = CreatePen(PS_SOLID, thickness, border);
			HBRUSH brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
			HGDIOBJ old_pen = SelectObject(hdc, pen);
			HGDIOBJ old_brush = SelectObject(hdc, brush);
			Rectangle(hdc, x, y, x + width, y + height);
			SelectObject(hdc, old_pen);
			SelectObject(hdc, old_brush);
			DeleteObject(pen);
		}

		void FillRectColor(HDC hdc, int x, int y, int width, int height, COLORREF color)
		{
			RECT rect{ x, y, x + width, y + height };
			HBRUSH brush = CreateSolidBrush(color);
			FillRect(hdc, &rect, brush);
			DeleteObject(brush);
		}

		void DrawFilledCircle(HDC hdc, int x, int y, int radius, COLORREF fill, COLORREF border)
		{
			HPEN pen = CreatePen(PS_SOLID, 1, border);
			HBRUSH brush = CreateSolidBrush(fill);
			HGDIOBJ old_pen = SelectObject(hdc, pen);
			HGDIOBJ old_brush = SelectObject(hdc, brush);
			Ellipse(hdc, x - radius, y - radius, x + radius, y + radius);
			SelectObject(hdc, old_pen);
			SelectObject(hdc, old_brush);
			DeleteObject(pen);
			DeleteObject(brush);
		}

		void DrawCircle(HDC hdc, int x, int y, int radius, COLORREF border)
		{
			HPEN pen = CreatePen(PS_SOLID, 1, border);
			HBRUSH brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
			HGDIOBJ old_pen = SelectObject(hdc, pen);
			HGDIOBJ old_brush = SelectObject(hdc, brush);
			Ellipse(hdc, x - radius, y - radius, x + radius, y + radius);
			SelectObject(hdc, old_pen);
			SelectObject(hdc, old_brush);
			DeleteObject(pen);
		}

		void DrawPanel(HDC hdc, int x, int y, int width, int height, COLORREF fill, COLORREF border)
		{
			HPEN pen = CreatePen(PS_SOLID, 1, border);
			HBRUSH brush = CreateSolidBrush(fill);
			HGDIOBJ old_pen = SelectObject(hdc, pen);
			HGDIOBJ old_brush = SelectObject(hdc, brush);
			RoundRect(hdc, x, y, x + width, y + height, 10, 10);
			SelectObject(hdc, old_pen);
			SelectObject(hdc, old_brush);
			DeleteObject(pen);
			DeleteObject(brush);
		}

		void PumpMessages()
		{
			MSG msg{};
			while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
				if (msg.message == WM_QUIT)
					running = false;

				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}

		bool Init()
		{
			running = true;
			open = false;
			startup_hint_seen = false;
			vel_accumulator = 0.0f;
			vel_index = 0;
			last_frame = std::chrono::steady_clock::now();

			if (!overlay.Init())
				return false;

			if (!menu.Init())
				return false;

			menu.SetVSync(cfg::settings::vsync);
			ResizeVelocityBuffer();
			return true;
		}

		void Shutdown()
		{
			menu.Destroy();
			overlay.Destroy();
			DestroyFonts();
		}

		void ResizeVelocityBuffer()
		{
			const auto samples = static_cast<size_t>(std::max(cfg::world::velocity::sample_rate, 1) * std::max(cfg::world::velocity::sample_length, 1.0f));
			vel_buffer.assign(std::max<size_t>(samples, 2), 0);
			vel_index = 0;
			vel_accumulator = 0.0f;
		}

		void SetOpen(bool state)
		{
			if (open == state)
				return;

			open = state;
			if (open) {
				startup_hint_seen = true;
				menu.Show();
			}
			else {
				menu.Hide();
			}
		}

		void Tick()
		{
			using namespace std::chrono_literals;
			constexpr auto frame_budget = 16ms;

			while (running) {
				const auto frame_start = std::chrono::steady_clock::now();

				PumpMessages();

				if (!running)
					break;

				if (menu.ConsumeCloseRequest())
					SetOpen(false);

				HandleState();

				if (!HandleWindowOrder()) {
					if (cfg::settings::free_cpu)
						std::this_thread::sleep_for(10ms);
					continue;
				}

				RenderFrame();

				const auto frame_elapsed = std::chrono::steady_clock::now() - frame_start;
				if (frame_elapsed < frame_budget)
					std::this_thread::sleep_for(frame_budget - frame_elapsed);
				else if (cfg::settings::free_cpu)
					std::this_thread::sleep_for(1ms);
			}

			Shutdown();
		}

		void HandleState()
		{
			static bool was_holding = false;
			static bool was_ending = false;

			const bool pressed_insert = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
			const bool pressed_lshift = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
			const bool pressed_rshift_raw = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
			const bool pressed_shift_generic = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			const bool pressed_rshift = pressed_rshift_raw || (pressed_shift_generic && !pressed_lshift);
			const bool pressed_end = (GetAsyncKeyState(VK_END) & 0x8000) != 0;

			const bool should_toggle = !was_holding && (pressed_insert || pressed_rshift);
			const bool should_end = !was_ending && pressed_end;

			if (should_toggle) {
				SetOpen(!open);
				std::thread(Config::Write).detach();
			}

			if (should_end) {
				running = false;
				std::thread(Config::Write).detach();
			}

			was_holding = pressed_insert || pressed_rshift;
			was_ending = pressed_end;
		}

		bool HandleWindowOrder()
		{
			auto process = Engine::GetProcess();
			if (!process || (!process->hwnd_ && !process->UpdateHWND()))
				return false;

			if (!IsWindow(process->hwnd_)) {
				running = false;
				return false;
			}

			const HWND foreground = GetForegroundWindow();
			focused = foreground == process->hwnd_
				|| foreground == overlay.hwnd
				|| (open && foreground == menu.Hwnd());

			overlay.SetVisible(focused);

			if (!focused)
				return false;

			RECT client_rect{};
			RECT window_rect{};
			if (!GetClientRect(process->hwnd_, &client_rect) || !GetWindowRect(process->hwnd_, &window_rect))
				return false;

			POINT top_left{ client_rect.left, client_rect.top };
			POINT bottom_right{ client_rect.right, client_rect.bottom };
			ClientToScreen(process->hwnd_, &top_left);
			ClientToScreen(process->hwnd_, &bottom_right);

			RECT bounds{
				top_left.x,
				top_left.y,
				bottom_right.x,
				bottom_right.y
			};

			return overlay.SetBounds(bounds);
		}

		void RenderFrame()
		{
			const auto now = std::chrono::steady_clock::now();
			delta_time = std::chrono::duration<float>(now - last_frame).count();
			last_frame = now;
			if (delta_time > 0.0f)
				fps = 1.0f / delta_time;

			overlay.Clear();
			display_size = overlay.DisplaySize();

			HDC hdc = overlay.buffer_dc;
			Snapshot snapshot = Cache::CopySnapshot();
			matrix = snapshot.game.view_matrix;

			if (cfg::enabled) {
				RenderPlayers(hdc, snapshot);
				RenderCrosshair(hdc, snapshot.local);
			}

			RenderWatermark(hdc, snapshot);
			RenderSpectatorList(hdc, snapshot);
			RenderSpeedChart(hdc, snapshot);
			RenderRadar(hdc, snapshot);
			RenderBomb(hdc, snapshot);
			RenderStartupHint(hdc);

			overlay.Present();
			menu.Render();
		}

		void RenderPlayers(HDC hdc, const Snapshot& snapshot)
		{
			const auto& local = snapshot.local;

			for (const auto& player : snapshot.players) {
				if (!player.alive || player.localplayer)
					continue;

				const bool mate = player.team == local.team;
				if (!cfg::esp::team && mate)
					continue;
				if (cfg::esp::spotted && !player.spotted)
					continue;
				if (local.observer_services.target == player.pawn_controller_addr && local.observer_services.mode == ObserverMode::First)
					continue;

				RenderPlayerTracers(hdc, local, player, mate);
				RenderPlayer(hdc, player, mate);
			}
		}

		void RenderPlayer(HDC hdc, Player player, bool mate)
		{
			std::pair<Vec2_t, Vec2_t> bounds;
			if (!player.GetBounds(matrix, display_size, bounds) || !player.alive)
				return;

			if (cfg::esp::box) {
				const auto color = mate ? cfg::esp::colors::box_team : cfg::esp::colors::box_enemy;
				DrawRect(
					hdc,
					static_cast<int>(bounds.first.x),
					static_cast<int>(bounds.first.y),
					static_cast<int>(bounds.second.x - bounds.first.x),
					static_cast<int>(bounds.second.y - bounds.first.y),
					ToColorRef(color)
				);
			}

			if (cfg::esp::skeleton)
				RenderPlayerBones(hdc, player, mate);

			if (cfg::esp::head_tracker)
				RenderPlayerTracker(hdc, player, bounds, mate);

			RenderPlayerBars(hdc, player, bounds);
			RenderPlayerFlags(hdc, player, bounds, mate);
		}

		void RenderPlayerBones(HDC hdc, const Player& player, bool mate)
		{
			const COLORREF color = ToColorRef(mate ? cfg::esp::colors::skeleton_team : cfg::esp::colors::skeleton_enemy);

			for (const auto& bone : connections) {
				if (player.bone_list.size() <= static_cast<size_t>(bone[0]) || player.bone_list.size() <= static_cast<size_t>(bone[1]))
					continue;

				Vec2_t first{};
				Vec2_t second{};

				if (!matrix.wts(player.bone_list[bone[0]].pos, display_size, first))
					continue;
				if (!matrix.wts(player.bone_list[bone[1]].pos, display_size, second))
					continue;

				DrawLine(
					hdc,
					static_cast<int>(first.x),
					static_cast<int>(first.y),
					static_cast<int>(second.x),
					static_cast<int>(second.y),
					color
				);
			}
		}

		void RenderPlayerTracker(HDC hdc, const Player& player, const std::pair<Vec2_t, Vec2_t>& bounds, bool mate)
		{
			if (player.bone_list.empty())
				return;

			Vec2_t head{};
			if (!matrix.wts(player.bone_list[bone_index::head].pos, display_size, head))
				return;

			const auto width = bounds.second.x - bounds.first.x;
			DrawCircle(
				hdc,
				static_cast<int>(head.x),
				static_cast<int>(head.y),
				static_cast<int>(width / 6.0f),
				ToColorRef(mate ? cfg::esp::colors::tracker_team : cfg::esp::colors::tracker_enemy)
			);
		}

		void RenderPlayerBars(HDC hdc, const Player& player, const std::pair<Vec2_t, Vec2_t>& bounds)
		{
			const int left = static_cast<int>(bounds.first.x);
			const int top = static_cast<int>(bounds.first.y);
			const int right = static_cast<int>(bounds.second.x);
			const int bottom = static_cast<int>(bounds.second.y);
			const int height = std::max(bottom - top, 1);
			const int width = std::max(right - left, 1);

			if (cfg::esp::health) {
				const int bar_x = left - 6;
				const int filled = static_cast<int>(height * (std::clamp(player.health, 0, 100) / 100.0f));
				DrawRect(hdc, bar_x, top, 3, height, RGB(0, 0, 0));
				FillRectColor(hdc, bar_x + 1, bottom - filled, 1, filled, RGB(100, 255, 100));

				if (cfg::esp::health_number && player.health < 100) {
					const auto text = std::to_string(player.health);
					const SIZE size = MeasureText(hdc, text, 11);
					DrawText(hdc, bar_x - size.cx / 2, bottom - filled - size.cy / 2, text, RGB(255, 255, 255), 11);
				}
			}

			if (cfg::esp::armor) {
				const int bar_y = bottom + 4;
				const int filled = static_cast<int>(width * (std::clamp(player.armor, 0, 100) / 100.0f));
				DrawRect(hdc, left, bar_y, width, 3, RGB(0, 0, 0));
				FillRectColor(hdc, left + 1, bar_y + 1, std::max(filled - 2, 0), 1, RGB(150, 150, 255));
			}
		}

		void RenderPlayerFlags(HDC hdc, const Player& player, const std::pair<Vec2_t, Vec2_t>& bounds, bool mate)
		{
			const int center_x = static_cast<int>((bounds.first.x + bounds.second.x) * 0.5f);
			const int top = static_cast<int>(bounds.first.y);
			const int bottom = static_cast<int>(bounds.second.y);
			const int right = static_cast<int>(bounds.second.x);
			int right_offset = 0;

			if (cfg::esp::flags::name) {
				std::string name = player.name;
				if (player.bot)
					name += " (Bot)";
				const SIZE size = MeasureText(hdc, name, 12, true);
				DrawText(hdc, center_x - size.cx / 2, top - 18, name, RGB(255, 255, 255), 12, true);
			}

			if (cfg::esp::flags::ammo && player.ammo != -1) {
				const auto ammo = std::to_string(player.ammo);
				const SIZE size = MeasureText(hdc, ammo, 11);
				DrawText(hdc, center_x - size.cx / 2, bottom + 18, ammo, RGB(255, 255, 255), 11);
			}

			if (cfg::esp::flags::weapon) {
				const std::string weapon = player.weapon.name.empty() ? "Unknown" : player.weapon.name;
				const SIZE size = MeasureText(hdc, weapon, 11);
				DrawText(hdc, center_x - size.cx / 2, bottom + 4, weapon, RGB(255, 255, 255), 11);
			}

			auto draw_right_text = [&](const std::string& text, COLORREF color) {
				DrawText(hdc, right + 8, top - right_offset, text, color, 11);
				right_offset -= 14;
			};

			if (cfg::esp::flags::money && player.money)
				draw_right_text(std::format("{}$", player.money), RGB(255, 255, 255));

			if (cfg::esp::flags::ping && player.ping)
				draw_right_text(std::format("{}ms", player.ping), RGB(255, 255, 255));

			const auto flashed = mate ? cfg::esp::colors::flags::flashed_team : cfg::esp::colors::flags::flashed_enemy;
			const auto reloading = mate ? cfg::esp::colors::flags::reloading_team : cfg::esp::colors::flags::reloading_enemy;
			const auto defusing = mate ? cfg::esp::colors::flags::defusing_team : cfg::esp::colors::flags::defusing_enemy;
			const auto scoped = mate ? cfg::esp::colors::flags::scoped_team : cfg::esp::colors::flags::scoped_enemy;
			const auto c4 = mate ? cfg::esp::colors::flags::c4_team : cfg::esp::colors::flags::c4_enemy;

			if (cfg::esp::flags::flashed && player.flashed || cfg::dev::force_show_flags)
				draw_right_text("FLASH", ToColorRef(flashed));

			if (cfg::esp::flags::reloading && player.is_reloading || cfg::dev::force_show_flags)
				draw_right_text("RELOAD", ToColorRef(reloading));

			if (cfg::esp::flags::defusing && player.defusing || cfg::dev::force_show_flags)
				draw_right_text("DEFUSE", ToColorRef(defusing));

			if (cfg::esp::flags::scoped && player.scoped || cfg::dev::force_show_flags)
				draw_right_text("SCOPED", ToColorRef(scoped));

			if (cfg::esp::flags::has_c4 && player.has_c4 || cfg::dev::force_show_flags) {
				const bool blink = player.weapon.item_index == weapon_c4 && ((GetTickCount64() / 180) % 2 == 0);
				if (player.weapon.item_index != weapon_c4 || blink)
					draw_right_text("C4", ToColorRef(c4));
			}
		}

		void RenderPlayerTracers(HDC hdc, const Player& source, const Player& player, bool mate)
		{
			if (!cfg::esp::tracers)
				return;

			Vec2_t screen_pos{};
			bool projected = matrix.wts(player.pos, display_size, screen_pos, false);

			if (!projected) {
				Vec3_t dir = player.pos - source.pos;
				Vec3_t view_dir{
					matrix[0][0] * dir.x + matrix[0][1] * dir.y + matrix[0][2] * dir.z,
					matrix[1][0] * dir.x + matrix[1][1] * dir.y + matrix[1][2] * dir.z,
					matrix[2][0] * dir.x + matrix[2][1] * dir.y + matrix[2][2] * dir.z
				};

				if (view_dir.z > 0.0f) {
					view_dir.x = -view_dir.x;
					view_dir.y = -view_dir.y;
				}

				float len = std::sqrt(view_dir.x * view_dir.x + view_dir.y * view_dir.y);
				if (len > 0.001f) {
					view_dir.x /= len;
					view_dir.y /= len;
				}

				screen_pos.x = display_size.x * 0.5f + view_dir.x * display_size.x * 0.5f;
				screen_pos.y = display_size.y * 0.5f - view_dir.y * display_size.y * 0.5f;
				screen_pos.x = std::clamp(screen_pos.x, 10.0f, display_size.x - 10.0f);
				screen_pos.y = std::clamp(screen_pos.y, 10.0f, display_size.y - 10.0f);
			}

			DrawLine(
				hdc,
				static_cast<int>(display_size.x * 0.5f),
				static_cast<int>(display_size.y * 0.5f),
				static_cast<int>(screen_pos.x),
				static_cast<int>(screen_pos.y),
				ToColorRef(mate ? cfg::esp::colors::tracer_team : cfg::esp::colors::tracer_enemy)
			);
		}

		void RenderCrosshair(HDC hdc, const Player& local)
		{
			if (!cfg::world::crosshair::enabled || local.scoped)
				return;

			static const std::vector<WeaponIds> valid_weapons = { weapon_ssg08, weapon_awp, weapon_g3sg1, weapon_scar20 };
			if (local.weapon.item_index == -1 || std::find(valid_weapons.begin(), valid_weapons.end(), local.weapon.item_index) == valid_weapons.end())
				return;

			const int center_x = static_cast<int>(display_size.x * 0.5f);
			const int center_y = static_cast<int>(display_size.y * 0.5f);
			DrawLine(hdc, center_x - 6, center_y, center_x + 7, center_y, RGB(255, 255, 255));
			DrawLine(hdc, center_x, center_y - 6, center_x, center_y + 7, RGB(255, 255, 255));
		}

		void RenderWatermark(HDC hdc, const Snapshot& snapshot)
		{
			if (!cfg::settings::watermark)
				return;

			std::string text = std::format("cs2-external-esp | {}fps", static_cast<int>(fps));
			if (snapshot.globals.in_match)
				text += std::format(" | {}", snapshot.globals.map_name);

			const SIZE size = MeasureText(hdc, text, 12, true);
			const int margin = 10;
			const int padding = 8;
			const int width = size.cx + padding * 2;
			const int height = size.cy + padding * 2;
			const int x = static_cast<int>(display_size.x) - width - margin;
			const int y = margin;

			DrawPanel(hdc, x, y, width, height, RGB(15, 15, 15), RGB(80, 80, 80));
			DrawText(hdc, x + padding, y + padding - 1, text, RGB(255, 255, 255), 12, true);
		}

		void RenderSpectatorList(HDC hdc, const Snapshot& snapshot)
		{
			if (!cfg::world::spectators::enabled)
				return;

			std::vector<std::string> lines;
			for (const auto& player : snapshot.players) {
				if (player.alive)
					continue;

				const int target_index = player.observer_services.target;
				if (target_index == 0)
					continue;

				const Player* target = nullptr;
				for (const auto& candidate : snapshot.players) {
					if (candidate.pawn_controller_addr == target_index) {
						target = &candidate;
						break;
					}
				}

				if (cfg::world::spectators::self_only && (!target || !target->localplayer))
					continue;

				if (cfg::world::spectators::detailed) {
					std::string target_name = "Invalid";
					if (cfg::world::spectators::self_only)
						target_name = "You";
					else if (player.observer_services.mode == ObserverMode::Free)
						target_name = "No One";
					else if (target)
						target_name = target->name;

					lines.push_back(std::format("{} | {} | {}", player.name, player.observer_services.ToString(), target_name));
				}
				else {
					lines.push_back(player.name);
				}
			}

			if (lines.empty() && !open)
				return;

			const int x = static_cast<int>(cfg::world::spectators::pos.x);
			const int y = static_cast<int>(cfg::world::spectators::pos.y);
			int width = 170;
			int height = 28;

			for (const auto& line : lines) {
				const SIZE line_size = MeasureText(hdc, line, 11);
				width = std::max<int>(width, line_size.cx + 16);
				height += line_size.cy + 4;
			}

			DrawPanel(hdc, x, y, width, height, RGB(18, 18, 18), RGB(70, 70, 70));
			DrawText(hdc, x + 8, y + 6, "Spectators", RGB(220, 220, 220), 12, true);

			int line_y = y + 24;
			if (lines.empty()) {
				DrawText(hdc, x + 8, line_y, "No spectators", RGB(140, 140, 140), 11);
				return;
			}

			for (const auto& line : lines) {
				DrawText(hdc, x + 8, line_y, line, RGB(255, 255, 255), 11);
				line_y += 14;
			}
		}

		void RenderSpeedChart(HDC hdc, const Snapshot& snapshot)
		{
			if (!cfg::world::velocity::enabled)
				return;

			const auto& local = snapshot.local;
			if (!open && !local.alive)
				return;

			const int rate = std::max(cfg::world::velocity::sample_rate, 1);
			const float length = std::max(cfg::world::velocity::sample_length, 1.0f);
			const size_t required_size = static_cast<size_t>(rate * length);
			if (vel_buffer.size() != std::max<size_t>(required_size, 2))
				ResizeVelocityBuffer();

			Vec2_t speed_2d(local.vel.x, local.vel.y);
			const int speed = static_cast<int>(std::floor(speed_2d.len()));

			vel_accumulator += delta_time;
			const float interval = 1.0f / rate;
			while (vel_accumulator >= interval) {
				vel_accumulator -= interval;
				vel_buffer[vel_index % vel_buffer.size()] = speed;
				vel_index = (vel_index + 1) % vel_buffer.size();
			}

			const int x = static_cast<int>(cfg::world::velocity::pos.x);
			const int y = static_cast<int>(cfg::world::velocity::pos.y);
			const int width = static_cast<int>(cfg::world::velocity::size.x);
			const int height = static_cast<int>(cfg::world::velocity::size.y);
			const int padding = 10;
			const int left = x + padding;
			const int right = x + width - padding;
			const int top = y + padding;
			const int bottom = y + height - padding;

			DrawPanel(hdc, x, y, width, height, RGB(18, 18, 18), RGB(70, 70, 70));

			int max_speed = 1;
			for (int value : vel_buffer)
				max_speed = std::max(max_speed, value);

			for (size_t i = 1; i < vel_buffer.size(); ++i) {
				const float t0 = static_cast<float>(i - 1) / static_cast<float>(vel_buffer.size() - 1);
				const float t1 = static_cast<float>(i) / static_cast<float>(vel_buffer.size() - 1);
				const int v0 = vel_buffer[(i - 1 + vel_index) % vel_buffer.size()];
				const int v1 = vel_buffer[(i + vel_index) % vel_buffer.size()];

				const int x0 = left + static_cast<int>(t0 * (right - left));
				const int x1 = left + static_cast<int>(t1 * (right - left));
				const int y0 = bottom - static_cast<int>((static_cast<float>(v0) / max_speed) * (bottom - top));
				const int y1 = bottom - static_cast<int>((static_cast<float>(v1) / max_speed) * (bottom - top));

				DrawLine(hdc, x0, y0, x1, y1, RGB(255, 255, 255));
			}

			DrawText(hdc, x + width / 2 - 10, bottom - 12, std::to_string(speed), RGB(255, 255, 255), 11, true);
		}

		void RenderRadar(HDC hdc, const Snapshot& snapshot)
		{
			if (!cfg::world::radar::enabled)
				return;

			const auto& local = snapshot.local;
			if (!open && !local.alive)
				return;

			const int x = static_cast<int>(cfg::world::radar::pos.x);
			const int y = static_cast<int>(cfg::world::radar::pos.y);
			const int width = static_cast<int>(cfg::world::radar::size.x);
			const int height = static_cast<int>(cfg::world::radar::size.y);
			const float range = std::max(cfg::world::radar::range, 100.0f);

			const float cx = x + width * 0.5f;
			const float cy = y + height * 0.5f;
			const float radius = std::min(width, height) * 0.5f;

			DrawPanel(hdc, x, y, width, height, RGB(18, 18, 18), RGB(70, 70, 70));
			DrawCircle(hdc, static_cast<int>(cx), static_cast<int>(cy), static_cast<int>(radius * 0.333f), RGB(50, 50, 50));
			DrawCircle(hdc, static_cast<int>(cx), static_cast<int>(cy), static_cast<int>(radius * 0.666f), RGB(50, 50, 50));
			DrawLine(hdc, x + 4, static_cast<int>(cy), x + width - 4, static_cast<int>(cy), RGB(50, 50, 50));
			DrawLine(hdc, static_cast<int>(cx), y + 4, static_cast<int>(cx), y + height - 4, RGB(50, 50, 50));

			for (const auto& player : snapshot.players) {
				if (!player.alive || player.localplayer)
					continue;

				const Vec3_t delta = player.pos - local.pos;
				const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y);
				if (dist > range)
					continue;

				const float nx = delta.x / range;
				const float ny = delta.y / range;

				float sx = 0.0f;
				float sy = 0.0f;
				if (!cfg::world::radar::no_rotate) {
					float rx = matrix[0][0];
					float ry = matrix[0][1];
					float len = std::sqrt(rx * rx + ry * ry);
					if (len > 0.001f) {
						rx /= len;
						ry /= len;
					}

					const float fx = -ry;
					const float fy = rx;
					const float rad_x = nx * rx + ny * ry;
					const float rad_y = nx * fx + ny * fy;
					sx = cx + rad_x * (width * 0.5f - 6.0f);
					sy = cy - rad_y * (height * 0.5f - 6.0f);
				}
				else {
					sx = cx + nx * (width * 0.5f - 6.0f);
					sy = cy - ny * (height * 0.5f - 6.0f);
				}

				const bool mate = player.team == local.team;
				DrawFilledCircle(
					hdc,
					static_cast<int>(sx),
					static_cast<int>(sy),
					4,
					mate ? RGB(0, 220, 80) : RGB(220, 50, 50),
					RGB(0, 0, 0)
				);
			}

			DrawFilledCircle(hdc, static_cast<int>(cx), static_cast<int>(cy), 5, RGB(100, 180, 255), RGB(0, 0, 0));
			DrawText(hdc, x + 6, y + 4, "Radar", RGB(180, 180, 180), 11, true);
		}

		void RenderBomb(HDC hdc, const Snapshot& snapshot)
		{
			if (!cfg::world::bomb::location && !cfg::world::bomb::timer)
				return;

			const auto& bomb = snapshot.bomb;
			const auto& local = snapshot.local;

			if (!bomb.is_planted && !open)
				return;
			if (bomb.is_planted && !bomb.pos.length() && !open)
				return;
			if (!local.alive && !open)
				return;

			std::string text;
			if (cfg::world::bomb::location)
				text += std::format("SITE {}", bomb.site == BombSite::B ? "B" : "A");
			if (cfg::world::bomb::timer) {
				if (!text.empty())
					text += " | ";
				text += std::format("{:.1f}s", bomb.is_planted ? bomb.time_left : 40.0f);
			}

			const SIZE text_size = MeasureText(hdc, text, 12, true);
			const int padding = 8;
			const int width = text_size.cx + padding * 2 + 22;
			const int height = text_size.cy + padding * 2 + (cfg::world::bomb::timer ? 6 : 0);

			Vec2_t screen_pos{};
			bool on_top = bomb.is_planted ? matrix.wts(bomb.pos, display_size, screen_pos) : false;
			if (open || local.pos.dist_to(bomb.pos) > 1500.0f)
				on_top = false;

			int x = 0;
			int y = 0;
			if (on_top && bomb.is_planted) {
				x = static_cast<int>(screen_pos.x - width * 0.5f);
				y = static_cast<int>(screen_pos.y + 4.0f);
			}
			else {
				x = static_cast<int>(cfg::world::bomb::pos.x);
				y = static_cast<int>(cfg::world::bomb::pos.y);
			}

			DrawPanel(hdc, x, y, width, height, RGB(18, 18, 18), RGB(70, 70, 70));
			DrawText(hdc, x + padding, y + padding - 1, "C4", RGB(255, 80, 80), 12, true);
			DrawText(hdc, x + padding + 18, y + padding - 1, text, RGB(240, 240, 240), 12, true);

			if (cfg::world::bomb::timer) {
				const float progress = std::clamp((bomb.is_planted ? bomb.time_left : 40.0f) / 40.0f, 0.0f, 1.0f);
				const int bar_x = x + 4;
				const int bar_y = y + height - 5;
				const int bar_width = width - 8;
				FillRectColor(hdc, bar_x, bar_y, bar_width, 2, RGB(40, 40, 40));
				FillRectColor(
					hdc,
					bar_x,
					bar_y,
					static_cast<int>(bar_width * progress),
					2,
					progress > 0.5f ? RGB(static_cast<int>((1.0f - progress) * 2.0f * 255.0f), 220, 50)
						: RGB(220, static_cast<int>(progress * 2.0f * 220.0f), 50)
				);
			}
		}

		void RenderStartupHint(HDC hdc)
		{
			if (startup_hint_seen)
				return;

			const std::string first = "To OPEN the menu, use Insert or Right Shift";
			const std::string second = "To CLOSE, press End";
			const SIZE first_size = MeasureText(hdc, first, 13, true);
			const SIZE second_size = MeasureText(hdc, second, 13, true);

			DrawText(hdc, static_cast<int>(display_size.x * 0.5f - first_size.cx * 0.5f), 80, first, RGB(255, 255, 255), 13, true);
			DrawText(hdc, static_cast<int>(display_size.x * 0.5f - second_size.cx * 0.5f), 96, second, RGB(255, 255, 255), 13, true);
		}
	};

	CompatibilityRendererImpl& GetCompat()
	{
		static CompatibilityRendererImpl instance{};
		return instance;
	}
}

bool CompatibilityRenderer::Init()
{
	return GetCompat().Init();
}

void CompatibilityRenderer::Destroy()
{
	GetCompat().running = false;
}

void CompatibilityRenderer::Thread()
{
	GetCompat().Tick();
}

bool CompatibilityRenderer::IsOpen()
{
	return GetCompat().open;
}

void CompatibilityRenderer::SetOpen(bool open)
{
	GetCompat().SetOpen(open);
}

void CompatibilityRenderer::SetVSync(bool enable)
{
	GetCompat().menu.SetVSync(enable);
}
