/*
 * 迁移自然方向为竖屏时保存的旋转锁定。
 *
 * Settings.System.USER_ROTATION是Surface.ROTATION_*值，从自然方向起算。旧版本的自然方向是面板的竖屏，
 * 现在是横屏，即旧的ROTATION_90（设备树中面板的rotation，见device.mk）。
 * 因此保存的值现在会多转四分之一圈：在横屏下锁定的方向（旧的ROTATION_90）会变成倒置的竖屏。
 * 这里把它往回调一次。
 *
 * 只有从旧版本升级上来的设置才带着这样的值。SettingsProvider在新建的数据库里也会写入默认值
 * （def_user_rotation），只看这个值无法区分，要看是否已完成开机设置：升级上来的设备在旧版本上
 * 完成过开机设置，新安装的系统第一次开机时开机设置还没有完成。无论哪种情况，第一次运行后都记为已迁移。
 */
package com.huawei.gaokun3.parts;

import android.content.ContentResolver;
import android.content.Context;
import android.content.SharedPreferences;
import android.provider.Settings;
import android.view.Surface;

final class RotationLockMigration {

    private static final String PREFS = "rotation_lock";
    private static final String KEY_DONE = "landscape_natural";

    private RotationLockMigration() {}

    static void apply(Context context) {
        // 用设备加密存储：BootReceiver在首次解锁前也会运行。
        SharedPreferences prefs = context.createDeviceProtectedStorageContext()
                .getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (prefs.getBoolean(KEY_DONE, false)) {
            return;
        }
        ContentResolver resolver = context.getContentResolver();
        boolean carriedOver =
                Settings.Global.getInt(resolver, Settings.Global.DEVICE_PROVISIONED, 0) != 0;
        int saved = Settings.System.getInt(resolver, Settings.System.USER_ROTATION,
                Surface.ROTATION_0);
        if (carriedOver && saved >= Surface.ROTATION_0 && saved <= Surface.ROTATION_270) {
            // 新的ROTATION_n是旧的ROTATION_(n + 1)。
            int shifted = (saved + 3) % 4;
            if (!Settings.System.putInt(resolver, Settings.System.USER_ROTATION, shifted)) {
                return; // 下次开机再试。
            }
        }
        prefs.edit().putBoolean(KEY_DONE, true).apply();
    }
}
