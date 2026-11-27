/*
 * M-Pencil settings, in Settings → Connected devices: which pen is connected, how strongly the ink
 * is smoothed, what a double-tap on the pen's side does, and disconnecting the pen.
 *
 * The settings are two persist.sys.* properties: persist.sys.gaokun3.pen_beta, the position
 * filter's beta (500, 300, 200, 125 from the weakest to the strongest smoothing), and
 * persist.sys.gaokun3.pen_side (eraser, button, none).  stylus/gk3pend.rc copies the known values
 * to vendor.gaokun3.pen_*, which gk3pend reads again whenever a property changes, so a change
 * applies from the next frame.  The detour through init has the same reason as
 * SpeakerFxDialogActivity's: this app cannot set vendor properties and gk3pend cannot read system
 * ones.
 *
 * The page shows the persist.sys values, the only stored copy; unset or unknown values show as the
 * defaults (beta 500, none), which gk3pend uses from the next boot on.  The double-tap is off by
 * default: Android shows nothing when the pen turns into an eraser, and in apps that do not handle
 * the eraser it then seems to have stopped writing.
 *
 * The connected pen, shown under the page title, comes from vendor.gaokun3.pen_conn, which gk3pend
 * sets (0: none, else the generation, 1 for a model it does not know), read every second while the
 * page is shown.
 * Disconnecting deletes the pairing: the page sets the non-persistent sys.gaokun3.pen_unpair to 1
 * or 2, the other value each time, stylus/gk3pend.rc copies it to vendor.gaokun3.pen_unpair, and
 * gk3pend sends the controller 0x1500.  The pen then only hovers until it is docked again, as the
 * confirmation says; the button shows only while a pen is connected.
 */
package com.huawei.gaokun3.parts;

import android.app.AlertDialog;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemProperties;

import androidx.preference.ListPreference;
import androidx.preference.Preference;
import com.android.settingslib.collapsingtoolbar.CollapsingToolbarBaseActivity;
import com.android.settingslib.widget.SettingsBasePreferenceFragment;
import com.android.settingslib.widget.SliderPreference;
import com.google.android.material.appbar.CollapsingToolbarLayout;

public class StylusSettingsActivity extends CollapsingToolbarBaseActivity {

    private static final String BETA_PROP = "persist.sys.gaokun3.pen_beta";
    private static final String SIDE_PROP = "persist.sys.gaokun3.pen_side";
    /** Slider positions from weak to strong; a lower beta smooths more and lags more. */
    private static final String[] BETAS = { "500", "300", "200", "125" };
    private static final String SIDE_DEFAULT = "none";
    private static final String CONN_PROP = "vendor.gaokun3.pen_conn";
    private static final String UNPAIR_PROP = "sys.gaokun3.pen_unpair";
    private static final long CONN_POLL_MS = 1000;

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
        private DangerButtonPreference mUnpair;
        private String mModel;      // the connected pen's name, null when none is connected
        private final Handler mHandler = new Handler(Looper.getMainLooper());
        private final Runnable mPoll = new Runnable() {
            @Override
            public void run() {
                showConnection();
                mHandler.postDelayed(this, CONN_POLL_MS);
            }
        };

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
            mUnpair = findPreference("pen_unpair");
            mUnpair.setOnClickListener(v -> confirmUnpair());
        }

        @Override
        public void onResume() {
            super.onResume();
            mPoll.run();
        }

        @Override
        public void onPause() {
            super.onPause();
            mHandler.removeCallbacks(mPoll);
        }

        private void showLevel(int level) {
            mSmooth.setSummary(getResources().getStringArray(R.array.stylus_smooth_levels)[level]);
        }

        private void showConnection() {
            String gen = SystemProperties.get(CONN_PROP, "0");
            boolean connected = !gen.isEmpty() && !"0".equals(gen);
            mModel = connected ? ("2".equals(gen) || "3".equals(gen) ? "M-Pencil " + gen : "M-Pencil")
                    : null;
            CollapsingToolbarLayout toolbar =
                    ((CollapsingToolbarBaseActivity) requireActivity()).getCollapsingToolbarLayout();
            if (toolbar != null) {
                toolbar.setSubtitle(connected ? getString(R.string.stylus_pen_connected, mModel)
                        : getString(R.string.stylus_pen_none));
            }
            mUnpair.setVisible(connected);
        }

        private void confirmUnpair() {
            if (mModel == null) {
                return;
            }
            new AlertDialog.Builder(requireContext())
                    .setTitle(R.string.stylus_unpair_dialog_title)
                    .setMessage(getString(R.string.stylus_unpair_dialog_text, mModel))
                    .setPositiveButton(android.R.string.ok, (d, which) -> {
                        String last = SystemProperties.get(UNPAIR_PROP, "");
                        SystemProperties.set(UNPAIR_PROP, "1".equals(last) ? "2" : "1");
                    })
                    .setNegativeButton(android.R.string.cancel, null)
                    .show();
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
