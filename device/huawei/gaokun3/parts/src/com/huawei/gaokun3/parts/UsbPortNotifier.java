/*
 * USB-C口（port0，靠近电源键的那个）的反向供电提示。
 *
 * 只看一个属性vendor.gaokun3.usbrole.reversed，由USB端口HAL（usb/Follow.h）设（vendor_gaokun3_prop，
 * sepolicy给了system_app读）：= 1表示平板在给对端供电、对端不支持PD、约6秒没有数据连接（插Mac时偶发，
 * 拔插一次就好；内核换不了供电方向，ucsi_pr_swap要PD）。条件不成立或拔线即清0，通知随之撤下。
 * 同一组的vendor.gaokun3.usbrole.broken（切到host后xhci没有起来，由HAL设）不再通知：
 * 它针对的待机后port0失灵已由vreg_l9d常开（patches/kernel/dts/0014）解决。HAL读它，避免在host没起来时
 * 误报反向供电。
 *
 * ★ 为什么是轮询而不是属性回调：SystemProperties.addChangeCallback只在有人调reportSyspropChanged()
 *   时才回调（frameworks/base core/java/android/os/SystemProperties.java），init / vendor进程setprop不会触发它。所以：
 *   · 亮屏期间每3秒读一次属性（读属性是本进程内的共享内存读，不走binder、不fork）；
 *   · 息屏时不轮询，亮屏广播一来立刻读一次，用户看到屏幕时通知就在；
 *   · 开机由BootReceiver拉起本服务（START_STICKY，被杀了系统会再拉起）。
 * ★ 本应用是android.uid.system：后台起服务不受限制（与system_server同一个uid，UidRecord不会idle，
 *   ActivityManagerService.getAppStartModeLOSP）；发通知的权限检查走checkComponentPermission，system uid一律放行。
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
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemProperties;
import android.util.Log;

public class UsbPortNotifier extends Service {

    private static final String TAG = "Gaokun3UsbPort";

    static final String PROP_REVERSED = "vendor.gaokun3.usbrole.reversed";

    private static final String CHANNEL = "usb_port";
    private static final int ID_REVERSED = 2;

    /** 亮屏时的轮询间隔。属性本身要约6秒才置位，3秒的延迟用户感觉不到。 */
    private static final long POLL_MS = 3000;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private NotificationManager mNm;
    private boolean mScreenOn;
    private boolean mShownReversed;

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
                mHandler.post(mPoll);   // 亮屏 / 解锁：立刻读一次，再接着轮询
            }
        }
    };

    @Override
    public void onCreate() {
        super.onCreate();
        mNm = getSystemService(NotificationManager.class);
        // 进程被杀后经START_STICKY拉起时mShownReversed是false：属性若已清0，旧通知就不会被取消 ⇒ 先清一遍，
        // 下面轮询再按属性重新发出。
        mNm.cancel(ID_REVERSED);
        NotificationChannel ch = new NotificationChannel(CHANNEL,
                getString(R.string.usb_port_channel), NotificationManager.IMPORTANCE_DEFAULT);
        ch.setDescription(getString(R.string.usb_port_channel_desc));
        mNm.createNotificationChannel(ch);

        IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_SCREEN_ON);
        f.addAction(Intent.ACTION_SCREEN_OFF);
        f.addAction(Intent.ACTION_USER_PRESENT);
        // 这三个都是系统发的受保护广播；NOT_EXPORTED 照样收得到系统 uid 发来的广播。
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
        boolean reversed = "1".equals(SystemProperties.get(PROP_REVERSED, ""));

        if (reversed != mShownReversed) {
            if (reversed) {
                post(ID_REVERSED, R.string.usb_port_reversed_title, R.string.usb_port_reversed_text);
            } else {
                mNm.cancel(ID_REVERSED);   // 重插 / 拔线后 HAL 清 0，通知跟着消失
            }
            mShownReversed = reversed;
            Log.i(TAG, PROP_REVERSED + "=" + (reversed ? 1 : 0));
        }
    }

    private void post(int id, int title, int text) {
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_usb)
                .setContentTitle(getString(title))
                .setContentText(getString(text))
                .setStyle(new Notification.BigTextStyle().bigText(getString(text)))
                .setCategory(Notification.CATEGORY_STATUS)
                .setOnlyAlertOnce(true)
                .setShowWhen(true)
                .build();
        mNm.notify(id, n);
    }
}
