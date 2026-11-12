#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter_windows.h>
#include <windows.h>

#include "flutter_window.h"
#include "utils.h"

// 窗口标题，也用来找已经开着的那一个
static const wchar_t kTitle[] = L"Gaokun Android Installer";

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
  // 只开一个：UAC 弹得慢时连点两下会开出两个，两个同时下载会往同一个 .part 里写（审查 11-12 第 12 条）。
  // 已经开着时把那个窗口提到前面、自己退出。互斥量在进程退出时由系统释放，不用手动关。
  // Debug 版（flutter run、integration_test）不查：开着一个 Release 版时它们会直接退出（修复审查 11-12 第 7 条）
#ifdef NDEBUG
  HANDLE single = ::CreateMutexW(nullptr, TRUE, L"Local\\GaokunAndroidInstaller");
  if (single != nullptr && ::GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND other = ::FindWindowW(L"FLUTTER_RUNNER_WIN32_WINDOW", kTitle);
    if (other != nullptr) {
      ::ShowWindow(other, ::IsIconic(other) ? SW_RESTORE : SW_SHOW);
      ::SetForegroundWindow(other);
    }
    return EXIT_SUCCESS;
  }
#endif

  // Attach to console when present (e.g., 'flutter run') or create a
  // new console when running with a debugger.
  if (!::AttachConsole(ATTACH_PARENT_PROCESS) && ::IsDebuggerPresent()) {
    CreateAndAttachConsole();
  }

  // Initialize COM, so that it is available for use in the library and/or
  // plugins.
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  flutter::DartProject project(L"data");

  std::vector<std::string> command_line_arguments =
      GetCommandLineArguments();

  project.set_dart_entrypoint_arguments(std::move(command_line_arguments));

  // gaokun3：窗口按主屏工作区算大小，保持界面 1280×800 的比例（lib/app.dart 的画布，最小化与关闭也画在画布里），
  // 放不下 1:1 时等比缩小 —— 平板在 200% 缩放下工作区只有约 1280×752 逻辑像素
  RECT work;
  SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
  const POINT top_left = {work.left, work.top};
  const double scale = FlutterDesktopGetDpiForMonitor(MonitorFromPoint(top_left, MONITOR_DEFAULTTOPRIMARY)) / 96.0;
  const double work_w = (work.right - work.left) / scale, work_h = (work.bottom - work.top) / scale;
  double h = 800, w = 1280;
  if (h > work_h * 0.94) {
    h = work_h * 0.94;
    w = h * 1280 / 800;
  }
  if (w > work_w * 0.96) {
    w = work_w * 0.96;
    h = w * 800 / 1280;
  }
  FlutterWindow window(project);
  Win32Window::Point origin(static_cast<unsigned int>(work.left / scale + (work_w - w) / 2),
                            static_cast<unsigned int>(work.top / scale + (work_h - h) / 2));
  Win32Window::Size size(static_cast<unsigned int>(w), static_cast<unsigned int>(h));
  if (!window.Create(kTitle, origin, size)) {
    return EXIT_FAILURE;
  }
  window.SetQuitOnClose(true);

  ::MSG msg;
  while (::GetMessage(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
  }

  ::CoUninitialize();
  return EXIT_SUCCESS;
}
