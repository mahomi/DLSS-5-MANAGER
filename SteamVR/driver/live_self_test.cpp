#include "live_control.h"
#include <cstdio>
#include <thread>
#include <atomic>
int main(){
    int failures=0;auto check=[&](bool ok,const char* label){printf("%s: %s\n",label,ok?"PASS":"FAIL");if(!ok)++failures;};
    const uint64_t now=vrlive::unixMs();
    auto make=[&](bool active,bool nr,uint64_t lease){return std::string("{\"version\":1,\"active\":")+(active?"true":"false")+",\"nr\":"+(nr?"true":"false")+",\"fast\":true,\"target\":2,\"stability\":0.9,\"work_scale\":0.25,\"intensity\":1,\"local_tone\":0.5,\"local_structure\":1,\"skin_structure\":-1,\"style\":1,\"lease_until_unix_ms\":"+std::to_string(lease)+"}";};
    check(vrlive::parse(make(true,true,now+3000),now).enabled(now),"valid live lease");
    check(!vrlive::parse(make(false,true,now+2000),now).enabled(now),"active off immediately bypasses");
    check(!vrlive::parse(make(true,false,now+2000),now).enabled(now),"NR off bypasses");
    check(!vrlive::parse(make(true,true,now),now).enabled(now),"expired lease bypasses");
    check(vrlive::parse(make(true,true,now+3005),now).valid,"concurrent heartbeat timestamp tolerance");
    check(!vrlive::parse(make(true,true,now+3501),now).valid,"overlong lease invalid");
    auto bad=make(true,true,now+2000);bad.replace(bad.find("0.25"),4,"NaN");check(!vrlive::parse(bad,now).valid,"NaN settings invalid");
    check(!vrlive::parse(make(true,true,now+2000).substr(0,30),now).valid,"partial control invalid");
    check(!vrlive::parse(std::string(8193,'x'),now).valid,"oversize control invalid");
    check(vrlive::workDimension(2880,.25f)==720 && vrlive::workDimension(2011,.25f)==502 && vrlive::workDimension(128,.25f)==64,"quarter work scale even and at least 64");
    vrlive::PairDecision d;
    check(d.accept(9,9,9,9,9,true,true,25000),"25ms completed pair accepted");
    check(d.accept(9,9,9,9,9,true,true,300000),"300ms completed pair accepted without performance cutoff");
    check(!d.accept(9,8,9,9,9,true,true,100),"stale eye completion rejected");
    check(!d.accept(9,9,9,8,9,true,true,100),"unfinished output fence rejected");
    check(!d.accept(9,9,9,9,9,true,false,100),"one eye failure rejects both");
    check(!d.accept(9,9,9,9,9,true,true,vrlive::WatchdogUs+1),"only long recovery watchdog rejects elapsed time");
    auto custom=make(true,true,now+2000);custom.insert(custom.size()-1,",\"pair_budget_ms\":8,\"passes\":6,\"auto_budget\":true");
    check(vrlive::parse(custom,now).valid && d.accept(9,9,9,9,9,true,true,300000),"legacy short deadline settings cannot reject completed pair");
    auto unknown=make(true,true,now+2000);unknown.insert(unknown.size()-1,",\"unknown\":30");check(!vrlive::parse(unknown,now).valid,"unknown optional key rejected");
    d.miss();d.miss();check(!d.latched,"two budget misses can recover");d.miss();check(d.latched && !d.accept(9,9,9,9,9,true,true,1),"three misses latch both raw");d.observe(false);check(!d.latched && d.misses==0,"NR off re-arms latch");
    d.hit();d.miss();check(d.latched && !d.accept(10,10,10,10,10,true,true,100),"single miss after visible NR latches live raw and forbids alternating recovery");
    d.observe(false);check(d.accept(10,10,10,10,10,true,true,100),"explicit OFF re-arms anti-flicker guard");
    auto path=std::filesystem::current_path()/L"logs"/L"atomic-test.json";vrlive::atomicWrite(path,make(true,true,now+2000));std::atomic_bool stop{false};std::atomic_int invalid{0};
    std::thread reader([&]{while(!stop){if(!vrlive::readControl(path,now).valid)++invalid;}});
    for(int i=0;i<30;++i)check(vrlive::atomicWrite(path,make(i%2==0,true,now+2000)),"atomic status/control replace");stop=true;reader.join();check(invalid==0,"atomic replacement never observed partial JSON");
    std::filesystem::remove(path);printf("failures=%d\n",failures);return failures?1:0;
}
