package org.gaokun.hosprobe;

import android.os.Build;
import android.os.PowerManager;
import java.util.Map;
import java.util.TreeMap;

/** Public API-only, single headroom sample. No sensor load or temperature injection. */
final class ThermalProbe {
    static void run(Main activity) {
        PowerManager power = activity.getSystemService(PowerManager.class);
        if (power == null) {
            activity.log("FAIL thermal PowerManager unavailable");
            return;
        }
        activity.test("thermal-status", () -> {
            int status = power.getCurrentThermalStatus();
            activity.log("RAW thermal-status=" + status);
            activity.log((status >= PowerManager.THERMAL_STATUS_NONE &&
                    status <= PowerManager.THERMAL_STATUS_SHUTDOWN ? "PASS" : "FAIL") +
                    " thermal-status legal range 0..6");
        });
        activity.test("thermal-headroom", () -> {
            // Exactly one call. Repeated calls inside ten seconds can produce NaN.
            float headroom = power.getThermalHeadroom(0);
            activity.log("RAW thermal-headroom forecastSeconds=0 value=" + headroom);
            activity.log((Float.isFinite(headroom) && headroom >= 0 ? "PASS" : "FAIL") +
                    " thermal-headroom finite/nonnegative (dimensionless, not Celsius)");
        });
        if (Build.VERSION.SDK_INT < 35) {
            activity.log("SKIP thermal-thresholds public API requires SDK35");
            return;
        }
        activity.test("thermal-thresholds", () -> {
            Map<Integer, Float> thresholds;
            try {
                thresholds = power.getThermalHeadroomThresholds();
            } catch (UnsupportedOperationException | NoSuchMethodError unavailable) {
                activity.log("SKIP thermal-thresholds public API unavailable: " + unavailable);
                return;
            }
            activity.log("RAW thermal-thresholds=" +
                    (thresholds == null ? "null" : new TreeMap<>(thresholds)));
            boolean valid = thresholds != null && !thresholds.isEmpty();
            if (thresholds != null) for (Map.Entry<Integer, Float> entry : thresholds.entrySet()) {
                Integer key = entry.getKey(); Float value = entry.getValue();
                valid &= key != null && key >= PowerManager.THERMAL_STATUS_NONE &&
                        key <= PowerManager.THERMAL_STATUS_SHUTDOWN && value != null &&
                        Float.isFinite(value) && value >= 0;
            }
            activity.log((valid ? "PASS" : "FAIL") +
                    " thermal-thresholds nonempty/legal status/finite nonnegative values");
        });
    }
}
