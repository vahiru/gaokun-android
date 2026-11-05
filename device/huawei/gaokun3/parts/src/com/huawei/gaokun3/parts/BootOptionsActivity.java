/*
 * 启动选项（统一启动入口，tools/gk3boot/README.md）。
 *
 * 三项，按 boot_control HAL（device/huawei/gaokun3/boot_control/Gk3Boot.cpp）开机完成时导出的属性决定显示与否：
 *   · "重启到 Windows"   vendor.gaokun3.bootentry.windows=1（ESP 上有 EFI/Microsoft/Boot/bootmgfw.efi）。
 *       点了 ⇒ 请求 next_windows（HAL 把一次性意图写进 misc 的 GK3 记录）⇒ 等回执 ⇒ 普通重启。真正转去 Windows 的是
 *       下一次开机的入口 gk3boot（写 LoaderEntryOneShot 再复位，所以会看到两次开机画面）。Android 不碰 efivarfs（U14 选 a）。
 *   · "开机默认进入"     同上条件。改了 ⇒ 请求 default_windows / default_android，下次开机由入口改写 LoaderEntryDefault。
 *       显示的当前值：有没应用的请求就显示请求（vendor.gaokun3.bootentry.default_pending），否则显示入口记下的缓存
 *       （vendor.gaokun3.bootentry.default）。
 *   · "重启到引导菜单"   vendor.gaokun3.bootentry.menu=1（这次经入口开机、分派开着、执行端在 ESP 上 —— 否则 BCB 只被记录、
 *       不执行，点了只会重启回 Android）。走标准的 reboot,bootloader（init 写 bootonce-bootloader，入口据此进执行端菜单，
 *       那里有 "Other systems"）。
 *
 * ★ 请求怎么到 HAL：本应用是 system_app（coredomain），写不了任何 vendor 属性（sepolicy/vendor_gaokun3_props.te 顶上），
 *   所以设 system_prop 的 sys.gaokun3.bootreq，由 HAL 的 rc 在 vendor_init 里转成 vendor.gaokun3.bootentry.request + ring，
 *   HAL 回 vendor.gaokun3.bootentry.ack = <请求>:<ok|error:原因>:<序号>。先设 none 再设目标值：同一个请求连按两次也会再触发。
 * ★ 页面本身由 BootEntryNotifier 按同样的属性启用 / 停用（manifest 里缺省停用），没有任何一项可用时设置里看不到它。
 * ★ 独立的类、独立的 xml / 字符串文件（res/values*\/strings_boot_options.xml），不动别的页面。
 *
 * ⬜ 未编译、未上机（双系统没有真机样本：要群友的双系统盘 D5 / D6 或 Parallels D4 之后的那套）。
 * 判据：adb root 后 setprop vendor.gaokun3.bootentry.windows 1 → 设置 → 系统 → 启动选项出现两项；
 *   点"重启到 Windows" → logcat -b kernel 里 HAL 打 "request next_windows written to the GK3 record" → 重启。
 */
package com.huawei.gaokun3.parts;

import android.app.AlertDialog;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.text.TextUtils;
import android.util.Log;
import android.widget.Toast;

import androidx.preference.ListPreference;
import androidx.preference.Preference;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

public class BootOptionsActivity extends CollapsingToolbarBaseActivity {

    static final String TAG = "Gaokun3BootOptions";

    static final String PROP_WINDOWS = "vendor.gaokun3.bootentry.windows";
    static final String PROP_MENU = "vendor.gaokun3.bootentry.menu";
    static final String PROP_DEFAULT = "vendor.gaokun3.bootentry.default";
    static final String PROP_DEFAULT_PENDING = "vendor.gaokun3.bootentry.default_pending";
    static final String PROP_ACK = "vendor.gaokun3.bootentry.ack";
    /** system_prop：HAL 的 rc 只认 next_windows / default_windows / default_android 三个字面值 */
    static final String PROP_REQUEST = "sys.gaokun3.bootreq";

    /** 有没有任何一项可用（BootEntryNotifier 据此启用 / 停用本页） */
    static boolean anyAvailable() {
        return "1".equals(SystemProperties.get(PROP_WINDOWS, "")) || "1".equals(SystemProperties.get(PROP_MENU, ""));
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState == null) {
            getSupportFragmentManager().beginTransaction()
                    .replace(com.android.settingslib.collapsingtoolbar.R.id.content_frame, new BootOptionsFragment())
                    .commit();
        }
    }

    public static class BootOptionsFragment extends SettingsBasePreferenceFragment {

        private static final long ACK_TIMEOUT_MS = 5000;
        private static final long ACK_POLL_MS = 100;

        private final Handler mHandler = new Handler(Looper.getMainLooper());
        private Preference mWindows;
        private ListPreference mDefault;
        private Preference mMenu;
        private boolean mBusy;

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.boot_options_prefs, rootKey);
            mWindows = findPreference("boot_to_windows");
            mDefault = findPreference("boot_default_os");
            mMenu = findPreference("boot_to_menu");

            mWindows.setOnPreferenceClickListener(p -> {
                confirm(R.string.boot_options_windows_confirm, () -> request("next_windows", true));
                return true;
            });
            mDefault.setOnPreferenceChangeListener((p, v) -> {
                String want = "windows".equals(v) ? "default_windows" : "default_android";
                request(want, false);
                return false;   // 等 HAL 回执成功再改显示（refresh）
            });
            mMenu.setOnPreferenceClickListener(p -> {
                confirm(R.string.boot_options_menu_confirm, () -> reboot("bootloader"));
                return true;
            });
        }

        @Override
        public void onResume() {
            super.onResume();
            refresh();
        }

        @Override
        public void onDestroy() {
            mHandler.removeCallbacksAndMessages(null);
            super.onDestroy();
        }

        private void refresh() {
            boolean win = "1".equals(SystemProperties.get(PROP_WINDOWS, ""));
            boolean menu = "1".equals(SystemProperties.get(PROP_MENU, ""));
            mWindows.setVisible(win);
            mDefault.setVisible(win);
            mMenu.setVisible(menu);
            findPreference("boot_options_none").setVisible(!win && !menu);
            String pending = SystemProperties.get(PROP_DEFAULT_PENDING, "");
            String cur = !TextUtils.isEmpty(pending) ? pending : SystemProperties.get(PROP_DEFAULT, "");
            boolean known = "windows".equals(cur) || "android".equals(cur);
            if (known) {
                mDefault.setValue(cur);
            }
            String label = "windows".equals(cur) ? getString(R.string.boot_options_os_windows)
                    : "android".equals(cur) ? getString(R.string.boot_options_os_android)
                    : getString(R.string.boot_options_os_unknown);
            mDefault.setSummary(!TextUtils.isEmpty(pending)
                    ? getString(R.string.boot_options_default_pending, label) : label);
        }

        private void confirm(int message, Runnable yes) {
            new AlertDialog.Builder(requireContext())
                    .setMessage(message)
                    .setPositiveButton(android.R.string.ok, (d, w) -> yes.run())
                    .setNegativeButton(android.R.string.cancel, null)
                    .show();
        }

        /** 发请求、等回执；thenReboot = 成功后普通重启（"重启到 Windows"） */
        private void request(String req, boolean thenReboot) {
            if (mBusy) {
                return;
            }
            mBusy = true;
            final String before = SystemProperties.get(PROP_ACK, "");
            SystemProperties.set(PROP_REQUEST, "none");
            SystemProperties.set(PROP_REQUEST, req);
            final long t0 = SystemClock.elapsedRealtime();
            mHandler.post(new Runnable() {
                @Override
                public void run() {
                    String ack = SystemProperties.get(PROP_ACK, "");
                    if (!ack.equals(before) && ack.startsWith(req + ":")) {
                        mBusy = false;
                        String rest = ack.substring(req.length() + 1);
                        int last = rest.lastIndexOf(':');
                        String result = last > 0 ? rest.substring(0, last) : rest;
                        Log.i(TAG, "request " + req + " -> " + result);
                        if ("ok".equals(result)) {
                            if (thenReboot) {
                                reboot(null);
                            } else {
                                toast(getString(R.string.boot_options_default_saved));
                                refresh();
                            }
                        } else {
                            toast(getString(R.string.boot_options_failed, result));
                        }
                    } else if (SystemClock.elapsedRealtime() - t0 > ACK_TIMEOUT_MS) {
                        mBusy = false;
                        Log.w(TAG, "request " + req + ": no ack from the boot HAL within 5 s");
                        toast(getString(R.string.boot_options_failed, "timeout"));
                    } else {
                        mHandler.postDelayed(this, ACK_POLL_MS);
                    }
                }
            });
        }

        private void reboot(String reason) {
            // 本应用是平台签名（Android.bp certificate: platform），REBOOT 是 signature 权限（manifest 里声明了）
            PowerManager pm = requireContext().getSystemService(PowerManager.class);
            Log.i(TAG, "reboot(" + reason + ")");
            pm.reboot(reason);
        }

        private void toast(String s) {
            Toast.makeText(requireContext(), s, Toast.LENGTH_LONG).show();
        }
    }
}
