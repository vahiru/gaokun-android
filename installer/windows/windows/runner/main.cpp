#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter_windows.h>
#include <windows.h>

#include "flutter_window.h"
#include "utils.h"

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE prev,
                      _In_ wchar_t *command_line, _In_ int show_command) {
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
  if (!window.Create(L"gaokun3", origin, size)) {
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
