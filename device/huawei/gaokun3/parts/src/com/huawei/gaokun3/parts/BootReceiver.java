/*
 * 开机后拉起两个后台服务：
 *   · UsbPortNotifier（v1.0 PWR-4 / USB-1）：USB-C 口的提示；
 *   · BootEntryNotifier（2026-10-05，统一启动入口 S9）：入口回落 / 绕过等通知；
 *   · KeyboardReplugWatcher（2026-10-05，v1.0 DISP-15）：键盘开关关着时，拔插键盘盖后再关一次。
 *
 * ★ 2026-10-05：改成 directBootAware + 同时收 LOCKED_BOOT_COMPLETED（TODO 里记过的小改进）。
 *   BOOT_COMPLETED 要等用户第一次解锁才发；有锁屏密码时，开机到解锁之间 Parts 什么都看不到
 *   （1.0.0-dev 验收时 UsbPortNotifier 就是这么"没起来"的）。LOCKED_BOOT_COMPLETED 在用户启动、
 *   还没解锁时就发；两个服务都只读属性、只发通知、只用设备加密存储，解锁前跑没有问题。
 *   解锁后的 BOOT_COMPLETED 照样会来：startService 对已在跑的服务只是多一次 onStartCommand，
 *   BootEntryNotifier 另有按开机令牌去重，不会重复弹。
 * 本应用是 android.uid.system，后台 startService 不受限（见 UsbPortNotifier 顶部的注释）。
 */
package com.huawei.gaokun3.parts;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        if (!Intent.ACTION_LOCKED_BOOT_COMPLETED.equals(action)
                && !Intent.ACTION_BOOT_COMPLETED.equals(action)) {
            return;
        }
        LidBehaviorDefault.apply(context);
        context.startService(new Intent(context, UsbPortNotifier.class));
        context.startService(new Intent(context, BootEntryNotifier.class));
        context.startService(new Intent(context, KeyboardReplugWatcher.class));
    }
}
