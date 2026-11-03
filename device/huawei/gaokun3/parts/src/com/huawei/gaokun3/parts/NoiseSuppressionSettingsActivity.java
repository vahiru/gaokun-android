/*
 * Microphone noise suppression, in Settings → Sound: in which scenes the built-in microphone's
 * sound goes through DeepFilterNet3 after the microphone array (micarray/README.md).
 *
 * Only writes three persist.sys.* properties, "1" or "0": persist.sys.gaokun3.denoise.call (calls,
 * VOICE_COMMUNICATION), .record (MIC, DEFAULT) and .camcorder (CAMCORDER).  etc/denoise.rc copies
 * them to vendor.gaokun3.denoise.*, which the capture hook reads when a recording starts, so a
 * change applies from the next call or recording.  The detour through init has the same reason as
 * SpeakerFxSettingsActivity's: this app cannot set vendor properties and the audio HAL cannot read
 * system ones.
 *
 * The page shows the persist.sys values, the only stored copy; unset means the hook's defaults:
 * calls on, recording and camcorder off.
 */
package com.huawei.gaokun3.parts;

import android.os.Bundle;
import android.os.SystemProperties;

import androidx.preference.Preference;
import androidx.preference.SwitchPreferenceCompat;

import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;

public class NoiseSuppressionSettingsActivity extends CollapsingToolbarBaseActivity {

    private static final String PROP_PREFIX = "persist.sys.gaokun3.denoise.";
    /** Preference keys and the scenes they switch; calls are on unless set to "0". */
    private static final String[] KEYS = { "denoise_call", "denoise_record", "denoise_camcorder" };
    private static final String[] SCENES = { "call", "record", "camcorder" };
    private static final boolean[] DEFAULTS = { true, false, false };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState == null) {
            getSupportFragmentManager().beginTransaction()
                    .replace(com.android.settingslib.collapsingtoolbar.R.id.content_frame,
                            new NoiseSuppressionFragment())
                    .commit();
        }
    }

    public static class NoiseSuppressionFragment extends SettingsBasePreferenceFragment
            implements Preference.OnPreferenceChangeListener {

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.noise_suppression_prefs, rootKey);
            for (int i = 0; i < KEYS.length; i++) {
                SwitchPreferenceCompat s = findPreference(KEYS[i]);
                String value = SystemProperties.get(PROP_PREFIX + SCENES[i], "");
                // The hook's reading: a default-on scene is off only at "0", the others on only at "1".
                s.setChecked(DEFAULTS[i] ? !"0".equals(value) : "1".equals(value));
                s.setOnPreferenceChangeListener(this);
            }
        }

        @Override
        public boolean onPreferenceChange(Preference preference, Object newValue) {
            for (int i = 0; i < KEYS.length; i++) {
                if (KEYS[i].equals(preference.getKey())) {
                    SystemProperties.set(PROP_PREFIX + SCENES[i],
                            Boolean.TRUE.equals(newValue) ? "1" : "0");
                }
            }
            return true;
        }
    }
}
