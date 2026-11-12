// The clock, weather, experience mode, seed and feature switches on the server, offline: the server and players (real
// net::Sessions) over UDP on 127.0.0.1. Checks: the first game in is the world source at once and is asked for its
// mode and switches; what it gives is kept and told to whoever comes; the others get the source's World as a host's;
// a World from a game that isn't the source is ignored; when the source leaves the next game takes over after
// following the kept clock a while; all of it survives a restart; "forget" asks again. Exit code 0 when all held.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net/session.h"
#include "server/server.h"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

constexpr uint16_t kPort = 27393;
const char* const kVersion = "test-version";
const char* const kMode = "{\"m_CurrentModeType\":\"Stalker\"}";

struct Player {
    explicit Player(const char* n) : name(n), session([](const std::string&) {}) {}
    std::string name;
    net::Session session;
    int flags = -1;            // the server's last Msg::Server (-1: none yet)
    std::string mode;          // the last GameMode
    int32_t seed = 0;
    int features = -1;         // the last Features' bits
    float snow = -2;
    bool publishing = false;   // sends its World every 300 ms while pumped
    float time = 0;
    double last_publish = 0;

    bool start() {
        net::Config cfg;
        cfg.role = net::Role::Client;
        cfg.transport = "udp";
        cfg.host_ip = "127.0.0.1";
        cfg.port = kPort;
        cfg.name = name;
        cfg.body = "mackenzie";
        cfg.game_version = kVersion;
        if (!session.start(cfg)) return false;
        net::StateMsg s;
        s.valid = true;
        s.scene = "LakeRegion";
        session.set_local_state(s);
        session.set_ready(true);
        return true;
    }
    void send_world(float t) {
        net::WorldMsg w;
        w.valid = true;
        w.normalized_time = t;
        w.day = 12;
        w.day_length_s = 7200;
        w.weather_set = "Clear_Set";
        w.wind_strength = 2;
        w.world_id = "sandbox1:1:1";
        net::Writer out(net::Msg::World);
        net::write_world(out, w);
        session.send_packet(out, false);
    }
    void send_mode(const std::string& text, int32_t s) {
        net::Writer w(net::Msg::GameMode, net::kMaxMessage);
        net::write_game_mode(w, text, s);
        session.send_packet(w, true);
    }
    void send_features(uint32_t bits, float sn) {
        net::Writer w(net::Msg::Features, 64);
        net::write_features(w, bits, sn);
        session.send_packet(w, true);
    }
    void take() {
        std::vector<net::Packet> in;
        session.drain_packets(in);
        for (const net::Packet& p : in) {
            net::Reader r = net::packet_body(p);
            if (p.type == net::Msg::Server) {
                uint8_t f = 0;
                if (net::read_server(r, f)) flags = f;
            } else if (p.type == net::Msg::GameMode) {
                net::read_game_mode(r, mode, seed);
            } else if (p.type == net::Msg::Features) {
                uint32_t b = 0;
                float sn = -1;
                if (net::read_features(r, b, sn)) features = static_cast<int>(b), snow = sn;
            }
        }
    }
};

void pump(srv::Server& server, const std::vector<Player*>& players, double ms) {
    const double until = net::now_ms() + ms;
    while (net::now_ms() < until) {
        server.update();
        for (Player* p : players) {
            if (p->publishing && net::now_ms() - p->last_publish >= 300) {
                p->last_publish = net::now_ms();
                p->send_world(p->time);
            }
            p->session.update();
            p->take();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

std::unique_ptr<srv::Server> start_server(const std::string& file) {
    auto server = std::make_unique<srv::Server>([](const std::string& line) { std::printf("  [server] %s\n", line.c_str()); });
    srv::Options opt;
    opt.bind_ip = "127.0.0.1";
    opt.port = kPort;
    opt.game_version = kVersion;
    opt.world_file = file;
    opt.save_every_s = 1;
    return server->start(opt) ? std::move(server) : nullptr;
}

bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

}  // namespace

int main() {
    using namespace net;
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tldserver_shared_test";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string file = (dir / "world.bin").string();

    auto server = start_server(file);
    check(server != nullptr, "the server starts");
    if (!server) return 1;

    Player a("Kerel"), b("Shatsca");
    check(a.start(), "the first player joins");
    std::vector<Player*> all = {&a};
    pump(*server, all, 1500);
    check(a.flags == (kServerIs | kServerWorldSource | kServerWantsMode | kServerWantsFeatures),
          "the first game in is told: a dedicated server, you run the clock, give your mode and switches");
    check(server->world().source() == a.session.status().self_id, "the server counts it as the world source");

    a.publishing = true;
    a.time = 0.5f;
    a.send_mode(kMode, 777);
    a.send_features(9, 20.0f);
    pump(*server, all, 1200);
    check(a.flags == (kServerIs | kServerWorldSource), "once given, the server stops asking for mode and switches");
    check(server->world().has_clock() && server->world().has_mode() && server->world().has_features(), "the server keeps all three");
    check(a.features == (9 | 16), "the giver hears the kept switches back (every game follows what is kept)");

    check(b.start(), "a second player joins");
    all.push_back(&b);
    pump(*server, all, 2500);
    check(b.flags == kServerIs, "the second game is told only that this is a dedicated server");
    {
        const auto w = b.session.remote_world();
        check(w && w->valid && near(w->normalized_time, 0.5f) && w->day == 12 && w->weather_set == "Clear_Set" &&
                  w->wind_strength == 2,
              "it gets the source's clock, weather and wind as a host's World");
        check(w && w->world_id == "server", "under one world id, whoever's game runs the clock");
    }
    check(b.mode == kMode && b.seed == 777, "it gets the kept experience mode and seed");
    check(b.features == (9 | 16), "and the kept switches (animals: always each run by one game)");

    a.send_features(1, 33.0f);  // (the source's own switches differ now: only its trees' snow counts)
    pump(*server, all, 600);
    check(b.features == (9 | 16) && near(b.snow, 33.0f), "the source's trees' snow goes on, with the kept switches");
    b.send_world(0.9f);
    b.send_mode("{\"m_CurrentModeType\":\"Pilgrim\"}", 1);
    b.send_features(64, 1.0f);
    pump(*server, all, 1200);
    {
        const auto w = a.session.remote_world();
        check(w && near(w->normalized_time, 0.5f), "a World from a game that isn't the source is ignored");
    }
    check(a.mode.empty() && a.features == (9 | 16), "so are a second mode and second switches");

    a.publishing = false;
    a.session.stop();
    all = {&b};
    pump(*server, all, 3000);  // (b has followed the kept clock for more than kFollowMs by now)
    check(server->world().source() == b.session.status().self_id && b.flags == (kServerIs | kServerWorldSource),
          "the source left: the next game (in long enough) takes over");
    b.publishing = true;
    b.time = 0.6f;
    pump(*server, all, 1500);
    b.publishing = false;
    b.session.stop();
    pump(*server, all, 300);
    server->stop();
    server.reset();

    server = start_server(file);
    check(server != nullptr && server->world().has_clock() && server->world().has_mode() && server->world().has_features(),
          "after a restart the clock, mode and switches are still kept");
    if (!server) return 1;
    Player c("Szymon");
    check(c.start(), "a player joins the restarted server");
    all = {&c};
    pump(*server, all, 2500);
    {
        const auto w = c.session.remote_world();
        check(w && w->valid && near(w->normalized_time, 0.6f), "it gets the clock as it stood when the last game left");
    }
    check(c.mode == kMode && c.seed == 777 && c.features == (9 | 16), "and the mode, seed and switches");
    check(c.flags == kServerIs && server->world().source() < 0, "it follows the kept clock first: not the source yet");
    pump(*server, all, 5000);
    check(c.flags == (kServerIs | kServerWorldSource), "after a few seconds it runs the clock");

    bool quit = false;
    check(server->command("forget mode", &quit).rfind("forgotten", 0) == 0, "console: forget mode");
    pump(*server, all, 500);
    check(c.flags == (kServerIs | kServerWorldSource | kServerWantsMode), "the server asks for a mode again");
    check(server->command("forget nonsense", &quit).rfind("forget ", 0) == 0, "console: forget what?");

    c.session.stop();
    server->stop();
    std::filesystem::remove_all(dir, ec);
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
