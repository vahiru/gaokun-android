/*
 * 「MateBook E Go设置」页：注入到设置首页通知、提示音和振动所在的分组中，紧跟在「crDroid设置」后面，
 * 把本应用注入到设置各处的条目集中列出，便于发现（res/xml/device_settings_prefs.xml）。
 *
 * 首页条目的位置由manifest里的meta-data决定：com.android.settings.group_key指向首页的
 * top_level_personalize_category分组，com.android.settings.order为-55，排在crDroid设置（-60）与
 * 通知（-50）之间。首页的注入项直接使用这个order（首页没有占位条目，不加偏移）。
 *
 * 原有的条目都保留在原处。页面类条目以intent打开原有的activity；磁吸键盘、防误触模式、性能模式
 * 与两个「为游戏自动…」开关在这里直接读写，与设置里的开关条目改同一个属性（各Provider的isEnabled()/setEnabled()），
 * 改完通知设置刷新对应的开关条目。控制中心的磁贴改动属性后也会发出同一通知，本页监听这些URI，
 * 下拉控制中心切换时开关随即更新。桌面模式开关只在本页和控制中心的磁贴中提供，读写
 * Settings.Secure中的一项（DesktopModeSetting），本页直接监听这项设置。
 *
 * 启动选项与BootOptionsActivity一样，只在该activity已启用时显示（BootEntryNotifier按HAL导出的属性启用）。
 */
package com.huawei.gaokun3.parts;

import android.content.ComponentName;
import android.content.Context;
import android.content.pm.PackageManager;
import android.database.ContentObserver;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Toast;

import androidx.preference.Preference;
import androidx.preference.SwitchPreferenceCompat;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.drawer.EntriesProvider;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

public class DeviceSettingsActivity extends CollapsingToolbarBaseActivity {

    private static final String TAG = "DeviceSettings";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState == null) {
            getSupportFragmentManager().beginTransaction()
                    .replace(com.android.settingslib.collapsingtoolbar.R.id.content_frame,
                            new DeviceSettingsFragment())
                    .commit();
        }
    }

    public static class DeviceSettingsFragment extends SettingsBasePreferenceFragment
            implements Preference.OnPreferenceChangeListener {

        private SwitchPreferenceCompat mKeyboard;
        private SwitchPreferenceCompat mTouchProtection;
        private SwitchPreferenceCompat mTouchGameAuto;
        private SwitchPreferenceCompat mPerformanceMode;
        private SwitchPreferenceCompat mPerfGameAuto;
        private SwitchPreferenceCompat mDesktopMode;
        private Preference mSpeakerFx;
        private Preference mBootCategory;

        private final ContentObserver mObserver =
                new ContentObserver(new Handler(Looper.getMainLooper())) {
                    @Override
                    public void onChange(boolean selfChange) {
                        refresh();
                    }
                };

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.device_settings_prefs, rootKey);
            mKeyboard = findPreference("keyboard");
            mTouchProtection = findPreference("touch_protection");
            mTouchGameAuto = findPreference("touch_protection_game_auto");
            mPerformanceMode = findPreference("performance_mode");
            mPerfGameAuto = findPreference("performance_mode_game_auto");
            mDesktopMode = findPreference("desktop_mode");
            mSpeakerFx = findPreference("speaker_fx");
            mBootCategory = findPreference("category_boot");
            mKeyboard.setOnPreferenceChangeListener(this);
            mTouchProtection.setOnPreferenceChangeListener(this);
            mTouchGameAuto.setOnPreferenceChangeListener(this);
            mPerformanceMode.setOnPreferenceChangeListener(this);
            mPerfGameAuto.setOnPreferenceChangeListener(this);
            mDesktopMode.setOnPreferenceChangeListener(this);
        }

        // 从各页面或扬声器音效对话框返回时onResume，重新读取全部状态。
        @Override
        public void onResume() {
            super.onResume();
            refresh();
            observe(KeyboardProvider.AUTHORITY, KeyboardProvider.KEY);
            observe(TouchProtectionProvider.AUTHORITY, TouchProtectionProvider.KEY);
            observe(TouchProtectionProvider.AUTHORITY, TouchProtectionProvider.GAME_AUTO_KEY);
            observe(PerformanceModeProvider.AUTHORITY, PerformanceModeProvider.KEY);
            observe(PerformanceModeProvider.AUTHORITY, PerformanceModeProvider.GAME_AUTO_KEY);
            requireContext().getContentResolver().registerContentObserver(
                    DesktopModeSetting.uri(), false, mObserver);
        }

        @Override
        public void onPause() {
            super.onPause();
            requireContext().getContentResolver().unregisterContentObserver(mObserver);
        }

        private void observe(String authority, String key) {
            Uri uri = SettingsEntryUris.build(authority, EntriesProvider.METHOD_IS_CHECKED, key);
            requireContext().getContentResolver().registerContentObserver(uri, false, mObserver);
        }

        private void refresh() {
            Context context = getContext();
            if (context == null) {
                return;
            }
            mKeyboard.setChecked(KeyboardProvider.isEnabled());
            mTouchProtection.setChecked(TouchProtectionProvider.isEnabled());
            mTouchGameAuto.setChecked(TouchProtectionProvider.isGameAutoEnabled());
            mPerformanceMode.setChecked(PerformanceModeProvider.isEnabled());
            mPerfGameAuto.setChecked(PerformanceModeProvider.isGameAutoEnabled());
            mDesktopMode.setChecked(DesktopModeSetting.isEnabled(context));
            mSpeakerFx.setSummary(SpeakerFxDialogActivity.currentLabel(context));
            mBootCategory.setVisible(context.getPackageManager().getComponentEnabledSetting(
                    new ComponentName(context, BootOptionsActivity.class))
                    == PackageManager.COMPONENT_ENABLED_STATE_ENABLED);
        }

        // 返回false时开关保持原状；与设置里的开关条目一样，提示与该开关对应的错误信息。
        // 桌面模式开关没有对应的设置条目与错误信息，写入失败时只记录日志。
        @Override
        public boolean onPreferenceChange(Preference preference, Object newValue) {
            Context context = requireContext();
            boolean checked = (Boolean) newValue;
            if (preference == mDesktopMode) {
                try {
                    if (DesktopModeSetting.setEnabled(context, checked)) {
                        return true;
                    }
                    Log.w(TAG, "cannot write " + DesktopModeSetting.KEY);
                } catch (RuntimeException e) {
                    Log.w(TAG, "cannot write " + DesktopModeSetting.KEY, e);
                }
                return false;
            }
            try {
                if (preference == mKeyboard) {
                    KeyboardProvider.setEnabled(checked);
                    KeyboardProvider.notifySwitchChanged(context);
                } else if (preference == mTouchProtection) {
                    TouchProtectionProvider.setEnabled(checked);
                    TouchProtectionProvider.notifySwitchChanged(context);
                } else if (preference == mTouchGameAuto) {
                    TouchProtectionProvider.setGameAutoEnabled(checked);
                    TouchProtectionProvider.notifyGameAutoSwitchChanged(context);
                } else if (preference == mPerformanceMode) {
                    PerformanceModeProvider.setEnabled(checked);
                    PerformanceModeProvider.notifySwitchChanged(context);
                } else if (preference == mPerfGameAuto) {
                    PerformanceModeProvider.setGameAutoEnabled(checked);
                    PerformanceModeProvider.notifyGameAutoSwitchChanged(context);
                }
                return true;
            } catch (RuntimeException e) {
                Log.w(TAG, "cannot change " + preference.getKey(), e);
                int error = preference == mKeyboard ? R.string.keyboard_error
                        : preference == mTouchProtection || preference == mTouchGameAuto
                                ? R.string.touch_mode_error
                        : R.string.perf_mode_error;
                Toast.makeText(context, error, Toast.LENGTH_SHORT).show();
                return false;
            }
        }
    }
}
