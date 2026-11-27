/*
 * 手写笔（M-Pencil）的两条通知，状态来自 gk3pend（stylus/gk3pend.c 的 pen_state）设的两个属性
 * （vendor_gaokun3_prop，sepolicy 给了 system_app 读）：
 *   · vendor.gaokun3.pen_unpaired = 1：有笔在屏幕上书写，却收不到它的压感（未与本机配对的笔；
 *     已配对的另一支笔放下约 20 秒、停发压感之后，换用未配对的笔也算）⇒ 提示把笔吸附到充电区域
 *     完成配对；回到 0 时撤下。
 *   · vendor.gaokun3.pen_paired = "<毫秒>,<代数>"：每完成一次配对换一个值 ⇒ 发“已连接到 M-Pencil 2/3”，
 *     点按打开“手写笔设置”（StylusSettingsActivity）。同一个值只通知一次：记在设备加密存储里，
 *     服务被杀后由 START_STICKY 拉起、或框架重启后重新拉起都不会重复弹。值里是 gk3pend 的单调时钟毫秒数，
 *     属性不持久（每次开机从空开始），所以单凭值就能区分每一次配对；不能用 Settings.Global.BOOT_COUNT
 *     区分开机，它在框架每次重启时都会加一，而 vendor 属性在框架重启后还在。
 *
 * 轮询而不是属性回调，理由同 UsbPortNotifier 顶部：亮屏期间每 3 秒读一次，息屏不读，亮屏时立刻读一次；
 * 开机由 BootReceiver 拉起。
 */
package com.huawei.gaokun3.parts;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
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
import android.os.SystemProperties;
import android.util.Log;

public class StylusNotifier extends Service {

    private static final String TAG = "Gaokun3Stylus";

    static final String PROP_UNPAIRED = "vendor.gaokun3.pen_unpaired";
    static final String PROP_PAIRED = "vendor.gaokun3.pen_paired";

    private static final String CHANNEL = "stylus";
    private static final int ID_UNPAIRED = 21;
    private static final int ID_PAIRED = 22;

    private static final String PREFS = "stylus_notifier";
    private static final String KEY_PAIRED_SEEN = "paired_seen";

    private static final long POLL_MS = 3000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private NotificationManager mNm;
    private SharedPreferences mPrefs;
    private boolean mScreenOn;
    private boolean mShownUnpaired;

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
        unregisterReceiver(mScreenReceiver);
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void refresh() {
        String paired = SystemProperties.get(PROP_PAIRED, "");
        if (!paired.isEmpty() && !paired.equals(mPrefs.getString(KEY_PAIRED_SEEN, ""))) {
            mPrefs.edit().putString(KEY_PAIRED_SEEN, paired).commit();   // 先记下再发，进程随后被杀也不会重复
            postPaired(model(paired));
            Log.i(TAG, PROP_PAIRED + "=" + paired);
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
    }

    /** "<毫秒>,<代数>"里的代数：2、3，认不出（0 或缺省）时不写代数。 */
    private static String model(String paired) {
        int comma = paired.indexOf(',');
        String gen = comma >= 0 ? paired.substring(comma + 1).trim() : "";
        return "2".equals(gen) || "3".equals(gen) ? "M-Pencil " + gen : "M-Pencil";
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

    private void postPaired(String model) {
        mNm.cancel(ID_UNPAIRED);
        mShownUnpaired = false;
        Intent open = new Intent(this, StylusSettingsActivity.class)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        mNm.notify(ID_PAIRED, new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_stylus)
                .setContentTitle(getString(R.string.stylus_paired_title, model))
                .setContentText(getString(R.string.stylus_paired_text, model))
                .setContentIntent(pi)
                .setAutoCancel(true)
                .setCategory(Notification.CATEGORY_STATUS)
                .setShowWhen(true)
                .build());
    }
}
