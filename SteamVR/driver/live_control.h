#pragma once
#include <windows.h>
#include <string>
#include <map>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
namespace vrlive {
// Recovery protection only: ordinary frame-time targets do not reject NR.
constexpr uint64_t WatchdogUs=2000000;
inline uint64_t unixMs(){FILETIME f{};GetSystemTimeAsFileTime(&f);ULARGE_INTEGER u{};u.LowPart=f.dwLowDateTime;u.HighPart=f.dwHighDateTime;return (u.QuadPart-116444736000000000ULL)/10000;}
struct Control {
    bool valid=false,active=false,nr=false,fast=false;
    int target=2,style=1;
    int passes=1;bool autoBudget=false;
    uint64_t pairBudgetUs=25000;
    float stability=.9f,scale=.25f,intensity=1,localTone=.5f,localStructure=1,skinStructure=-1;
    uint64_t lease=0;
    bool enabled(uint64_t now)const{return valid && active && nr && lease>now && lease<=now+3500;}
    unsigned effectivePasses()const{return fast?1u:unsigned(passes);}
    // Legacy timing fields are parsed for old managers but no longer control delivery.
};
// Strict flat schema: duplicate/unknown keys, partial writes, NaN and oversized files fail closed.
inline Control parse(const std::string& s,uint64_t now){
    Control c; if(s.empty() || s.size()>8192)return c;
    size_t p=0;std::map<std::string,std::string> values;
    auto white=[&](){while(p<s.size() && (s[p]==' ' || s[p]=='\r' || s[p]=='\n' || s[p]=='\t'))++p;};
    white();if(p==s.size() || s[p++]!='{')return c;
    for(;;){white();if(p>=s.size() || s[p++]!='"')return c;const auto start=p;
        while(p<s.size() && s[p]!='"'){if(s[p]=='\\' || (unsigned char)s[p]<32)return c;++p;}
        if(p>=s.size())return c;const auto key=s.substr(start,p-start);++p;white();if(p>=s.size() || s[p++]!=':')return c;white();const auto begin=p;
        while(p<s.size() && s[p]!=',' && s[p]!='}')++p;if(p>=s.size())return c;
        auto token=s.substr(begin,p-begin);while(!token.empty() && isspace((unsigned char)token.back()))token.pop_back();
        if(!values.emplace(key,token).second)return c;
        const auto delimiter=s[p++];if(delimiter=='}')break;
    }
    white();if(p!=s.size() || values.size()!=13+values.count("pair_budget_ms")+values.count("passes")+values.count("auto_budget"))return c;
    auto boolean=[&](const char* k,bool& v){auto i=values.find(k);if(i==values.end())return false;if(i->second=="true"){v=true;return true;}if(i->second=="false"){v=false;return true;}return false;};
    auto num=[&](const char* k,double low,double high,double& v){auto i=values.find(k);if(i==values.end() || i->second.empty())return false;char* end=nullptr;v=strtod(i->second.c_str(),&end);return end==i->second.c_str()+i->second.size() && std::isfinite(v) && v>=low && v<=high;};
    double v=0;
    if(!num("version",1,1,v) || !boolean("active",c.active) || !boolean("nr",c.nr) || !boolean("fast",c.fast))return c;
    if(!num("target",1,6,v) || floor(v)!=v)return c;c.target=int(v);
    if(!num("style",0,2,v) || floor(v)!=v)return c;c.style=int(v);
    if(!num("stability",0,.9,v))return c;c.stability=float(v);
    if(!num("work_scale",.15,.65,v))return c;c.scale=float(v);
    if(!num("intensity",0,2,v))return c;c.intensity=float(v);
    if(!num("local_tone",0,2,v))return c;c.localTone=float(v);
    if(!num("local_structure",0,2,v))return c;c.localStructure=float(v);
    if(!num("skin_structure",-1,2,v))return c;c.skinStructure=float(v);
    // Older managers omit this optional key and use the measured 25 ms default.
    if(values.count("pair_budget_ms")){if(!num("pair_budget_ms",8,250,v))return c;c.pairBudgetUs=uint64_t(v*1000);}
    if(values.count("passes")){if(!num("passes",1,6,v) || floor(v)!=v)return c;c.passes=int(v);}
    if(values.count("auto_budget") && !boolean("auto_budget",c.autoBudget))return c;
    // Reader time can precede a concurrent atomic heartbeat replacement by a few ms.
    if(!num("lease_until_unix_ms",0,double(now+3500),v) || floor(v)!=v)return c;c.lease=uint64_t(v);
    c.valid=true;return c;
}
inline Control readControl(const std::filesystem::path& path,uint64_t now){
    // Atomic rename must remain possible while this reader has the old file open.
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return {};
    LARGE_INTEGER size{};std::string text;DWORD bytes=0;
    const bool sized=GetFileSizeEx(file,&size) && size.QuadPart>0 && size.QuadPart<=8192;
    if(sized)text.resize(size_t(size.QuadPart));
    const bool read=sized && ReadFile(file,text.data(),DWORD(text.size()),&bytes,nullptr) && bytes==text.size();
    CloseHandle(file);return read?parse(text,now):Control{};
}
inline bool atomicWrite(const std::filesystem::path& path,const std::string& text){
    const auto temp=std::filesystem::path(path.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L".tmp");
    HANDLE f=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(f==INVALID_HANDLE_VALUE)return false;
    DWORD bytes=0;const bool ok=WriteFile(f,text.data(),DWORD(text.size()),&bytes,nullptr) && bytes==text.size() && FlushFileBuffers(f);CloseHandle(f);
    if(!ok){DeleteFileW(temp.c_str());return false;}
    // Antivirus/indexing can briefly open the destination without delete sharing.
    for(unsigned attempt=0;attempt<10;++attempt){
        if(MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))return true;
        const auto error=GetLastError();if(error!=ERROR_SHARING_VIOLATION && error!=ERROR_ACCESS_DENIED)break;
        Sleep(1);
    }
    DeleteFileW(temp.c_str());return false;
}
inline uint32_t workDimension(uint32_t full,float scale){return (std::max)(64u,uint32_t(full*scale)&~1u);}
struct PairDecision {
    unsigned misses=0;bool latched=false,delivered=false;
    void observe(bool requested){if(!requested){misses=0;latched=false;delivered=false;}}
    bool accept(uint64_t current,uint64_t left,uint64_t right,uint64_t lf,uint64_t rf,bool lok,bool rok,uint64_t elapsed){return !latched && current>0 && left==current && right==current && lf!=UINT64_MAX && rf!=UINT64_MAX && lf>=current && rf>=current && lok && rok && elapsed<=WatchdogUs;}
    // Once NR is visible, even isolated misses must not alternate NR/raw.
    // A failure switches once to live raw and requires deliberate re-arming.
    void miss(){if(++misses>=3 || delivered)latched=true;}
    void hit(){misses=0;delivered=true;}
};
}
