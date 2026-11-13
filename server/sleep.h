#pragma once
// Sleeping together on the server: the host's part of the mod's coop_sleep.cpp (its decide() and the host's half of
// receive()). In the mod a host's game decides from everyone's State (in bed or waiting, and the vote) and starts the
// shared sleep in every game; with a server no game is the host, so the decision is made here and every game does what
// a friend's game does: sleeps the Start it is sent, says Woke when TLD wakes it, and wakes on End.
//
//   everyone in the game is in and has voted, for kSettleMs  ->  Start (id, the winning choice, its hours) to all
//   Woke from one of them                                    ->  End Woken (who, why) to all: everyone wakes
//   kDoneMs after Start                                      ->  End Done: a game still asleep ends its sleep
//
// The server is in no game, so it is never one of the sleepers (the mod's games leave out a host with no valid State
// too). Someone in a menu or loading isn't waited for; someone who left isn't either. The clock is the kept world's
// (the "world source" game's); dawn and dusk are TLD's usual 7:00 and 19:00 unless a game said otherwise.
//
// No session here: what goes out goes through `Send`, so it is tested without a network. One thread.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/protocol.h"
#include "net/sleep_vote.h"

namespace srv {

class Sleep {
public:
    using Send = std::function<void(const net::SleepMsg& m)>;  // to every player
    using LogFn = std::function<void(const std::string&)>;

    static constexpr double kSettleMs = 2000;  // the mod's: everyone in this long before the sleep starts
    static constexpr double kDoneMs = 30000;   // a shared skip takes a game about 8 s of real time: over well before
    static constexpr double kStaleMs = 90000;  // a shared sleep nobody got up from by now is forgotten

    Sleep(Send send, LogFn log);

    // Every round: the players whose game is in (linked, a valid State), as their States say.
    void tick(const std::vector<net::Sleeper>& players, const net::SleepClock& clock, double now_ms);
    // A player's Sleep message.
    void on_msg(uint8_t from, const net::SleepMsg& m, double now_ms);

    uint32_t current() const { return id_; }  // the shared sleep going on (0 none)
    uint32_t count() const { return next_id_; }

private:
    void logf(const char* fmt, ...);

    Send send_;
    LogFn log_;
    uint32_t next_id_ = 0;
    uint32_t id_ = 0;            // the shared sleep going on
    double started_ms_ = 0;
    bool done_sent_ = false;
    std::vector<uint8_t> in_;    // who it was started for and hasn't been seen up since
    double settled_ms_ = 0;
};

}  // namespace srv
