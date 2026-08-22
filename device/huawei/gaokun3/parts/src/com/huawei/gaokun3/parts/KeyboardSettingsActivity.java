/*
 * 磁吸键盘开关。
 *
 * 只做一件事：把 persist.sys.gaokun3.keyboard 设成 "1" 或 "0"。
 * 真正动硬件的是 init 触发器 + /vendor/bin/gaokun3-keyboard.sh，
 * 它写内核的 /sys/class/input/inputN/inhibited。
 *
 * ★ 用属性而不是让应用直接写 sysfs：应用（哪怕 system uid）不该有写
 *   /sys/class/input 的权限，而 init 触发器是 Android 里做这件事的标准通路，
 *   将来转 SELinux enforcing 也不用为这个 app 开特权。
 *
 * ★ 用【平台自带】的 android.preference 而不是 androidx：这个应用在设备树里编，
 *   少一个静态库依赖就少一处构建失败的可能。它确实已废弃，但功能完整，
 *   而且这个界面只有一个开关。
 */
package com.huawei.gaokun3.parts;

import android.app.Activity;
import android.os.Bundle;
import android.os.SystemProperties;
import android.preference.Preference;
import android.preference.PreferenceFragment;
import android.preference.SwitchPreference;

public class KeyboardSettingsActivity extends Activity {

    /** init 触发器监听的属性。persist.sys.* 的上下文是 system_prop，系统应用可写。 */
    private static final String PROP = "persist.sys.gaokun3.keyboard";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // ⚠️ 必须用自己的 layout（带 fitsSystemWindows），不能直接塞
        //    android.R.id.content：targetSdk 35+ 强制 edge-to-edge，
        //    那样开关会被画到标题栏底下，界面看起来"没有开关"。
        setContentView(R.layout.settings_activity);
        if (savedInstanceState == null) {
            getFragmentManager().beginTransaction()
                    .replace(R.id.container, new KeyboardFragment())
                    .commit();
        }
    }

    public static class KeyboardFragment extends PreferenceFragment
            implements Preference.OnPreferenceChangeListener {

        private SwitchPreference mSwitch;

        @Override
        public void onCreate(Bundle savedInstanceState) {
            super.onCreate(savedInstanceState);
            addPreferencesFromResource(R.xml.keyboard_prefs);
            mSwitch = (SwitchPreference) findPreference("keyboard_enabled");
            // 属性没设过 = 键盘开着。默认永远偏向"能用"——写坏了也不至于把
            // 用户唯一的输入设备锁死。
            mSwitch.setChecked(!"0".equals(SystemProperties.get(PROP, "1")));
            mSwitch.setOnPreferenceChangeListener(this);
        }

        @Override
        public boolean onPreferenceChange(Preference preference, Object newValue) {
            SystemProperties.set(PROP, Boolean.TRUE.equals(newValue) ? "1" : "0");
            return true;
        }
    }
}
