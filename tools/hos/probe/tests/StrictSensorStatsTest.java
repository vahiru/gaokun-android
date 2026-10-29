package org.gaokun.hosprobe;
public class StrictSensorStatsTest {
 static void check(boolean condition){if(!condition)throw new AssertionError();}
 static StrictSensorStats stat(){StrictSensorStats s=new StrictSensorStats();s.registered=true;return s;}
 public static void main(String[] args){
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
  System.out.println("Strict sensor finite/warmup/recovery/timestamp/registration/window checks PASS");
 }
}
