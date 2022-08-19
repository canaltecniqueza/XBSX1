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

#include <chrono>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#endif

#ifdef _UWP
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/base.h>
#endif

#include "fmt/core.h"

#include "common/Assertions.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/Exceptions.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/Frontend/FullscreenUI.h"
#include "pcsx2/Frontend/GameList.h"
#include "pcsx2/Frontend/InputManager.h"
#include "pcsx2/Frontend/ImGuiManager.h"
#include "pcsx2/Frontend/INISettingsInterface.h"
#include "pcsx2/Frontend/LogSink.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GS.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/HostDisplay.h"
#include "pcsx2/HostSettings.h"
#include "pcsx2/PAD/Host/PAD.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"

#include "NoGUIHost.h"
#include "NoGUIPlatform.h"
#include "svnrev.h"

#include "pcsx2/DebugTools/Debug.h"

using namespace std::chrono_literals;

static constexpr u32 SETTINGS_VERSION = 1;
static constexpr auto CPU_THREAD_POLL_INTERVAL = 8ms; // how often we'll poll controllers when paused

std::unique_ptr<NoGUIPlatform> g_nogui_window;

//////////////////////////////////////////////////////////////////////////
// Local function declarations
//////////////////////////////////////////////////////////////////////////
namespace NoGUIHost
{
	static bool InitializeConfig();
	static bool ShouldUsePortableMode();
	static void SetResourcesDirectory();
	static void SetDataDirectory();
	static void HookSignals();
	static bool SetCriticalFolders();
	static void SetDefaultConfig();
	static void StartCPUThread();
	static void StopCPUThread();
	static void ProcessCPUThreadEvents(bool block);
	static void ProcessCPUThreadPlatformMessages();
	static void CPUThreadEntryPoint();
	static void CPUThreadMainLoop();
	static std::string GetWindowTitle(const std::string& game_title);
	static void UpdateWindowTitle(const std::string& game_title);
	static void GameListRefreshThreadEntryPoint(bool invalidate_cache);
} // namespace NoGUIHost

//////////////////////////////////////////////////////////////////////////
// Local variable declarations
//////////////////////////////////////////////////////////////////////////
static std::unique_ptr<INISettingsInterface> s_base_settings_interface;
static std::unique_ptr<HostDisplay> s_host_display;
static Threading::KernelSemaphore s_host_display_created;
alignas(16) static SysMtgsThread s_mtgs_thread;
static std::atomic_bool s_running{false};
static bool s_batch_mode = false;
static bool s_is_fullscreen = false;
static bool s_save_state_on_shutdown = false;

static Threading::Thread s_cpu_thread;
static std::mutex s_cpu_thread_events_mutex;
static std::condition_variable s_cpu_thread_event_done;
static std::condition_variable s_cpu_thread_event_posted;
static std::deque<std::pair<std::function<void()>, bool>> s_cpu_thread_events;
static u32 s_blocking_cpu_events_pending = 0; // TODO: Token system would work better here.

static std::mutex s_game_list_refresh_lock;
static std::thread s_game_list_refresh_thread;
static FullscreenUI::ProgressCallback* s_game_list_refresh_progress = nullptr;

//////////////////////////////////////////////////////////////////////////
// Initialization/Shutdown
//////////////////////////////////////////////////////////////////////////

bool NoGUIHost::Initialize()
{
	if (!InitializeConfig())
	{
		g_nogui_window->ReportError("Error", "Failed to initialize config.");
		return false;
	}

	// the rest of initialization happens on the CPU thread.
	HookSignals();
	StartCPUThread();
	return true;
}

void NoGUIHost::Shutdown()
{
	StopCPUThread();
}

bool NoGUIHost::SetCriticalFolders()
{
	EmuFolders::AppRoot = Path::Canonicalize(Path::GetDirectory(FileSystem::GetProgramPath()));
	SetResourcesDirectory();
	SetDataDirectory();

	// allow SetDataDirectory() to change settings directory (if we want to split config later on)
	if (EmuFolders::Settings.empty())
		EmuFolders::Settings = Path::Combine(EmuFolders::DataRoot, "inis");

	// Write crash dumps to the data directory, since that'll be accessible for certain.
	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	// the resources directory should exist, bail out if not
	if (!FileSystem::DirectoryExists(EmuFolders::Resources.c_str()))
	{
		g_nogui_window->ReportError("Error", "Resources directory is missing, your installation is incomplete.");
		return false;
	}

	return true;
}

bool NoGUIHost::ShouldUsePortableMode()
{
	// Check whether portable.ini exists in the program directory.
	return FileSystem::FileExists(Path::Combine(EmuFolders::AppRoot, "portable.ini").c_str());
}

void NoGUIHost::SetResourcesDirectory()
{
#ifndef __APPLE__
	// On Windows/Linux, these are in the binary directory.
	EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "resources");
#else
	// On macOS, this is in the bundle resources directory.
	EmuFolders::Resources = Path::Combine(EmuFolders::AppRoot, "../Resources");
#endif
}

void NoGUIHost::SetDataDirectory()
{
	if (ShouldUsePortableMode())
	{
		EmuFolders::DataRoot = EmuFolders::AppRoot;
		return;
	}

#if defined(_UWP)
	const auto local_location = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
	EmuFolders::DataRoot = StringUtil::WideStringToUTF8String(local_location.Path());
#elif defined(_WIN32)
	// On Windows, use My Documents\PCSX2 to match old installs.
	PWSTR documents_directory;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &documents_directory)))
	{
		if (std::wcslen(documents_directory) > 0)
			EmuFolders::DataRoot = Path::Combine(StringUtil::WideStringToUTF8String(documents_directory), "PCSX2");
		CoTaskMemFree(documents_directory);
	}
#elif defined(__linux__)
	// Check for $HOME/PCSX2 first, for legacy installs.
	const char* home_dir = getenv("HOME");
	std::string legacy_dir(home_dir ? Path::Combine(home_dir, "PCSX2") : std::string());
	if (!legacy_dir.empty() && FileSystem::DirectoryExists(legacy_dir.c_str()))
	{
		EmuFolders::DataRoot = std::move(legacy_dir);
	}
	else
	{
		// otherwise, use $XDG_CONFIG_HOME/PCSX2.
		const char* xdg_config_home = getenv("XDG_CONFIG_HOME");
		if (xdg_config_home && xdg_config_home[0] == '/' && FileSystem::DirectoryExists(xdg_config_home))
		{
			EmuFolders::DataRoot = Path::Combine(xdg_config_home, "PCSX2");
		}
		else if (!legacy_dir.empty())
		{
			// fall back to the legacy PCSX2-in-home.
			EmuFolders::DataRoot = std::move(legacy_dir);
		}
	}
#elif defined(__APPLE__)
	static constexpr char MAC_DATA_DIR[] = "Library/Application Support/PCSX2";
	const char* home_dir = getenv("HOME");
	if (home_dir)
		EmuFolders::DataRoot = Path::Combine(home_dir, MAC_DATA_DIR);
#endif

	// make sure it exists
	if (!EmuFolders::DataRoot.empty() && !FileSystem::DirectoryExists(EmuFolders::DataRoot.c_str()))
	{
		// we're in trouble if we fail to create this directory... but try to hobble on with portable
		if (!FileSystem::CreateDirectoryPath(EmuFolders::DataRoot.c_str(), false))
			EmuFolders::DataRoot.clear();
	}

	// couldn't determine the data directory? fallback to portable.
	if (EmuFolders::DataRoot.empty())
		EmuFolders::DataRoot = EmuFolders::AppRoot;
}

bool NoGUIHost::InitializeConfig()
{
	if (!SetCriticalFolders())
		return false;

	const std::string path(Path::Combine(EmuFolders::Settings, "PCSX2.ini"));
	s_base_settings_interface = std::make_unique<INISettingsInterface>(std::move(path));
	Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());

	uint settings_version;
	if (!s_base_settings_interface->Load() || !s_base_settings_interface->GetUIntValue("UI", "SettingsVersion", &settings_version) ||
		settings_version != SETTINGS_VERSION)
	{
		SetDefaultConfig();
		s_base_settings_interface->Save();
	}

	// TODO: Handle reset to defaults if load fails.
	EmuFolders::LoadConfig(*s_base_settings_interface.get());
	EmuFolders::EnsureFoldersExist();
	Host::UpdateLogging();
	return true;
}

void NoGUIHost::SetDefaultConfig()
{
	EmuConfig = Pcsx2Config();
	EmuFolders::SetDefaults();
	EmuFolders::EnsureFoldersExist();
	VMManager::SetHardwareDependentDefaultSettings(EmuConfig);

	SettingsInterface& si = *s_base_settings_interface.get();
	si.SetUIntValue("UI", "SettingsVersion", SETTINGS_VERSION);

	{
		SettingsSaveWrapper wrapper(si);
		EmuConfig.LoadSave(wrapper);
	}

	EmuFolders::Save(si);
	PAD::SetDefaultConfig(si);

	g_nogui_window->SetDefaultConfig(si);
}

SettingsInterface* NoGUIHost::GetBaseSettingsInterface()
{
	return s_base_settings_interface.get();
}

std::string NoGUIHost::GetBaseStringSettingValue(const char* section, const char* key, const char* default_value /*= ""*/)
{
	auto lock = Host::GetSettingsLock();
	return s_base_settings_interface->GetStringValue(section, key, default_value);
}

bool NoGUIHost::GetBaseBoolSettingValue(const char* section, const char* key, bool default_value /*= false*/)
{
	auto lock = Host::GetSettingsLock();
	return s_base_settings_interface->GetBoolValue(section, key, default_value);
}

int NoGUIHost::GetBaseIntSettingValue(const char* section, const char* key, int default_value /*= 0*/)
{
	auto lock = Host::GetSettingsLock();
	return s_base_settings_interface->GetIntValue(section, key, default_value);
}

float NoGUIHost::GetBaseFloatSettingValue(const char* section, const char* key, float default_value /*= 0.0f*/)
{
	auto lock = Host::GetSettingsLock();
	return s_base_settings_interface->GetFloatValue(section, key, default_value);
}

std::vector<std::string> NoGUIHost::GetBaseStringListSetting(const char* section, const char* key)
{
	auto lock = Host::GetSettingsLock();
	return s_base_settings_interface->GetStringList(section, key);
}

void NoGUIHost::SetBaseBoolSettingValue(const char* section, const char* key, bool value)
{
	auto lock = Host::GetSettingsLock();
	s_base_settings_interface->SetBoolValue(section, key, value);
	SaveSettings();
}

void NoGUIHost::SetBaseIntSettingValue(const char* section, const char* key, int value)
{
	auto lock = Host::GetSettingsLock();
	s_base_settings_interface->SetIntValue(section, key, value);
	SaveSettings();
}

void NoGUIHost::SetBaseFloatSettingValue(const char* section, const char* key, float value)
{
	auto lock = Host::GetSettingsLock();
	s_base_settings_interface->SetFloatValue(section, key, value);
	SaveSettings();
}

void NoGUIHost::SetBaseStringSettingValue(const char* section, const char* key, const char* value)
{
	auto lock = Host::GetSettingsLock();
	s_base_settings_interface->SetStringValue(section, key, value);
	SaveSettings();
}

void NoGUIHost::SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values)
{
	auto lock = Host::GetSettingsLock();
	s_base_settings_interface->SetStringList(section, key, values);
	SaveSettings();
}

bool NoGUIHost::AddBaseValueToStringList(const char* section, const char* key, const char* value)
{
	auto lock = Host::GetSettingsLock();
	if (!s_base_settings_interface->AddToStringList(section, key, value))
		return false;

	SaveSettings();
	return true;
}

bool NoGUIHost::RemoveBaseValueFromStringList(const char* section, const char* key, const char* value)
{
	auto lock = Host::GetSettingsLock();
	if (!s_base_settings_interface->RemoveFromStringList(section, key, value))
		return false;

	SaveSettings();
	return true;
}

void NoGUIHost::RemoveBaseSettingValue(const char* section, const char* key)
{
	auto lock = Host::GetSettingsLock();
	s_base_settings_interface->DeleteValue(section, key);
	SaveSettings();
}

void NoGUIHost::SaveSettings()
{
	auto lock = Host::GetSettingsLock();
	if (!s_base_settings_interface->Save())
		Console.Error("Failed to save settings.");
}

bool NoGUIHost::InBatchMode()
{
	return s_batch_mode;
}

void NoGUIHost::SetBatchMode(bool enabled)
{
	s_batch_mode = enabled;
	if (enabled)
		GameList::Refresh(false);
}

void NoGUIHost::StartVM(std::shared_ptr<VMBootParameters> params)
{
	Host::RunOnCPUThread([params = std::move(params)]() {
		if (!VMManager::Initialize(*params))
			return;

		VMManager::SetState(VMState::Running);
	});
}

void NoGUIHost::ProcessPlatformWindowResize(s32 width, s32 height, float scale)
{
	Host::RunOnCPUThread([width, height, scale]() { GetMTGS().ResizeDisplayWindow(width, height, scale); });
}

void NoGUIHost::ProcessPlatformMouseMoveEvent(float x, float y)
{
	InputManager::UpdatePointerAbsolutePosition(0, x, y);
}

void NoGUIHost::ProcessPlatformMouseButtonEvent(s32 button, bool pressed)
{
	Host::RunOnCPUThread(
		[button, pressed]() { InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), pressed ? 1.0f : 0.0f); });
}

void NoGUIHost::ProcessPlatformMouseWheelEvent(float x, float y)
{
	if (x != 0.0f)
		InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelX, x);
	if (y != 0.0f)
		InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelY, y);
}

void NoGUIHost::ProcessPlatformKeyEvent(s32 key, bool pressed)
{
	Host::RunOnCPUThread([key, pressed]() { InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), pressed ? 1.0f : 0.0f); });
}

void NoGUIHost::PlatformWindowFocusGained()
{
	// TODO
}

void NoGUIHost::PlatformWindowFocusLost()
{
	// TODO
}

bool NoGUIHost::GetSavedPlatformWindowGeometry(s32* x, s32* y, s32* width, s32* height)
{
	auto lock = Host::GetSettingsLock();

	bool result = s_base_settings_interface->GetIntValue("NoGUI", "WindowX", x);
	result = result && s_base_settings_interface->GetIntValue("NoGUI", "WindowY", y);
	result = result && s_base_settings_interface->GetIntValue("NoGUI", "WindowWidth", width);
	result = result && s_base_settings_interface->GetIntValue("NoGUI", "WindowHeight", height);
	return result;
}

void NoGUIHost::SavePlatformWindowGeometry(s32 x, s32 y, s32 width, s32 height)
{
	if (s_is_fullscreen)
		return;

	auto lock = Host::GetSettingsLock();
	s_base_settings_interface->SetIntValue("NoGUI", "WindowX", x);
	s_base_settings_interface->SetIntValue("NoGUI", "WindowY", y);
	s_base_settings_interface->SetIntValue("NoGUI", "WindowWidth", width);
	s_base_settings_interface->SetIntValue("NoGUI", "WindowHeight", height);
	s_base_settings_interface->Save();
}

std::string NoGUIHost::GetAppNameAndVersion()
{
	std::string ret;
	if constexpr (!PCSX2_isReleaseVersion && GIT_TAGGED_COMMIT)
	{
		ret = "PCSX2 Nightly - " GIT_TAG;
	}
	else if constexpr (PCSX2_isReleaseVersion)
	{
#define APPNAME_STRINGIZE(x) #x
		ret = "PCSX2 " APPNAME_STRINGIZE(PCSX2_VersionHi) "." APPNAME_STRINGIZE(PCSX2_VersionMid) "." APPNAME_STRINGIZE(PCSX2_VersionLo);
#undef APPNAME_STRINGIZE
	}
	else
	{
		return "PCSX2 " GIT_REV;
	}

	return ret;
}

std::string NoGUIHost::GetAppConfigSuffix()
{
#if defined(PCSX2_DEBUG)
	return " [Debug]";
#elif defined(PCSX2_DEVBUILD)
	return " [Devel]";
#else
	return std::string();
#endif
}

void NoGUIHost::StartCPUThread()
{
	s_running.store(true, std::memory_order_release);
	s_cpu_thread.Start(CPUThreadEntryPoint);
}

void NoGUIHost::StopCPUThread()
{
	if (!s_cpu_thread.Joinable())
		return;

	{
		std::unique_lock lock(s_cpu_thread_events_mutex);
		s_running.store(false, std::memory_order_release);
		s_cpu_thread_event_posted.notify_one();
	}
	s_cpu_thread.Join();
}

void NoGUIHost::ProcessCPUThreadPlatformMessages()
{
	// This is lame. On Win32, we need to pump messages, even though *we* don't have any windows
	// on the CPU thread, because SDL creates a hidden window for raw input for some game controllers.
	// If we don't do this, we don't get any controller events.
#if defined(_WIN32) && !defined(_UWP)
	MSG msg;
	while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
#endif
}

void NoGUIHost::ProcessCPUThreadEvents(bool block)
{
	std::unique_lock lock(s_cpu_thread_events_mutex);

	for (;;)
	{
		if (s_cpu_thread_events.empty())
		{
			if (!block || !s_running.load(std::memory_order_acquire))
				return;

			// we still need to keep polling the controllers when we're paused
			do
			{
				ProcessCPUThreadPlatformMessages();
				InputManager::PollSources();
			} while (!s_cpu_thread_event_posted.wait_for(lock, CPU_THREAD_POLL_INTERVAL, []() { return !s_cpu_thread_events.empty(); }));
		}

		// return after processing all events if we had one
		block = false;

		auto event = std::move(s_cpu_thread_events.front());
		s_cpu_thread_events.pop_front();
		lock.unlock();
		event.first();
		lock.lock();

		if (event.second)
		{
			s_blocking_cpu_events_pending--;
			s_cpu_thread_event_done.notify_one();
		}
	}
}

void NoGUIHost::CPUThreadEntryPoint()
{
	Threading::SetNameOfCurrentThread("CPU Thread");
	PerformanceMetrics::SetCPUThread(Threading::ThreadHandle::GetForCallingThread());

	if (VMManager::Internal::InitializeGlobals() && VMManager::Internal::InitializeMemory())
	{
		// we want settings loaded so we choose the correct renderer
		// this also sorts out input sources.
		VMManager::LoadSettings();

		// start the GS thread up and get it going
		if (GetMTGS().WaitForOpen())
		{
			// kick a game list refresh if we're not in batch mode
			if (!InBatchMode())
				Host::RefreshGameListAsync(false);

			CPUThreadMainLoop();

			Host::CancelGameListRefresh();
			GetMTGS().WaitForClose();
		}
		else
		{
			g_nogui_window->ReportError("Error", "MTGS open failed.");
		}

		InputManager::CloseSources();
	}
	else
	{
		g_nogui_window->ReportError("Error", "Failed to initalize globals/memory.");
	}

	VMManager::Internal::ReleaseMemory();

	PerformanceMetrics::SetCPUThread(Threading::ThreadHandle());
	g_nogui_window->QuitMessageLoop();
}

void NoGUIHost::CPUThreadMainLoop()
{
	for (;;)
	{
		switch (VMManager::GetState())
		{
			case VMState::Running:
				VMManager::Execute();
				break;

			case VMState::Paused:
				ProcessCPUThreadEvents(true);
				break;

			case VMState::Stopping:
				VMManager::Shutdown(s_save_state_on_shutdown);
				break;

			case VMState::Shutdown:
			{
				ProcessCPUThreadEvents(true);
				if (!s_running.load(std::memory_order_acquire))
					return;
			}
			break;

			default:
				break;
		}
	}
}

std::optional<std::vector<u8>> Host::ReadResourceFile(const char* filename)
{
	const std::string path(Path::Combine(EmuFolders::Resources, filename));
	std::optional<std::vector<u8>> ret(FileSystem::ReadBinaryFile(path.c_str()));
	if (!ret.has_value())
		Console.Error("Failed to read resource file '%s'", filename);
	return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
	const std::string path(Path::Combine(EmuFolders::Resources, filename));
	std::optional<std::string> ret(FileSystem::ReadFileToString(path.c_str()));
	if (!ret.has_value())
		Console.Error("Failed to read resource file to string '%s'", filename);
	return ret;
}

void Host::ReportErrorAsync(const std::string_view& title, const std::string_view& message)
{
	if (!title.empty() && !message.empty())
	{
		Console.Error(
			"ReportErrorAsync: %.*s: %.*s", static_cast<int>(title.size()), title.data(), static_cast<int>(message.size()), message.data());
	}
	else if (!message.empty())
	{
		Console.Error("ReportErrorAsync: %.*s", static_cast<int>(message.size()), message.data());
	}

	g_nogui_window->ReportError(title, message);
}

void Host::OnInputDeviceConnected(const std::string_view& identifier, const std::string_view& device_name)
{
	Host::AddKeyedOSDMessage(fmt::format("{} Connected", identifier),
		fmt::format("{} Connected.", identifier), 3.0f);
}

void Host::OnInputDeviceDisconnected(const std::string_view& identifier)
{
	Host::AddKeyedOSDMessage(
		fmt::format("{}", identifier), fmt::format("{} Disconnected.", identifier), 3.0f);
}

HostDisplay* Host::GetHostDisplay()
{
	return s_host_display.get();
}

HostDisplay* Host::AcquireHostDisplay(HostDisplay::RenderAPI api)
{
	g_nogui_window->ExecuteInMessageLoop([api]() {
		if (g_nogui_window->CreatePlatformWindow(NoGUIHost::GetWindowTitle(VMManager::GetGameName())))
		{
			const std::optional<WindowInfo> wi(g_nogui_window->GetPlatformWindowInfo());
			if (wi.has_value())
			{
				s_host_display = HostDisplay::CreateDisplayForAPI(api);
				if (s_host_display)
				{
					if (!s_host_display->CreateRenderDevice(wi.value(), Host::GetStringSettingValue("EmuCore/GS", "Adapter", ""),
							EmuConfig.GetEffectiveVsyncMode(), Host::GetBoolSettingValue("EmuCore/GS", "ThreadedPresentation", false),
							Host::GetBoolSettingValue("EmuCore/GS", "UseDebugDevice", false)))
					{
						s_host_display.reset();
					}
				}
			}

			if (s_host_display)
				s_host_display->DoneRenderContextCurrent();
			else
				g_nogui_window->DestroyPlatformWindow();
		}

		s_host_display_created.Post();
	});

	s_host_display_created.Wait();

	if (!s_host_display)
	{
		g_nogui_window->ReportError("Error", "Failed to create host display.");
		return nullptr;
	}

	if (!s_host_display->MakeRenderContextCurrent() || !s_host_display->InitializeRenderDevice(EmuFolders::Cache, false) ||
		!ImGuiManager::Initialize())
	{
		g_nogui_window->ReportError("Error", "Failed to initialize render device.");
		ReleaseHostDisplay();
		return nullptr;
	}

	Console.WriteLn(Color_StrongGreen, "%s Graphics Driver Info:", HostDisplay::RenderAPIToString(s_host_display->GetRenderAPI()));
	Console.Indent().WriteLn(s_host_display->GetDriverInfo());

	if (!FullscreenUI::Initialize())
	{
		g_nogui_window->ReportError("Error", "Failed to initialize fullscreen UI");
		ReleaseHostDisplay();
		return nullptr;
	}

	return s_host_display.get();
}

void Host::ReleaseHostDisplay()
{
	if (!s_host_display)
		return;

	ImGuiManager::Shutdown();

	s_host_display.reset();

	g_nogui_window->ExecuteInMessageLoop([]() { g_nogui_window->DestroyPlatformWindow(); });
}

bool Host::BeginPresentFrame(bool frame_skip)
{
	if (s_host_display->BeginPresent(frame_skip))
		return true;

	// don't render imgui
	ImGuiManager::NewFrame();
	return false;
}

void Host::EndPresentFrame()
{
	if (GSDumpReplayer::IsReplayingDump())
		GSDumpReplayer::RenderUI();

	FullscreenUI::Render();
	ImGuiManager::RenderOSD();
	s_host_display->EndPresent();
	ImGuiManager::NewFrame();
}

void Host::ResizeHostDisplay(u32 new_window_width, u32 new_window_height, float new_window_scale)
{
	s_host_display->ResizeRenderWindow(new_window_width, new_window_height, new_window_scale);
	ImGuiManager::WindowResized();
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
	g_nogui_window->RequestRenderWindowSize(width, height);
}

void Host::UpdateHostDisplay()
{
	// not used in nogui, except if we do exclusive fullscreen
}

std::string NoGUIHost::GetWindowTitle(const std::string& game_title)
{
	std::string suffix(GetAppConfigSuffix());
	std::string window_title;
	if (!VMManager::HasValidVM() || game_title.empty())
		window_title = GetAppNameAndVersion() + suffix;
	else
		window_title = game_title;

	return window_title;
}

void NoGUIHost::UpdateWindowTitle(const std::string& game_title)
{
	g_nogui_window->SetPlatformWindowTitle(GetWindowTitle(game_title));
}

void Host::OnVMStarting()
{
	Console.WriteLn("Host::OnVMStarting()");
	s_save_state_on_shutdown = false;
}

void Host::OnVMStarted()
{
	Console.WriteLn("Host::OnVMStarted()");
}

void Host::OnVMDestroyed()
{
	Console.WriteLn("Host::OnVMDestroyed()");
}

void Host::OnVMPaused()
{
	Console.WriteLn("Host::OnVMPaused()");
}

void Host::OnVMResumed()
{
	Console.WriteLn("Host::OnVMResumed()");
}

void Host::OnGameChanged(const std::string& disc_path, const std::string& game_serial, const std::string& game_name, u32 game_crc)
{
	Console.WriteLn("Host::OnGameChanged(%s, %s, %s, %08X)", disc_path.c_str(), game_serial.c_str(), game_name.c_str(), game_crc);
	NoGUIHost::UpdateWindowTitle(game_name);
}

void Host::OnPerformanceMetricsUpdated()
{
}

void Host::OnSaveStateLoading(const std::string_view& filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view& filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view& filename)
{
}

void Host::InvalidateSaveStateCache()
{
}

void Host::PumpMessagesOnCPUThread()
{
	NoGUIHost::ProcessCPUThreadPlatformMessages();
	NoGUIHost::ProcessCPUThreadEvents(false);
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	std::unique_lock lock(s_cpu_thread_events_mutex);
	s_cpu_thread_events.emplace_back(std::move(function), block);
	s_cpu_thread_event_posted.notify_one();
	if (block)
		s_cpu_thread_event_done.wait(lock, []() { return s_blocking_cpu_events_pending == 0; });
}

void NoGUIHost::GameListRefreshThreadEntryPoint(bool invalidate_cache)
{
	Threading::SetNameOfCurrentThread("Game List Refresh");

	FullscreenUI::ProgressCallback callback("game_list_refresh");
	std::unique_lock lock(s_game_list_refresh_lock);
	s_game_list_refresh_progress = &callback;

	lock.unlock();
	GameList::Refresh(invalidate_cache, &callback);
	lock.lock();

	s_game_list_refresh_progress = nullptr;
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
	CancelGameListRefresh();

	s_game_list_refresh_thread = std::thread(NoGUIHost::GameListRefreshThreadEntryPoint, invalidate_cache);
}

void Host::CancelGameListRefresh()
{
	std::unique_lock lock(s_game_list_refresh_lock);
	if (!s_game_list_refresh_thread.joinable())
		return;

	if (s_game_list_refresh_progress)
		s_game_list_refresh_progress->SetCancelled();

	lock.unlock();
	s_game_list_refresh_thread.join();
}

bool Host::IsFullscreen()
{
	return s_is_fullscreen;
}

void Host::SetFullscreen(bool enabled)
{
	if (s_is_fullscreen == enabled)
		return;

	s_is_fullscreen = enabled;
	g_nogui_window->SetFullscreen(enabled);
}

void Host::RequestExit(bool save_state_if_running)
{
	if (VMManager::HasValidVM())
	{
		s_save_state_on_shutdown = save_state_if_running;
		VMManager::SetState(VMState::Stopping);
	}

	// clear the running flag, this'll break out of the main CPU loop once the VM is shutdown.
	s_running.store(false, std::memory_order_release);
}

void Host::RequestVMShutdown(bool save_state)
{
	if (VMManager::HasValidVM())
	{
		s_save_state_on_shutdown = save_state;
		VMManager::SetState(VMState::Stopping);
	}
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view& str)
{
	return g_nogui_window->ConvertHostKeyboardStringToCode(str);
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return g_nogui_window->ConvertHostKeyboardCodeToString(code);
}

SysMtgsThread& GetMTGS()
{
	return s_mtgs_thread;
}

// ------------------------------------------------------------------------
// Hotkeys
// ------------------------------------------------------------------------

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()


//////////////////////////////////////////////////////////////////////////
// Interface Stuff
//////////////////////////////////////////////////////////////////////////

const IConsoleWriter* PatchesCon = &Console;

static void SignalHandler(int signal)
{
	// First try the normal (graceful) shutdown/exit.
	static bool graceful_shutdown_attempted = false;
	if (!graceful_shutdown_attempted)
	{
		std::fprintf(stderr, "Received CTRL+C, attempting graceful shutdown. Press CTRL+C again to force.\n");
		graceful_shutdown_attempted = true;

		// This could be a bit risky invoking from a signal handler... hopefully it's okay.
		// FIXME
		//QMetaObject::invokeMethod(g_main_window, &MainWindow::requestExit, Qt::QueuedConnection);
		return;
	}

	std::signal(signal, SIG_DFL);

	// MacOS is missing std::quick_exit() despite it being C++11...
#ifndef __APPLE__
	std::quick_exit(1);
#else
	_Exit(1);
#endif
}

void NoGUIHost::HookSignals()
{
	std::signal(SIGINT, SignalHandler);
	std::signal(SIGTERM, SignalHandler);
}
