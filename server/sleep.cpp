#include "server/sleep.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace srv {

Sleep::Sleep(Send send, LogFn log) : send_(std::move(send)), log_(std::move(log)) {}

void Sleep::logf(const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    if (log_) log_(text);
}

void Sleep::tick(const std::vector<net::Sleeper>& players, const net::SleepClock& clock, double now_ms) {
    if (id_) {
        // Going on: over once everyone it was started for has been seen up (their State says "in" a moment longer
        // than their game sleeps), or left.
        in_.erase(std::remove_if(in_.begin(), in_.end(),
                                 [&](uint8_t id) {
                                     return std::none_of(players.begin(), players.end(), [&](const net::Sleeper& p) {
                                         return p.player == id && p.in_bed;
                                     });
                                 }),
                  in_.end());
        if (!done_sent_ && now_ms - started_ms_ >= kDoneMs) {
            done_sent_ = true;
            if (!in_.empty()) {
                net::SleepMsg m;
                m.op = net::SleepOp::End;
                m.id = id_;
                m.end = net::SleepEnd::Done;
                send_(m);
                logf("sleep: shared sleep #%u should be over: telling the %zu still in", id_, in_.size());
            }
        }
        if (in_.empty() || now_ms - started_ms_ >= kStaleMs) {
            logf("sleep: shared sleep #%u is over (%.0f s)", id_, (now_ms - started_ms_) / 1000.0);
            id_ = 0;
            in_.clear();
        }
        settled_ms_ = 0;
        return;
    }
    const net::SleepOutcome o = net::decide_sleep(players, clock);
    if (o.kind != net::SleepOutcome::Kind::Shared) {
        settled_ms_ = 0;
        return;
    }
    if (!settled_ms_) settled_ms_ = now_ms;
    if (now_ms - settled_ms_ < kSettleMs) return;
    settled_ms_ = 0;
    id_ = ++next_id_;
    started_ms_ = now_ms;
    done_sent_ = false;
    in_.clear();
    std::string votes;
    for (const net::Sleeper& p : players) {
        in_.push_back(p.player);
        votes += (votes.empty() ? "" : ", ") + std::string("player ") + std::to_string(p.player) + ": " +
                 net::sleep_choice_name(p.vote) + (p.waiting ? " (waiting)" : "");
    }
    logf("sleep: everyone is in (%s): shared sleep #%u, %s wins, %.2f h from %02d:%02d", votes.c_str(), id_,
         net::sleep_choice_name(o.choice), o.hours, static_cast<int>(clock.now_h) % 24,
         static_cast<int>(clock.now_h * 60.0f) % 60);
    net::SleepMsg m;
    m.op = net::SleepOp::Start;
    m.id = id_;
    m.choice = static_cast<uint8_t>(o.choice);
    m.hours = o.hours;
    send_(m);
}

void Sleep::on_msg(uint8_t from, const net::SleepMsg& m, double now_ms) {
    (void)now_ms;
    if (m.op != net::SleepOp::Woke || !id_ || m.id != id_) return;  // Start and End are ours to send
    const char* why = m.why == net::SleepWhy::Animal     ? "by an animal"
                      : m.why == net::SleepWhy::Cold     ? "by the cold"
                      : m.why == net::SleepWhy::NotInBed ? "wasn't in bed for it"
                      : m.why == net::SleepWhy::Stopped  ? "stopped passing time"
                                                         : "woken";
    logf("sleep: player %u in shared sleep #%u: %s: everyone wakes", from, id_, why);
    net::SleepMsg end;
    end.op = net::SleepOp::End;
    end.id = id_;
    end.end = net::SleepEnd::Woken;
    end.who = from;
    end.why = m.why;
    send_(end);
    done_sent_ = true;  // nothing more to say about this one
}

}  // namespace srv
