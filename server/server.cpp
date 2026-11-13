#include "server/server.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace srv {

Server::Server(LogFn log) : log_(std::move(log)), session_(log_), world_(session_, log_) {}

Server::~Server() { stop(); }

bool Server::start(const Options& options) {
    opt_ = options;
    if (!opt_.world_file.empty() && !world_.load(opt_.world_file)) return false;  // never write over one we can't read
    backup_world();
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
    started_ms_ = net::now_ms();
    write_status(true);
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
    if (now - last_status_ms_ >= 5000) write_status(true);
    if (world_.dirty() && now - last_save_ms_ >= opt_.save_every_s * 1000) save();
}

// The world file as it was when the server started, copied to backups/world-YYYYMMDD-HHMMSS.bin beside it (the newest
// kMaxBackups kept): a bad session can be undone by putting one back as world.bin.
void Server::backup_world() {
    constexpr size_t kMaxBackups = 20;
    if (opt_.world_file.empty()) return;
    std::error_code ec;
    const std::filesystem::path file(opt_.world_file);
    if (!std::filesystem::is_regular_file(file, ec)) return;
    const std::filesystem::path dir = file.parent_path() / "backups";
    std::filesystem::create_directories(dir, ec);
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
    const std::filesystem::path to = dir / (file.stem().string() + "-" + stamp + file.extension().string());
    std::filesystem::copy_file(file, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        log_("world: couldn't make a backup in " + dir.string() + " (" + ec.message() + ")");
        return;
    }
    log_("world: backup of the file as it was: backups/" + to.filename().string());
    std::vector<std::filesystem::path> old;
    const std::string prefix = file.stem().string() + "-";
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().filename().string().rfind(prefix, 0) == 0 && e.path().extension() == file.extension())
            old.push_back(e.path());
    std::sort(old.begin(), old.end());
    for (size_t i = 0; i + kMaxBackups < old.size(); ++i) std::filesystem::remove(old[i], ec);
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
    write_status(false);
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
            // F11 reports and "POBIERZ LOGI": as a host's game, on to the others as they came (the server has no log
            // of a game to add; the asker gets the other players' logs)
            case net::Msg::Report:
            case net::Msg::LogsWant:
            case net::Msg::LogsFile: {
                if (p.data.size() <= net::kHeaderSize) break;
                const size_t size = p.data.size() - net::kHeaderSize;
                net::Writer w(p.type, size + 64);
                w.put_raw(p.data.data() + net::kHeaderSize, size);
                session_.send_packet_except(p.from, w, true);
                break;
            }
            default:
                break;  // (carcass harvests, the slot machine: not on the server yet)
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

namespace {
std::string json_text(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c);
        else if (c < 0x20) out += ' ';
        else out += static_cast<char>(c);
    }
    return out + "\"";
}
}  // namespace

// status.json beside the world (whole, then swapped in): what tools/server_bot.py shows on Discord.
void Server::write_status(bool online) {
    last_status_ms_ = net::now_ms();
    if (opt_.status_file.empty()) return;
    std::string j = "{\"online\": " + std::string(online ? "true" : "false");
    j += ", \"time\": " + std::to_string(static_cast<long long>(std::time(nullptr)));
    j += ", \"up_s\": " + std::to_string(static_cast<long long>((net::now_ms() - started_ms_) / 1000));
    j += ", \"name\": " + json_text(opt_.name) + ", \"port\": " + std::to_string(opt_.port);
    j += ", \"game_version\": " + json_text(opt_.game_version) + ", \"max\": " + std::to_string(net::kMaxPlayers - 1);
    j += ", \"clock\": " + json_text(world_.clock_text());
    j += ", \"players\": [";
    bool first = true;
    if (online)
        for (const PlayerLine& p : players()) {
            j += std::string(first ? "" : ", ") + "{\"id\": " + std::to_string(p.id) + ", \"name\": " + json_text(p.name) +
                 ", \"scene\": " + json_text(p.scene) + "}";
            first = false;
        }
    j += "]}\n";
    const std::filesystem::path file(opt_.status_file);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << j;
        if (!out) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
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
