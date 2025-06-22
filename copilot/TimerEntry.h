// TimerEntry.h
#ifndef TIMERENTRY_H
#define TIMERENTRY_H

#include <chrono>
#include <functional>
#include <cstdint>

struct TimerEntry {
    uint32_t sequence;
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> on_timeout;
};

struct TimerEntryPtrCompare {
    bool operator()(const TimerEntry* a, const TimerEntry* b) const {
        return a->deadline > b->deadline;  // earliest deadline has highest priority
    }
};

#endif // TIMERENTRY_H
