#pragma once
// tldserver's core: the shared co-op game without a game. Stage 0 (see SERVER_PLAN.md in the mod's repo): it sits in
// the host's seat of a net::Session (player 0, "Serwer"), never shows a body of its own (its State is never valid),
// lets everyone in (up to the session's 3 friends for now), passes on the players' states (the session does that)
// and chat, and answers the console. The world store comes in stage 1.
//
// Threading: everything from one thread (the server's loop), like net::Session's update().

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "net/session.h"

namespace srv {

struct Options {
    std::string bind_ip = "0.0.0.0";
    uint16_t port = 27015;
    std::string name = "Serwer";   // how the players see the server in their list
    std::string game_version;      // must match the players' TLD exactly (tld_Data/StreamingAssets/version.txt)
};

struct PlayerLine {
    uint8_t id = 0;
    std::string name;
    std::string scene;  // "" while loading / in a menu
    double rtt_ms = -1;
    uint32_t link = 0;
};

class Server {
public:
    using LogFn = std::function<void(const std::string&)>;
    explicit Server(LogFn log);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(const Options& options);
    // One round: network in, chat passed on, network out. Call often (the loop waits on wait()).
    void update();
    void wait(int timeout_ms);
    void stop();

    std::vector<PlayerLine> players() const;
    bool kick(uint8_t id);                // turned away for as long as this server runs
    void say(const std::string& text);    // a chat line from the server to everyone
    // A console line ("players", "kick 2", "say hello", "help"). Returns the answer; sets *quit on "stop".
    std::string command(const std::string& line, bool* quit);

    uint64_t chat_passed() const { return chat_passed_; }

private:
    void on_packets();
    void report_changes();

    LogFn log_;
    Options opt_;
    net::Session session_;
    bool started_ = false;
    uint64_t chat_passed_ = 0;
    std::vector<net::Packet> in_;
    std::vector<std::pair<uint8_t, std::string>> known_;  // who was in, last round (for joined / left lines)
};

}  // namespace srv
