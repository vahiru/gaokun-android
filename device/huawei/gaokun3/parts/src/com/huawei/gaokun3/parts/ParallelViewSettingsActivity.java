/*
 * 平行视界（设置 → 应用）：按应用开启frameworks-base/0006的OVERRIDE_ENABLE_PARALLEL_VIEW。
 *
 * 开启后，窗口管理扩展库（frameworks-base/0007）在应用启动时读取应用自带的assets/easygo.json
 * （华为平板分栏规则中的magicwindow部分），横屏时按规则把页面左右分栏显示。
 *
 * 只列出带启动器图标、带有magicwindow规则且规则未排除平板的非系统应用。声明了
 * PROPERTY_ACTIVITY_EMBEDDING_SPLITS_ENABLED的应用自己使用分栏，扩展库不为它们载入规则，也不列出。
 * 这里只判断规则是否存在，规则的解析只在扩展库中进行。
 *
 * 开关保存为兼容框架的按应用覆盖，重启后保留，默认全部关闭。开启时同时覆盖FORCE_RESIZE_APP：
 * 系统只允许从可调整大小的页面发起分栏，而部分应用的页面声明了不可调整大小。覆盖只在应用进程启动时
 * 读取，因此切换后结束该应用的进程，做法与am compat相同（killUid，不会让应用进入停止状态）。
 */
package com.huawei.gaokun3.parts;

import android.app.ActivityManager;
import android.app.compat.CompatChanges;
import android.app.compat.PackageOverride;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.os.RemoteException;
import android.os.UserHandle;
import android.util.Log;
import android.view.WindowManager;

import androidx.preference.Preference;
import androidx.preference.PreferenceCategory;
import androidx.preference.SwitchPreferenceCompat;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.text.Collator;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class ParallelViewSettingsActivity extends CollapsingToolbarBaseActivity {

    private static final String TAG = "Gaokun3ParallelView";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState == null) {
            getSupportFragmentManager().beginTransaction()
                    .replace(com.android.settingslib.collapsingtoolbar.R.id.content_frame,
                            new ParallelViewFragment())
                    .commit();
        }
    }

    /** 应用是否开启了平行视界。 */
    static boolean isEnabled(String packageName) {
        return CompatChanges.isChangeEnabled(ActivityInfo.OVERRIDE_ENABLE_PARALLEL_VIEW,
                packageName, Process.myUserHandle());
    }

    /** 开启或关闭平行视界，然后结束该应用的进程。 */
    static void setEnabled(Context context, String packageName, boolean enabled) {
        if (enabled) {
            PackageOverride on = new PackageOverride.Builder().setEnabled(true).build();
            Map<Long, PackageOverride> overrides = new HashMap<>();
            overrides.put(ActivityInfo.OVERRIDE_ENABLE_PARALLEL_VIEW, on);
            overrides.put(ActivityInfo.FORCE_RESIZE_APP, on);
            CompatChanges.putPackageOverrides(packageName, overrides);
        } else {
            Set<Long> changes = new HashSet<>();
            changes.add(ActivityInfo.OVERRIDE_ENABLE_PARALLEL_VIEW);
            changes.add(ActivityInfo.FORCE_RESIZE_APP);
            CompatChanges.removePackageOverrides(packageName, changes);
        }
        try {
            int uid = context.getPackageManager().getPackageUid(packageName, 0);
            ActivityManager.getService().killUid(UserHandle.getAppId(uid), UserHandle.USER_ALL,
                    "parallel view " + (enabled ? "enabled" : "disabled"));
        } catch (PackageManager.NameNotFoundException | RemoteException e) {
            Log.w(TAG, "Could not stop " + packageName, e);
        }
    }

    /** 应用是否自己声明使用分栏（扩展库不为这类应用载入规则）。 */
    private static boolean usesEmbeddingItself(PackageManager pm, String packageName) {
        try {
            return pm.getProperty(WindowManager.PROPERTY_ACTIVITY_EMBEDDING_SPLITS_ENABLED,
                    packageName).getBoolean();
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    /** 应用的assets/easygo.json中是否有magicwindow规则，且规则未排除平板（excludeDevice含PAD）。 */
    private static boolean hasTabletRules(Context context, String packageName) {
        try (InputStream in = context.createPackageContext(packageName, 0).getAssets()
                .open("easygo.json")) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
            // JSONObject会跳过开头的字节顺序标记。
            String json = new String(out.toByteArray(), StandardCharsets.UTF_8);
            JSONArray entities = new JSONObject(json).optJSONArray("logicEntities");
            for (int i = 0; entities != null && i < entities.length(); i++) {
                JSONObject entity = entities.optJSONObject(i);
                JSONObject head = entity != null ? entity.optJSONObject("head") : null;
                JSONObject body = entity != null ? entity.optJSONObject("body") : null;
                if (head == null || body == null
                        || !"magicwindow".equals(head.optString("function"))) {
                    continue;
                }
                // 与扩展库相同，只看第一个magicwindow部分。
                for (String device : body.optString("excludeDevice").split("\\|")) {
                    if ("PAD".equalsIgnoreCase(device.trim())) {
                        return false;
                    }
                }
                return true;
            }
            return false;
        } catch (PackageManager.NameNotFoundException | IOException | JSONException e) {
            return false;
        }
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

    /** 带启动器图标、带有平板分栏规则的非系统应用，按名称排序。读取较慢，在后台线程调用。 */
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
            if (usesEmbeddingItself(pm, ai.packageName)
                    || !hasTabletRules(context, ai.packageName)) {
                continue;
            }
            apps.add(new AppEntry(ai.packageName, ai.loadLabel(pm), ai.loadIcon(pm)));
        }
        Collator collator = Collator.getInstance();
        apps.sort((a, b) -> collator.compare(a.label.toString(), b.label.toString()));
        return apps;
    }

    public static class ParallelViewFragment extends SettingsBasePreferenceFragment
            implements Preference.OnPreferenceChangeListener {

        private final ExecutorService mExecutor = Executors.newSingleThreadExecutor();
        private final Handler mHandler = new Handler(Looper.getMainLooper());
        private PreferenceCategory mApps;

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.parallel_view_prefs, rootKey);
            mApps = findPreference("parallel_view_apps");
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
            mApps.removeAll();
            if (apps.isEmpty()) {
                Preference none = new Preference(context);
                none.setTitle(R.string.parallel_view_no_apps);
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
                p.setChecked(isEnabled(entry.packageName));
                p.setOnPreferenceChangeListener(this);
                mApps.addPreference(p);
            }
        }

        @Override
        public boolean onPreferenceChange(Preference preference, Object newValue) {
            setEnabled(requireContext(), preference.getKey(), Boolean.TRUE.equals(newValue));
            return true;
        }
    }
}
