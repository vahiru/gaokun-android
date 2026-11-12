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
  void SetBusy(bool busy, bool awake, const std::wstring& power_reason, const std::wstring& shutdown_reason);

  // 正在下载：关闭窗口（Alt+F4、任务栏）先交给界面问一句（closeRequested），确认之后界面调 forceClose
  bool confirm_close_ = false;
  bool force_close_ = false;

  // 自绘标题栏的几何（界面按 canvas_w × canvas_h 放得下的比例缩放，多出来的方向把画布加宽或加高、铺满窗口；
  // 最上面 strip 高的一行是标题栏，右上角 buttons 宽的一段是最小化 / 最大化 / 关闭按钮）。
  // setCaption 之前 strip 为 0：没有标题栏区域
  double canvas_w_ = 1280, canvas_h_ = 800, strip_ = 0, buttons_ = 0;

  // 客户区坐标的这一点是不是标题栏（拖动窗口、双击最大化的区域，不含按钮）
  bool InCaption(POINT client);

  // Flutter 的子窗口盖满客户区：在它的窗口过程前面接一层，标题栏区域的命中测试返回 HTTRANSPARENT，
  // 交给本窗口按 HTCAPTION 处理 —— 这样鼠标、触摸、手写笔都由系统拖动（原先模拟鼠标按下，触摸拖不动）
  static LRESULT CALLBACK ChildProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  WNDPROC child_proc_ = nullptr;

  // 最近一次告诉界面的最大化状态（WM_SIZE 时有变化才发）
  int last_maximized_ = -1;
};

#endif  // RUNNER_FLUTTER_WINDOW_H_
