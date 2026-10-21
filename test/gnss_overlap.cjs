// Run the production worker with deterministic RTOS queues and a fake parser.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const assert = require('assert');
const source = fs.readFileSync('src/gnss.cpp', 'utf8');
function body(signature) {
    const start=source.indexOf(signature), open=source.indexOf('{',start);
    assert(start>=0); let depth=0;
    for(let i=open;i<source.length;i++) {
        if(source[i]==='{') ++depth;
        if(source[i]==='}' && --depth===0) return source.slice(start,i+1);
    }
    throw Error('Unclosed function');
}
const shared=source.slice(source.indexOf('struct GnssCommand {'), source.indexOf('\n}\n\nvoid setup_gnss'));
const led=body('void setGnssSearchLed(');
assert(source.includes('xTaskCreatePinnedToCore(gnssWorker, "gps", 4096, nullptr, 1, &gnssTask, 0)'));
assert(!fs.readFileSync('src/lorawan.cpp','utf8').includes('poll_gnss'));
assert(!body('bool get_location()').includes('GPS.'));
assert(!body('bool get_location()').includes('Serial.'));
assert(!source.includes('printGnssStatus'));
const stop=body('void stop_gnss()');
assert(stop.indexOf('xSemaphoreTake') < stop.indexOf('Serial1.end'));
assert(stop.indexOf('xSemaphoreTake') < stop.indexOf('vQueueDelete'));
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'gnss-task-'));
try {
    const binary=path.join(dir,'test');
    cp.execFileSync('c++',['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial' , '-DTRACKER_WEB_DEBUG=0','-x','c++','-','-o',binary],{input:`
#include <cassert>
#include <cstdint>
#include <cstring>
uint32_t now=0;
uint32_t millis() { return now; }
constexpr int HIGH=1, LOW=0,pdTRUE=1;
namespace Board { constexpr int statusLed=40; }
void digitalWrite(int,int) {}
namespace AppConfig { constexpr uint32_t firstGpsTimeoutMs=120000,gpsTimeoutMs=30000,gpsRequiredFixes=3; }
namespace TripControl { bool initial=false; int errors=0;
void markInitialFix() { initial=true; } void preparationFailed(){++errors;} }
int fixLogs=0,statusLogs=0,timeoutLogs=0;
struct {
 template<class... T> void printf(const char* format,T...) {
  if(strstr(format,"Fix acquired"))++fixLogs;
  if(strstr(format,"elapsed="))++statusLogs;
 }
 void println(const char* line){if(strstr(line,"Timeout:"))++timeoutLogs;}
 void write(char) {}
} Serial;
bool onWorker=false,qualify=true;
struct { int queued=300,reads=0; bool available(){assert(onWorker); return queued>0;}
int read(){assert(onWorker); --queued; ++reads; return 'x';} } Serial1;
struct Stat { bool isValid(){ assert(onWorker); return true; }
double hdop(){assert(onWorker);return 1.5;}
uint32_t value(){ assert(onWorker); return 8; } };
struct { Stat satellites,location,hdop;
bool encode(char){assert(onWorker); return true;}
uint32_t charsProcessed(){assert(onWorker); return Serial1.reads;}
uint32_t passedChecksum(){assert(onWorker); return 1;}
uint32_t failedChecksum(){assert(onWorker); return 0;}
} GPS;
struct FixStability { double latitude=13.7,longitude=100.7;
const char* reason="waiting-stable-fixes";char quality='1';uint8_t count=0;
bool observe(){assert(onWorker); return qualify;} };
using TaskHandle_t=void*;
using SemaphoreHandle_t=void*;
struct Queue { size_t size; bool valid=false; unsigned char bytes[256]{}; };
using QueueHandle_t=Queue*;
int xQueueOverwrite(QueueHandle_t q,const void* p){memcpy(q->bytes,p,q->size); q->valid=true; return 1;}
int xQueuePeek(QueueHandle_t q,void* p,int){if(!q->valid)return 0;memcpy(p,q->bytes,q->size);return 1;}
int xQueueReceive(QueueHandle_t q,void* p,int t){if(!xQueuePeek(q,p,t))return 0;q->valid=false;return 1;}
bool stopping=false,acknowledged=false;
int iterations=0,previousReads=0,stepMs=5;
int xTaskNotifyWait(uint32_t,uint32_t,uint32_t* bits,int){*bits=stopping?1:0;return 1;}
int pdMS_TO_TICKS(int ms){return ms;}
void vTaskDelay(int ms){assert(Serial1.reads-previousReads<=128);previousReads=Serial1.reads;
now+=stepMs;if(++iterations==10)stopping=true;}
void xSemaphoreGive(SemaphoreHandle_t){acknowledged=true;}
struct WorkerStopped {};
void vTaskDelete(void*){assert(acknowledged);throw WorkerStopped{};}
bool echoRawNmea=false,hasPreviousFix=false;
int32_t rtc_lat=0,rtc_lon=0;
// Worker logs are counted without coupling parser access to the main task.
${led}
${shared}
${body('void start_gnss_acquisition(bool restart)')}
${body('void service_gnss()')}
enum class GnssStatus { Unavailable, Searching, FixReady, TimedOut };
${body('GnssStatus gnss_status()')}
void runWorker(){
 stopping=false;acknowledged=false;iterations=0;onWorker=true;previousReads=Serial1.reads;
 try{gnssWorker(nullptr);}catch(WorkerStopped&){}
 onWorker=false;assert(acknowledged && iterations==10);
}
int main(){
 Queue commands{sizeof(GnssCommand)},snapshots{sizeof(GnssSnapshot)};
 gnssCommands=&commands;gnssSnapshots=&snapshots;gnssRunning=true;
 start_gnss_acquisition(false);assert(acquisitionTimeout==120000 && acquisitionGeneration==1);
 now=5;start_gnss_acquisition(false);assert(acquisitionStart==0 && acquisitionGeneration==1);
 runWorker();assert(Serial1.reads==300 && fixLogs==1);
 assert(rtc_lat==0 && !TripControl::initial); // Worker must not write RTC/trip.
 service_gnss();assert(rtc_lat==13700000 && rtc_lon==100700000 && !TripControl::initial);
 assert(gnss_status()==GnssStatus::FixReady);
 assert(hasPreviousFix);
 finishAcquisition();assert(!acquiring);
 start_gnss_acquisition(false);assert(acquisitionTimeout==30000 && acquisitionGeneration==2);
 GnssSnapshot snapshot;assert(!readSnapshot(snapshot)); // Reject prior generation.
 qualify=false;Serial1.queued=10;stepMs=500;runWorker();
 assert(statusLogs==4 && fixLogs==1 && timeoutLogs==0);
 assert(readSnapshot(snapshot) && !snapshot.qualified);
 service_gnss();assert(rtc_lat==13700000); // No-fix never overwrites cache with 0,0.
 finishAcquisition();start_gnss_acquisition(false);now+=30000;qualify=true;Serial1.queued=10;
 runWorker();assert(readSnapshot(snapshot) && !snapshot.qualified && snapshot.complete);
 assert(timeoutLogs==1 && fixLogs==1);
 assert(gnss_status()==GnssStatus::TimedOut); // One timeout, no duplicate fix log.
 const auto expiredGeneration=acquisitionGeneration;
 start_gnss_acquisition(true);
 assert(acquisitionGeneration==expiredGeneration+1 && acquisitionStart==now);
 assert(!readSnapshot(snapshot)); // Retry cannot reuse the previous timeout.
 gnssRunning=false;assert(!readSnapshot(snapshot));
}
`});
    cp.execFileSync(binary);
} finally { fs.rmSync(dir,{recursive:true,force:true}); }
