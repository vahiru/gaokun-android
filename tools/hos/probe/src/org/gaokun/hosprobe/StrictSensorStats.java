package org.gaokun.hosprobe;
import java.util.Arrays;
import java.util.ArrayList;
import java.util.List;
/** No Android dependency: every event, including warmup, must be valid. */
final class StrictSensorStats {
    int events, samples, nonFinite, nanEvents, badTimestamp;
    long previousTimestamp, firstValidDelayNs = -1;
    float[] last = new float[0];
    double[] sum = new double[0];
    boolean registered;
    private final boolean observeVectors;
    private final List<ObservedEvent> firstEvents = new ArrayList<>();
    final VectorObservation warmup = new VectorObservation();
    final VectorObservation sample = new VectorObservation();

    StrictSensorStats() { this(0); }
    StrictSensorStats(int sensorType) {
        // Android types1/4 only. Fusion validity remains checked, but does not
        // acquire additional raw-event diagnostics or imply physical accuracy.
        observeVectors = sensorType == 1 || sensorType == 4;
    }
    static final class VectorObservation {
        int finiteVectors, zeroVectors;
        double minNorm = Double.POSITIVE_INFINITY;
        void accept(float[] values) {
            double x=values[0], y=values[1], z=values[2];
            double norm = Math.sqrt(x*x+y*y+z*z);
            finiteVectors++;
            minNorm = Math.min(minNorm,norm);
            if (x == 0.0 && y == 0.0 && z == 0.0) zeroVectors++;
        }
        String summary(String prefix) {
            return " "+prefix+"FiniteVectors="+finiteVectors+
                " "+prefix+"MinNorm="+(finiteVectors == 0 ? "none" : Double.toString(minNorm))+
                " "+prefix+"ZeroVectors="+zeroVectors;
        }
    }
    static final class ObservedEvent {
        final long sensorTimestampNs, arrivalElapsedRealtimeNs, elapsedSinceStartNs;
        final String phase;
        final int accuracy;
        final boolean finite, timestampValid;
        private final float[] values;
        ObservedEvent(float[] source, long timestamp, long arrival, long start,
                      String phase, boolean finite, boolean timestampValid, int accuracy) {
            this.accuracy = accuracy;
            values = source.clone(); // SensorEvent.values is reused by Android.
            sensorTimestampNs=timestamp; arrivalElapsedRealtimeNs=arrival;
            elapsedSinceStartNs=arrival-start; this.phase=phase;
            this.finite=finite; this.timestampValid=timestampValid;
        }
        float[] copyValues() { return values.clone(); }
        String summary() {
            String[] bits = new String[values.length];
            for (int i=0;i<values.length;i++) bits[i] = "0x"+Integer.toHexString(Float.floatToRawIntBits(values[i]));
            return "phase="+phase+" sensorTimestampNs="+sensorTimestampNs+
                " arrivalElapsedRealtimeNs="+arrivalElapsedRealtimeNs+
                " elapsedSinceStartNs="+elapsedSinceStartNs+
                " finite="+finite+" timestampValid="+timestampValid+
                " accuracy="+(accuracy == Integer.MIN_VALUE ? "unknown" : Integer.toString(accuracy))+
                " values="+Arrays.toString(values)+" rawBits="+Arrays.toString(bits);
        }
    }
    synchronized ObservedEvent[] observedEvents() {
        return firstEvents.toArray(new ObservedEvent[0]);
    }
    synchronized void accept(float[] values, long timestamp, long arrival,
                             long start, long sampleStart) {
        accept(values,timestamp,arrival,start,sampleStart,Integer.MIN_VALUE);
    }
    synchronized void accept(float[] values, long timestamp, long arrival,
                             long start, long sampleStart, int accuracy) {
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
        if (observeVectors) {
            boolean inWarmup = arrival >= start && arrival < sampleStart;
            boolean inSample = arrival >= sampleStart && arrival < sampleStart + 5000000000L;
            if (firstEvents.size() < 8) firstEvents.add(new ObservedEvent(values,timestamp,arrival,start,
                inWarmup ? "warmup" : inSample ? "sample" : "outside",finite,timeOK,accuracy));
            // Zero is legitimate in free fall or for a stationary gyro. These
            // observations never alter event values, sample eligibility or PASS.
            if (finite) {
                if (inWarmup) warmup.accept(values);
                else if (inSample) sample.accept(values);
            }
        }
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
            " last="+Arrays.toString(last)+" mean="+Arrays.toString(mean)+
            (observeVectors ? warmup.summary("obsWarmup")+sample.summary("obsSample") : "");
    }
}
