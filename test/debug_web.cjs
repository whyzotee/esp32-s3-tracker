// Exercise the enabled production server with deterministic Wi-Fi/RTOS mocks.
const fs = require('fs');
const cp = require('child_process');
const os = require('os');
const path = require('path');
const assert = require('assert');
const vm = require('vm');
const root = path.resolve(__dirname,'..');
const dir = fs.mkdtempSync(path.join(os.tmpdir(),'tracker-web-'));
async function main() {
try {
    fs.writeFileSync(path.join(dir,'lorawan.h'), '#pragma once\n#include <cstdint>\nnamespace LoRaCredentials { constexpr uint64_t devEui=0x0011223344556677ULL; }\n');
    fs.writeFileSync(path.join(dir,'Arduino.h'), `
#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <type_traits>
class String {
    std::string value;
public:
    String()=default;
    String(const char* text):value(text){}
    template<class T, typename=std::enable_if_t<std::is_integral_v<T>>>
    String(T n):value(std::to_string(n)){}
    String(float n,int digits) {std::ostringstream s;s<<std::fixed<<std::setprecision(digits)<<n;value=s.str();}
    const char* c_str()const{return value.c_str();}
    void reserve(size_t n){value.reserve(n);}
    String& operator=(const char* s){value=s;return *this;}
    String& operator+=(const char* s){value+=s;return *this;}
    String& operator+=(const String& s){value+=s.value;return *this;}
    String& operator+=(char c){value+=c;return *this;}
    template<class T, typename=std::enable_if_t<std::is_integral_v<T>>>
    String& operator+=(T n){value+=std::to_string(n);return *this;}
};
inline uint32_t now=100;
inline uint32_t millis(){return now;}
inline struct {
    uint64_t getEfuseMac(){return 0x123456;}
    uint32_t getFreeHeap(){return 123000;}
} ESP;
inline struct {
    void println(const char*){}
    template<class... T>void printf(const char*,T...){}
} Serial;
using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
constexpr int pdPASS=1,eSetBits=1,portMAX_DELAY=-1;
using TaskHandle_t=void*;
struct Semaphore {bool given=false;};
using SemaphoreHandle_t=Semaphore*;
inline bool failTask=false;
inline unsigned notifications=0;
inline void(*pendingTask)(void*)=nullptr;
inline SemaphoreHandle_t xSemaphoreCreateBinary(){return new Semaphore;}
inline void vSemaphoreDelete(SemaphoreHandle_t s){delete s;}
inline int xTaskCreatePinnedToCore(void(*fn)(void*),const char*,int,void*,int,TaskHandle_t* h,int core){
    assert(core==0);if(failTask)return 0;pendingTask=fn;*h=reinterpret_cast<void*>(1);return pdPASS;
}
inline int xTaskNotify(TaskHandle_t,unsigned value,int){notifications=value;return 1;}
inline int xTaskNotifyWait(unsigned,unsigned,uint32_t* out,int){*out=notifications;notifications=0;return 1;}
inline void xSemaphoreGive(SemaphoreHandle_t s){s->given=true;}
struct TaskExit{};
[[noreturn]] inline void vTaskDelete(void*){throw TaskExit{};}
inline void xSemaphoreTake(SemaphoreHandle_t s,int){
    if(!s->given){assert(pendingTask);try{pendingTask(nullptr);}catch(TaskExit&){}}
    assert(s->given);s->given=false;
}
inline int pdMS_TO_TICKS(int ms){return ms;}
inline void vTaskDelay(int){assert(false);}
`);
    fs.writeFileSync(path.join(dir,'WiFi.h'), `
#pragma once
#include <Arduino.h>
constexpr int WIFI_OFF=0,WIFI_AP=1;
struct IPAddress {IPAddress(int,int,int,int){}};
inline struct {
    bool fail=false,on=false;
    std::string ssid,password;
    bool mode(int mode){on=mode!=WIFI_OFF;return !fail;}
    bool softAPConfig(IPAddress,IPAddress,IPAddress){return !fail;}
    bool softAP(const char* s,const char* p){ssid=s;password=p;return !fail;}
    void softAPdisconnect(bool){on=false;}
} WiFi;
`);
    fs.writeFileSync(path.join(dir,'WebServer.h'), `
#pragma once
#include <Arduino.h>
constexpr int HTTP_GET=1;
class WebServer {
public:
    std::map<std::string,std::function<void()>> routes;
    std::function<void()> missing;
    String response;
    int responseCode=0;
    bool started=false;
    WebServer(int port){assert(port==80);}
    void on(const char* path,int method,std::function<void()> fn){assert(method==HTTP_GET);routes[path]=fn;}
    void onNotFound(std::function<void()> fn){missing=fn;}
    void begin(){started=true;}
    void stop(){started=false;}
    void handleClient(){assert(false);}
    void sendHeader(const char* key,const char* value){assert(std::string(key)=="Cache-Control");assert(std::string(value)=="no-store");}
    void send(int code,const char*,const String& body){responseCode=code;response=body;}
    void send_P(int code,const char* type,const char* body){send(code,type,String(body));}
};
`);
    fs.writeFileSync(path.join(dir,'test.cpp'), `
#include <iostream>
#include "debug_web.cpp"
int main(){
    assert(!DebugWeb::active());
    WiFi.fail=true;DebugWeb::begin();assert(!DebugWeb::active() && !WiFi.on);
    WiFi.fail=false;failTask=true;DebugWeb::begin();assert(!DebugWeb::active() && !WiFi.on);
    failTask=false;DebugWeb::begin();assert(DebugWeb::active() && WiFi.on);
    assert(WiFi.ssid=="Tracker-Debug-001122");
    DebugWeb::begin(); // Idempotent, retains the original owner.
    DebugWeb::State s;s.phase=2;s.gps=1;s.initialFix=true;s.alarm=true;
    s.batteryValid=true;s.batteryMv=4000;s.batteryPercent=75;s.radioWaitMs=12345;
    DebugWeb::publish(s);DebugWeb::joined(true);
    DebugWeb::gpsProgress(2,7,true);
    DebugWeb::telemetry(13.7f,100.7f,1,24);
    DebugWeb::transmission(2,true,2);
    const auto feed=[](const char* text){DebugWeb::recordSerial(
        reinterpret_cast<const uint8_t*>(text),strlen(text));};
    feed("[APP] Downlink port=11 command=5\\r\\n");
    std::cout<<statusJson().c_str()<<"\\n";
    for(int i=0;i<80;++i){++now;feed("bounded ring\\n");}
    feed("quote\\\" slash\\\\ tab\\t <script>\\n");
    DebugWeb::telemetry(NAN,INFINITY,4,0);
    std::cout<<statusJson().c_str()<<"\\n";
    DebugWeb::stop();assert(!DebugWeb::active() && !WiFi.on && !server.started);
    DebugWeb::stop(); // Safe before sleep after the lifecycle already stopped it.
    assert(server.routes.size()==2);
    server.routes.at("/")();assert(server.responseCode==200);
    server.routes.at("/api/status")();assert(server.responseCode==200);
    server.missing();assert(server.responseCode==404);
}
`);
    const binary=path.join(dir,'test');
    cp.execFileSync('c++',['-std=c++17', '-DTRACKER_LOG_HOST_TEST', '-DTrackerLog=Serial','-DTRACKER_WEB_DEBUG=1','-I'+dir,'-I'+path.join(root,'include'),
        '-I'+path.join(root,'src'),path.join(dir,'test.cpp'),'-o',binary]);
    const [first,last]=cp.execFileSync(binary,{encoding:'utf8'}).trim().split('\n').map(JSON.parse);
    assert.equal(first.status.phase,'Travelling');
    assert.equal(first.status.gps,'FixReady');
    assert.equal(first.status.satellites,7);
    assert.equal(first.status.battery_mv,4000);
    assert.equal(first.status.network_ack,true);
    assert.equal(first.status.radio_wait_ms,12345);
    assert(first.events.some(e=>e.message.includes('command=5')));
    assert.equal(last.events.length,64);
    assert.equal(last.status.latitude,null);
    assert.equal(last.status.longitude,null);
    assert.equal(last.events.at(-1).message,'quote" slash\\ tab\t <script>');

    // Execute the actual embedded page script with a fake browser DOM.
    const page=fs.readFileSync(path.join(root,'include/debug_web_page.h'),'utf8');
    assert(!/<style|stylesheet|https?:\/\/|innerHTML/.test(page));
    const script=page.match(/<script>([\s\S]*?)<\/script>/)[1];
    const elements=Object.fromEntries(['refresh','auto','status','events','connection'].map(id=>[
        id,{textContent:'',checked:true,addEventListener(){}}]));
    const context={
        document:{getElementById:id=>elements[id]},AbortController,Date,
        setInterval(){},setTimeout(){},clearTimeout(){},
        fetch:async(url,options)=>{assert.equal(url,'/api/status');assert.equal(options.cache,'no-store');return {ok:true,json:async()=>last};}
    };
    vm.createContext(context);
    vm.runInContext(script,context);
    await new Promise(resolve=>setImmediate(resolve));
    assert.equal(JSON.parse(elements.status.textContent).latitude,null);
    assert(elements.events.textContent.includes('<script>')); // Literal text, not HTML.
    context.fetch=async()=>{throw new Error('offline');};
    await vm.runInContext('refresh()',context);
    assert(elements.connection.textContent.includes('Disconnected'));
    console.log('Debug web AP failure/stop, read-only routes, JSON/ring and plain HTML tests passed');
} finally {fs.rmSync(dir,{recursive:true,force:true});}
}
main().catch(error=>{console.error(error);process.exitCode=1;});
