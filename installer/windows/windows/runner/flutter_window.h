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

  // 正在写盘：拒绝关闭（自绘的关闭键、Alt+F4、任务栏上的关闭都不行）
  bool busy_ = false;
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
