/*
 * LiveDisplay HAL for the Huawei MateBook E Go (gaokun3)：只注册IAdaptiveBacklight/default，
 * 开关面板的CABC（内容自适应背光控制）。
 *
 * 用户入口是设置→显示→LiveDisplay页中的“降低耗电”。lineage-sdk的DisplayHardwareController
 * 在启动时和设置变化时调用setEnabled；开关状态保存在LineageSettings.System.DISPLAY_CABC
 * （键名display_low_power），默认值是lineage-sdk的config_defaultCABC（true），与出厂配置一致。
 *
 * 面板驱动（patches/kernel/drm/0022）在背光设备上提供cabc节点：0关闭，1界面模式，2静态图片，3视频。
 * 打开时写1，即两块面板初始化序列里出厂配置的模式。驱动保存模式并在每次面板上电后重新写入，
 * 所以这里不需要跟随亮屏和熄屏。
 *
 * 节点在开机时由init.gaokun3.rc交给system。class hal可能先于这一步启动，所以每次调用都重新打开节点，
 * 启动时不访问它。
 *
 * 节点不存在（内核不带上述补丁）时照常注册：VINTF声明了这个接口，LineageHardwareManager用
 * waitForDeclaredService获取它，不注册会让system_server一直等待。此时两个方法都返回错误，
 * 框架记录日志后按关闭处理。
 */
#define LOG_TAG "vendor.lineage.livedisplay-service.gaokun3"

#include <aidl/vendor/lineage/livedisplay/BnAdaptiveBacklight.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <string>

namespace {

using ::aidl::vendor::lineage::livedisplay::BnAdaptiveBacklight;
using ::android::base::ReadFileToString;
using ::android::base::Trim;
using ::android::base::WriteStringToFile;

// 与init.gaokun3.rc中brightness节点的路径相同：面板背光注册在第二路DSI（ae96000.dsi）上。
constexpr char kCabcPath[] = "/sys/class/backlight/ae96000.dsi.0/cabc";

class AdaptiveBacklight : public BnAdaptiveBacklight {
  public:
    ndk::ScopedAStatus getEnabled(bool* _aidl_return) override {
        std::string value;
        if (!ReadFileToString(kCabcPath, &value)) {
            PLOG(ERROR) << "Failed to read " << kCabcPath;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        // 手动写入的2、3（静态图片、视频模式）也算打开。
        *_aidl_return = Trim(value) != "0";
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus setEnabled(bool enabled) override {
        if (!WriteStringToFile(enabled ? "1" : "0", kCabcPath)) {
            PLOG(ERROR) << "Failed to write " << kCabcPath;
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        return ndk::ScopedAStatus::ok();
    }
};

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto backlight = ndk::SharedRefBase::make<AdaptiveBacklight>();

    const std::string instance = std::string() + AdaptiveBacklight::descriptor + "/default";
    binder_status_t status =
            AServiceManager_addService(backlight->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register " << instance;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
