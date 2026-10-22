// Run the complete production audio driver on real host threads with RTOS mocks.
// Explicitly pause a command AFTER dequeue to reproduce the former stale-work race.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const root = path.resolve(__dirname, '..');
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'tracker-audio-'));
try {
    fs.writeFileSync(path.join(dir, 'Arduino.h'), `
#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdint>
constexpr int pdTRUE=1, pdPASS=1, portMAX_DELAY=-1, OUTPUT=1, LOW=0;
inline thread_local bool workerThread=false;
inline std::atomic<bool> workerStarted{false}, pauseDequeued{false}, dequeued{false}, resume{false};
inline std::atomic<unsigned> queueWrites{0};
inline std::mutex hardwareLock;
inline std::vector<int> notes;
inline bool attached=false;
inline int pinLevel=0;
inline const auto epoch=std::chrono::steady_clock::now();
inline uint32_t millis() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now()-epoch).count()/100; // 10x faster.
}
inline void delay(int ms) { std::this_thread::sleep_for(std::chrono::microseconds(ms*100)); }
inline void pinMode(int,int) {}
inline void digitalWrite(int,int level) { std::lock_guard<std::mutex> l(hardwareLock); pinLevel=level; }
inline bool ledcAttach(int,int,int) {
    assert(workerThread); std::lock_guard<std::mutex> l(hardwareLock); attached=true; return true;
}
inline void ledcWriteTone(int,int hz) {
    assert(workerThread); std::lock_guard<std::mutex> l(hardwareLock); if(hz)notes.push_back(hz);
}
inline void ledcDetach(int) {
    assert(workerThread); std::lock_guard<std::mutex> l(hardwareLock); attached=false;
}
struct Semaphore { std::mutex lock; std::condition_variable cv; int count=0; };
using SemaphoreHandle_t=Semaphore*;
inline SemaphoreHandle_t xSemaphoreCreateBinary() { return new Semaphore; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { auto s=new Semaphore; s->count=1; return s; }
inline int xSemaphoreTake(SemaphoreHandle_t s,int timeout) {
    std::unique_lock<std::mutex> l(s->lock);
    if(timeout==0 && !s->count)return 0;
    s->cv.wait(l,[&]{return s->count>0;}); --s->count; return 1;
}
inline void xSemaphoreGive(SemaphoreHandle_t s) {
    std::lock_guard<std::mutex> l(s->lock); ++s->count; s->cv.notify_one();
}
inline void vSemaphoreDelete(SemaphoreHandle_t s) { delete s; }
struct Queue {
    size_t capacity,size; std::mutex lock; std::condition_variable cv;
    std::deque<std::vector<char>> entries;
    Queue(size_t c,size_t s):capacity(c),size(s){}
};
using QueueHandle_t=Queue*;
inline QueueHandle_t xQueueCreate(int c,size_t s) { return new Queue(c,s); }
inline int xQueueSend(QueueHandle_t q,const void* data,int timeout) {
    std::unique_lock<std::mutex> l(q->lock);
    if(timeout==0 && q->entries.size()==q->capacity)return 0;
    q->cv.wait(l,[&]{return q->entries.size()<q->capacity;});
    const char* p=static_cast<const char*>(data);
    q->entries.emplace_back(p,p+q->size); ++queueWrites; q->cv.notify_all(); return 1;
}
inline int xQueueReceive(QueueHandle_t q,void* data,int timeout) {
    {
        std::unique_lock<std::mutex> l(q->lock);
        if(timeout==0 && q->entries.empty())return 0;
        q->cv.wait(l,[&]{return !q->entries.empty();});
        memcpy(data,q->entries.front().data(),q->size);q->entries.pop_front();q->cv.notify_all();
    }
    if(workerThread && pauseDequeued.exchange(false)) {
        dequeued=true;
        while(!resume)std::this_thread::yield();
    }
    return 1;
}
inline void vQueueDelete(QueueHandle_t q) { delete q; }
inline int xTaskCreate(void(*fn)(void*),const char*,int,void* arg,int,void*) {
    std::thread([=]{workerThread=true;workerStarted=true;fn(arg);}).detach(); return pdPASS;
}
inline struct {
    void println(const char*) {}
    template<class... T> void printf(const char*,T...) {}
} Serial;
`);
    fs.writeFileSync(path.join(dir, 'board_pins.h'), '#pragma once\nnamespace Board { constexpr int buzzer=4; }\n');
    fs.writeFileSync(path.join(dir, 'test.cpp'), `
#include <Arduino.h>
#include <audio_feedback.h>
#include <cstdlib>
using AudioFeedback::Event;
template<class Predicate> void await(Predicate predicate) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(!predicate()) {
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}
size_t noteCount() { std::lock_guard<std::mutex> l(hardwareLock);return notes.size(); }
void quiet() { std::lock_guard<std::mutex> l(hardwareLock);assert(!attached && pinLevel==LOW); }
int main(int argc,char**) {
    AudioFeedback::begin();
    await([]{return workerStarted.load();});
    if(argc==1) {
        // A stale command is outside the queue before cancellation occurs.
        pauseDequeued=true;
        AudioFeedback::play(Event::Sos);
        await([]{return dequeued.load();});
        std::atomic<bool> silent{false};
        std::thread caller([&]{AudioFeedback::silence();silent=true;});
        // Verify a barrier was queued (rather than relying on a timing sleep).
        await([]{return queueWrites>=2;});
        assert(!silent);
        resume=true;
        caller.join();
        quiet(); assert(noteCount()==0); // Old dequeued melody never starts.
        AudioFeedback::startLoop(Event::Sos);
        await([]{return noteCount()>0;});
        AudioFeedback::silence();quiet();
        const auto count=noteCount(); delay(500);assert(noteCount()==count);
        // Synchronous requests also use the worker, not the calling thread.
        AudioFeedback::playAndWait(Event::LoRaConnected);quiet();
    } else if(argc>5) {
        AudioFeedback::startLoop(Event::SosAcknowledged);
        await([]{return noteCount()>=8;}); // At least two complete four-note phrases.
        AudioFeedback::silence(); quiet();
        { std::lock_guard<std::mutex> l(hardwareLock);
          for(int i=0;i<8;++i) assert(notes[i]==(i%4==3 ? 2640 : 2900)); }
        const auto count=noteCount(); delay(500); assert(noteCount()==count);
    } else if(argc>4) {
        AudioFeedback::startLoop(Event::Sos);
        await([]{return noteCount()>0;});
        AudioFeedback::prepareSleep();quiet();
        const auto count=noteCount();
        AudioFeedback::startLoop(Event::LoRaJoining);
        AudioFeedback::playAndWait(Event::LoRaConnected);
        delay(500);quiet();assert(noteCount()==count);
    } else {
        // Cancel either a working loop or SOS/Fall with queued ACK feedback.
        const Event loop=argc==2 ? Event::LoRaJoining : argc==3 ? Event::Sos : Event::Fall;
        AudioFeedback::startLoop(loop);
        await([]{return noteCount()>0;});
        AudioFeedback::play(Event::LoRaConnected);
        AudioFeedback::shutdown(); quiet();
        const auto count=noteCount();
        {
            std::lock_guard<std::mutex> l(hardwareLock);
            assert(count>=4);
            assert(notes[count-3]==2637 && notes[count-2]==2093 && notes[count-1]==1568);
        }
        // Producers racing shutdown cannot revive the buzzer while held off.
        std::thread producer([]{
            for(int i=0;i<100;++i) {
                AudioFeedback::startLoop(Event::Sos);
                AudioFeedback::play(Event::Error);
                AudioFeedback::stopLoop(Event::Sos);
            }
        });
        AudioFeedback::playAndWait(Event::DeepSleep);
        AudioFeedback::shutdown();
        producer.join(); delay(1000);quiet();assert(noteCount()==count);
        AudioFeedback::silence();quiet();assert(noteCount()==count);
    }
    std::_Exit(0); // Worker is deliberately permanent, as on the MCU.
}
`);
    const binary = path.join(dir, 'test');
    cp.execFileSync('c++', ['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial' , '-DTRACKER_WEB_DEBUG=0', '-pthread', '-I'+dir, '-I'+path.join(root,'include'),
        path.join(root,'src/audio_feedback.cpp'), path.join(dir,'test.cpp'), '-o',binary]);
    for (const args of [[],['working'],['sos','ack'],['fall','ack','held'],['timed','sleep','gpio','hold'],['application','ack','repeats','fourth','note']])
        cp.execFileSync(binary,args,{timeout:10000});
    console.log('Audio worker ownership, stale dequeue cancellation and terminal shutdown tests passed');
} finally {
    fs.rmSync(dir,{recursive:true,force:true});
}
