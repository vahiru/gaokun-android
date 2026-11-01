/*
 * Kernel version reported to apps (patches/0085), in Settings → System.
 *
 * Only writes two persist.sys.* properties. At the next boot init.gaokun3.rc hands
 * persist.sys.gaokun3.app_kernel_release to the kernel before zygote starts, and the kernel
 * answers app UIDs with it. System components keep the real version, this page included,
 * which is why it can show that version from uname().
 *
 * A change needs a restart: zygote and every running app have already read the old value,
 * and the kernel node is only written during boot.
 */
package com.huawei.gaokun3.parts;

import android.app.AlertDialog;
import android.os.Bundle;
import android.os.PowerManager;
import android.os.SystemProperties;
import android.system.Os;
import android.text.InputFilter;
import android.text.InputType;
import android.widget.Toast;

import androidx.preference.EditTextPreference;
import androidx.preference.Preference;
import androidx.preference.SwitchPreferenceCompat;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

import java.util.regex.Pattern;

public class AppKernelSettingsActivity extends CollapsingToolbarBaseActivity {

    private static final String PROP_CUSTOM = "persist.sys.gaokun3.app_kernel_custom";
    private static final String PROP_RELEASE = "persist.sys.gaokun3.app_kernel_release";

    /** The rule of app_release_valid() in the kernel; a value it refuses would be dropped at boot. */
    private static final Pattern VALID_RELEASE = Pattern.compile("[0-9]+\\.[0-9]+[A-Za-z0-9._+-]*");
    /** __NEW_UTS_LEN */
    private static final int MAX_RELEASE_LENGTH = 64;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState == null) {
            getSupportFragmentManager().beginTransaction()
                    .replace(com.android.settingslib.collapsingtoolbar.R.id.content_frame,
                            new AppKernelFragment())
                    .commit();
        }
    }

    static boolean isValidRelease(String release) {
        return release.length() <= MAX_RELEASE_LENGTH && VALID_RELEASE.matcher(release).matches();
    }

    public static class AppKernelFragment extends SettingsBasePreferenceFragment
            implements Preference.OnPreferenceChangeListener {

        private SwitchPreferenceCompat mSwitch;
        private EditTextPreference mRelease;

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.app_kernel_prefs, rootKey);
            mSwitch = findPreference("app_kernel_enabled");
            mRelease = findPreference("app_kernel_release");

            boolean on = "1".equals(SystemProperties.get(PROP_CUSTOM, ""));
            String release = SystemProperties.get(PROP_RELEASE, "");
            mSwitch.setChecked(on);
            mRelease.setText(release);
            mRelease.setSummary(release);
            mRelease.setEnabled(on);
            mRelease.setOnBindEditTextListener(edit -> {
                edit.setSingleLine(true);
                // A composing IME would turn a version number into something the kernel refuses.
                edit.setInputType(InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
                edit.setFilters(new InputFilter[] {
                        new InputFilter.LengthFilter(MAX_RELEASE_LENGTH)});
            });
            findPreference("app_kernel_real").setSummary(Os.uname().release);

            mSwitch.setOnPreferenceChangeListener(this);
            mRelease.setOnPreferenceChangeListener(this);
        }

        @Override
        public boolean onPreferenceChange(Preference preference, Object newValue) {
            if (preference == mSwitch) {
                boolean on = Boolean.TRUE.equals(newValue);
                SystemProperties.set(PROP_CUSTOM, on ? "1" : "0");
                mRelease.setEnabled(on);
            } else {
                String release = (String) newValue;
                if (!isValidRelease(release)) {
                    Toast.makeText(requireContext(), R.string.app_kernel_version_invalid,
                            Toast.LENGTH_LONG).show();
                    return false;
                }
                SystemProperties.set(PROP_RELEASE, release);
                mRelease.setSummary(release);
            }
            new AlertDialog.Builder(requireContext())
                    .setMessage(R.string.app_kernel_restart_message)
                    .setPositiveButton(R.string.app_kernel_restart_now, (d, w) ->
                            requireContext().getSystemService(PowerManager.class).reboot(null))
                    .setNegativeButton(R.string.app_kernel_restart_later, null)
                    .show();
            return true;
        }
    }
}
