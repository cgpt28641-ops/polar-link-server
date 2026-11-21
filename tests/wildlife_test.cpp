// The animals' arbiter on the server (server/wildlife.h), without a network: what goes out is caught. Checks: a game's
// animals enter the registry and its frames go on to the others; who spawns where; a handover to the nearer player
// (Release -> Take -> Take); an owner who left is replaced at once from the registry; a shot goes to the animal's
// owner; a refusal blocks that player; a frozen animal comes back aged (moved on, or bled out); deaths and spawn
// regions reach the spawning game; a body whose owner left goes to someone near; the registry survives the file.
// Exit code 0 when everything held.

#include <cstdio>
#include <string>
#include <vector>

#include "server/wildlife.h"

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

struct Sent {
    int to = -1;      // one player; -1: everyone (but `except`)
    int except = -1;
    net::Msg type = net::Msg::Hello;
    std::vector<uint8_t> data;
    bool reliable = false;
};
std::vector<Sent> g_sent;

srv::Wildlife::Out catcher() {
    srv::Wildlife::Out o;
    auto keep = [](int to, int except, const net::Writer& w, bool reliable) {
        Sent s;
        s.to = to;
        s.except = except;
        s.type = static_cast<net::Msg>(w.data()[6]);
        s.data.assign(w.data(), w.data() + w.size());
        s.reliable = reliable;
        g_sent.push_back(std::move(s));
    };
    o.to = [keep](uint8_t p, const net::Writer& w, bool r) { keep(p, -1, w, r); };
    o.except = [keep](uint8_t p, const net::Writer& w, bool r) { keep(-1, p, w, r); };
    o.all = [keep](const net::Writer& w, bool r) { keep(-1, -1, w, r); };
    return o;
}

net::Packet packet(uint8_t from, const net::Writer& w) {
    net::Packet p;
    p.type = static_cast<net::Msg>(w.data()[6]);
    p.from = from;
    p.data.assign(w.data(), w.data() + w.size());
    return p;
}

// The WildCtl messages sent to `to` (-1: to everyone) with this op, oldest first.
std::vector<net::WildCtlMsg> ctl(int to, net::WildOp op) {
    std::vector<net::WildCtlMsg> out;
    for (const Sent& s : g_sent) {
        if (s.type != net::Msg::WildCtl || s.to != to) continue;
        net::Packet p;
        p.data = s.data;
        net::Reader r = net::packet_body(p);
        net::WildCtlMsg m;
        if (net::read_wild_ctl(r, m) && m.op == op) out.push_back(std::move(m));
    }
    return out;
}

net::AnimalRec animal(uint32_t id, float x, float z, uint16_t epoch = 0) {
    net::AnimalRec a;
    a.id = id;
    a.kind = 1;
    a.mode = net::wild::kModeWander;
    a.x = x;
    a.z = z;
    a.hp = 100;
    a.epoch = epoch;
    a.region = 77;
    return a;
}

net::Packet frame(uint8_t from, const std::string& scene, const std::vector<net::AnimalRec>& animals) {
    net::WildFrameMsg m;
    m.owner = from;
    m.scene = scene;
    m.animals = animals;
    net::Writer w(net::Msg::WildFrame);
    net::write_wild_frame(w, m);
    return packet(from, w);
}

net::Packet ctl_packet(uint8_t from, const net::WildCtlMsg& m) {
    net::Writer w(net::Msg::WildCtl, net::kMaxMessage);
    net::write_wild_ctl(w, m);
    return packet(from, w);
}

srv::Wildlife::Present at(uint8_t id, const std::string& scene, float x, float z) {
    srv::Wildlife::Present p;
    p.id = id;
    p.scene = scene;
    p.x = x;
    p.z = z;
    p.yaw = 180;  // looking away from +z
    return p;
}

}  // namespace

int main() {
    using srv::Wildlife;
    std::vector<std::string> log;
    Wildlife w(catcher(), [&](const std::string& line) { log.push_back(line); std::printf("  [server] %s\n", line.c_str()); });
    double now = 1000, hours = 100;
    auto run = [&](const std::vector<Wildlife::Present>& players, double ms) {
        for (double end = now + ms; now < end; now += 50) w.tick(players, now, hours);
    };

    // --- one game alone: it spawns, its animals are shared
    run({at(1, "Lake", 0, 0)}, 300);
    {
        const auto c = ctl(-1, net::WildOp::Controllers);
        check(!c.empty() && c.back().scenes.size() == 1 && c.back().scenes[0].scene == "Lake" && c.back().scenes[0].player == 1 &&
                  c.back().scenes[0].fresh == 1 && c.back().per_player == 50,
              "the first game in a scene spawns there (nothing shared yet), told to everyone");
    }
    check(w.on_packet(frame(1, "Lake", {animal(100, 5, 5)}), now), "a game's frame is the arbiter's");
    check(w.animals() == 1 && w.owner(100) == 1, "its animal is in the registry, run by that game");
    check(!g_sent.empty() && g_sent.back().type == net::Msg::WildFrame && g_sent.back().except == 1, "the frame goes on to the others");

    // --- a second player comes, nearer: after a hold the animal is handed over
    g_sent.clear();
    run({at(1, "Lake", 0, 200), at(2, "Lake", 5, 0)}, 2500);
    check(ctl(1, net::WildOp::Release).empty(), "no handover before the nearer player has been nearer for a while");
    run({at(1, "Lake", 0, 200), at(2, "Lake", 5, 0)}, 1500);
    {
        const auto r = ctl(1, net::WildOp::Release);
        check(r.size() == 1 && r[0].id == 100 && r[0].to == 2 && r[0].epoch == 1, "then its owner is told to release it to the nearer one");
        net::WildCtlMsg t;
        t.op = net::WildOp::Take;
        t.id = 100;
        t.to = 2;
        t.epoch = 1;
        t.scene = "Lake";
        t.state = animal(100, 6, 6, 1);
        t.text = "its plans";
        w.on_packet(ctl_packet(1, t), now);
        const auto got = ctl(2, net::WildOp::Take);
        check(got.size() == 1 && got[0].id == 100 && got[0].to == 2 && got[0].epoch == 1 && got[0].text == "its plans" &&
                  got[0].state.x == 6,
              "the old owner's state goes on to the new one");
        check(w.owner(100) == 2, "who runs it from now");
    }

    // --- a shot on it goes to the game that runs it
    g_sent.clear();
    {
        net::HitMsg h;
        h.animal = 100;
        net::Writer hw(net::Msg::Hit);
        net::write_hit(hw, h);
        w.on_packet(packet(1, hw), now);
        check(g_sent.size() == 1 && g_sent[0].type == net::Msg::WildHit && g_sent[0].to == 2, "a hit by another player goes to its owner");
        g_sent.clear();
        w.on_packet(packet(2, hw), now);
        check(g_sent.empty(), "the owner's own hit goes nowhere (its game applied it)");
    }

    // --- the owner leaves the scene: the other takes it at once, from what the registry knows
    g_sent.clear();
    w.on_packet(frame(2, "Lake", {animal(100, 8, 8, 1)}), now);
    run({at(1, "Lake", 0, 50), at(2, "Mountain", 0, 0)}, 400);
    {
        const auto t = ctl(1, net::WildOp::Take);
        check(t.size() == 1 && t[0].id == 100 && t[0].epoch == 2 && t[0].state.x == 8 && t[0].text == "its plans" && t[0].flags == 0,
              "an owner gone from the scene is replaced at once (the last record, TLD's record)");
        check(w.owner(100) == 1, "the other player runs it");
    }

    // --- it can't take it: someone else, not them for a minute
    {
        net::WildCtlMsg r;
        r.op = net::WildOp::Refuse;
        r.id = 100;
        r.epoch = 2;
        w.on_packet(ctl_packet(1, r), now);
        check(w.owner(100) == net::kNoPlayer, "a refusal leaves it with nobody");
        g_sent.clear();
        run({at(1, "Lake", 0, 50)}, 1000);
        check(ctl(1, net::WildOp::Take).empty(), "and that player isn't handed it again right away");
        run({at(1, "Lake", 0, 50), at(3, "Lake", 0, 60)}, 600);
        check(ctl(3, net::WildOp::Take).size() == 1 && w.owner(100) == 3, "another player near it is");
    }

    // --- nobody near: frozen; back two game hours later: it has moved on
    run({at(3, "Mountain", 0, 0)}, 400);
    check(w.owner(100) == net::kNoPlayer, "with nobody in its scene it is frozen (nobody runs it)");
    hours += 2;
    g_sent.clear();
    run({at(2, "Lake", 10, 10)}, 400);
    {
        const auto t = ctl(2, net::WildOp::Take);
        check(t.size() == 1 && (t[0].flags & net::kTakeMoved) && t[0].hours > 1.9 && t[0].hours < 2.1 && t[0].text.empty(),
              "whoever comes near gets it as it would be by now: moved on in its region");
    }

    // --- a bleeding one left alone bleeds out: a body, and the spawning game hears of the death
    {
        net::AnimalRec b = animal(200, 20, 20);
        b.flags = net::kAnimalBleeding;
        b.bleed_limit = 30;  // minutes
        w.on_packet(frame(2, "Lake", {b}), now);
        run({at(2, "Mountain", 0, 0)}, 400);  // (frozen: #200 and #100)
        hours += 1;
        g_sent.clear();
        run({at(1, "Lake", 20, 25)}, 400);
        const auto t = ctl(1, net::WildOp::Take);
        bool body = false;
        for (const auto& m : t) body |= m.id == 200 && (m.flags & net::kTakeDead) && (m.state.flags & net::kAnimalDead) && m.state.hp == 0;
        check(body, "a bleeding animal left alone long enough is handed out as a body");
        const auto k = ctl(1, net::WildOp::Kill);
        check(k.size() == 1 && k[0].id == 200 && k[0].region == 77 && k[0].scene == "Lake", "and the game that spawns there counts the death");
    }

    // --- a body whose owner left goes to someone near (in the mod: to the host's game; here to a player's)
    {
        net::AnimalRec d = animal(300, 30, 30);
        d.flags = net::kAnimalDead;
        d.mode = net::wild::kModeDead;
        d.hp = 0;
        w.on_packet(frame(1, "Lake", {d}), now);
        net::WildCtlMsg k;
        k.op = net::WildOp::Keep;
        k.id = 300;
        k.flags = net::kTakeDead | net::kTakeBody;
        k.scene = "Lake";
        k.state = d;
        k.text = "half the meat left";
        w.on_packet(ctl_packet(1, k), now);
        g_sent.clear();
        run({at(1, "Mountain", 0, 0), at(2, "Lake", 30, 35)}, 600);
        const auto t = ctl(2, net::WildOp::Take);
        bool got = false;
        for (const auto& m : t)
            got |= m.id == 300 && (m.flags & net::kTakeDead) && (m.flags & net::kTakeBody) && m.text == "half the meat left";
        check(got, "a carcass whose owner left goes to a player near it, as harvested");
        // Shatsca 20.11: that player's save already put the same carcass back: it says so, and the record goes
        const size_t before = w.animals();
        uint16_t epoch = 0;
        for (const auto& m : t)
            if (m.id == 300) epoch = m.epoch;
        net::WildCtlMsg plain;
        plain.op = net::WildOp::Refuse;
        plain.id = 300;
        plain.epoch = epoch;
        net::WildCtlMsg dup = plain;
        dup.flags = net::kRefuseDuplicate;
        {
            net::Writer wr(net::Msg::WildCtl, net::kMaxMessage);
            net::write_wild_ctl(wr, dup);
            net::Reader rd(wr.data() + net::kHeaderSize, wr.size() - net::kHeaderSize);
            net::WildCtlMsg back;
            check(net::read_wild_ctl(rd, back) && back.flags == net::kRefuseDuplicate && back.id == 300,
                  "a refusal's flags go over the wire");
            net::Writer wp(net::Msg::WildCtl, net::kMaxMessage);
            net::write_wild_ctl(wp, plain);
            check(wp.size() + 1 == wr.size(), "a plain refusal is as it was (older readers)");
        }
        w.on_packet(ctl_packet(2, dup), now);
        check(w.animals() + 1 == before && w.owner(300) == net::kNoPlayer, "a body their save already has is forgotten");
    }

    // --- one carcass kept several times (Shatsca 21.11: 16 dead stags on 4 spots by the tracks)
    {
        Wildlife t(catcher(), [](const std::string&) {});
        auto dead = [](uint32_t id, float x, float z) {
            net::AnimalRec d = animal(id, x, z);
            d.flags = net::kAnimalDead;
            d.mode = net::wild::kModeDead;
            d.hp = 0;
            return d;
        };
        t.on_packet(frame(1, "Track", {dead(700, 10, 10), dead(701, 10.3f, 10.1f), dead(702, 10.1f, 9.8f), dead(703, 30, 30),
                                     dead(704, 30.2f, 30)}), now);
        t.tick({at(1, "Elsewhere", 0, 0)}, now + 1000, 10);  // owner left: all frozen
        t.tick({at(1, "Elsewhere", 0, 0)}, now + 3000, 10);
        const size_t merged = t.merge_dead_twins();
        check(t.animals() == 2 && merged + 3 >= 3, "dead ones of a kind on one spot become one record per spot");
        t.on_packet(frame(2, "Track", {dead(800, 30.1f, 30.1f)}), now + 5000);
        check(t.animals() == 2, "a carcass a save put back where a frozen one lies replaces it");
    }

    // --- deaths with nobody spawning there wait; the regions go from one spawning game to the next
    {
        net::WildCtlMsg k;
        k.op = net::WildOp::Kill;
        k.id = 999;
        k.scene = "Coast";
        k.region = 5;
        w.on_packet(ctl_packet(2, k), now);
        net::WildCtlMsg reg;
        reg.op = net::WildOp::Regions;
        reg.scene = "Coast";
        net::WildRegionState st;
        st.key = 5;
        st.pending = 2;
        st.next_h = 10;
        reg.regions.push_back(st);
        run({at(1, "Coast", 0, 0)}, 400);  // player 1 spawns in Coast: gets the queued death
        check(ctl(1, net::WildOp::Kill).size() >= 1 && ctl(1, net::WildOp::Kill).back().id == 999,
              "a death where nobody spawned is told to the next game that does");
        w.on_packet(ctl_packet(1, reg), now);
        check(w.region_scenes() == 1, "the spawning game's regions are kept");
        run({at(3, "Mountain", 0, 0)}, 400);
        hours += 4;
        g_sent.clear();
        run({at(3, "Coast", 0, 0)}, 400);
        const auto r = ctl(3, net::WildOp::Regions);
        check(r.size() == 1 && r[0].regions.size() == 1 && r[0].regions[0].key == 5 && r[0].regions[0].pending == 2 &&
                  r[0].regions[0].next_h > 5.9f && r[0].regions[0].next_h < 6.1f,
              "the next game to spawn there gets them, their times brought up to now");
    }

    // --- a game back in a scene shares its own saved animal anew where a frozen one stands: one animal, not two
    {
        Wildlife t(catcher(), [](const std::string& l) { std::printf("  [server] %s\n", l.c_str()); });
        double n = 1000;
        auto go = [&](const std::vector<Wildlife::Present>& pl, double ms) {
            for (double end = n + ms; n < end; n += 50) t.tick(pl, n, 1.0);
        };
        go({at(1, "Dale", 0, 0)}, 400);
        t.on_packet(frame(1, "Dale", {animal(500, 30, 30), animal(501, 900, 900)}), n);
        go({at(1, "Dale", 0, 0)}, 200);
        go({at(1, "Hut", 0, 0)}, 600);  // away: both frozen
        check(t.animals() == 2 && t.owner(500) == net::kNoPlayer, "left behind: two frozen animals");
        go({at(1, "Dale", 5000, 5000)}, 300);  // back, far from both
        t.on_packet(frame(1, "Dale", {animal(0x02000001, 32, 31)}), n);  // TLD's own copy of #500, shared as new
        check(t.animals() == 2 && t.owner(0x02000001) == 1 && t.owner(501) == net::kNoPlayer,
              "its own copy shared as new: the frozen twin beside it is forgotten, the far one stays");
        net::AnimalRec other = animal(0x02000002, 902, 900);
        other.kind = 4;
        t.on_packet(frame(1, "Dale", {other}), n);
        check(t.animals() == 3, "another kind beside a frozen one is no twin");
    }

    // --- the file: the registry and the regions come back, frozen
    {
        std::vector<std::pair<bool, std::vector<uint8_t>>> file;
        w.save([&](bool regions, const net::Writer& wr) { file.emplace_back(regions, std::vector<uint8_t>(wr.data(), wr.data() + wr.size())); });
        Wildlife again(catcher(), [](const std::string&) {});
        size_t bad = 0;
        for (const auto& [regions, msg] : file)
            if (!again.load(regions, msg)) ++bad;
        check(bad == 0 && again.animals() == w.animals() && again.region_scenes() == w.region_scenes() && again.animals() >= 2,
              "the registry and the spawn regions survive the world file");
        check(again.owner(100) == net::kNoPlayer, "after a restart nobody runs anything until someone comes near");
    }

    // --- carcasses (Shatsca 16.11): the game that runs a dead animal lists its carcass; the others in that scene get
    // it; their harvests go to that game with the server's seqs, and its acks come back to each harvester as its own
    {
        Wildlife c(catcher(), [](const std::string&) {});
        double t = 1e6;
        c.tick({at(1, "Rail", 0, 0), at(2, "Rail", 3, 0), at(3, "Elsewhere", 0, 0)}, t, 10);
        net::AnimalRec dead = animal(4000, 1, 1);
        dead.flags |= net::kAnimalDead;
        dead.hp = 0;
        c.on_packet(frame(2, "Rail", {dead}), t);
        check(c.owner(4000) == 2, "a wolf player 2's game runs, dead there");
        auto carcass_list = [](uint8_t from, uint32_t ack) {
            net::CarcassRec r;
            r.animal = 4000;
            r.prefab = "WILDLIFE_Wolf";
            r.meat_units = 5000000000ll;
            r.hide = 1;
            r.gut = 2;
            r.harvest_ack = ack;
            net::Writer w(net::Msg::Carcasses);
            net::write_carcasses(w, {r});
            return packet(from, w);
        };
        auto lists_to = [](int to) {
            std::vector<net::CarcassRec> out;
            for (const Sent& s : g_sent) {
                if (s.type != net::Msg::Carcasses || s.to != to) continue;
                net::Packet p;
                p.data = s.data;
                net::Reader r = net::packet_body(p);
                std::vector<net::CarcassRec> l;
                if (net::read_carcasses(r, l)) out.insert(out.end(), l.begin(), l.end());
            }
            return out;
        };
        g_sent.clear();
        c.on_packet(carcass_list(2, 0), t);
        check(lists_to(1).size() == 1 && lists_to(1)[0].animal == 4000 && lists_to(1)[0].harvest_ack == 0,
              "its carcass goes to the other player in the scene");
        check(lists_to(3).empty() && lists_to(2).empty(), "not to its own game, nor to one in another scene");
        g_sent.clear();
        c.on_packet(carcass_list(1, 0), t);
        check(lists_to(2).empty(), "a list from a game that doesn't run it goes nowhere");
        g_sent.clear();
        net::HarvestMsg h;
        h.animal = 4000;
        h.seq = 7;
        h.meat_units = 1000000000ll;
        net::Writer hw(net::Msg::Harvest);
        net::write_harvest(hw, h);
        c.on_packet(packet(1, hw), t);
        uint32_t passed_seq = 0;
        for (const Sent& s : g_sent) {
            if (s.type != net::Msg::Harvest || s.to != 2) continue;
            net::Packet p;
            p.data = s.data;
            net::Reader r = net::packet_body(p);
            net::HarvestMsg got;
            if (net::read_harvest(r, got) && got.animal == 4000 && got.meat_units == h.meat_units) passed_seq = got.seq;
        }
        check(passed_seq != 0, "player 1's harvest goes to player 2, who runs it, with the server's seq");
        g_sent.clear();
        c.on_packet(carcass_list(2, 0), t);
        check(lists_to(1).size() == 1 && lists_to(1)[0].harvest_ack == 0, "not counted there yet: no ack for player 1");
        g_sent.clear();
        c.on_packet(carcass_list(2, passed_seq), t);
        check(lists_to(1).size() == 1 && lists_to(1)[0].harvest_ack == 7, "counted there: player 1 sees its own seq 7 acked");
        g_sent.clear();
        net::Writer qw(net::Msg::Quartered);
        qw.put(static_cast<uint32_t>(4000));
        c.on_packet(packet(1, qw), t);
        bool quartered = false;
        for (const Sent& s : g_sent) quartered |= s.type == net::Msg::Quartered && s.to == 2;
        check(quartered, "a quartering goes to the game that runs it");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
