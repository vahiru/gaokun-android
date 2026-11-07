/*
 * 磁吸键盘"关掉"之后，拔下再插回还得是关着的（v1.0 DISP-15 / HW-11，2026-10-05）。
 *
 * ★ "关掉键盘"指的是 Parts 的键盘开关（设置 → 系统 → 磁吸键盘，或控制中心的磁贴）：
 *   设 persist.sys.gaokun3.keyboard=0 → etc/keyboard.rc → /vendor/bin/gaokun3-keyboard.sh off，
 *   把名字以 "HID 12d1:10b8" 开头的 input 设备（键盘 + 触控板，共 6–7 个）的
 *   /sys/class/input/inputN/inhibited 写成 1（docs/dev/hardware.md）。
 * ⚠️ 缺陷：inhibited 是【每个 input 设备】的状态。键盘盖拔下再插回，内核新建的是一组新的 input 设备，
 *   inhibited 默认 0 ⇒ 开关还显示"关"，键盘却又能用了。属性没变，init 的触发器不会再跑。
 *
 * 修法：本服务常驻（BootReceiver 拉起，START_STICKY），用 InputManager.InputDeviceListener
 *   （frameworks/base core/java/android/hardware/input/InputManager.java:430、:1695-1702，crDroid 16.0）
 *   看新加进来的设备；是这把键盘、且开关是关着的，就设 sys.gaokun3.keyboard.replug=<时间戳>，
 *   keyboard.rc 里 `on property:sys.gaokun3.keyboard.replug=* && property:persist.sys.gaokun3.keyboard=0`
 *   再跑一次 gaokun3-keyboard.sh off。
 *   · 为什么还经 init 而不是自己写 sysfs：理由同 KeyboardSettingsActivity 顶部（应用不该有写
 *     /sys/class/input 的权限；sys.* 是 system_prop，system_app 可写：refs/lineage-sepolicy/private/system_app.te:43）。
 *   · 为什么要去抖：一次插入会连着加 6–7 个设备，gaokun3_kbd_off 是 oneshot，跑着的时候再 start 是空操作
 *     ⇒ 最后一个设备加进来之后再等 DEBOUNCE_MS 才设一次属性，脚本一次把所有设备都处理掉。
 *     代价：插回之后的这 1 秒多键盘是能用的。
 *   · 值用 elapsedRealtime：每次都不一样，属性一定"变了"、触发器一定会跑。
 *
 * 判据（⬜ 未编译、未上机）：开关关掉 → 拔下键盘盖 → 插回 → 约 2 秒内
 *   `cat /sys/class/input/input*\/inhibited`（名字 HID 12d1:10b8 的那几个）全是 1、键盘与触控板没反应；
 *   logcat -s gaokun3-keyboard Gaokun3KbdReplug 里先有本类的"新设备 … 再关一次"，再有脚本的"键盘已 off"。
 *   开关开着时拔插，什么都不做（logcat 里没有本类的行）。
 */
package com.huawei.gaokun3.parts;

import android.app.Service;
import android.content.Intent;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.util.Log;
import android.view.InputDevice;

public class KeyboardReplugWatcher extends Service implements InputManager.InputDeviceListener {

    private static final String TAG = "Gaokun3KbdReplug";

    /** 与 KeyboardSettingsActivity / KeyboardTileService 同一个开关属性。 */
    private static final String PROP_SWITCH = "persist.sys.gaokun3.keyboard";
    /** etc/keyboard.rc 盯着的触发属性。 */
    private static final String PROP_REPLUG = "sys.gaokun3.keyboard.replug";

    /** 华为这把键盘盖：USB 12d1:10b8（实测 input 设备名全部以 "HID 12d1:10b8" 开头，#65）。 */
    private static final int VENDOR = 0x12d1;
    private static final int PRODUCT = 0x10b8;
    private static final String NAME_PREFIX = "HID 12d1:10b8";

    private static final long DEBOUNCE_MS = 1500;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private InputManager mIm;

    private final Runnable mReapply = new Runnable() {
        @Override
        public void run() {
            // 等的这 1.5 秒里用户可能刚把开关打开 —— 以触发那一刻为准
            if (!isSwitchedOff()) {
                return;
            }
            Log.i(TAG, "键盘重新接上、开关是关着的 ⇒ 再关一次");
            SystemProperties.set(PROP_REPLUG, Long.toString(SystemClock.elapsedRealtime()));
        }
    };

    private static boolean isSwitchedOff() {
        // 与设置页同一个判定：属性没设过 = 键盘开着；只有显式的 "0" 才是关
        return "0".equals(SystemProperties.get(PROP_SWITCH, "1"));
    }

    private static boolean isOurKeyboard(InputDevice d) {
        if (d == null) {
            return false;
        }
        if (d.getVendorId() == VENDOR && d.getProductId() == PRODUCT) {
            return true;
        }
        String name = d.getName();
        return name != null && name.startsWith(NAME_PREFIX);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mIm = getSystemService(InputManager.class);
        if (mIm == null) {
            Log.w(TAG, "拿不到 InputManager，不看键盘拔插");
            return;
        }
        mIm.registerInputDeviceListener(this, mHandler);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        mHandler.removeCallbacks(mReapply);
        if (mIm != null) {
            mIm.unregisterInputDeviceListener(this);
        }
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        if (!isSwitchedOff()) {
            return;
        }
        InputDevice d = mIm.getInputDevice(deviceId);
        if (!isOurKeyboard(d)) {
            return;
        }
        Log.i(TAG, "新设备 " + deviceId + "（" + d.getName() + "），" + DEBOUNCE_MS + " ms 后再关一次");
        mHandler.removeCallbacks(mReapply);
        mHandler.postDelayed(mReapply, DEBOUNCE_MS);
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
    }
}
