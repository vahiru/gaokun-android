#!/usr/bin/env python3
"""Extract real postEvents bodies and verify empty vectors have no side effects.
A checked vector stand-in detects baseline front() misuse without invoking C++ UB.
No FMQ, binder, wakelock or hardware operations are performed.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def body(source):
    start=source.index('    void postEvents(const std::vector<Event>& events, bool wakeup) override {')
    brace=source.index('{',start);depth=0
    for end in range(brace,len(source)):
        if source[end]=='{':depth+=1
        elif source[end]=='}':
            depth-=1
            if not depth:return source[brace:end+1]
    raise ValueError('unterminated postEvents')


PREFIX=r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <vector>
struct Event {int data;};
struct EmptyFront {};
static int frontCalls, writes, wakes, lockUpdates, eventsWritten;
struct EventVector:std::vector<Event> {
 using std::vector<Event>::vector;
 const Event& front()const {++frontCalls;if(empty())throw EmptyFront();return std::vector<Event>::front();}
};
struct Queue {bool succeeds=true;bool write(const Event*e,size_t n){assert(e&&n);++writes;eventsWritten=n;return succeeds;}};
struct Flag {void wake(uint32_t f){assert(f==1);++wakes;}};
struct BnSensors {static constexpr uint32_t EVENT_QUEUE_FLAG_BITS_READ_AND_PROCESS=1;};
struct Sensors {
 std::mutex mWriteLock;Queue *mEventQueue;Flag *mEventQueueFlag;
 void updateWakeLock(size_t n,int handled){assert(n>0&&!handled);++lockUpdates;}
 void postEvents(const EventVector& events,bool wakeup);
};
'''
TESTS=r'''
static void reset(){frontCalls=writes=wakes=lockUpdates=eventsWritten=0;}
int main(){
 Queue q;Flag flag;Sensors s;s.mEventQueue=&q;s.mEventQueueFlag=&flag;
 for(bool wakeup:{false,true}) {
  reset();bool bad=false;try{s.postEvents(EventVector{},wakeup);}catch(const EmptyFront&){bad=true;}
#ifdef CANDIDATE
  assert(!bad&&!frontCalls&&!writes&&!wakes&&!lockUpdates);
#else
  assert(bad&&frontCalls==1&&!writes&&!wakes&&!lockUpdates);
#endif
 }
 reset();s.postEvents(EventVector{{0}},false);
 assert(frontCalls==1&&writes==1&&wakes==1&&!lockUpdates&&eventsWritten==1);
 reset();s.postEvents(EventVector{{0},{42}},true);
 assert(frontCalls==1&&writes==1&&wakes==1&&lockUpdates==1&&eventsWritten==2);
 reset();q.succeeds=false;s.postEvents(EventVector{{0}},true);
 assert(frontCalls==1&&writes==1&&!wakes&&!lockUpdates);
 reset();q.succeeds=true;s.mEventQueueFlag=nullptr;s.postEvents(EventVector{{0}},true);
 assert(frontCalls==1&&writes==1&&!wakes&&!lockUpdates);
 reset();s.mEventQueue=nullptr;s.postEvents(EventVector{{0}},true);
 assert(!frontCalls&&!writes&&!wakes&&!lockUpdates);
#ifdef CANDIDATE
 std::cout<<"PASS candidate: empty events do not call front/FMQ/wake/wakelock; real zero/nonempty paths unchanged\n";
#else
 std::cout<<"PASS baseline reproduction: empty postEvents calls vector.front() without a size check\n";
#endif
}
'''

def main():
    ap=argparse.ArgumentParser(description=__doc__);cache=Path.home()/'.cache/hos-r19-fusion-probe/producer-audit'
    ap.add_argument('--baseline',type=Path,default=cache/'baseline-src');ap.add_argument('--candidate',type=Path,default=cache/'candidate-src');a=ap.parse_args()
    for label,root in [('baseline',a.baseline),('candidate',a.candidate)]:
        src=(root/'include/sensors-impl/Sensors.h').read_text()
        fixture=PREFIX+'\nvoid Sensors::postEvents(const EventVector& events,bool wakeup) '+body(src)+'\n'+TESTS
        with tempfile.TemporaryDirectory(prefix='hos-empty-post-'+label+'-') as tmp:
            p=Path(tmp);(p/'fixture.cpp').write_text(fixture)
            cmd=[os.environ.get('CXX','c++'),'-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined']
            if label=='candidate':cmd+=['-DCANDIDATE=1']
            subprocess.run(cmd+[str(p/'fixture.cpp'),'-o',str(p/'fixture')],check=True)
            subprocess.run([str(p/'fixture')],check=True)
if __name__=='__main__':main()
