#include "server/server.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace srv {

Server::Server(LogFn log) : log_(std::move(log)), session_(log_), world_(session_, log_) {}

Server::~Server() { stop(); }

bool Server::start(const Options& options) {
    opt_ = options;
    if (!opt_.world_file.empty() && !world_.load(opt_.world_file)) return false;  // never write over one we can't read
    last_save_ms_ = net::now_ms();
    net::Config cfg;
    cfg.role = net::Role::Host;
    cfg.transport = "udp";
    cfg.bind_ip = opt_.bind_ip;
    cfg.port = opt_.port;
    cfg.name = opt_.name;
    cfg.body = "";               // no body: the server is nobody in the world
    cfg.game_version = opt_.game_version;
    cfg.content = "?";
    cfg.content_check = false;   // the server has no DLC of its own; the players' own check is between them
    cfg.ask_to_join = false;
    if (!session_.start(cfg)) return false;
    started_ = true;
    world_.on_started();
    // Never a valid state: the players' games keep the server's "avatar" hidden. Always "in game" (START pressed),
    // so a player in the lobby goes straight in.
    net::StateMsg nobody;
    nobody.valid = false;
    session_.set_local_state(nobody);
    session_.set_ready(true);
    char line[160];
    snprintf(line, sizeof(line), "server '%s' listening on %s:%u (UDP), game version '%s'", opt_.name.c_str(),
             opt_.bind_ip.c_str(), static_cast<unsigned>(opt_.port), opt_.game_version.c_str());
    log_(line);
    return true;
}

void Server::update() {
    if (!started_) return;
    session_.update();
    on_packets();
    const double now = net::now_ms();
    world_.tick(now);
    report_changes();
    if (world_.dirty() && now - last_save_ms_ >= opt_.save_every_s * 1000) save();
}

bool Server::save() {
    last_save_ms_ = net::now_ms();
    if (opt_.world_file.empty()) return false;
    if (!world_.save(opt_.world_file)) return false;
    log_("world: saved (" + world_.describe() + ")");
    return true;
}

void Server::wait(int timeout_ms) {
    if (started_) session_.wait(timeout_ms);
}

void Server::stop() {
    if (!started_) return;
    started_ = false;
    if (world_.dirty()) save();
    session_.stop();
    log_("server stopped");
}

void Server::on_packets() {
    in_.clear();
    session_.drain_packets(in_);
    const double now = net::now_ms();
    for (const net::Packet& p : in_) {
        if (world_.on_packet(p, now)) continue;
        switch (p.type) {
            case net::Msg::Chat: {  // as the host's game does: pass a friend's line on to the others
                net::Reader r = net::packet_body(p);
                std::string who, said;
                if (!net::read_chat(r, who, said)) break;
                net::Writer w(net::Msg::Chat, 1024);
                net::write_chat(w, who, said);
                session_.send_packet_except(p.from, w, true);
                ++chat_passed_;
                log_("chat: " + who + ": " + said);
                break;
            }
            default:
                break;  // animals, sleep, the train...: later stages
        }
    }
}

void Server::report_changes() {
    const net::Status st = session_.status();
    std::vector<std::pair<uint8_t, std::string>> now;
    for (const net::PlayerInfo& p : st.players)
        if (p.linked) now.emplace_back(p.id, p.name);
    for (const auto& p : now)
        if (std::find(known_.begin(), known_.end(), p) == known_.end())
            log_("joined: " + p.second + " (id " + std::to_string(p.first) + ")");
    for (const auto& p : known_)
        if (std::find(now.begin(), now.end(), p) == now.end())
            log_("left: " + p.second + " (id " + std::to_string(p.first) + ")");
    known_ = std::move(now);
}

std::vector<PlayerLine> Server::players() const {
    std::vector<PlayerLine> out;
    const net::Status st = session_.status();
    for (const net::PlayerInfo& p : st.players) {
        if (!p.linked) continue;
        PlayerLine l;
        l.id = p.id;
        l.name = p.name;
        l.rtt_ms = p.rtt_ms;
        l.link = p.link;
        if (auto s = session_.remote_state(p.id); s && s->valid) l.scene = s->scene;
        out.push_back(std::move(l));
    }
    return out;
}

bool Server::kick(uint8_t id) {
    for (const PlayerLine& p : players())
        if (p.id == id) {
            log_("kicked: " + p.name + " (id " + std::to_string(id) + ")");
            return session_.turn_away(p.link);
        }
    return false;
}

void Server::say(const std::string& text) {
    if (text.empty()) return;
    net::Writer w(net::Msg::Chat, 1024);
    net::write_chat(w, opt_.name, text);
    session_.send_packet(w, true);
    log_("chat: " + opt_.name + ": " + text);
}

std::string Server::command(const std::string& line, bool* quit) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    if (cmd.empty()) return {};
    if (cmd == "help" || cmd == "?")
        return "players | kick <id> | say <text> | world | save | forget mode|features|clock | stop";
    if (cmd == "players") {
        const auto list = players();
        if (list.empty()) return "nobody is in";
        std::string out;
        for (const PlayerLine& p : list) {
            char row[200];
            snprintf(row, sizeof(row), "%u  %-20s %-28s %s", static_cast<unsigned>(p.id), p.name.c_str(),
                     p.scene.empty() ? "(loading / menu)" : p.scene.c_str(),
                     p.rtt_ms < 0 ? "-" : (std::to_string(static_cast<int>(p.rtt_ms + 0.5)) + " ms").c_str());
            out += row;
            out += '\n';
        }
        out.pop_back();
        return out;
    }
    if (cmd == "kick") {
        int id = -1;
        in >> id;
        if (id < 1 || id > 255) return "kick <id> (see players)";
        return kick(static_cast<uint8_t>(id)) ? "kicked" : "no player with that id";
    }
    if (cmd == "say") {
        std::string text;
        std::getline(in, text);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        if (text.empty()) return "say <text>";
        say(text);
        return "said";
    }
    if (cmd == "world")
        return world_.describe() + (world_.source() >= 0 ? "; player " + std::to_string(world_.source()) + "'s game runs the clock"
                                                         : "; nobody's game runs the clock");
    if (cmd == "forget") {
        std::string what;
        in >> what;
        return world_.forget(what) ? "forgotten: the next game in gives its own " + what : "forget mode | features | clock";
    }
    if (cmd == "save") {
        if (opt_.world_file.empty()) return "no world file";
        return save() ? "saved" : "couldn't save (see above)";
    }
    if (cmd == "stop" || cmd == "quit" || cmd == "exit") {
        if (quit) *quit = true;
        return "stopping";
    }
    return "unknown command '" + cmd + "' (help)";
}

}  // namespace srv
