#include "pch.h"
#include "CEnvironmentData.h"
#include <sstream>
#include <vector>
#include <cstdlib>
#include <cerrno>
#include <cmath>
namespace Environment {
namespace {
bool Unsigned(const std::string& s,unsigned long& v) {
    if(s.empty()) return false;
    for(char c:s) if(c<'0'||c>'9')return false;
    errno=0;char* end=nullptr;v=std::strtoul(s.c_str(),&end,10);
    return errno==0 && end && *end=='\0';
}
bool Number(const std::string& s,double& v) {
    if(s.empty())return false;
    errno=0;char* end=nullptr;v=std::strtod(s.c_str(),&end);
    return errno==0 && end && *end=='\0' && std::isfinite(v);
}
}
bool TryParseLine(const std::string& line, Sample& out) {
    if(line.size()>128 || line.compare(0,4,"ENV,")!=0) return false;
    std::istringstream stream(line);std::string part;std::vector<std::string> p;
    while(std::getline(stream,part,',')) p.push_back(part);
    if(p.size()!=9 || p[0]!="ENV" || p[1]!="1")return false;
    Sample tmp;
    if(!Unsigned(p[2],tmp.sequence)||!Unsigned(p[3],tmp.deviceMs))return false;
    if(p[6]=="NORMAL")tmp.state=State::Normal;
    else if(p[6]=="WARNING")tmp.state=State::Warning;
    else if(p[6]=="CRITICAL")tmp.state=State::Critical;
    else if(p[6]=="SENSOR_ERROR")tmp.state=State::SensorError;
    else return false;
    if(p[7]!="0"&&p[7]!="1")return false;
    if(p[8]!="0"&&p[8]!="1")return false;
    tmp.muted=(p[7]=="1"); tmp.valid=(p[8]=="1");
    if(tmp.valid) {
        if(!Number(p[4],tmp.temperature)||!Number(p[5],tmp.humidity)) return false;
        if(tmp.humidity<0||tmp.humidity>100||tmp.temperature<-20||tmp.temperature>80)return false;
        if(tmp.state==State::SensorError)return false;
    } else if(tmp.state!=State::SensorError)return false;
    out=tmp;return true;
}
void History::Add(const Sample& s){
    if(!samples_.empty() && s.sequence<=samples_.back().sequence)return;
    samples_.push_back(s);while(samples_.size()>capacity_)samples_.pop_front();
}
}
