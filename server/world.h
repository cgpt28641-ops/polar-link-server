#pragma once
// The shared world on the server (SERVER_PLAN stages 1-3): what a host's game keeps for scenes it isn't in, kept here
// for every scene, always. The server is "a host that is in no scene": it never has a game of its own to look at, so
// everything it knows came from the players' games as protocol messages, and that is how it is stored, passed on and
// written to disk (the mod's world_keep file format: mod/src/world_keep.h, so a host's worlds/<save>.bin loads as is).
//
//   items       a scene's loose items: the newest ItemPut per item, and which are gone (ItemGone). A scene with a
//               whole list (ItemList, from a host's file) is told as that list to a player who arrives (they drop
//               what isn't on it and ask for what they lack: ItemWant); one without is told as the changes kept.
//               Two takes of one item at about the same time: net::Claims decides (a later one is refused).
//   containers  the newest contents per container (ContainerPut); a player opening one asks (ContainerWant) and gets
//               them, or "nothing known: yours stands". One player at a time in a container (net::LockTable).
//   objects     broken down / harvested / locks / placements / bodies: the newest ObjectPut per object.
//   fires, water  the newest FirePut / WaterPut per fire / source.
//   animals     the mod's wildlife ownership with the host's part done here (server/wildlife.h): the registry, who
//               runs which animal, who spawns where, deaths and spawn regions; kept in the file too.
//   sleep       sleeping and passing time together: decided here from the players' States (server/sleep.h).
//   passed on   blood, spray paint, house lights, handing an item over (Give): to the other players, nothing kept.
//   clock, weather, mode, seed, feature switches: the server has no game to run them. One player's game at a time is
//               the "world source" (the mod's coop_server.h): its World (clock, weather, wind) comes here, is kept and
//               goes to the others as a host's would; a game that comes in follows the kept world for a few seconds
//               before it may be picked, and with nobody in the clock stands still. The experience mode + seed and the
//               feature switches are the first game's to give them, kept for good (console: forget mode / features).
// A player who arrives in a scene (linked, caught up, a valid State there) gets what is kept for it once per arrival.
//
// One thread (the server's loop).

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "net/claims.h"
#include "net/container_locks.h"
#include "net/session.h"
#include "server/sleep.h"
#include "server/wildlife.h"

namespace srv {

class World {
public:
    using LogFn = std::function<void(const std::string&)>;
    World(net::Session& session, LogFn log);

    // The world file: read at start (missing: an empty world), written whole to <file>.tmp and swapped in.
    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file);
    bool dirty() const { return dirty_; }

    // One of the players' messages. False: not a world message (the caller's).
    void on_started();  // once the session runs: the kept clock and weather go out again
    bool on_packet(const net::Packet& p, double now_ms);
    bool forget(const std::string& what);  // console: "mode", "features" or "clock": the next game gives its own
    int source() const { return source_; } // the player whose game is the world's clock and weather (-1 nobody)
    bool has_mode() const { return have_mode_; }
    bool has_features() const { return have_features_; }
    bool has_clock() const { return have_world_; }
    const Wildlife& wildlife() const { return wildlife_; }
    const Sleep& sleep() const { return sleep_; }
    // Every round: arrivals, locks whose player went quiet or left.
    void tick(double now_ms);

    struct Counts {
        size_t scenes = 0, items = 0, gone = 0, listed_scenes = 0, containers = 0, objects = 0, fires = 0, water = 0,
               other = 0;
    };
    Counts counts() const;
    std::string describe() const;  // one line, for the console's "world"
    std::string clock_text() const;  // "day 12 14:05", or "" before any game gave a clock

private:
    struct Scene {
        std::unordered_map<std::string, net::ItemPutMsg> items;   // pdid -> the item lying there
        std::unordered_map<std::string, net::ItemGoneMsg> gone;   // pdid -> it left (not kept once the scene is listed)
        bool listed = false;                                      // `list` is every loose item of the scene
        std::unordered_set<uint64_t> list;                        // pdid hashes
        std::unordered_map<std::string, std::string> containers;  // id -> contents
        std::map<std::string, net::ObjectState> objects;          // kind + id -> state
        std::unordered_map<std::string, net::FirePutMsg> fires;
        std::unordered_map<std::string, net::WaterPutMsg> water;
    };
    struct Seen {  // the newest change applied per thing from one player (reliable messages can arrive out of order)
        uint32_t link = 0;
        std::unordered_map<std::string, uint32_t> seq;
    };
    struct Arrival {
        uint32_t link = 0;
        std::string scene;      // what was last sent for (this link)
        std::string pending;    // seen there, to be sent at due_ms
        double due_ms = 0;
    };

    struct Friend {  // what we told one player's connection, and since when its game is in
        uint32_t link = 0;
        double in_game_since = -1;  // -1: not in the game (or away too long)
        double last_valid = 0;
        uint8_t flags = 0xFF;       // the Msg::Server flags last sent
        double flags_ms = 0;
        bool told_mode = false, told_features = false;
    };
    bool shared_packet(const net::Packet& p);
    void shared_tick(const net::Status& st, double now_ms);
    bool newest(const net::Packet& p, const std::string& key, uint32_t seq);
    void item_put(const net::Packet& p, net::ItemPutMsg m);
    void item_gone(const net::Packet& p, net::ItemGoneMsg m, double now_ms);
    void item_want(const net::Packet& p, const net::ItemHashesMsg& m);
    void container_put(const net::Packet& p, net::ContainerPutMsg m);
    void container_want(const net::Packet& p, const net::ContainerWantMsg& m);
    void container_lock(const net::Packet& p, const net::ContainerLockMsg& m, double now_ms);
    void tell_lock(const std::string& key, int to = -1);
    void not_yours(uint8_t to, const std::string& scene, const std::string& pdid, bool tossed);
    void send_scene(uint8_t to, const std::string& scene);
    net::SleepClock sleep_clock(double now_ms) const;
    void logf(const char* fmt, ...);

    net::Session& session_;
    LogFn log_;
    std::unordered_map<std::string, Scene> scenes_;
    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> other_;  // records of the file we don't handle yet (animals)
    std::array<Seen, net::kMaxPlayers> seen_{};
    std::array<Arrival, net::kMaxPlayers> arrivals_{};
    net::Claims claims_;
    net::LockTable locks_;
    double last_lock_tick_ = 0;
    std::array<Friend, net::kMaxPlayers> friends_{};
    Wildlife wildlife_;
    Sleep sleep_;
    double world_ms_ = 0;  // when the newest World came (the clock between two of them: sleep_clock)
    double hours_ = 0;  // the world's clock in game hours, counted from the clock's steps (it only ever goes on)
    int source_ = -1;
    bool have_world_ = false;
    net::WorldMsg world_;
    bool have_mode_ = false;
    std::string mode_;
    int32_t seed_ = 0;
    bool have_features_ = false;
    uint32_t features_ = 0;
    uint32_t seq_ = 0;  // our change counter (what we pass on and send goes as the host's own)
    bool dirty_ = false;
};

}  // namespace srv
