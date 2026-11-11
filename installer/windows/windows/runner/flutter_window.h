#ifndef RUNNER_FLUTTER_WINDOW_H_
#define RUNNER_FLUTTER_WINDOW_H_

#include <flutter/dart_project.h>
#include <flutter/encodable_value.h>
#include <flutter/flutter_view_controller.h>
#include <flutter/method_channel.h>

#include <memory>

#include "win32_window.h"

// A window that does nothing but host a Flutter view.
class FlutterWindow : public Win32Window {
 public:
  // Creates a new FlutterWindow hosting a Flutter view running |project|.
  explicit FlutterWindow(const flutter::DartProject& project);
  virtual ~FlutterWindow();

 protected:
  // Win32Window:
  bool OnCreate() override;
  void OnDestroy() override;
  LRESULT MessageHandler(HWND window, UINT const message, WPARAM const wparam,
                         LPARAM const lparam) noexcept override;

 private:
  // The project to run.
  flutter::DartProject project_;

  // The Flutter instance hosted by this window.
  std::unique_ptr<flutter::FlutterViewController> flutter_controller_;

  // gaokun3：界面自绘标题栏，拖动 / 最小化 / 关闭经这个通道（lib/ui/title_bar.dart 的 WindowControl）
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> window_channel_;

  // 正在安装或卸载：拒绝关闭窗口（自绘的关闭键、Alt+F4、任务栏上的关闭），阻止关机与系统睡眠
  bool busy_ = false;

  // 安装或卸载期间的电源请求（阻止系统进入睡眠状态、保持屏幕开启）；不在进行时为 nullptr
  HANDLE power_request_ = nullptr;

  // 开始或结束一次安装或卸载：设置或撤销电源请求与阻止关机的原因
  void SetBusy(bool busy, const std::wstring& power_reason, const std::wstring& shutdown_reason);
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
