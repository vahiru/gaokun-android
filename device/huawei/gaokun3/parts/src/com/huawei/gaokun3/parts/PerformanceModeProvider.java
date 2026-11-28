/*
 * 性能模式开关，以开关条目的形式直接注入「设置 → 电池」，没有单独的页面。
 *
 * 注入方式是SettingsLib的EntriesProvider：设置在电池页的分类里查到这个provider，按
 * getMetaData()显示标题、说明和图标，开关的状态和切换经isSwitchChecked() /
 * onSwitchCheckedChanged()读写（DashboardFragment把带开关的注入项渲染成SwitchPreferenceCompat）。
 * provider受WRITE_SECURE_SETTINGS保护，只有设置等系统组件能调用。
 *
 * 只做一件事：把persist.sys.gaokun3.perf_mode设成"performance"或"balanced"。
 * init（thermal/android.hardware.thermal-service.gaokun3.rc的触发器）把它转成
 * vendor.gaokun3.perf_mode，温控HAL据此和电源状态改写板载热敏电阻温区的降频点：开启时用华为
 * 性能模式的数值，关闭时按是否接电源在两套默认值之间自动切换（游戏期间自动开启时
 * 见下文）。各档数值只在thermal/Thermal.cpp里。
 * 没设过或认不出的值都按关闭显示，与开机后的HAL一致（运行中HAL对认不出的值保持上一个模式）。
 * 用电池时也允许开启。
 *
 * 「MateBook E Go设置」页（DeviceSettingsActivity）也有这个开关，改的是同一个属性，改完通知设置刷新。
 *
 * 紧随其后的第二个开关「为游戏自动开启性能模式」写persist.sys.gaokun3.perf_mode_auto（"1"/"0"，默认"1"，
 * device.mk）：性能模式关闭时，游戏在前台期间由温控HAL的.rc把vendor.gaokun3.perf_mode转写为performance，
 * 离开游戏后恢复，perf_mode本身不改。只有字面值"1"算开启，与该.rc一致。
 */
package com.huawei.gaokun3.parts;

import android.content.Context;
import android.os.SystemProperties;
import android.util.Log;

import com.android.settingslib.drawer.EntriesProvider;
import com.android.settingslib.drawer.EntryController;
import com.android.settingslib.drawer.ProviderSwitch;

import java.util.List;

public class PerformanceModeProvider extends EntriesProvider {

    private static final String TAG = "PerformanceModeProvider";

    static final String AUTHORITY = "com.huawei.gaokun3.parts.performancemode";
    static final String KEY = "gaokun3_performance_mode";
    static final String PROP = "persist.sys.gaokun3.perf_mode";

    /** 只有字面值"performance"算开启。 */
    static boolean isEnabled() {
        return "performance".equals(SystemProperties.get(PROP, ""));
    }

    /** SystemProperties.set()失败时抛出RuntimeException，由调用方处理。 */
    static void setEnabled(boolean enabled) {
        SystemProperties.set(PROP, enabled ? "performance" : "balanced");
    }

    static void notifySwitchChanged(Context context) {
        SettingsEntryUris.notifySwitchChanged(context, AUTHORITY, KEY);
    }

    static final String GAME_AUTO_KEY = "gaokun3_performance_mode_game_auto";
    static final String GAME_AUTO_PROP = "persist.sys.gaokun3.perf_mode_auto";

    static boolean isGameAutoEnabled() {
        return "1".equals(SystemProperties.get(GAME_AUTO_PROP, ""));
    }

    /** SystemProperties.set()失败时抛出RuntimeException，由调用方处理。 */
    static void setGameAutoEnabled(boolean enabled) {
        SystemProperties.set(GAME_AUTO_PROP, enabled ? "1" : "0");
    }

    static void notifyGameAutoSwitchChanged(Context context) {
        SettingsEntryUris.notifySwitchChanged(context, AUTHORITY, GAME_AUTO_KEY);
    }

    @Override
    protected List<? extends EntryController> createEntryControllers() {
        return List.of(new PerformanceModeController(getContext()),
                new GameAutoController(getContext()));
    }

    static class PerformanceModeController extends EntryController implements ProviderSwitch {

        private final Context mContext;

        PerformanceModeController(Context context) {
            mContext = context;
        }

        // 注入项的键会成为电池页上的偏好键，加前缀以免与设置自身的键重名。
        @Override
        public String getKey() {
            return KEY;
        }

        @Override
        protected MetaData getMetaData() {
            return new MetaData("com.android.settings.category.ia.battery")
                    .setTitle(R.string.perf_mode_title)
                    .setSummary(R.string.perf_mode_summary)
                    .setIcon(R.drawable.ic_performance_mode);
        }

        @Override
        public boolean isSwitchChecked() {
            return isEnabled();
        }

        // SystemProperties.set()失败时会抛出异常；在这里捕获并返回false，设置会提示错误并把开关
        // 复原，而不是让设置进程崩溃。
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
            return mContext.getString(R.string.perf_mode_error);
        }
    }

    /** 「为游戏自动开启性能模式」，与性能模式开关同在「设置 → 电池」，排在它之后。 */
    static class GameAutoController extends EntryController implements ProviderSwitch {

        private final Context mContext;

        GameAutoController(Context context) {
            mContext = context;
        }

        @Override
        public String getKey() {
            return GAME_AUTO_KEY;
        }

        @Override
        protected MetaData getMetaData() {
            return new MetaData("com.android.settings.category.ia.battery")
                    .setTitle(R.string.perf_mode_game_auto_title)
                    .setSummary(R.string.perf_mode_game_auto_summary);
        }

        @Override
        public boolean isSwitchChecked() {
            return isGameAutoEnabled();
        }

        @Override
        public boolean onSwitchCheckedChanged(boolean checked) {
            try {
                setGameAutoEnabled(checked);
                return true;
            } catch (RuntimeException e) {
                Log.w(TAG, "cannot set " + GAME_AUTO_PROP, e);
                return false;
            }
        }

        @Override
        public String getSwitchErrorMessage(boolean attemptedChecked) {
            return mContext.getString(R.string.perf_mode_error);
        }
    }
}
