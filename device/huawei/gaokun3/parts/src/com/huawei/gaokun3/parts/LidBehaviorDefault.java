/*
 * Closing the keyboard cover turns the screen off: the default for installs that predate it.
 *
 * config_lidControlsSleep only counts when the settings database is created, so a device that
 * updates over the air keeps LID_BEHAVIOR = 0. No screen offers this setting, so that 0 is not
 * a choice anyone made. Change it once, and leave the setting alone from then on.
 */
package com.huawei.gaokun3.parts;

import android.content.ContentResolver;
import android.content.Context;
import android.content.SharedPreferences;
import android.provider.Settings;

final class LidBehaviorDefault {

    private static final String PREFS = "lid_behavior";
    private static final String KEY_APPLIED = "default_applied";

    /** WindowManagerPolicy.WindowManagerFuncs: LID_BEHAVIOR_NONE and LID_BEHAVIOR_SLEEP. */
    private static final int LID_BEHAVIOR_NONE = 0;
    private static final int LID_BEHAVIOR_SLEEP = 1;

    private LidBehaviorDefault() {}

    static void apply(Context context) {
        // Device-protected: BootReceiver also runs before the first unlock.
        SharedPreferences prefs = context.createDeviceProtectedStorageContext()
                .getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        if (prefs.getBoolean(KEY_APPLIED, false)) {
            return;
        }
        ContentResolver resolver = context.getContentResolver();
        boolean ignored = Settings.Global.getInt(resolver, Settings.Global.LID_BEHAVIOR,
                LID_BEHAVIOR_NONE) == LID_BEHAVIOR_NONE;
        if (ignored && !Settings.Global.putInt(resolver, Settings.Global.LID_BEHAVIOR,
                LID_BEHAVIOR_SLEEP)) {
            return; // Try again at the next boot.
        }
        prefs.edit().putBoolean(KEY_APPLIED, true).apply();
    }
}
