/*
 * 注入设置的条目在设置一侧监听的URI。
 *
 * 设置给带开关或动态说明的注入项按content://<authority>/<method>/<key>注册ContentObserver
 * （SettingsLib TileUtils.buildUri）。属性在设置以外被改动时（控制中心磁贴、对话框），
 * 对相应URI调用notifyChange，正在显示的设置页才会重新读取开关状态或说明。
 */
package com.huawei.gaokun3.parts;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;

import com.android.settingslib.drawer.EntriesProvider;

final class SettingsEntryUris {

    private SettingsEntryUris() {
    }

    static Uri build(String authority, String method, String key) {
        return new Uri.Builder()
                .scheme(ContentResolver.SCHEME_CONTENT)
                .authority(authority)
                .appendPath(method)
                .appendPath(key)
                .build();
    }

    /** 通知设置重新读取开关状态。 */
    static void notifySwitchChanged(Context context, String authority, String key) {
        context.getContentResolver().notifyChange(
                build(authority, EntriesProvider.METHOD_IS_CHECKED, key), null);
    }

    /** 通知设置重新读取动态说明。 */
    static void notifySummaryChanged(Context context, String authority, String key) {
        context.getContentResolver().notifyChange(
                build(authority, EntriesProvider.METHOD_GET_DYNAMIC_SUMMARY, key), null);
    }
}
