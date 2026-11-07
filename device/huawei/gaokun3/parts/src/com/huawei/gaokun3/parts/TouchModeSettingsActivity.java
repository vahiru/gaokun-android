/*
 * 触摸手感开关（2026-10-25 用户要求：game / daily 做成开关）：替代 touchmode.rc 顶上那条
 * `adb shell su -c "setprop persist.sys.gaokun3.touch_mode …"`（发布构建上普通 adb shell 设不了 system_prop）。
 *
 * 只做一件事：把 persist.sys.gaokun3.touch_mode 设成 "game" 或 "daily"。
 * init（etc/touchmode.rc 的两条字面值触发器）起 /vendor/bin/gaokun3-touch-mode.sh，
 * 由它写驱动的 algo/ 参数 —— 两版预设差什么只看那个脚本，这里不重复。
 *
 * ★ 不用像待机开关那样绕 vendor 属性：persist.sys.* 是 system_prop
 *   （refs/lineage-sepolicy/private/property_contexts:77），system_app 本来就能写
 *   （private/system_app.te:43），init 触发器直接认它。
 *
 * ★ 开关显示的就是这个属性：镜像默认 game（device.mk 的 PRODUCT_SYSTEM_EXT_PROPERTIES），
 *   没设过 / 认不出的值都按 game 显示 —— 与 touchmode.rc 一致（只有字面 "daily" 会切到 daily）。
 *
 * 生效时机：立刻（脚本写 sysfs，驱动下一帧就用新参数），不用重启，重启后保留。
 * 控制中心磁贴见 TouchModeTileService。
 */
package com.huawei.gaokun3.parts;

import android.os.Bundle;
import android.os.SystemProperties;

import androidx.preference.Preference;
import androidx.preference.SwitchPreferenceCompat;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

public class TouchModeSettingsActivity extends CollapsingToolbarBaseActivity {

    static final String PROP = "persist.sys.gaokun3.touch_mode";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState == null) {
            getSupportFragmentManager().beginTransaction()
                    .replace(com.android.settingslib.collapsingtoolbar.R.id.content_frame,
                            new TouchModeFragment())
                    .commit();
        }
    }

    /** 开关打开 = game。只有字面 "daily" 算日常模式，其余（含没设过）都是镜像默认的 game。 */
    static boolean isGameMode() {
        return !"daily".equals(SystemProperties.get(PROP, "game"));
    }

    static void setGameMode(boolean game) {
        SystemProperties.set(PROP, game ? "game" : "daily");
    }

    public static class TouchModeFragment extends SettingsBasePreferenceFragment
            implements Preference.OnPreferenceChangeListener {

        private SwitchPreferenceCompat mSwitch;

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.touch_mode_prefs, rootKey);
            mSwitch = findPreference("touch_game_mode");
            mSwitch.setOnPreferenceChangeListener(this);
        }

        @Override
        public void onResume() {
            super.onResume();
            // 控制中心磁贴可能在这一页开着的时候改过它
            mSwitch.setChecked(isGameMode());
        }

        @Override
        public boolean onPreferenceChange(Preference preference, Object newValue) {
            setGameMode(Boolean.TRUE.equals(newValue));
            return true;
        }
    }
}
