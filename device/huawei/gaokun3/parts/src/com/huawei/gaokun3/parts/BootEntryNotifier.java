/*
 * 统一启动入口（gk3boot.efi）的通知（tools/gk3boot/README.md）。
 *
 * 数据全来自 boot_control HAL 的开机完成线程（device/huawei/gaokun3/boot_control/Gk3Boot.cpp）设的
 * vendor.gaokun3.bootentry.*（vendor_gaokun3_prop，sepolicy 给了 system_app 读）：
 *   .done      本次开机的令牌；HAL 最后写它 ⇒ 有值 = 其余几项都已就绪
 *   .notify    要通知的事件，逗号分隔（fallback / bcb_dropped / boot_corrupt / wipe_failed / …）。
 *              HAL 取的是 GK3 事件环里【没通知过】的那几条、并当场置"已通知" ⇒ 同一件事只会出现在一次开机里
 *   .bypassed  1 = 入口已部署、这次开机却没经过它
 *
 * ★ 开机后轮询 .done（每 2 秒，最多 10 分钟）而不是一直挂着：HAL 只在 sys.boot_completed=1 之后干一次活，
 *   做完就不会再变；属性回调又收不到 vendor 侧 setprop（UsbPortNotifier 顶部有出处）。
 * ★ 去重：处理过的令牌记在【设备加密】存储（createDeviceProtectedStorageContext）里 —— 本服务在锁屏解锁之前
 *   （LOCKED_BOOT_COMPLETED）就会被拉起，那时凭据加密的存储还打不开；同一次开机里服务被杀重启、或解锁后
 *   BOOT_COMPLETED 再拉一次，都不会重复弹。
 *
 * ★ S15（双系统，2026-10-05）：intent_dropped / default_reset / loader_replaced 各有一条专门的通知；另外按
 *   vendor.gaokun3.bootentry.windows / .menu 启用或停用设置里的「启动选项」页（BootOptionsActivity）。
 *
 * 判据（⬜ 未编译、未上机）：
 *   · 人为造一条：adb root 后 setprop vendor.gaokun3.bootentry.notify fallback、再 setprop …done test1，
 *     然后 am startservice -n com.huawei.gaokun3.parts/.BootEntryNotifier → 通知栏出现"已自动退回旧版本"；
 *   · E8（真 OTA 回滚演练）回到旧槽后、锁屏解锁之前通知就在。
 */
package com.huawei.gaokun3.parts;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.text.TextUtils;
import android.util.Log;

public class BootEntryNotifier extends Service {

    private static final String TAG = "Gaokun3BootEntry";

    static final String PROP_DONE = "vendor.gaokun3.bootentry.done";
    static final String PROP_NOTIFY = "vendor.gaokun3.bootentry.notify";
    static final String PROP_BYPASSED = "vendor.gaokun3.bootentry.bypassed";

    private static final String CHANNEL = "boot_entry";
    private static final int ID_FALLBACK = 11;
    private static final int ID_BCB_DROPPED = 12;
    private static final int ID_OTHER = 13;
    private static final int ID_BYPASSED = 14;
    // S15 双系统
    private static final int ID_INTENT_DROPPED = 15;
    private static final int ID_DEFAULT_RESET = 16;
    private static final int ID_LOADER_REPLACED = 17;

    private static final String PREFS = "boot_entry";
    private static final String KEY_HANDLED = "handled_token";

    private static final long POLL_MS = 2000;
    /** HAL 在开机完成后几秒内就写 .done；10 分钟还没有 = HAL 没跑到那一步（老 vendor / 崩了），不再等。 */
    private static final long GIVE_UP_MS = 10 * 60 * 1000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mPolling;
    private long mStartedAt;

    private final Runnable mPoll = new Runnable() {
        @Override
        public void run() {
            String token = SystemProperties.get(PROP_DONE, "");
            if (!TextUtils.isEmpty(token)) {
                handle(token);
                finish();
            } else if (SystemClock.elapsedRealtime() - mStartedAt > GIVE_UP_MS) {
                Log.w(TAG, PROP_DONE + " still empty after 10 min; giving up for this boot");
                finish();
            } else {
                mHandler.postDelayed(this, POLL_MS);
            }
        }
    };

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (!mPolling) {
            mPolling = true;
            mStartedAt = SystemClock.elapsedRealtime();
            mHandler.post(mPoll);
        }
        // 被杀了不必重启：解锁后的 BOOT_COMPLETED 会再拉一次，令牌去重保证不重复弹
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        mHandler.removeCallbacks(mPoll);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void finish() {
        mPolling = false;
        stopSelf();
    }

    private SharedPreferences prefs() {
        // 设备加密存储：锁屏解锁之前就能读写（见文件头）
        return createDeviceProtectedStorageContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    private void handle(String token) {
        updateBootOptions();
        SharedPreferences p = prefs();
        if (token.equals(p.getString(KEY_HANDLED, ""))) {
            return;   // 这次开机已经处理过
        }
        String notify = SystemProperties.get(PROP_NOTIFY, "");
        boolean bypassed = "1".equals(SystemProperties.get(PROP_BYPASSED, ""));
        Log.i(TAG, "token=" + token + " notify=" + notify + " bypassed=" + bypassed);

        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel ch = new NotificationChannel(CHANNEL,
                getString(R.string.boot_entry_channel), NotificationManager.IMPORTANCE_DEFAULT);
        ch.setDescription(getString(R.string.boot_entry_channel_desc));
        nm.createNotificationChannel(ch);

        StringBuilder other = new StringBuilder();
        for (String ev : notify.split(",")) {
            ev = ev.trim();
            if (ev.isEmpty()) {
                continue;
            }
            if ("fallback".equals(ev)) {
                String slot = SystemProperties.get("ro.boot.slot_suffix", "");
                post(nm, ID_FALLBACK, getString(R.string.boot_entry_fallback_title),
                        getString(R.string.boot_entry_fallback_text, slot));
            } else if ("bcb_dropped".equals(ev)) {
                post(nm, ID_BCB_DROPPED, getString(R.string.boot_entry_bcb_dropped_title),
                        getString(R.string.boot_entry_bcb_dropped_text));
            } else if ("intent_dropped".equals(ev)) {   // S15：入口作废了"重启到 Windows" / 关机进 Windows / 设默认
                post(nm, ID_INTENT_DROPPED, getString(R.string.boot_entry_intent_dropped_title),
                        getString(R.string.boot_entry_intent_dropped_text));
            } else if ("default_reset".equals(ev)) {    // S15：不合法的 LoaderEntryDefault 被入口删了
                post(nm, ID_DEFAULT_RESET, getString(R.string.boot_entry_default_reset_title),
                        getString(R.string.boot_entry_default_reset_text));
            } else if ("loader_replaced".equals(ev)) {  // S15：Windows 换掉了回落路径上的 systemd-boot（只通知）
                post(nm, ID_LOADER_REPLACED, getString(R.string.boot_entry_loader_replaced_title),
                        getString(R.string.boot_entry_loader_replaced_text));
            } else {
                if (other.length() > 0) {
                    other.append(", ");
                }
                other.append(ev);
            }
        }
        if (other.length() > 0) {
            post(nm, ID_OTHER, getString(R.string.boot_entry_other_title),
                    getString(R.string.boot_entry_other_text, other.toString()));
        }
        if (bypassed) {
            post(nm, ID_BYPASSED, getString(R.string.boot_entry_bypassed_title),
                    getString(R.string.boot_entry_bypassed_text));
        }
        // commit 而不是 apply：紧接着就 stopSelf，进程可能随即被回收
        p.edit().putString(KEY_HANDLED, token).commit();
    }

    /**
     * S15：设置里的「启动选项」页（BootOptionsActivity，manifest 里缺省停用）只在 HAL 报了可用的项时启用。
     * 每次开机按这一次 HAL 的结果对齐一次（不受令牌去重影响：解锁后再拉一次也只是同一个结果）。
     */
    private void updateBootOptions() {
        boolean want = BootOptionsActivity.anyAvailable();
        int state = want ? PackageManager.COMPONENT_ENABLED_STATE_ENABLED
                : PackageManager.COMPONENT_ENABLED_STATE_DEFAULT;   // DEFAULT = manifest 里的 enabled="false"
        ComponentName cn = new ComponentName(this, BootOptionsActivity.class);
        PackageManager pm = getPackageManager();
        if (pm.getComponentEnabledSetting(cn) != state) {
            pm.setComponentEnabledSetting(cn, state, PackageManager.DONT_KILL_APP);
            Log.i(TAG, "BootOptionsActivity " + (want ? "enabled" : "disabled"));
        }
    }

    private void post(NotificationManager nm, int id, String title, String text) {
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(android.R.drawable.stat_sys_warning)
                .setContentTitle(title)
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setCategory(Notification.CATEGORY_SYSTEM)
                .setShowWhen(true)
                .build();
        nm.notify(id, n);
    }
}
