#include "flutter_window.h"

#include <optional>
#include <string>
#include <variant>

#include <flutter/standard_method_codec.h>
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
        } else if (m == "startDrag") {
          // 把这次按下交给 Windows 当作拖动标题栏（系统的移动循环接管鼠标，松开时结束）
          ReleaseCapture();
          SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
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
          // 参数：{busy: bool, power: 电源请求的原因, shutdown: 阻止关机的原因}
          const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
          auto str = [args](const char* key) {
            if (args == nullptr) return std::string();
            auto it = args->find(flutter::EncodableValue(key));
            const auto* v = it == args->end() ? nullptr : std::get_if<std::string>(&it->second);
            return v ? *v : std::string();
          };
          bool busy = false;
          if (args != nullptr) {
            auto it = args->find(flutter::EncodableValue("busy"));
            const auto* b = it == args->end() ? nullptr : std::get_if<bool>(&it->second);
            busy = b != nullptr && *b;
          }
          SetBusy(busy, Utf16FromUtf8(str("power")), Utf16FromUtf8(str("shutdown")));
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

void FlutterWindow::SetBusy(bool busy, const std::wstring& power_reason, const std::wstring& shutdown_reason) {
  HWND hwnd = GetHandle();
  if (busy && !busy_) {
    // 电源请求：系统不因空闲而进入睡眠状态，屏幕保持开启（powercfg /requests 中显示原因）。
    // 合上键盘或按电源按钮进入的睡眠无法由应用阻止，进度页另有提示
    std::wstring reason = power_reason.empty() ? std::wstring(L"Gaokun Android") : power_reason;
    REASON_CONTEXT ctx = {};
    ctx.Version = POWER_REQUEST_CONTEXT_VERSION;
    ctx.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    ctx.Reason.SimpleReasonString = reason.data();
    power_request_ = PowerCreateRequest(&ctx);
    if (power_request_ == INVALID_HANDLE_VALUE) {
      power_request_ = nullptr;
    } else {
      PowerSetRequest(power_request_, PowerRequestSystemRequired);
      PowerSetRequest(power_request_, PowerRequestDisplayRequired);
    }
    // 关机、重启、注销时 Windows 在"以下应用正在阻止关机"中显示这个原因（配合 WM_QUERYENDSESSION 返回 FALSE）
    ShutdownBlockReasonCreate(hwnd, shutdown_reason.empty() ? reason.c_str() : shutdown_reason.c_str());
  } else if (!busy && busy_) {
    if (power_request_ != nullptr) {
      PowerClearRequest(power_request_, PowerRequestSystemRequired);
      PowerClearRequest(power_request_, PowerRequestDisplayRequired);
      CloseHandle(power_request_);
      power_request_ = nullptr;
    }
    ShutdownBlockReasonDestroy(hwnd);
  }
  busy_ = busy;
}

void FlutterWindow::OnDestroy() {
  SetBusy(false, std::wstring(), std::wstring());
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
    MessageBeep(MB_ICONWARNING);
    return 0;
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
