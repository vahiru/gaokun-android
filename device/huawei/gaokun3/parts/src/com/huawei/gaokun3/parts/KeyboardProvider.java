/*
 * 磁吸键盘开关，以开关条目的形式直接注入「设置 → 已连接的设备」，没有单独的页面。
 *
 * 注入方式与性能模式相同（PerformanceModeProvider）：设置在「已连接的设备」页的分类里查到这个
 * provider，按getMetaData()显示标题、说明和图标，开关的状态和切换经isSwitchChecked() /
 * onSwitchCheckedChanged()读写。provider受WRITE_SECURE_SETTINGS保护，只有设置等系统组件能调用。
 *
 * 只做一件事：把persist.sys.gaokun3.keyboard设成"1"或"0"。真正动硬件的是init触发器
 * （etc/keyboard.rc）和/vendor/bin/gaokun3-keyboard.sh，它写内核的/sys/class/input/inputN/inhibited。
 * 应用即使是system uid也不该有写/sys/class/input的权限，经init改是Android里做这件事的标准通路。
 * 属性没设过按开启显示，与init一致：默认偏向键盘可用，写坏了也不至于锁死唯一的输入设备。
 *
 * 控制中心的磁贴（KeyboardTileService）和「MateBook E Go设置」页（DeviceSettingsActivity）改的是同一个属性，
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

public class KeyboardProvider extends EntriesProvider {

    private static final String TAG = "KeyboardProvider";

    static final String AUTHORITY = "com.huawei.gaokun3.parts.keyboard";
    static final String KEY = "gaokun3_keyboard";
    static final String PROP = "persist.sys.gaokun3.keyboard";

    /** 只有字面值"0"算关闭。 */
    static boolean isEnabled() {
        return !"0".equals(SystemProperties.get(PROP, "1"));
    }

    /** SystemProperties.set()失败时抛出RuntimeException，由调用方处理。 */
    static void setEnabled(boolean enabled) {
        SystemProperties.set(PROP, enabled ? "1" : "0");
    }

    static void notifySwitchChanged(Context context) {
        SettingsEntryUris.notifySwitchChanged(context, AUTHORITY, KEY);
    }

    @Override
    protected List<? extends EntryController> createEntryControllers() {
        return List.of(new KeyboardController(getContext()));
    }

    static class KeyboardController extends EntryController implements ProviderSwitch {

        private final Context mContext;

        KeyboardController(Context context) {
            mContext = context;
        }

        // 注入项的键会成为页面上的偏好键，加前缀以免与设置自身的键重名。
        @Override
        public String getKey() {
            return KEY;
        }

        @Override
        protected MetaData getMetaData() {
            return new MetaData("com.android.settings.category.ia.connect")
                    .setTitle(R.string.keyboard_title)
                    .setSummary(R.string.keyboard_summary)
                    .setIcon(R.drawable.ic_keyboard);
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
            return mContext.getString(R.string.keyboard_error);
        }
    }
}
