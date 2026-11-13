// Sleeping together decided on the server (server/sleep.h), without a network: what goes out is caught. Checks: nobody
// in bed starts nothing; one of two in bed waits; both in start one sleep after the settle time with the winning
// choice; someone in a menu isn't waited for; a player alone sleeps; Woke wakes everyone; no second sleep starts while
// the players' States still say "in"; the next one does once they got up; resting never starts one; a game still
// asleep long after is told the sleep is over.
// Exit code 0 when everything held.

#include <cstdio>
#include <string>
#include <vector>

#include "server/sleep.h"

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

std::vector<net::SleepMsg> g_sent;
std::vector<uint8_t> g_to;

net::Sleeper up(uint8_t id) { return {id, false, net::SleepChoice::None, false}; }
net::Sleeper bed(uint8_t id, net::SleepChoice c) { return {id, true, c, false}; }
net::Sleeper wait(uint8_t id, net::SleepChoice c) { return {id, true, c, true}; }

}  // namespace

int main() {
    srv::Sleep sleep([](uint8_t to, const net::SleepMsg& m) { g_sent.push_back(m); g_to.push_back(to); }, [](const std::string& s) { std::printf("    %s\n", s.c_str()); });
    net::SleepClock k;
    k.now_h = 22;
    double t = 1000;
    auto run = [&](const std::vector<net::Sleeper>& players, double ms) {
        for (double end = t + ms; t < end; t += 100) sleep.tick(players, k, t);
    };

    run({up(1), up(2)}, 5000);
    check(g_sent.empty(), "nobody in bed: nothing starts");

    run({bed(1, net::SleepChoice::H8), up(2)}, 5000);
    check(g_sent.empty(), "one of two in bed: waits for the other");

    run({bed(1, net::SleepChoice::H8), bed(2, net::SleepChoice::H4)}, 1500);
    check(g_sent.empty(), "both in: not before the settle time");
    run({bed(1, net::SleepChoice::H8), bed(2, net::SleepChoice::H4)}, 1000);
    check(g_sent.size() == 2 && g_to[0] != g_to[1] && g_sent[1].op == net::SleepOp::Start && g_sent[0].op == net::SleepOp::Start && g_sent[0].id == 1 &&
              g_sent[0].choice == static_cast<uint8_t>(net::SleepChoice::H4) && g_sent[0].hours == 4,
          "both in: one Start, a tie goes to the shorter sleep (4 h)");

    run({bed(1, net::SleepChoice::H8), bed(2, net::SleepChoice::H4)}, 6000);
    check(g_sent.size() == 2, "their States still say in: no second Start");
    {
        net::SleepMsg stray;  // a game that wasn't in it (it was in a menu) says "not in bed"
        stray.op = net::SleepOp::Woke;
        stray.id = 1;
        stray.why = net::SleepWhy::NotInBed;
        sleep.on_msg(3, stray, t);
        check(g_sent.size() == 2, "Woke from someone who isn't one of its sleepers: ignored");
    }

    // player 2 is woken by a wolf
    net::SleepMsg woke;
    woke.op = net::SleepOp::Woke;
    woke.id = 1;
    woke.why = net::SleepWhy::Animal;
    sleep.on_msg(2, woke, t);
    check(g_sent.size() == 3 && g_to[2] == 1 && g_sent[2].op == net::SleepOp::End && g_sent[2].end == net::SleepEnd::Woken &&
              g_sent[2].who == 2 && g_sent[2].why == net::SleepWhy::Animal && g_sent[2].id == 1,
          "Woke: End Woken to everyone, with who and why");
    woke.id = 7;
    sleep.on_msg(2, woke, t);
    check(g_sent.size() == 3, "Woke for another sleep: ignored");

    run({up(1), up(2)}, 1000);
    check(sleep.current() == 0, "everyone up: the sleep is over");
    g_sent.clear(), g_to.clear();

    // someone in a menu (no valid State: not in the list) isn't waited for; a player alone sleeps
    run({wait(1, net::SleepChoice::H1)}, 2500);
    check(g_sent.size() == 1 && g_sent[0].op == net::SleepOp::Start && g_sent[0].id == 2 && g_sent[0].hours == 1,
          "a player alone (the other in a menu) passes time: Start #2, 1 h");
    run({up(1), up(2)}, 500);
    g_sent.clear(), g_to.clear();

    // till dawn from 22:00 with dawn at 7:00 is 9 h; two votes beat one
    run({bed(1, net::SleepChoice::Dawn), bed(2, net::SleepChoice::Dawn), bed(3, net::SleepChoice::H4)}, 2500);
    check(g_sent.size() == 3 && g_sent[0].choice == static_cast<uint8_t>(net::SleepChoice::Dawn) && g_sent[0].hours == 9,
          "three in: TILL DAWN with two votes wins, 9 h from 22:00");
    // one game never gets up: told the sleep is over, once
    run({up(1), up(2), bed(3, net::SleepChoice::H4)}, 35000);
    check(g_sent.size() == 4 && g_to[3] == 3 && g_sent[3].op == net::SleepOp::End && g_sent[3].end == net::SleepEnd::Done && g_sent[3].id == 3,
          "a game still asleep 30 s later: End Done, and no new sleep meanwhile");
    run({up(1), up(2), up(3)}, 500);
    check(sleep.current() == 0, "it got up: over");
    g_sent.clear(), g_to.clear();

    // resting is no vote
    run({bed(1, net::SleepChoice::Rest), bed(2, net::SleepChoice::Rest)}, 5000);
    check(g_sent.empty(), "everyone resting: nothing starts");
    run({bed(1, net::SleepChoice::H8), bed(2, net::SleepChoice::Rest)}, 5000);
    check(g_sent.empty(), "one resting: the other's sleep waits");

    // a player leaves while the other is in bed: not waited for
    run({bed(1, net::SleepChoice::H12)}, 2500);
    check(g_sent.size() == 1 && g_sent[0].op == net::SleepOp::Start && g_sent[0].hours == 12, "the one resting left: the other sleeps 12 h");

    std::printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall good\n", g_failures);
    return g_failures ? 1 : 0;
}
