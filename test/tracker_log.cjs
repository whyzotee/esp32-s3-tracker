// Compile the production console tee in both enabled and zero-overhead modes.
const fs=require('fs'), cp=require('child_process'), os=require('os'), path=require('path');
const root=path.resolve(__dirname,'..'),dir=fs.mkdtempSync(path.join(os.tmpdir(),'tracker-console-'));
try {
    fs.writeFileSync(path.join(dir,'Arduino.h'), `
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
class Print {
public:
    virtual ~Print()=default;
    virtual size_t write(uint8_t)=0;
    virtual size_t write(const uint8_t* b,size_t n){for(size_t i=0;i<n;i++)write(b[i]);return n;}
    size_t write(const char* s){return write(reinterpret_cast<const uint8_t*>(s),strlen(s));}
    void println(const char* s){write(s);write("\\r\\n");}
    void printf(const char* format,...){char b[256];va_list args;va_start(args,format);vsnprintf(b,sizeof(b),format,args);va_end(args);write(b);}
};
inline struct : Print {
    using Print::write;
    bool connected=true;int flushes=0;std::string output;
    size_t write(uint8_t c)override{if(!connected)return 0;output+=char(c);return 1;}
    size_t write(const uint8_t* b,size_t n)override{if(!connected)return 0;output.append(reinterpret_cast<const char*>(b),n);return n;}
    void flush(){++flushes;}
} Serial;
`);
    fs.writeFileSync(path.join(dir,'test.cpp'), `
#include <cassert>
#include <tracker_log.h>
#include "tracker_log.cpp"
std::string captured;
#if TRACKER_WEB_DEBUG
namespace DebugWeb {
void recordSerial(const uint8_t* b,size_t n){captured.append(reinterpret_cast<const char*>(b),n);}
}
#endif
int main(){
    TrackerLog.printf("[GPS] satellites=%d HDOP=%.2f\\n",6,3.5);
    TrackerLog.println("[AUDIO] error melody queued");
    TrackerLog.flush();
    assert(Serial.flushes==1);
    const auto expected=Serial.output;
#if TRACKER_WEB_DEBUG
    assert(captured==expected && !captured.empty());
    Serial.connected=false;
    TrackerLog.println("still visible on Wi-Fi");
    assert(Serial.output==expected && captured.find("still visible on Wi-Fi")!=std::string::npos);
#else
    assert(captured.empty() && !expected.empty());
#endif
}
`);
    for(const flag of [0,1]){
        const bin=path.join(dir,'test'+flag);
        cp.execFileSync('c++',['-std=c++17','-DTRACKER_WEB_DEBUG='+flag,'-I'+dir,'-I'+path.join(root,'include'),
            '-I'+path.join(root,'src'),path.join(dir,'test.cpp'),'-o',bin]);
        cp.execFileSync(bin);
    }
    console.log('Serial/web tee formatting, disconnected USB capture and disabled alias tests passed');
}finally{fs.rmSync(dir,{recursive:true,force:true});}
