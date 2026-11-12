// tldserver: the TLDCoop_DLC (polar-link) dedicated server. No game needed.
//
//   tldserver [--port 27015] [--bind 0.0.0.0] [--name Serwer] [--game-version V | --game-dir DIR] [--world FILE]
//
// The world is kept in FILE (default: world.bin next to tldserver.exe), written every 30 s when it changed and on
// stop. A host's own worlds\<save>.bin (the mod's world_keep file) can be copied there to start from that world.
// The game version must be the players' exactly: given with --game-version, read from
// DIR/tld_Data/StreamingAssets/version.txt with --game-dir, or from version.txt next to tldserver.exe (copy it there
// from the game).
// Console: players | kick <id> | say <text> | world | save | forget mode|features|clock | stop (also Ctrl+C, and
// closing the window: the world is saved first). Everything is written to tldserver.log too.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "server/server.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef TLDSERVER_BUILD
#define TLDSERVER_BUILD "local"
#endif
#ifndef TLDSERVER_MOD
#define TLDSERVER_MOD "?"
#endif

namespace {

std::atomic<bool> g_stop{false};
std::mutex g_out_mu;
std::ofstream g_log;

void on_signal(int) { g_stop = true; }

#ifdef _WIN32
// The window's X, logging off, Windows shutting down: Windows ends the process as soon as this returns (or after about
// 5 s), so it waits here until the loop has saved the world and stopped.
std::atomic<bool> g_done{false};
BOOL WINAPI on_console(DWORD) {
    g_stop = true;
    for (int i = 0; i < 400 && !g_done; ++i) Sleep(10);
    return TRUE;
}
#endif

void out(const std::string& text) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    std::lock_guard lock(g_out_mu);
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::printf("[%s] %s\n", stamp, line.c_str());
        if (g_log) g_log << '[' << stamp << "] " << line << '\n';
    }
    std::fflush(stdout);
    if (g_log) g_log.flush();
}

std::string read_trimmed(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF) s.erase(0, 3);  // UTF-8 BOM
    return s;
}

// Lines typed into the console, taken by the server's loop.
std::mutex g_cmd_mu;
std::vector<std::string> g_cmds;

void read_console() {
    std::string line;
    while (!g_stop && std::getline(std::cin, line)) {
        std::lock_guard lock(g_cmd_mu);
        g_cmds.push_back(line);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::error_code ec;
    const std::filesystem::path exe_dir = std::filesystem::absolute(std::filesystem::path(argv[0]), ec).parent_path();
    g_log.open(exe_dir / "tldserver.log", std::ios::app);

    srv::Options opt;
    std::string game_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--port") opt.port = static_cast<uint16_t>(std::atoi(next().c_str()));
        else if (a == "--bind") opt.bind_ip = next();
        else if (a == "--name") opt.name = next();
        else if (a == "--game-version") opt.game_version = next();
        else if (a == "--game-dir") game_dir = next();
        else if (a == "--world") opt.world_file = next();
        else if (a == "--help" || a == "-h" || a == "/?") {
            std::printf("tldserver [--port 27015] [--bind 0.0.0.0] [--name Serwer] [--game-version V | --game-dir DIR] [--world FILE]\n");
            return 0;
        } else {
            std::printf("unknown option '%s' (--help)\n", a.c_str());
            return 2;
        }
    }
    if (opt.game_version.empty() && !game_dir.empty())
        opt.game_version = read_trimmed(std::filesystem::path(game_dir) / "tld_Data" / "StreamingAssets" / "version.txt");
    if (opt.game_version.empty()) opt.game_version = read_trimmed(exe_dir / "version.txt");

    out(std::string("tldserver build ") + TLDSERVER_BUILD + ", mod " + TLDSERVER_MOD + " (protocol " + std::to_string(net::kProtocolVersion) + ")");
    if (opt.game_version.empty()) {
        out("no game version: copy tld_Data\\StreamingAssets\\version.txt from the game next to tldserver.exe, or "
            "start with --game-version V / --game-dir DIR");
        return 2;
    }

    if (opt.world_file.empty()) opt.world_file = (exe_dir / "world.bin").string();

#ifdef _WIN32
    SetConsoleCtrlHandler(on_console, TRUE);
#endif
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    srv::Server server(out);
    if (!server.start(opt)) {
        out("could not start (the world file unreadable, or the port in use?)");
        return 3;
    }
    out("type 'help' for commands");
    std::thread(read_console).detach();

    bool quit = false;
    while (!g_stop && !quit) {
        server.update();
        std::vector<std::string> cmds;
        {
            std::lock_guard lock(g_cmd_mu);
            cmds.swap(g_cmds);
        }
        for (const std::string& c : cmds) {
            const std::string answer = server.command(c, &quit);
            if (!answer.empty()) out(answer);
        }
        server.wait(5);
    }
    server.stop();
#ifdef _WIN32
    g_done = true;
#endif
    return 0;
}
