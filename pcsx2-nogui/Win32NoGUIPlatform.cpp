/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"

#include "Win32NoGUIPlatform.h"
#include "Win32KeyNames.h"
#include "NoGUIHost.h"

#include "common/StringUtil.h"
#include "common/Threading.h"

#include "pcsx2/HostSettings.h"
#include "pcsx2/windows/resource.h"

static constexpr LPCWSTR WINDOW_CLASS_NAME = L"PCSX2NoGUI";
static constexpr DWORD WINDOWED_STYLE = WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_MINIMIZEBOX | WS_SYSMENU | WS_SIZEBOX;
static constexpr DWORD WINDOWED_EXSTYLE = WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;
static constexpr DWORD FULLSCREEN_STYLE = WS_POPUP | WS_MINIMIZEBOX;

static float GetWindowScale(HWND hwnd)
{
	static UINT(WINAPI * get_dpi_for_window)(HWND hwnd);
	if (!get_dpi_for_window)
	{
		HMODULE mod = GetModuleHandle(L"user32.dll");
		if (mod)
			get_dpi_for_window = reinterpret_cast<decltype(get_dpi_for_window)>(GetProcAddress(mod, "GetDpiForWindow"));
	}
	if (!get_dpi_for_window)
		return 1.0f;

	// less than 100% scaling seems unlikely.
	const UINT dpi = hwnd ? get_dpi_for_window(hwnd) : 96;
	return (dpi > 0) ? std::max(1.0f, static_cast<float>(dpi) / 96.0f) : 1.0f;
}

Win32NoGUIPlatform::Win32NoGUIPlatform()
{
	m_message_loop_running.store(true, std::memory_order_release);
}

Win32NoGUIPlatform::~Win32NoGUIPlatform()
{
	UnregisterClassW(WINDOW_CLASS_NAME, GetModuleHandle(nullptr));
}

bool Win32NoGUIPlatform::Initialize()
{
	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(WNDCLASSEXW);
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = GetModuleHandle(nullptr);
	wc.hIcon = LoadIconA(wc.hInstance, (LPCSTR)IDI_ICON2);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszMenuName = NULL;
	wc.lpszClassName = WINDOW_CLASS_NAME;
	wc.hIconSm = LoadIconA(wc.hInstance, (LPCSTR)IDI_ICON2);

	if (!RegisterClassExW(&wc))
	{
		MessageBoxW(nullptr, L"Window registration failed.", L"Error", MB_ICONERROR | MB_OK);
		return false;
	}

	m_window_thread_id = GetCurrentThreadId();
	return true;
}

void Win32NoGUIPlatform::ReportError(const std::string_view& title, const std::string_view& message)
{
	const std::wstring title_copy(StringUtil::UTF8StringToWideString(title));
	const std::wstring message_copy(StringUtil::UTF8StringToWideString(message));

	MessageBoxW(m_hwnd, message_copy.c_str(), title_copy.c_str(), MB_ICONERROR | MB_OK);
}

void Win32NoGUIPlatform::SetDefaultConfig(SettingsInterface& si)
{
}

bool Win32NoGUIPlatform::CreatePlatformWindow(std::string title)
{
	s32 window_x, window_y, window_width, window_height;
	if (!NoGUIHost::GetSavedPlatformWindowGeometry(&window_x, &window_y, &window_width, &window_height))
	{
		window_x = CW_USEDEFAULT;
		window_y = CW_USEDEFAULT;
		window_width = DEFAULT_WINDOW_WIDTH;
		window_height = DEFAULT_WINDOW_HEIGHT;
	}

	HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, WINDOW_CLASS_NAME, StringUtil::UTF8StringToWideString(title).c_str(), WINDOWED_STYLE,
		window_x, window_y, window_width, window_height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
	if (!hwnd)
	{
		MessageBoxW(nullptr, L"CreateWindowEx failed.", L"Error", MB_ICONERROR | MB_OK);
		return false;
	}

	// deliberately not stored to m_hwnd yet, because otherwise the msg handlers will run
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);
	m_hwnd = hwnd;
	m_window_scale = GetWindowScale(m_hwnd);
	m_last_mouse_buttons = 0;

	if (m_fullscreen.load(std::memory_order_acquire))
		SetFullscreen(true);

	return true;
}

void Win32NoGUIPlatform::DestroyPlatformWindow()
{
	if (!m_hwnd)
		return;

	RECT rc;
	if (!m_fullscreen.load(std::memory_order_acquire) && GetWindowRect(m_hwnd, &rc))
	{
		NoGUIHost::SavePlatformWindowGeometry(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
	}

	DestroyWindow(m_hwnd);
	m_hwnd = {};
}

std::optional<WindowInfo> Win32NoGUIPlatform::GetPlatformWindowInfo()
{
	if (!m_hwnd)
		return std::nullopt;

	RECT rc = {};
	GetWindowRect(m_hwnd, &rc);

	WindowInfo wi;
	wi.surface_width = static_cast<u32>(rc.right - rc.left);
	wi.surface_height = static_cast<u32>(rc.bottom - rc.top);
	wi.surface_scale = m_window_scale;
	wi.type = WindowInfo::Type::Win32;
	wi.window_handle = m_hwnd;
	return wi;
}

void Win32NoGUIPlatform::SetPlatformWindowTitle(std::string title)
{
	if (!m_hwnd)
		return;

	SetWindowTextW(m_hwnd, StringUtil::UTF8StringToWideString(title).c_str());
}

std::optional<u32> Win32NoGUIPlatform::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
	std::optional<DWORD> converted(Win32KeyNames::GetKeyCodeForName(str));
	return converted.has_value() ? std::optional<u32>(static_cast<u32>(converted.value())) : std::nullopt;
}

std::optional<std::string> Win32NoGUIPlatform::ConvertHostKeyboardCodeToString(u32 code)
{
	const char* converted = Win32KeyNames::GetKeyName(code);
	return converted ? std::optional<std::string>(converted) : std::nullopt;
}

void Win32NoGUIPlatform::RunMessageLoop()
{
	while (m_message_loop_running.load(std::memory_order_acquire))
	{
		MSG msg;
		if (GetMessageW(&msg, NULL, 0, 0))
		{
			// handle self messages (when we don't have a window yet)
			if (msg.hwnd == NULL && msg.message >= WM_FIRST && msg.message <= WM_LAST)
			{
				WndProc(NULL, msg.message, msg.wParam, msg.lParam);
			}
			else
			{
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}
	}
}

void Win32NoGUIPlatform::ExecuteInMessageLoop(std::function<void()> func)
{
	std::function<void()>* pfunc = new std::function<void()>(std::move(func));
	if (m_hwnd)
		PostMessageW(m_hwnd, WM_FUNC, 0, reinterpret_cast<LPARAM>(pfunc));
	else
		PostThreadMessageW(m_window_thread_id, WM_FUNC, 0, reinterpret_cast<LPARAM>(pfunc));
}

void Win32NoGUIPlatform::QuitMessageLoop()
{
	m_message_loop_running.store(false, std::memory_order_release);
	PostThreadMessageW(m_window_thread_id, WM_WAKEUP, 0, 0);
}

void Win32NoGUIPlatform::SetFullscreen(bool enabled)
{
	if (!m_hwnd || m_fullscreen.load(std::memory_order_acquire) == enabled)
		return;

	LONG style = GetWindowLong(m_hwnd, GWL_STYLE);
	LONG exstyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
	RECT rc;

	if (enabled)
	{
		HMONITOR monitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
		if (!monitor)
			return;

		MONITORINFO mi = {sizeof(MONITORINFO)};
		if (!GetMonitorInfo(monitor, &mi) || !GetWindowRect(m_hwnd, &m_windowed_rect))
			return;

		style = (style & ~WINDOWED_STYLE) | FULLSCREEN_STYLE;
		exstyle = (style & ~WINDOWED_EXSTYLE);
		rc = mi.rcMonitor;
	}
	else
	{
		style = (style & ~FULLSCREEN_STYLE) | WINDOWED_STYLE;
		exstyle = exstyle | WINDOWED_EXSTYLE;
		rc = m_windowed_rect;
	}

	SetWindowLongPtrW(m_hwnd, GWL_STYLE, style);
	SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exstyle);
	SetWindowPos(m_hwnd, NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_SHOWWINDOW);

	m_fullscreen.store(enabled, std::memory_order_release);
}

bool Win32NoGUIPlatform::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
	RECT rc;
	if (!m_hwnd || m_fullscreen.load(std::memory_order_acquire) || !GetWindowRect(m_hwnd, &rc))
	{
		return false;
	}

	return SetWindowPos(m_hwnd, NULL, rc.left, rc.top, new_window_width, new_window_height, SWP_SHOWWINDOW);
}

LRESULT CALLBACK Win32NoGUIPlatform::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	Win32NoGUIPlatform* platform = static_cast<Win32NoGUIPlatform*>(g_nogui_window.get());
	if (hwnd != platform->m_hwnd && msg != WM_FUNC)
		return DefWindowProcW(hwnd, msg, wParam, lParam);

	switch (msg)
	{
		case WM_SIZE:
		{
			const u32 width = LOWORD(lParam);
			const u32 height = HIWORD(lParam);
			NoGUIHost::ProcessPlatformWindowResize(width, height, platform->m_window_scale);
		}
		break;

		case WM_KEYDOWN:
		case WM_KEYUP:
		{
			const bool pressed = (msg == WM_KEYDOWN);
			NoGUIHost::ProcessPlatformKeyEvent(wParam, pressed);
		}
		break;

		case WM_MOUSEMOVE:
		{
			const float x = static_cast<float>(static_cast<s16>(LOWORD(lParam)));
			const float y = static_cast<float>(static_cast<s16>(HIWORD(lParam)));
			NoGUIHost::ProcessPlatformMouseMoveEvent(x, y);
		}
		break;

		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_XBUTTONDOWN:
		case WM_XBUTTONUP:
		{
			const DWORD buttons = static_cast<DWORD>(wParam);
			const DWORD changed = platform->m_last_mouse_buttons ^ buttons;
			platform->m_last_mouse_buttons = buttons;

			static constexpr DWORD masks[] = {MK_LBUTTON, MK_RBUTTON, MK_MBUTTON, MK_XBUTTON1, MK_XBUTTON2};
			for (u32 i = 0; i < std::size(masks); i++)
			{
				if (changed & masks[i])
					NoGUIHost::ProcessPlatformMouseButtonEvent(i, (buttons & masks[i]) != 0);
			}
		}
		break;

		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL:
		{
			const float d = std::clamp(static_cast<float>(static_cast<s16>(HIWORD(wParam))) / static_cast<float>(WHEEL_DELTA), -1.0f, 1.0f);
			NoGUIHost::ProcessPlatformMouseWheelEvent((msg == WM_MOUSEHWHEEL) ? d : 0.0f, (msg == WM_MOUSEWHEEL) ? d : 0.0f);
		}
		break;

		case WM_CLOSE:
		case WM_QUIT:
		{
			Host::RunOnCPUThread([]() { Host::RequestExit(EmuConfig.SaveStateOnShutdown); });
		}
		break;

		case WM_FUNC:
		{
			std::function<void()>* pfunc = reinterpret_cast<std::function<void()>*>(lParam);
			if (pfunc)
			{
				(*pfunc)();
				delete pfunc;
			}
		}
		break;

		case WM_WAKEUP:
			break;

		default:
			return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	return 0;
}


std::unique_ptr<NoGUIPlatform> NoGUIPlatform::CreateWin32Platform()
{
	std::unique_ptr<Win32NoGUIPlatform> ret(new Win32NoGUIPlatform());
	if (!ret->Initialize())
		return {};

	return ret;
}
