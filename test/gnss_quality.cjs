// Exercise the actual quality filter: six satellites alone must not qualify.
const fs=require('fs'),cp=require('child_process'),os=require('os'),path=require('path');
const source=fs.readFileSync(path.join(__dirname,'../src/gnss.cpp'),'utf8');
const filter=source.slice(source.indexOf('struct FixStability {'),source.indexOf('// Only the GPS task'));
const dir=fs.mkdtempSync(path.join(os.tmpdir(),'tracker-quality-'));
try{
    const bin=path.join(dir,'test');
    cp.execFileSync('c++',['-std=c++17','-I'+path.join(__dirname,'../include'),'-x','c++','-','-o',bin],{input:`
#include <cassert>
#include <cstring>
#include <cstdint>
#include <tracker_app.h>
uint32_t now=0;
uint32_t millis(){return now;}
struct TinyGPSCustom {
    bool updated=false;const char* quality="1";
    bool isUpdated(){return updated;}
    const char* value(){updated=false;return quality;}
} gpFixQuality,gnFixQuality;
struct Field {
    bool valid=true;uint32_t ageMs=0,number=6;double dilution=1.5;
    bool isValid(){return valid;}
    uint32_t age(){return ageMs;}
    uint32_t value(){return number;}
    double hdop(){return dilution;}
    double lat(){return 13.7;}double lng(){return 100.7;}
};
struct {Field time,location,hdop,satellites;} GPS;
double distance=0;
struct TinyGPSPlus {static double distanceBetween(double,double,double,double){return distance;}};
${filter}
bool observe(FixStability& f,uint32_t epoch){
    now+=1000;GPS.time.number=epoch*100;gpFixQuality.updated=true;return f.observe();
}
int main(){
    FixStability f;
    GPS.hdop.dilution=3.5;assert(!observe(f,1));
    assert(!strcmp(f.reason,"HDOP-too-high") && f.count==0);
    GPS.hdop.dilution=1.5;gpFixQuality.quality="0";assert(!observe(f,2));
    assert(!strcmp(f.reason,"fix-type"));
    gpFixQuality.quality="1";GPS.location.ageMs=2001;assert(!observe(f,3));
    assert(!strcmp(f.reason,"stale-location"));
    GPS.location.ageMs=0;GPS.satellites.number=5;assert(!observe(f,4));
    assert(!strcmp(f.reason,"too-few-satellites"));
    GPS.satellites.number=6;
    assert(!observe(f,5));assert(!observe(f,6));assert(observe(f,7));
    assert(f.count==3); // Same original thresholds: 3 distinct fixes over 2s.
    f=FixStability{};assert(!observe(f,8));
    assert(!observe(f,8) && f.count==1); // Duplicate epoch never counts twice.
    now+=3000;assert(!observe(f,9) && f.count==1);
    assert(!strcmp(f.reason,"fix-gap"));
    distance=1000;assert(!observe(f,10) && f.count==1);
    assert(!strcmp(f.reason,"position-jump"));
    distance=0; f=FixStability{}; GPS.hdop.dilution=2.51;
    assert(!observe(f,11) && f.count==0);
    assert(!strcmp(f.reason,"HDOP-too-high"));
    GPS.hdop.dilution=2.50;
    assert(!observe(f,12)); assert(!observe(f,13)); assert(observe(f,14));
    f=FixStability{}; GPS.hdop.dilution=2.30;
    assert(!observe(f,15)); assert(!observe(f,16)); assert(observe(f,17));
}
`});
    cp.execFileSync(bin);
    console.log('GPS six-satellite quality rejection reasons and unchanged stability thresholds passed');
}finally{fs.rmSync(dir,{recursive:true,force:true});}
