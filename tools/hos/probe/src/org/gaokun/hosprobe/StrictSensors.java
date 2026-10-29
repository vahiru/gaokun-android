package org.gaokun.hosprobe;
import android.hardware.*;
import android.os.*;
import java.util.*;
import java.util.concurrent.*;

final class StrictSensors {
    static void run(Main activity, int rounds) throws Exception {
        if (rounds < 1 || rounds > 3) throw new IllegalArgumentException("sensor_rounds must be 1..3");
        SensorManager sm=activity.getSystemService(SensorManager.class);
        HandlerThread thread=new HandlerThread("strict-sensor-probe");
        thread.start();
        Handler handler=new Handler(thread.getLooper());
        boolean allPass=true;
        try {
            for (int round=1;round<=rounds;round++) {
                final long start=SystemClock.elapsedRealtimeNanos();
                final long sampleStart=start+2000000000L;
                Map<Integer,StrictSensorStats> stats=new LinkedHashMap<>();
                Map<Integer,Sensor> sensors=new LinkedHashMap<>();
                for (int type:new int[]{1,4,9,15}) {
                    stats.put(type,new StrictSensorStats());
                    sensors.put(type,sm.getDefaultSensor(type));
                }
                SensorEventListener listener=new SensorEventListener() {
                    public void onAccuracyChanged(Sensor sensor,int accuracy) {}
                    public void onSensorChanged(SensorEvent e) {
                        StrictSensorStats s=stats.get(e.sensor.getType());
                        if (s!=null) s.accept(e.values,e.timestamp,SystemClock.elapsedRealtimeNanos(),start,sampleStart);
                    }
                };
                activity.log("BEGIN strict sensors round="+round+" warmupMs=2000 sampleMs=5000 simultaneous=true");
                try {
                    for (int type:stats.keySet()) {
                        Sensor sensor=sensors.get(type);
                        if (sensor!=null) stats.get(type).registered=sm.registerListener(listener,sensor,SensorManager.SENSOR_DELAY_GAME,handler);
                    }
                    Thread.sleep(7000);
                } finally {
                    sm.unregisterListener(listener);
                }
                CountDownLatch drained=new CountDownLatch(1);
                if (!handler.post(drained::countDown) || !drained.await(2,TimeUnit.SECONDS))
                    throw new IllegalStateException("sensor callback queue did not drain");
                for (int type:stats.keySet()) {
                    StrictSensorStats s=stats.get(type); Sensor sensor=sensors.get(type);
                    boolean pass=s.passed(); allPass &= pass;
                    activity.log((pass?"PASS":"FAIL")+" strict-sensor round="+round+" type="+type+
                        " sensor="+(sensor==null?"MISSING":sensor.getName())+" "+s.summary());
                }
                if (round<rounds) Thread.sleep(1000);
            }
            activity.log((allPass?"PASS":"FAIL")+" strict-sensors rounds="+rounds+
                " (finite/timestamp/data delivery only; orientation accuracy unverified)");
        } finally {
            thread.quitSafely();
            thread.join(3000);
        }
    }
}
