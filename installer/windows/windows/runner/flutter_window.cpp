#include "flutter_window.h"

#include <windowsx.h>

#include <optional>
#include <string>
#include <variant>

#include <flutter/standard_method_codec.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "flutter/generated_plugin_registrant.h"
#include "utils.h"

namespace {

std::wstring Utf16FromUtf8(const std::string& s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  if (n <= 1) {
    return std::wstring();
  }
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
  w.resize(n - 1);
  return w;
}

// 系统的"选择文件夹"对话框（gk3/window 的 pickFolder）。取消或失败时返回空串
std::string PickFolder(HWND owner, const std::string& title_utf8) {
  std::string out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
    return out;
  }
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  if (!title_utf8.empty()) {
    dlg->SetTitle(Utf16FromUtf8(title_utf8).c_str());
  }
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out = Utf8FromUtf16(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  // 自绘标题栏的拖动：子窗口的命中测试先过 ChildProc（见 flutter_window.h）。this 用窗口属性存，
  // 不占 GWLP_USERDATA（那是 Flutter 自己的）
  HWND child = flutter_controller_->view()->GetNativeWindow();
  SetPropW(child, L"gk3.window", this);
  child_proc_ = reinterpret_cast<WNDPROC>(
      SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FlutterWindow::ChildProc)));

  window_channel_ = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
      flutter_controller_->engine()->messenger(), "gk3/window",
      &flutter::StandardMethodCodec::GetInstance());
  window_channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
        HWND hwnd = GetHandle();
        const std::string& m = call.method_name();
        if (m == "minimize") {
          ShowWindow(hwnd, SW_MINIMIZE);
        } else if (m == "close") {
          if (!busy_) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
          }
        } else if (m == "forceClose") {
          // 界面已经问过、收拾好了（下载中途退出）：不再问。写盘期间（busy_）仍然不关
          if (!busy_) {
            force_close_ = true;
            PostMessage(hwnd, WM_CLOSE, 0, 0);
          }
        } else if (m == "maximize") {
          ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (m == "setCaption") {
          // 参数：{canvasW, canvasH, strip, buttons}（界面的逻辑像素，见 flutter_window.h 的 canvas_w_ 等）
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          auto num = [args](const char* key, double fallback) {
            if (args == nullptr) return fallback;
            auto it = args->find(flutter::EncodableValue(key));
            const auto* v = it == args->end() ? nullptr : std::get_if<double>(&it->second);
            return v ? *v : fallback;
          };
          canvas_w_ = num("canvasW", canvas_w_);
          canvas_h_ = num("canvasH", canvas_h_);
          strip_ = num("strip", strip_);
          buttons_ = num("buttons", buttons_);
        } else if (m == "startDrag") {
          // 把这次按下交给 Windows 当作拖动标题栏（系统的移动循环接管鼠标，松开时结束）
          ReleaseCapture();
          SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        } else if (m == "downloadsDir") {
          // 当前用户的"下载"文件夹（已知文件夹：用户把它挪到别的盘时也对，不拼 %USERPROFILE%\Downloads）。
          // 下载的安装文件放在它下面的 Gaokun Android\<版本>\（lib/session.dart 的 Session.downloadsDir）
          PWSTR path = nullptr;
          std::string out;
          if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path)) && path != nullptr) {
            out = Utf8FromUtf16(path);
          }
          CoTaskMemFree(path);
          if (out.empty()) {
            result->Success();
          } else {
            result->Success(flutter::EncodableValue(out));
          }
          return;
        } else if (m == "pickFolder") {
          const auto* t = std::get_if<std::string>(call.arguments());
          std::string path = PickFolder(hwnd, t ? *t : std::string());
          if (path.empty()) {
            result->Success();
          } else {
            result->Success(flutter::EncodableValue(path));
          }
          return;
        } else if (m == "setBusy") {
          // 参数：{busy: 拒绝关闭、阻止关机（gk3w 写盘期间）, awake: 保持唤醒与亮屏（整个任务期间，含下载）,
          //        power: 电源请求的原因, shutdown: 阻止关机的原因}
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          auto str = [args](const char* key) {
            if (args == nullptr) return std::string();
            auto it = args->find(flutter::EncodableValue(key));
            const auto* v = it == args->end() ? nullptr : std::get_if<std::string>(&it->second);
            return v ? *v : std::string();
          };
          auto flag = [args](const char* key, bool fallback) {
            if (args == nullptr) return fallback;
            auto it = args->find(flutter::EncodableValue(key));
            const auto* b = it == args->end() ? nullptr : std::get_if<bool>(&it->second);
            return b == nullptr ? fallback : *b;
          };
          const bool busy = flag("busy", false);
          confirm_close_ = flag("confirmClose", false);
          SetBusy(busy, flag("awake", busy), Utf16FromUtf8(str("power")), Utf16FromUtf8(str("shutdown")));
        } else {
          result->NotImplemented();
          return;
        }
        result->Success();
      });

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::SetBusy(bool busy, bool awake, const std::wstring& power_reason, const std::wstring& shutdown_reason) {
  HWND hwnd = GetHandle();
  std::wstring reason = power_reason.empty() ? std::wstring(L"Gaokun Android") : power_reason;
  awake = awake || busy;
  if (awake && power_request_ == nullptr) {
    // 电源请求：系统不因空闲而进入睡眠状态，屏幕保持开启（powercfg /requests 中显示原因）。下载期间也要：
    // 这台是现代待机设备，熄屏后桌面程序被挂起，下载会停住。合上键盘或按电源按钮进入的睡眠无法由应用阻止，进度页另有提示
    REASON_CONTEXT ctx = {};
    ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
    ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    ctx.Reason.SimpleReasonString = reason.data();
    HANDLE h = PowerCreateRequest(&ctx);
    if (h != INVALID_HANDLE_VALUE) {
      power_request_ = h;
      PowerSetRequest(power_request_, PowerRequestSystemRequired);
      PowerSetRequest(power_request_, PowerRequestDisplayRequired);
    }
  } else if (!awake && power_request_ != nullptr) {
    PowerClearRequest(power_request_, PowerRequestSystemRequired);
    PowerClearRequest(power_request_, PowerRequestDisplayRequired);
    CloseHandle(power_request_);
    power_request_ = nullptr;
  }
  if (busy && !busy_) {
    // 只在 gk3w 写盘期间：关机、重启、注销时 Windows 在"以下应用正在阻止关机"中显示这个原因（配合 WM_QUERYENDSESSION
    // 返回 FALSE），窗口拒绝关闭。下载期间不挡：关掉了 .part 还在，下次接着下
    ShutdownBlockReasonCreate(hwnd, shutdown_reason.empty() ? reason.c_str() : shutdown_reason.c_str());
  } else if (!busy && busy_) {
    ShutdownBlockReasonDestroy(hwnd);
  }
  busy_ = busy;
}

bool FlutterWindow::InCaption(POINT client) {
  if (strip_ <= 0 || canvas_w_ <= 0 || canvas_h_ <= 0) return false;
  RECT rc;
  if (!GetClientRect(GetHandle(), &rc)) return false;
  const double w = rc.right - rc.left, h = rc.bottom - rc.top;
  // 与 LogicalCanvas(extend: true) 相同：按画布放得下的比例缩放，多出来的方向把画布加宽或加高，铺满窗口
  const double s = (w / canvas_w_ < h / canvas_h_) ? w / canvas_w_ : h / canvas_h_;
  if (client.y < 0 || client.y >= strip_ * s) return false;
  return client.x >= 0 && client.x < w - buttons_ * s;
}

// static
LRESULT CALLBACK FlutterWindow::ChildProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  auto* self = static_cast<FlutterWindow*>(GetPropW(hwnd, L"gk3.window"));
  if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
  if (message == WM_NCHITTEST) {
    POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(self->GetHandle(), &pt);
    if (self->InCaption(pt)) return HTTRANSPARENT;
  } else if (message == WM_NCDESTROY) {
    WNDPROC old = self->child_proc_;
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(old));
    RemovePropW(hwnd, L"gk3.window");
    return CallWindowProcW(old, hwnd, message, wparam, lparam);
  }
  return CallWindowProcW(self->child_proc_, hwnd, message, wparam, lparam);
}

void FlutterWindow::OnDestroy() {
  SetBusy(false, false, std::wstring(), std::wstring());
  window_channel_ = nullptr;
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // 安装或卸载期间不关闭窗口（WM_CLOSE 来自 Alt+F4、任务栏、PostMessage）
  if (message == WM_CLOSE && busy_) {
    // forceClose 发出的 WM_CLOSE 赶上了写盘开始：这次不关，也不能留着 force_close_（之后的关闭会跳过界面的确认）
    force_close_ = false;
    MessageBeep(MB_ICONWARNING);
    return 0;
  }
  // 下载中途：先交给界面问"确定要退出吗"（确认后界面删掉下载的文件，再调 forceClose）
  if (message == WM_CLOSE && confirm_close_ && !force_close_ && window_channel_) {
    window_channel_->InvokeMethod("closeRequested", nullptr);
    return 0;
  }
  // 标题栏区域（子窗口在这里返回 HTTRANSPARENT，命中测试落到本窗口）：由系统拖动、双击最大化
  if (message == WM_NCHITTEST) {
    POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ScreenToClient(hwnd, &pt);
    if (InCaption(pt)) return HTCAPTION;
  }
  // 最大化 / 还原：告诉界面换按钮的图标（双击标题栏、Win+↑ 也走这里）
  if (message == WM_SIZE && window_channel_ && (wparam == SIZE_MAXIMIZED || wparam == SIZE_RESTORED)) {
    const int maximized = wparam == SIZE_MAXIMIZED ? 1 : 0;
    if (maximized != last_maximized_) {
      last_maximized_ = maximized;
      window_channel_->InvokeMethod("maximized", std::make_unique<flutter::EncodableValue>(maximized == 1));
    }
  }
  // 安装或卸载期间阻止关机、重启、注销：返回 FALSE，Windows 显示 ShutdownBlockReasonCreate 设置的原因。
  // 用户仍可选择强制关机，这一点无法阻止
  if (message == WM_QUERYENDSESSION && busy_) {
    return FALSE;
  }

  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
