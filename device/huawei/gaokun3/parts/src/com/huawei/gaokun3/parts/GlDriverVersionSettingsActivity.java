/*
 * GPU驱动兼容（设置 → 系统）：为选定的应用在GL_VERSION中报告高通驱动版本。
 *
 * 名单保存在Settings.Global的gaokun3_angle_gl_version_pkgs（逗号分隔的包名），默认为空。
 * patches/android/frameworks-base/0002经CoreSettingsObserver把它传给应用进程，
 * GraphicsEnvironment在应用启动时为名单中的应用设置ANGLE_GL_VERSION，原因见该补丁。
 *
 * 只列出带启动器图标的非系统应用。名单中已卸载应用的条目原样保留，重新安装后仍然生效。
 * 更改对之后启动的应用进程生效，已在运行的进程不受影响。
 */
package com.huawei.gaokun3.parts;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.text.TextUtils;

import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.SwitchPreferenceCompat;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

import java.text.Collator;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class GlDriverVersionSettingsActivity extends CollapsingToolbarBaseActivity {

    /** 与frameworks-base/0002中GraphicsEnvironment的ANGLE_GL_VERSION_PKGS相同。 */
    static final String SETTING = "gaokun3_angle_gl_version_pkgs";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState == null) {
            getSupportFragmentManager().beginTransaction()
                    .replace(com.android.settingslib.collapsingtoolbar.R.id.content_frame,
                            new GlDriverVersionFragment())
                    .commit();
        }
    }

    static Set<String> readPackages(ContentResolver cr) {
        Set<String> packages = new LinkedHashSet<>();
        String value = Settings.Global.getString(cr, SETTING);
        if (!TextUtils.isEmpty(value)) {
            for (String p : value.split(",")) {
                if (!p.isEmpty()) {
                    packages.add(p);
                }
            }
        }
        return packages;
    }

    static void writePackages(ContentResolver cr, Set<String> packages) {
        Settings.Global.putString(cr, SETTING, String.join(",", packages));
    }

    private static final class AppEntry {
        final String packageName;
        final CharSequence label;
        final Drawable icon;

        AppEntry(String packageName, CharSequence label, Drawable icon) {
            this.packageName = packageName;
            this.label = label;
            this.icon = icon;
        }
    }

    /** 带启动器图标的非系统应用，按名称排序。读取图标较慢，在后台线程调用。 */
    private static List<AppEntry> loadApps(Context context) {
        PackageManager pm = context.getPackageManager();
        Intent launcher = new Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER);
        List<AppEntry> apps = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        for (ResolveInfo ri : pm.queryIntentActivities(launcher, 0)) {
            ApplicationInfo ai = ri.activityInfo.applicationInfo;
            if ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0 || !seen.add(ai.packageName)) {
                continue;
            }
            apps.add(new AppEntry(ai.packageName, ai.loadLabel(pm), ai.loadIcon(pm)));
        }
        Collator collator = Collator.getInstance();
        apps.sort((a, b) -> collator.compare(a.label.toString(), b.label.toString()));
        return apps;
    }

    public static class GlDriverVersionFragment extends SettingsBasePreferenceFragment
            implements Preference.OnPreferenceChangeListener {

        private final ExecutorService mExecutor = Executors.newSingleThreadExecutor();
        private final Handler mHandler = new Handler(Looper.getMainLooper());
        private PreferenceCategory mApps;

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.gl_driver_version_prefs, rootKey);
            mApps = findPreference("gl_driver_version_apps");
            Context app = requireContext().getApplicationContext();
            mExecutor.execute(() -> {
                List<AppEntry> apps = loadApps(app);
                mHandler.post(() -> showApps(apps));
            });
        }

        @Override
        public void onDestroy() {
            mExecutor.shutdownNow();
            mHandler.removeCallbacksAndMessages(null);
            super.onDestroy();
        }

        private void showApps(List<AppEntry> apps) {
            if (!isAdded()) {
                return;
            }
            Context context = getPreferenceManager().getContext();
            Set<String> enabled = readPackages(context.getContentResolver());
            mApps.removeAll();
            if (apps.isEmpty()) {
                Preference none = new Preference(context);
                none.setTitle(R.string.gl_driver_version_no_apps);
                none.setSelectable(false);
                mApps.addPreference(none);
                return;
            }
            for (AppEntry entry : apps) {
                SwitchPreferenceCompat p = new SwitchPreferenceCompat(context);
                p.setKey(entry.packageName);
                p.setTitle(entry.label);
                p.setIcon(entry.icon);
                p.setPersistent(false);
                p.setChecked(enabled.contains(entry.packageName));
                p.setOnPreferenceChangeListener(this);
                mApps.addPreference(p);
            }
        }

        @Override
        public boolean onPreferenceChange(Preference preference, Object newValue) {
            ContentResolver cr = requireContext().getContentResolver();
            // 每次重新读取，保留名单中未列出（已卸载）的应用。
            Set<String> packages = readPackages(cr);
            if (Boolean.TRUE.equals(newValue)) {
                packages.add(preference.getKey());
            } else {
                packages.remove(preference.getKey());
            }
            writePackages(cr, packages);
            return true;
        }
    }
}
