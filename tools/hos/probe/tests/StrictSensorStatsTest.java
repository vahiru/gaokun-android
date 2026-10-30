package org.gaokun.hosprobe;
public class StrictSensorStatsTest {
 static void check(boolean condition){check(condition,"predicate failed");}
 static void check(boolean condition,String reason){if(!condition)throw new AssertionError(reason);}
 static StrictSensorStats stat(){return stat(0);}
 static StrictSensorStats stat(int type){StrictSensorStats s=new StrictSensorStats(type);s.registered=true;return s;}
 static void originalPredicates(){
  StrictSensorStats ok=stat();ok.accept(new float[]{0,0,9.8f},1,100,0,2000000000L);check(!ok.passed());
  ok.accept(new float[]{0,0,9.8f},2,3000000000L,0,2000000000L);check(ok.passed());
  for(float bad:new float[]{Float.NaN,Float.POSITIVE_INFINITY}){
   StrictSensorStats s=stat();s.accept(new float[]{bad,0,0},1,100,0,2000000000L);
   s.accept(new float[]{0,0,9.8f},2,3000000000L,0,2000000000L);check(!s.passed());check(s.nonFinite==1);
  }
  StrictSensorStats time=stat();time.accept(new float[]{0,0,9.8f},4,3000000000L,0,2000000000L);
  time.accept(new float[]{0,0,9.8f},4,4000000000L,0,2000000000L);check(!time.passed());
  StrictSensorStats missing=stat();missing.registered=false;missing.accept(new float[]{0,0,9.8f},1,3000000000L,0,2000000000L);check(!missing.passed());
  StrictSensorStats late=stat();late.accept(new float[]{0,0,9.8f},1,8000000000L,0,2000000000L);check(!late.passed());
 }
 static void zeroIsObservationOnly(){
  for(int type:new int[]{1,4}){
   StrictSensorStats s=stat(type);
   s.accept(new float[]{0,-0.0f,0},1,100,0,2000000000L);
   s.accept(new float[]{0,0,-0.0f},2,3000000000L,0,2000000000L);
   check(s.passed(),"real zero must not add a FAIL criterion");
   check(s.warmup.zeroVectors==1&&s.sample.zeroVectors==1,"phase zero counters");
   check(s.warmup.minNorm==0&&s.sample.minNorm==0,"zero norm remains zero");
   check(s.last[2]==0&&s.sum[0]==0&&s.samples==1,"observations must not replace measurements");
  }
 }
 static void windowsAndFiniteNorm(){
  long start=1000000000L,sampleStart=start+2000000000L;
  StrictSensorStats s=stat(1);
  s.accept(new float[]{3,4,0},1,start,start,sampleStart);
  s.accept(new float[]{0,0,0},2,sampleStart-1,start,sampleStart);
  s.accept(new float[]{3,4,12},3,sampleStart,start,sampleStart);
  s.accept(new float[]{3,4,0},4,sampleStart+4999999999L,start,sampleStart);
  s.accept(new float[]{0,0,0},5,sampleStart+5000000000L,start,sampleStart);
  check(s.warmup.finiteVectors==2&&s.warmup.zeroVectors==1&&s.warmup.minNorm==0,"warmup boundaries");
  check(s.sample.finiteVectors==2&&s.sample.zeroVectors==0&&s.sample.minNorm==5,"sample exclusive end");
  check(s.samples==2&&s.sum[2]==12&&s.passed(),"original sample sum/window retained");
  StrictSensorStats.ObservedEvent[] e=s.observedEvents();
  check(e[0].phase.equals("warmup")&&e[2].phase.equals("sample")&&e[4].phase.equals("outside"),"event phase snapshots");
  check(e[2].sensorTimestampNs==3&&e[2].arrivalElapsedRealtimeNs==sampleStart&&e[2].elapsedSinceStartNs==2000000000L,"timestamp and elapsed domains");
  StrictSensorStats invalid=stat(1);
  invalid.accept(new float[]{Float.NaN,0,0},1,start,start,sampleStart);
  invalid.accept(new float[]{Float.POSITIVE_INFINITY,0,0},2,sampleStart,start,sampleStart);
  invalid.accept(new float[]{1,2},3,sampleStart+1,start,sampleStart);
  check(invalid.warmup.finiteVectors==0&&invalid.sample.finiteVectors==0,"invalid/short vectors excluded from norm");
  check(invalid.summary().contains("obsWarmupMinNorm=none")&&invalid.summary().contains("obsSampleMinNorm=none"),"missing metadata cannot pretend norm0");
  invalid.accept(new float[]{0,0,9.8f},4,sampleStart+2,start,sampleStart);
  check(!invalid.passed()&&invalid.nonFinite==3&&invalid.nanEvents==1,"later good sample cannot erase FAIL");
  check(!invalid.observedEvents()[0].finite,"initial invalid event is preserved for diagnosis");
  StrictSensorStats tiny=stat(1);
  tiny.accept(new float[]{Float.MIN_VALUE,0,0},1,sampleStart,start,sampleStart);
  check(tiny.sample.minNorm>0&&tiny.sample.zeroVectors==0,"double norm must not underflow a real nonzero float vector");
  StrictSensorStats huge=stat(1);
  huge.accept(new float[]{Float.MAX_VALUE,Float.MAX_VALUE,Float.MAX_VALUE},1,sampleStart,start,sampleStart);
  check(Double.isFinite(huge.sample.minNorm)&&huge.passed(),"norm computation must not overflow finite float components");
  StrictSensorStats badTime=stat(1);
  badTime.accept(new float[]{0,0,0},0,start,start,sampleStart);
  badTime.accept(new float[]{3,4,0},1,sampleStart,start,sampleStart);
  check(!badTime.passed()&&badTime.warmup.zeroVectors==1,"timestamp FAIL retained while observing finite zero");
  check(!badTime.observedEvents()[0].timestampValid,"bad timestamp metadata");
 }
 static void reusedSensorArraysAreCopied(){
  StrictSensorStats s=stat(1);float[] reused=new float[3];
  for(int i=0;i<10;i++){
   reused[0]=i;reused[1]=i+1;reused[2]=i+2;
   s.accept(reused,100+i,1000+i,1000,2000001000L);
   reused[0]=999;reused[1]=Float.NaN;reused[2]=-999;
  }
  StrictSensorStats.ObservedEvent[] e=s.observedEvents();
  check(e.length==8,"first-event storage must stay bounded at8");
  for(int i=0;i<8;i++){
   float[] copied=e[i].copyValues();
   check(copied[0]==i&&copied[1]==i+1&&copied[2]==i+2,"SensorEvent.values reuse polluted snapshot");
   check(e[i].sensorTimestampNs==100+i&&e[i].elapsedSinceStartNs==i,"reused event timing polluted");
   copied[0]=-77;check(e[i].copyValues()[0]==i,"observer cannot mutate stored array");
  }
  e[0]=null;check(s.observedEvents()[0]!=null,"observer cannot mutate stored event list");
  check(s.last[0]==9&&s.last[1]==10&&s.nonFinite==0,"caller mutation cannot poison original stats");
 }
 static void diagnosticsOnlyForAccAndGyro(){
  for(int type:new int[]{0,9,15}){
   StrictSensorStats s=stat(type);s.accept(new float[]{0,0,0},1,3000000000L,0,2000000000L);
   check(s.passed()&&s.observedEvents().length==0,"extra diagnostic must not attach to fusion/unknown types");
   check(s.warmup.finiteVectors==0&&s.sample.finiteVectors==0&&!s.summary().contains("obsSample"),"type-limited norm metadata");
  }
 }
 static void accuracyIsObservationOnly(){
  for(int accuracy:new int[]{-1,0,1,2,3,7}){
   StrictSensorStats s=stat(4);
   float[] v={Float.intBitsToFloat(0xc20ba058),-0.0f,0};
   s.accept(v,1,3000000000L,0,2000000000L,accuracy);
   v[0]=1;
   check(s.passed(),"accuracy observation must not change original predicate");
   check(s.observedEvents()[0].accuracy==accuracy,"retain supplied accuracy without upgrading");
   check(s.observedEvents()[0].summary().contains("0xc20ba058"),"retain exact negative-fullscale bits");
   check(s.observedEvents()[0].summary().contains("0x80000000"),"retain signed zero bits");
  }
  StrictSensorStats unknown=stat(4);
  unknown.accept(new float[]{0,0,0},1,3000000000L,0,2000000000L);
  check(unknown.observedEvents()[0].summary().contains("accuracy=unknown"),"missing accuracy must not imply reliable");
 }
 public static void main(String[] args){
  originalPredicates();zeroIsObservationOnly();windowsAndFiniteNorm();
  reusedSensorArraysAreCopied();diagnosticsOnlyForAccAndGyro();accuracyIsObservationOnly();
  System.out.println("Strict sensor original PASS predicates + bounded acc/gyro phase/norm/zero/copy observations PASS");
 }
}
