package org.gaokun.hosprobe;
import java.util.Arrays;
/** No Android dependency: every event, including warmup, must be valid. */
final class StrictSensorStats {
    int events, samples, nonFinite, nanEvents, badTimestamp;
    long previousTimestamp, firstValidDelayNs = -1;
    float[] last = new float[0];
    double[] sum = new double[0];
    boolean registered;
    synchronized void accept(float[] values, long timestamp, long arrival,
                             long start, long sampleStart) {
        events++;
        last = values.clone();
        boolean finite = values.length >= 3;
        boolean nan = false;
        for (float v : values) {
            finite &= Float.isFinite(v);
            nan |= Float.isNaN(v);
        }
        if (!finite) nonFinite++;
        if (nan) nanEvents++;
        boolean timeOK = timestamp > 0 && (events == 1 || timestamp > previousTimestamp);
        if (!timeOK) badTimestamp++;
        previousTimestamp = timestamp;
        if (finite && timeOK) {
            if (firstValidDelayNs < 0) firstValidDelayNs = arrival - start;
            if (arrival >= sampleStart && arrival < sampleStart + 5000000000L) {
                if (sum.length == 0) sum = new double[values.length];
                for (int i=0; i<Math.min(sum.length,values.length); i++) sum[i] += values[i];
                samples++;
            }
        }
    }
    synchronized boolean passed() {
        return registered && samples > 0 && nonFinite == 0 && badTimestamp == 0;
    }
    synchronized String summary() {
        double[] mean = sum.clone();
        if (samples > 0) for (int i=0; i<mean.length; i++) mean[i] /= samples;
        return "registered="+registered+" events="+events+" validSamples="+samples+
            " nonFinite="+nonFinite+" NaN="+nanEvents+" badTimestamp="+badTimestamp+
            " firstValidMs="+(firstValidDelayNs < 0 ? -1 : firstValidDelayNs/1000000.0)+
            " last="+Arrays.toString(last)+" mean="+Arrays.toString(mean);
    }
}
