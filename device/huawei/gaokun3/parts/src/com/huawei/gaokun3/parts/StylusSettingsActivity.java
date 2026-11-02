/*
 * M-Pencil settings, in Settings → Connected devices: how strongly the ink is smoothed and what a
 * double-tap on the pen's side does.
 *
 * Only writes two persist.sys.* properties: persist.sys.gaokun3.pen_beta, the position filter's beta
 * (500, 300, 200, 125 from the weakest to the strongest smoothing), and persist.sys.gaokun3.pen_side
 * (eraser, button, none).  stylus/gk3pend.rc copies the known values to vendor.gaokun3.pen_*, which
 * gk3pend reads again whenever a property changes, so a change applies from the next frame.  The
 * detour through init has the same reason as SpeakerFxSettingsActivity's: this app cannot set vendor
 * properties and gk3pend cannot read system ones.
 *
 * The page shows the persist.sys values, the only stored copy; unset or unknown values show as the
 * defaults (beta 500, eraser), which gk3pend uses from the next boot on.
 */
package com.huawei.gaokun3.parts;

import android.os.Bundle;
import android.os.SystemProperties;

import androidx.preference.ListPreference;
import androidx.preference.Preference;
import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;
import com.android.settingslib.widget.SliderPreference;

public class StylusSettingsActivity extends CollapsingToolbarBaseActivity {

    private static final String BETA_PROP = "persist.sys.gaokun3.pen_beta";
    private static final String SIDE_PROP = "persist.sys.gaokun3.pen_side";
    /** Slider positions from weak to strong; a lower beta smooths more and lags more. */
    private static final String[] BETAS = { "500", "300", "200", "125" };
    private static final String SIDE_DEFAULT = "eraser";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (savedInstanceState == null) {
            getSupportFragmentManager().beginTransaction()
                    .replace(com.android.settingslib.collapsingtoolbar.R.id.content_frame,
                            new StylusFragment())
                    .commit();
        }
    }

    static int smoothLevel() {
        String beta = SystemProperties.get(BETA_PROP, BETAS[0]);
        for (int i = 0; i < BETAS.length; i++) {
            if (BETAS[i].equals(beta)) {
                return i;
            }
        }
        return 0;
    }

    public static class StylusFragment extends SettingsBasePreferenceFragment
            implements Preference.OnPreferenceChangeListener {

        private SliderPreference mSmooth;
        private ListPreference mSide;

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.stylus_prefs, rootKey);
            mSmooth = findPreference("pen_smooth");
            mSmooth.setMax(BETAS.length - 1);
            mSmooth.setTickVisible(true);
            // read out the level name rather than its index
            String[] names = getResources().getStringArray(R.array.stylus_smooth_levels);
            mSmooth.setLabelFormater(v -> names[Math.round(v)]);
            mSmooth.setValue(smoothLevel());
            showLevel(smoothLevel());
            mSmooth.setOnPreferenceChangeListener(this);
            mSide = findPreference("pen_side");
            String side = SystemProperties.get(SIDE_PROP, SIDE_DEFAULT);
            mSide.setValue(mSide.findIndexOfValue(side) >= 0 ? side : SIDE_DEFAULT);
            mSide.setSummaryProvider(ListPreference.SimpleSummaryProvider.getInstance());
            mSide.setOnPreferenceChangeListener(this);
        }

        private void showLevel(int level) {
            mSmooth.setSummary(getResources().getStringArray(R.array.stylus_smooth_levels)[level]);
        }

        @Override
        public boolean onPreferenceChange(Preference preference, Object newValue) {
            if (preference == mSmooth) {
                int level = (Integer) newValue;
                SystemProperties.set(BETA_PROP, BETAS[level]);
                showLevel(level);
            } else {
                SystemProperties.set(SIDE_PROP, String.valueOf(newValue));
            }
            return true;
        }
    }
}
