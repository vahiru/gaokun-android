/*
 * 手写笔（M-Pencil）的两条通知与电量小窗，状态来自 gk3pend（stylus/gk3pend.c 的 pen_state）设的属性
 * （vendor_gaokun3_prop，sepolicy 给了 system_app 读）：
 *   · vendor.gaokun3.pen_unpaired = 1：有笔在屏幕上书写，却收不到它的压感（未与本机配对的笔；
 *     已配对的另一支笔放下约 20 秒、停发压感之后，换用未配对的笔也算）⇒ 提示把笔吸附到充电区域
 *     完成配对；回到 0 时撤下。
 *   · vendor.gaokun3.pen_paired = "<毫秒>,<代数>"：每完成一次配对换一个值 ⇒ 撤下未配对提示，弹出电量小窗
 *     （StylusBatteryPopup，型号取值里的代数）。
 *   · vendor.gaokun3.pen_docked = "<毫秒>"：每次把已连接的笔吸附到充电区域换一个值 ⇒ 弹出电量小窗（型号取
 *     vendor.gaokun3.pen_conn 的代数）。新笔吸附时笔 MCU 走配对流程，不发吸附事件，由上一条在配对完成时弹出。
 *     两者的值都是 gk3pend 记下的开机以来毫秒数（CLOCK_BOOTTIME，含休眠时间），与
 *     SystemClock.elapsedRealtime() 同源：只在值是最近 FRESH_MS 内的事件时弹出。息屏吸附后系统休眠、
 *     很久以后才亮屏读到，或服务重新拉起时读到旧值，都不会补弹；单调时钟在休眠时停走，做不到这一点。
 *   · vendor.gaokun3.pen_nib_loose = "<毫秒>"：书写时笔尖信号弱、笔身信号如常（笔尖松动）⇒ 提示拧紧笔尖；
 *     回到 0（恢复正常书写）时撤下。同一个值只提示一次（记在设备加密存储里，服务被杀后由 START_STICKY
 *     拉起、或框架重启后重新拉起都不会重复弹），用户划掉后不会再弹，
 *     直到 gk3pend 判定恢复后又一次松动；gk3pend 重启时属性回到 0（提示撤下），笔尖仍松动时再写 8 笔会以新值重新提示。
 *
 * 轮询而不是属性回调，理由同 UsbPortNotifier 顶部：亮屏期间每 POLL_MS 读一次（读属性是本进程内的
 * 共享内存读），电量小窗要在吸附后立刻出现，所以间隔取 0.5 秒；息屏不读，亮屏时立刻读一次；
 * 开机由 BootReceiver 拉起。
 */
package com.huawei.gaokun3.parts;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.util.Log;

public class StylusNotifier extends Service {

    private static final String TAG = "Gaokun3Stylus";

    static final String PROP_UNPAIRED = "vendor.gaokun3.pen_unpaired";
    static final String PROP_PAIRED = "vendor.gaokun3.pen_paired";
    static final String PROP_DOCKED = "vendor.gaokun3.pen_docked";
    static final String PROP_CONN = "vendor.gaokun3.pen_conn";
    static final String PROP_NIB = "vendor.gaokun3.pen_nib_loose";

    private static final String CHANNEL = "stylus";
    private static final int ID_UNPAIRED = 21;
    private static final int ID_NIB = 23;

    private static final String PREFS = "stylus_notifier";
    private static final String KEY_NIB_SEEN = "nib_seen";

    private static final long POLL_MS = 500;
    private static final long FRESH_MS = 3000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private NotificationManager mNm;
    private SharedPreferences mPrefs;
    private boolean mScreenOn;
    private boolean mShownUnpaired;
    private String mNib;    // 上次读到的 pen_nib_loose；null 表示还没读过
    private String mPaired = "";    // 上次读到的 pen_paired
    private String mDocked = "";    // 上次读到的 pen_docked
    private StylusBatteryPopup mPopup;

    private final Runnable mPoll = new Runnable() {
        @Override
        public void run() {
            refresh();
            mHandler.removeCallbacks(this);
            if (mScreenOn) {
                mHandler.postDelayed(this, POLL_MS);
            }
        }
    };

    private final BroadcastReceiver mScreenReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            mScreenOn = !Intent.ACTION_SCREEN_OFF.equals(intent.getAction());
            mHandler.removeCallbacks(mPoll);
            if (mScreenOn) {
                mHandler.post(mPoll);
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        mNm = getSystemService(NotificationManager.class);
        mPrefs = createDeviceProtectedStorageContext().getSharedPreferences(PREFS, MODE_PRIVATE);
        mPopup = new StylusBatteryPopup(this);
        // 被杀后重新拉起时 mShownUnpaired 是 false：先撤掉可能留下的提示，下面轮询再按属性重新发。
        mNm.cancel(ID_UNPAIRED);
        mNm.createNotificationChannel(new NotificationChannel(CHANNEL,
                getString(R.string.stylus_channel), NotificationManager.IMPORTANCE_DEFAULT));

        IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_SCREEN_ON);
        f.addAction(Intent.ACTION_SCREEN_OFF);
        f.addAction(Intent.ACTION_USER_PRESENT);
        registerReceiver(mScreenReceiver, f, Context.RECEIVER_NOT_EXPORTED);

        mScreenOn = getSystemService(PowerManager.class).isInteractive();
        mHandler.post(mPoll);
        Log.i(TAG, "started, screenOn=" + mScreenOn);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        mHandler.removeCallbacks(mPoll);
        mPopup.hide();
        unregisterReceiver(mScreenReceiver);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void refresh() {
        String paired = SystemProperties.get(PROP_PAIRED, "");
        if (!paired.equals(mPaired)) {
            mPaired = paired;
            int comma = paired.indexOf(',');
            if (comma > 0) {
                mNm.cancel(ID_UNPAIRED);    // 配对完成：未配对提示不再成立
                mShownUnpaired = false;
                if (recent(paired.substring(0, comma))) {
                    mPopup.show(model(paired.substring(comma + 1).trim()));
                }
            }
            Log.i(TAG, PROP_PAIRED + "=" + paired);
        }

        String docked = SystemProperties.get(PROP_DOCKED, "");
        if (!docked.equals(mDocked)) {
            mDocked = docked;
            if (recent(docked)) {
                mPopup.show(model(SystemProperties.get(PROP_CONN, "")));
            }
            Log.i(TAG, PROP_DOCKED + "=" + docked);
        }

        boolean unpaired = "1".equals(SystemProperties.get(PROP_UNPAIRED, ""));
        if (unpaired != mShownUnpaired) {
            if (unpaired) {
                postUnpaired();
            } else {
                mNm.cancel(ID_UNPAIRED);
            }
            mShownUnpaired = unpaired;
            Log.i(TAG, PROP_UNPAIRED + "=" + (unpaired ? 1 : 0));
        }

        String nib = SystemProperties.get(PROP_NIB, "");
        if (!nib.equals(mNib)) {
            mNib = nib;
            if (nib.isEmpty() || "0".equals(nib)) {
                mNm.cancel(ID_NIB);
            } else if (!nib.equals(mPrefs.getString(KEY_NIB_SEEN, ""))) {
                mPrefs.edit().putString(KEY_NIB_SEEN, nib).commit();   // 先记下再发，进程随后被杀也不会重复
                postNib();
            }
            Log.i(TAG, PROP_NIB + "=" + nib);
        }
    }

    /** 笔的名称：代数为 2、3 时写出代数，认不出（0、1 或缺省）时只写 M-Pencil。 */
    private static String model(String gen) {
        return "2".equals(gen) || "3".equals(gen) ? "M-Pencil " + gen : "M-Pencil";
    }

    /** ms 是 gk3pend 记下的 CLOCK_BOOTTIME 毫秒数：事件发生在最近 FRESH_MS 内时为真。 */
    private static boolean recent(String ms) {
        try {
            long age = SystemClock.elapsedRealtime() - Long.parseLong(ms.trim());
            return age >= 0 && age <= FRESH_MS;
        } catch (NumberFormatException e) {
            return false;
        }
    }

    private void postUnpaired() {
        String text = getString(R.string.stylus_unpaired_text);
        mNm.notify(ID_UNPAIRED, new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_stylus)
                .setContentTitle(getString(R.string.stylus_unpaired_title))
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setCategory(Notification.CATEGORY_STATUS)
                .setOnlyAlertOnce(true)
                .setShowWhen(true)
                .build());
    }

    private void postNib() {
        String text = getString(R.string.stylus_nib_text);
        mNm.notify(ID_NIB, new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_stylus)
                .setContentTitle(getString(R.string.stylus_nib_title))
                .setContentText(text)
                .setStyle(new Notification.BigTextStyle().bigText(text))
                .setCategory(Notification.CATEGORY_STATUS)
                .setShowWhen(true)
                .build());
    }
}
