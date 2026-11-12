#pragma once
// The animals on the server: the mod's wildlife ownership ([wildlife] ownership=1; mod/src/wildlife_owner.inc) with the
// host's part done here. Each animal is run by one player's game (a real TLD animal there, a copy everywhere else); the
// arbiter - a host's game until now, this class on the server - keeps the registry of every shared animal, decides who
// runs each one (net::wild::decide: distance, who looks at it, link, who it is after), hands them over (Release ->
// Take), says which game spawns in which scene (Controllers), passes the owners' frames and sounds on, routes a shot
// to the game that runs the animal, counts deaths for the spawning game and keeps the spawn regions' state from one
// spawning game to the next. Animals nobody is near stay in the registry, frozen, with the world's hours noted.
//
// The server is a host that is never in a scene: what the mod's host does with its own game (keeping carcasses,
// taking animals itself) is left to the players' games, which already do it while a host is away - a body whose owner
// left goes to another game near it.
//
// No session here: what goes out goes through `Out`, so the logic is tested without a network. One thread.

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "net/protocol.h"
#include "net/wildlife_own.h"

namespace srv {

class Wildlife {
public:
    struct Out {
        std::function<void(uint8_t player, const net::Writer& w, bool reliable)> to;      // one player
        std::function<void(uint8_t player, const net::Writer& w, bool reliable)> except;  // everyone but one
        std::function<void(const net::Writer& w, bool reliable)> all;
    };
    using LogFn = std::function<void(const std::string&)>;

    // A player in the game now.
    struct Present {
        uint8_t id = 0;
        std::string scene;
        float x = 0, y = 0, z = 0, yaw = 0;
        bool inside = false;  // in a Seamless Interiors room
        double rtt_ms = 0;
    };

    Wildlife(Out out, LogFn log);

    // One of the players' messages. False: not about animals.
    bool on_packet(const net::Packet& p, double now_ms);
    // Every round. hours: the world's clock in game hours (it only ever goes on); < 0 unknown.
    void tick(const std::vector<Present>& players, double now_ms, double hours);

    // The registry and the spawn regions in the world file (the mod's world_keep records: Wildlife, WildRegions).
    bool load(bool regions, const std::vector<uint8_t>& msg);
    void rebase(double hours);  // a host's file: its game hours mean nothing here; everything counts from `hours`
    void save(const std::function<void(bool regions, const net::Writer& w)>& add) const;

    size_t animals() const { return registry_.size(); }
    size_t region_scenes() const { return regions_.size(); }
    uint8_t owner(uint32_t id) const;        // net::kNoPlayer: nobody (or unknown)
    uint8_t controller(const std::string& scene) const;
    bool take_dirty() {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }
    int per_player = 50;  // % more animals for each player beyond the first (the mod's [wildlife] per_player)

private:
    struct Entry {
        uint32_t id = 0;
        std::string scene;
        uint8_t owner = net::kNoPlayer;
        uint16_t epoch = 0;
        net::AnimalRec last;
        double last_ms = 0;
        std::string text;  // TLD's own record of it (BaseAi.Serialize) from its last handover
        bool frozen = false;
        uint8_t pending_to = net::kNoPlayer;
        uint16_t pending_epoch = 0;
        double pending_ms = 0;
        net::wild::Track track;
        std::array<double, net::kMaxPlayers> blocked_until{};
        double wrong_ms = 0;
        double frozen_h = -1;  // the world's hours when it was frozen (-1: unknown)
        std::string body;      // a carcass: BodyHarvest.Serialize() as its last owner had it
        double owned_ms = 0;
    };
    struct SceneControl {
        uint8_t player = net::kNoPlayer;
        bool fresh = false;
    };
    struct RegionStore {
        net::WildCtlMsg msg;
        uint8_t reporter = net::kNoPlayer;
    };

    static bool in_scene(const std::vector<Present>& players, uint8_t id, const std::string& scene);
    void deliver_take(const net::WildCtlMsg& t);
    net::WildCtlMsg take_from_registry(const Entry& e, uint8_t to, uint16_t epoch) const;
    void tell_controller_kill(const net::WildCtlMsg& k);
    static net::WildCtlMsg kill_of(const Entry& e, uint8_t flags, uint8_t died_on);
    void gone(uint32_t id, uint8_t from, double now);
    void store_regions(net::WildCtlMsg m, uint8_t from);
    void hand_regions(const std::string& scene, uint8_t to);
    void age_frozen(Entry& e, net::WildCtlMsg& t);
    void transfer(Entry& e, uint8_t to, const char* why, const std::vector<Present>& players, double now);
    void take(net::WildCtlMsg t, uint8_t from, double now);
    void refuse(uint32_t id, uint16_t epoch, uint8_t from, double now);
    std::vector<net::AnimalRec> ingest(uint8_t owner, const std::string& scene, const std::vector<net::AnimalRec>& recs, double now);
    void arbitrate(const std::vector<Present>& players, double now);
    void logf(const char* fmt, ...);

    Out out_;
    LogFn log_;
    std::unordered_map<uint32_t, Entry> registry_;
    std::unordered_map<std::string, SceneControl> controllers_;
    std::unordered_map<std::string, RegionStore> regions_;
    std::unordered_map<std::string, std::vector<net::WildCtlMsg>> kills_queued_;
    std::array<double, net::kMaxPlayers> keeps_nothing_until_{};
    std::array<int, net::kMaxPlayers> quick_put_aways_{};
    std::array<double, net::kMaxPlayers> quick_put_away_ms_{};
    std::array<std::pair<std::string, double>, net::kMaxPlayers> arrival_{};
    net::wild::RateLimit handovers_{4, 6};
    std::vector<Present> players_;  // as of the last tick
    double last_arbiter_ = 0, last_controllers_ = 0;
    std::string controllers_sent_;
    double hours_ = -1;
    bool dirty_ = false;
};

}  // namespace srv
