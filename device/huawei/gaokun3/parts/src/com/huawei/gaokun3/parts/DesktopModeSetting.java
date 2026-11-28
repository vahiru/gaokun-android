/*
 * 桌面模式：内屏为桌面优先（应用默认在桌面窗口中打开）还是触屏优先（平板模式）。
 *
 * 读写当前用户的Settings.Secure desktop_first_on_default_display，WMShell的
 * DesktopDisplayModeController（patches/android/frameworks-base/0012）监听这一项并切换内屏的窗口模式，
 * 设备overlay中的config_desktopFirstFollowsUserSetting打开这一行为。键名与补丁中的
 * DESKTOP_FIRST_ON_DEFAULT_DISPLAY一致，改名时两边一起改。
 *
 * 「MateBook E Go设置」页的开关与控制中心的磁贴（DesktopModeTileService）都改这一项；
 * 两个入口都直接读取设置，不需要互相通知。
 */
package com.huawei.gaokun3.parts;

import android.content.Context;
import android.net.Uri;
import android.provider.Settings;

final class DesktopModeSetting {

    static final String KEY = "desktop_first_on_default_display";

    private DesktopModeSetting() {}

    static Uri uri() {
        return Settings.Secure.getUriFor(KEY);
    }

    static boolean isEnabled(Context context) {
        return Settings.Secure.getInt(context.getContentResolver(), KEY, 0) != 0;
    }

    /**
     * 返回false表示无法连接SettingsProvider；权限不足等错误以异常抛出。SettingsProvider没有写入时
     * （例如值与原值相同）同样返回true。
     */
    static boolean setEnabled(Context context, boolean enabled) {
        return Settings.Secure.putInt(context.getContentResolver(), KEY, enabled ? 1 : 0);
    }
}
