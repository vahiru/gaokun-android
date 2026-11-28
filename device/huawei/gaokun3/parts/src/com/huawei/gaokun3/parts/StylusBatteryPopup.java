/*
 * 笔吸附到充电区域、或配对完成时，在屏幕顶部正中弹出的电量小窗：第一行是笔的型号，第二行是电池图标与电量，
 * 约 3 秒后自动消失。样式参照华为设备吸附手写笔时弹出的电量窗口（笔 MCU 吸附时发出的 0x28，在华为电脑管家中
 * 就是“显示电量窗口”）。
 *
 * 不用 Toast：Toast 的自定义视图（setView）自 Android 11 起已弃用，文字 Toast 只有系统样式。Parts 以 system UID
 * 运行，可以添加 TYPE_SECURE_SYSTEM_OVERLAY 窗口：它在锁屏之上，锁屏时也照常显示（TYPE_APPLICATION_OVERLAY
 * 在锁屏之下，锁屏时会被隐藏，这次吸附也就看不到了）。窗口不可聚焦、不接收触摸，点按会落到下面的应用。
 * 电量取自触控笔输入设备的电池（gk3pend 写给驱动、驱动注册为 m-pencil 的 power_supply，由 EventHub 关联到
 * 触控笔）；驱动尚无电量时系统给出 -1%，此时只显示型号。颜色随主题（深浅色、动态取色）。
 */
package com.huawei.gaokun3.parts;

import android.content.Context;
import android.graphics.Insets;
import android.graphics.PixelFormat;
import android.hardware.BatteryState;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.view.ContextThemeWrapper;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.LayoutInflater;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.TextView;

import java.text.NumberFormat;

final class StylusBatteryPopup {

    private static final long SHOW_MS = 3000;

    private final Context mContext;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Runnable mHide = this::hide;
    private View mView;

    StylusBatteryPopup(Context context) {
        Display display = context.getSystemService(DisplayManager.class)
                .getDisplay(Display.DEFAULT_DISPLAY);
        mContext = context.createDisplayContext(display)
                .createWindowContext(WindowManager.LayoutParams.TYPE_SECURE_SYSTEM_OVERLAY, null);
    }

    /** 弹出（已在显示时先移除再重新弹出）。model 为“M-Pencil 2”之类的名称。 */
    void show(String model) {
        WindowManager wm = mContext.getSystemService(WindowManager.class);
        hide();
        // 每次重新加载布局，深浅色切换后也用当时的主题
        Context themed = new ContextThemeWrapper(mContext, R.style.Theme_Material3_DynamicColors_DayNight);
        View v = LayoutInflater.from(themed).inflate(R.layout.stylus_battery_popup, null);
        ((TextView) v.findViewById(R.id.stylus_popup_model)).setText(model);
        float level = stylusBattery();
        if (Float.isNaN(level)) {
            v.findViewById(R.id.stylus_popup_battery).setVisibility(View.GONE);
        } else {
            ((BatteryLevelView) v.findViewById(R.id.stylus_popup_icon)).setLevel(level);
            ((TextView) v.findViewById(R.id.stylus_popup_level))
                    .setText(NumberFormat.getPercentInstance().format(level));
        }

        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT, WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_SECURE_SYSTEM_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.CENTER_HORIZONTAL;
        Insets bars = wm.getCurrentWindowMetrics().getWindowInsets()
                .getInsets(WindowInsets.Type.statusBars());
        lp.y = bars.top;
        lp.windowAnimations = android.R.style.Animation_Toast;
        lp.setTitle("StylusBatteryPopup");
        wm.addView(v, lp);
        mView = v;
        mHandler.postDelayed(mHide, SHOW_MS);
    }

    void hide() {
        mHandler.removeCallbacks(mHide);
        if (mView != null) {
            mContext.getSystemService(WindowManager.class).removeView(mView);
            mView = null;
        }
    }

    /** 触控笔的电量（0 到 1），无法读取时为 NaN。 */
    private float stylusBattery() {
        InputManager im = mContext.getSystemService(InputManager.class);
        for (int id : im.getInputDeviceIds()) {
            InputDevice d = im.getInputDevice(id);
            if (d == null || !d.supportsSource(InputDevice.SOURCE_STYLUS)) {
                continue;
            }
            BatteryState b = d.getBatteryState();
            float c = b.getCapacity();
            if (b.isPresent() && c >= 0f && c <= 1f) {     // 未知时为 -0.01（-1%）或 NaN
                return c;
            }
        }
        return Float.NaN;
    }
}
