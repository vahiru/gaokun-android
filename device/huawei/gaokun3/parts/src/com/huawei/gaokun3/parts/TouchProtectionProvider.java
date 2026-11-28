/*
 * 防误触模式开关，以开关条目的形式直接注入「设置 → 显示」，没有单独的页面。注入方式与性能模式相同
 * （PerformanceModeProvider），provider受WRITE_SECURE_SETTINGS保护。
 *
 * 只做一件事：把persist.sys.gaokun3.touch_mode设成"daily"（开启，识别手掌）或"game"（关闭，
 * 不识别手掌）。init（etc/touchmode.rc的两条字面值触发器）起/vendor/bin/gaokun3-touch-mode.sh写驱动的
 * algo/参数，两个预设只差手掌识别，差别只看那个脚本。切换立即生效，重启后保留。
 * persist.sys.*是system_prop，system_app能直接写，不必绕vendor属性。
 *
 * 开关与属性值的对应是反过来的：镜像默认game（device.mk），所以防误触模式默认关闭。
 * 只有字面值"daily"算开启，没设过或认不出的值都按关闭显示，与touchmode.rc一致。
 *
 * 控制中心的磁贴（TouchModeTileService）和「MateBook E Go设置」页（DeviceSettingsActivity）改的是同一个属性，
 * 改完通知设置刷新这个开关。
 */
package com.huawei.gaokun3.parts;

import android.content.Context;
import android.os.SystemProperties;
import android.util.Log;

import com.android.settingslib.drawer.EntriesProvider;
import com.android.settingslib.drawer.EntryController;
import com.android.settingslib.drawer.ProviderSwitch;

import java.util.List;

public class TouchProtectionProvider extends EntriesProvider {

    private static final String TAG = "TouchProtectionProvider";

    static final String AUTHORITY = "com.huawei.gaokun3.parts.touchprotection";
    static final String KEY = "gaokun3_touch_protection";
    static final String PROP = "persist.sys.gaokun3.touch_mode";

    static boolean isEnabled() {
        return "daily".equals(SystemProperties.get(PROP, "game"));
    }

    /** SystemProperties.set()失败时抛出RuntimeException，由调用方处理。 */
    static void setEnabled(boolean enabled) {
        SystemProperties.set(PROP, enabled ? "daily" : "game");
    }

    static void notifySwitchChanged(Context context) {
        SettingsEntryUris.notifySwitchChanged(context, AUTHORITY, KEY);
    }

    @Override
    protected List<? extends EntryController> createEntryControllers() {
        return List.of(new TouchProtectionController(getContext()));
    }

    static class TouchProtectionController extends EntryController implements ProviderSwitch {

        private final Context mContext;

        TouchProtectionController(Context context) {
            mContext = context;
        }

        // 注入项的键会成为页面上的偏好键，加前缀以免与设置自身的键重名。
        @Override
        public String getKey() {
            return KEY;
        }

        @Override
        protected MetaData getMetaData() {
            return new MetaData("com.android.settings.category.ia.display")
                    .setTitle(R.string.touch_mode_title)
                    .setSummary(R.string.touch_mode_summary)
                    .setIcon(R.drawable.ic_touch_protection);
        }

        @Override
        public boolean isSwitchChecked() {
            return isEnabled();
        }

        // 返回false时设置会提示getSwitchErrorMessage()并把开关复原，而不是让设置进程崩溃。
        @Override
        public boolean onSwitchCheckedChanged(boolean checked) {
            try {
                setEnabled(checked);
                return true;
            } catch (RuntimeException e) {
                Log.w(TAG, "cannot set " + PROP, e);
                return false;
            }
        }

        @Override
        public String getSwitchErrorMessage(boolean attemptedChecked) {
            return mContext.getString(R.string.touch_mode_error);
        }
    }
}
