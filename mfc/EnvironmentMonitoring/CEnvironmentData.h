#pragma once
#include <deque>
#include <string>
#include <cstdint>
namespace Environment {
enum class State { Normal, Warning, Critical, SensorError };
struct Sample {
    unsigned long sequence=0;
    unsigned long deviceMs=0;
    double temperature=0, humidity=0;
    State state=State::SensorError;
    bool muted=false, valid=false;
};
// Accepts only the CSV ENV,1,seq,millis,temp,humidity,state,muted,valid line.
// Non-ENV lines are ignored without consuming existing main protocol.
bool TryParseLine(const std::string& line, Sample& result);
class History {
public:
    explicit History(size_t capacity=300):capacity_(capacity){}
    void Add(const Sample& s);
    void Clear(){samples_.clear();}
    const std::deque<Sample>& Samples() const { return samples_; }
private:
    size_t capacity_;
    std::deque<Sample> samples_;
};
}
