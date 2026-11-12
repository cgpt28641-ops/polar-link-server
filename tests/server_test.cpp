// Stage 0 of tldserver, offline: the server and three players (real net::Sessions as the mod uses them) over UDP on
// 127.0.0.1. Checks: everyone gets in; nobody gets a valid state from the server (no body of its own); each player
// sees the other two's states through the server; a chat line reaches the others but not its sender; the server's own
// line reaches everyone; the console answers; a kicked player is out and stays out.
// Exit code 0 when everything held.

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "net/session.h"
#include "server/server.h"

namespace {

int g_failures = 0;
std::vector<std::string> g_server_log;

void check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

constexpr uint16_t kPort = 27391;
const char* const kVersion = "test-version";

struct Player {
    explicit Player(const char* n) : name(n), session([n](const std::string& line) { std::printf("  [%s] %s\n", n, line.c_str()); }) {}
    std::string name;
    net::Session session;
    std::vector<std::string> heard;  // chat lines "who: text"
};

void pump(srv::Server& server, std::vector<Player*>& players, double ms) {
    const double until = net::now_ms() + ms;
    while (net::now_ms() < until) {
        server.update();
        for (Player* p : players) {
            p->session.update();
            std::vector<net::Packet> in;
            p->session.drain_packets(in);
            for (const net::Packet& pk : in) {
                if (pk.type != net::Msg::Chat) continue;
                net::Reader r = net::packet_body(pk);
                std::string who, text;
                if (net::read_chat(r, who, text)) p->heard.push_back(who + ": " + text);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool start_player(Player& p, int index) {
    net::Config cfg;
    cfg.role = net::Role::Client;
    cfg.transport = "udp";
    cfg.host_ip = "127.0.0.1";
    cfg.port = kPort;
    cfg.name = p.name;
    cfg.body = "mackenzie";
    cfg.game_version = kVersion;
    cfg.content = "?";
    if (!p.session.start(cfg)) return false;
    net::StateMsg s;
    s.valid = true;
    s.scene = "LakeRegion";
    s.x = 100.0f + static_cast<float>(index);
    s.y = 20.0f;
    s.z = 300.0f;
    p.session.set_local_state(s);
    p.session.set_ready(true);
    return true;
}

bool heard(const Player& p, const std::string& line) {
    for (const std::string& h : p.heard)
        if (h == line) return true;
    return false;
}

}  // namespace

int main() {
    srv::Server server([](const std::string& line) {
        std::printf("  [server] %s\n", line.c_str());
        g_server_log.push_back(line);
    });
    srv::Options opt;
    opt.bind_ip = "127.0.0.1";
    opt.port = kPort;
    opt.game_version = kVersion;
    check(server.start(opt), "the server starts");

    Player karol("Kerel"), shatsca("Shatsca"), szymon("Szymon");
    std::vector<Player*> all = {&karol, &shatsca, &szymon};
    for (size_t i = 0; i < all.size(); ++i) check(start_player(*all[i], static_cast<int>(i)), "a player starts");
    pump(server, all, 2500);

    check(server.players().size() == 3, "three players are in");
    for (Player* p : all) {
        const net::Status st = p->session.status();
        check(st.connected, (p->name + " is connected").c_str());
        check(st.self_id >= 1 && st.self_id <= 3, (p->name + " got a friend's id").c_str());
        const auto from_server = p->session.remote_state(0);
        check(!from_server || !from_server->valid, (p->name + " gets no valid state from the server (no body)").c_str());
        int others = 0;
        for (const net::StateMsg& s : p->session.remote_states())
            if (s.player != 0 && s.player != st.self_id && s.valid && s.scene == "LakeRegion") ++others;
        check(others == 2, (p->name + " sees the other two players' states").c_str());
    }

    {
        net::Writer w(net::Msg::Chat, 1024);
        net::write_chat(w, "Kerel", "czesc");
        karol.session.send_packet(w, true);
    }
    pump(server, all, 800);
    check(heard(shatsca, "Kerel: czesc") && heard(szymon, "Kerel: czesc"), "a chat line reaches the other two");
    check(!heard(karol, "Kerel: czesc"), "the sender doesn't get their own line back");
    check(server.chat_passed() == 1, "the server passed on one line");

    bool quit = false;
    check(server.command("say serwer dziala", &quit) == "said", "console: say");
    pump(server, all, 800);
    check(heard(karol, "Serwer: serwer dziala") && heard(shatsca, "Serwer: serwer dziala") &&
              heard(szymon, "Serwer: serwer dziala"),
          "the server's own line reaches everyone");
    const std::string list = server.command("players", &quit);
    std::printf("  players:\n%s\n", list.c_str());
    check(list.find("Kerel") != std::string::npos && list.find("Szymon") != std::string::npos &&
              list.find("LakeRegion") != std::string::npos,
          "console: players lists names and scenes");
    check(server.command("nonsense", &quit).find("unknown") == 0, "console: unknown command");

    const uint8_t szymon_id = szymon.session.status().self_id;
    check(server.command("kick " + std::to_string(szymon_id), &quit) == "kicked", "console: kick");
    pump(server, all, 3000);
    check(server.players().size() == 2, "the kicked player is out");
    check(szymon.session.status().turned_away, "the kicked player knows");
    bool left_line = false;
    for (const std::string& l : g_server_log)
        if (l.rfind("left: Szymon", 0) == 0) left_line = true;
    check(left_line, "the log says who left");

    check(server.command("stop", &quit) == "stopping" && quit, "console: stop");
    for (Player* p : all) p->session.stop();
    server.stop();

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
