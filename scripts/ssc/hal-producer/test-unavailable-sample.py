#!/usr/bin/env python3
"""Extract real HAL methods from baseline/candidate and test cache validity gates.
No Android services, SSC traffic, device access, or generated sensor injection.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def function(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 0
    for end in range(brace, len(text)):
        if text[end] == '{': depth += 1
        elif text[end] == '}':
            depth -= 1
            if depth == 0: return text[start:end+1]
    raise ValueError('unclosed function ' + marker)


PREFIX = r'''
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
namespace android {
static int clockReads;
int64_t elapsedRealtimeNano() { return 123456789 + ++clockReads; }
}
namespace aidl { namespace android { namespace hardware { namespace sensors {
enum class SensorStatus {UNRELIABLE=0, ACCURACY_LOW=1, ACCURACY_MEDIUM=2, ACCURACY_HIGH=3};
enum class SensorType {ACCELEROMETER=1,GYROSCOPE=4,OTHER=99};
struct Payload {
 struct Vec3 {float x,y,z;SensorStatus status;};
 enum class Tag {vec3};
 Vec3 value;
 template<Tag T> void set(Vec3 v) {(void)T;value=v;}
};
struct Event {int sensorHandle;SensorType sensorType;int64_t timestamp;Payload payload;};
struct SensorInfo {int sensorHandle;SensorType type;};
class Sensor {
public:
 using EventPayload=Payload;
 SensorInfo mSensorInfo;
 explicit Sensor(SensorType type):mSensorInfo{42,type}{}
 virtual ~Sensor()=default;
 virtual void readEventPayload(EventPayload&)=0;
 std::vector<Event> readEvents();
};
class AccelSensor:public Sensor {
public: AccelSensor():Sensor(SensorType::ACCELEROMETER){}
 void readEventPayload(EventPayload&) override;
};
class GyroSensor:public Sensor {
public: GyroSensor():Sensor(SensorType::GYROSCOPE){}
 void readEventPayload(EventPayload&) override;
};
class OtherSensor:public Sensor {
public: int calls=0;OtherSensor():Sensor(SensorType::OTHER){}
 void readEventPayload(EventPayload& p) override {++calls;p.value={1,2,3,SensorStatus::ACCURACY_LOW};}
};
}}}}
'''
HUB = r'''
static constexpr auto kStale=std::chrono::milliseconds(2000);
class SscHub {
public:
 Sample initial, replacement;
 std::chrono::steady_clock::time_point at;
 int accelReads=0,gyroReads=0;
 static SscHub& Get(){static SscHub hub;return hub;}
 static Sample Fresh(const Sample&,std::chrono::steady_clock::time_point);
 Sample Accel(){++accelReads;return Fresh(accelReads==1?initial:replacement,at);}
 Sample Gyro(){++gyroReads;return Fresh(gyroReads==1?initial:replacement,at);}
 void reset(Sample s,bool expired=false){initial=s;replacement=Sample();accelReads=gyroReads=0;
  at=std::chrono::steady_clock::now()-(expired?std::chrono::seconds(10):std::chrono::seconds(0));}
};
'''
TESTS = r'''
using namespace aidl::android::hardware::sensors;
static void checkSensor(Sensor& sensor,bool gyro) {
 auto& hub=gaokun3::SscHub::Get();
 auto singleRead=[&](){assert(hub.accelReads==(gyro?0:1)&&hub.gyroReads==(gyro?1:0));};
 auto observe=[&](){android::clockReads=0;auto events=sensor.readEvents();singleRead();return events;};
 // Fresh default cache: there is no measurement, irrespective of its zeros.
 hub.reset(gaokun3::Sample());auto events=observe();
#ifdef CANDIDATE
 assert(events.empty()&&android::clockReads==0);
#else
 assert(events.size()==1&&android::clockReads==1);
 assert(events[0].payload.value.x==0&&events[0].payload.value.y==0&&events[0].payload.value.z==0);
 assert(events[0].payload.value.status==SensorStatus::UNRELIABLE);
#endif
 // Even nonzero garbage with valid=false must not be converted to an event.
 gaokun3::Sample absent;absent.v[0]=17;absent.accuracy=3;hub.reset(absent);events=observe();
#ifdef CANDIDATE
 assert(events.empty()&&android::clockReads==0);
#else
 assert(events.size()==1&&events[0].payload.value.x==17);
#endif
 // The actual Fresh implementation converts an expired valid sample to absence.
 gaokun3::Sample present;present.valid=true;present.v[0]=3;present.v[1]=4;present.v[2]=9.8f;present.accuracy=3;
 hub.reset(present,true);events=observe();
#ifdef CANDIDATE
 assert(events.empty()&&android::clockReads==0);
#else
 assert(events.size()==1&&events[0].payload.value.x==0);
#endif
 // Genuine zero data is preserved at every accuracy, including unreliable0.
 for(int accuracy=0;accuracy<=4;++accuracy) {
  gaokun3::Sample zero;zero.valid=true;zero.v[1]=-0.0f;zero.accuracy=accuracy;
  hub.reset(zero);events=observe();
  assert(events.size()==1&&android::clockReads==1);
  assert(events[0].payload.value.x==0&&events[0].payload.value.y==0&&events[0].payload.value.z==0);
  assert(events[0].payload.value.status==(accuracy<=3?static_cast<SensorStatus>(accuracy):SensorStatus::UNRELIABLE));
 }
 // One snapshot only: the mock's next lookup invalidates the cache. A guard
 // that reads twice would pass once then publish a zero, and fail this check.
 hub.reset(present);events=observe();assert(events.size()==1);
 assert(events[0].sensorHandle==42&&events[0].sensorType==sensor.mSensorInfo.type);
 assert(events[0].payload.value.x==3&&events[0].payload.value.y==4&&events[0].payload.value.z==9.8f);
 assert(events[0].payload.value.status==SensorStatus::ACCURACY_HIGH);
 // No hidden value filtering: presence gating is not a NaN/physical-value fix.
 present.v[0]=NAN;hub.reset(present);events=observe();
 assert(events.size()==1&&std::isnan(events[0].payload.value.x));
 // Repeated unavailable reads must not generate fresh timestamps in candidate.
 hub.reset(gaokun3::Sample());android::clockReads=0;
 for(int i=0;i<100;++i){events=sensor.readEvents();
#ifdef CANDIDATE
 assert(events.empty());
#else
 assert(events.size()==1);
#endif
 }
#ifdef CANDIDATE
 assert(android::clockReads==0);
#else
 assert(android::clockReads==100);
#endif
}
int main(){
 AccelSensor accel;GyroSensor gyro;checkSensor(accel,false);checkSensor(gyro,true);
 OtherSensor other;auto& hub=gaokun3::SscHub::Get();hub.reset(gaokun3::Sample());
 android::clockReads=0;auto event=other.readEvents();
 assert(event.size()==1&&other.calls==1&&!hub.accelReads&&!hub.gyroReads&&android::clockReads==1);
 assert(event[0].payload.value.x==1&&event[0].payload.value.status==SensorStatus::ACCURACY_LOW);
#ifdef CANDIDATE
 std::cout<<"PASS candidate: invalid/stale absence emits nothing or timestamp; valid zero preserved; single snapshot and other sensor path verified\n";
#else
 std::cout<<"PASS baseline reproduction: default/expired cache creates timestamped zero events\n";
#endif
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    cache = Path.home()/'.cache/hos-r19-fusion-probe/producer-audit'
    ap.add_argument('--baseline', type=Path, default=cache/'baseline-src')
    ap.add_argument('--candidate', type=Path, default=cache/'candidate-src')
    args = ap.parse_args()
    for mode, root in [('baseline', args.baseline), ('candidate', args.candidate)]:
        cpp=(root/'Sensor.cpp').read_text()
        hubcpp=(root/'SscHub.cpp').read_text()
        hubh=(root/'SscHub.h').read_text()
        sample=function(hubh,'struct Sample')+';'
        accuracy=function(cpp,'::aidl::android::hardware::sensors::SensorStatus SscAccuracyToStatus')
        read=function(cpp,'std::vector<Event> Sensor::readEvents()')
        accel=function(cpp,'void AccelSensor::readEventPayload')
        gyro=function(cpp,'void GyroSensor::readEventPayload')
        fresh=function(hubcpp,'Sample SscHub::Fresh')
        fixture=PREFIX+'\nnamespace gaokun3 {\n'+sample+HUB+fresh+'\n}\n'+accuracy
        fixture+='\nnamespace aidl { namespace android { namespace hardware { namespace sensors {\n'
        fixture+=read+'\n'+accel+'\n'+gyro+'\n}}}}\n'+TESTS
        with tempfile.TemporaryDirectory(prefix='hos-sensor-producer-'+mode+'-') as tmp:
            p=Path(tmp);(p/'fixture.cpp').write_text(fixture)
            cmd=[os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror',
                 '-fsanitize=address,undefined']
            if mode=='candidate': cmd+=['-DCANDIDATE=1']
            subprocess.run(cmd+[str(p/'fixture.cpp'),'-o',str(p/'fixture')],check=True)
            subprocess.run([str(p/'fixture')],check=True)


if __name__=='__main__': main()
