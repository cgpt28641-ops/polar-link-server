#include "server/world.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "net/uniques.h"
#include "world_keep.h"  // the mod's: the file's record tags (header only here)

namespace srv {
namespace {

using world_keep::Tag;

constexpr uint32_t kFileMagic = 0x57444C54;  // bytes "TLDW", as the mod's world_keep
constexpr uint32_t kFileVersion = 1;
// Ours, beyond the mod's tags: our change counter (u32). A restarted server goes on counting from it, so what it sends
// is never older than what a player's game already took from "the host" before the restart.
constexpr uint8_t kTagServerSeq = 200;
constexpr uint32_t kSeqMargin = 100000;  // sent since the last save, at most
constexpr double kArrivalDelayMs = 500;  // a player seen in a scene gets it this much later (their game has settled)
constexpr uint8_t kNobody = 0xFE;        // LockTable::expire's "keep": no player of ours is exempt

void put_u32(std::string& out, uint32_t v) { out.append(reinterpret_cast<const char*>(&v), sizeof(v)); }

bool get_u32(const std::string& in, size_t& at, uint32_t& v) {
    if (at + sizeof(v) > in.size()) return false;
    memcpy(&v, in.data() + at, sizeof(v));
    at += sizeof(v);
    return true;
}

std::string object_key(const net::ObjectState& s) {
    return std::to_string(static_cast<unsigned>(s.kind)) + "/" + s.id;
}

std::string short_id(const std::string& id) { return id.size() > 13 ? id.substr(0, 8) + ".." + id.substr(id.size() - 3) : id; }

void add_record(std::string& data, uint32_t& count, Tag tag, const net::Writer& w) {
    if (!w.ok()) return;
    data += static_cast<char>(tag);
    put_u32(data, static_cast<uint32_t>(w.size()));
    data.append(reinterpret_cast<const char*>(w.data()), w.size());
    ++count;
}

}  // namespace

World::World(net::Session& session, LogFn log) : session_(session), log_(std::move(log)) {}

void World::logf(const char* fmt, ...) {
    char line[768];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    log_(line);
}

// --- the file --------------------------------------------------------------------------------------------------------

bool World::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        logf("world: no file %s yet: an empty world", file.filename().string().c_str());
        return true;
    }
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t at = 0;
    uint32_t magic = 0, version = 0, count = 0;
    if (!get_u32(data, at, magic) || !get_u32(data, at, version) || !get_u32(data, at, count) || magic != kFileMagic ||
        version != kFileVersion) {
        logf("world: %s isn't a world file (or another version); not loaded", file.string().c_str());
        return false;
    }
    std::vector<world_keep::Record> records;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t size = 0;
        if (at + 1 > data.size()) break;
        const uint8_t tag = static_cast<uint8_t>(data[at++]);
        if (!get_u32(data, at, size) || at + size > data.size()) break;
        world_keep::Record r;
        r.tag = static_cast<Tag>(tag);
        r.msg.assign(data.begin() + static_cast<ptrdiff_t>(at), data.begin() + static_cast<ptrdiff_t>(at + size));
        at += size;
        records.push_back(std::move(r));
    }
    if (records.size() != count)
        logf("world: %s is cut short: %zu of %u record(s) read", file.filename().string().c_str(), records.size(), count);
    // What a host last saw in a scene first, then the changes made there since (they win).
    auto later = [](Tag t) { return t == Tag::ItemChange || t == Tag::ObjectChange; };
    std::stable_sort(records.begin(), records.end(),
                     [&](const world_keep::Record& a, const world_keep::Record& b) { return !later(a.tag) && later(b.tag); });
    size_t bad = 0;
    for (const world_keep::Record& r : records) {
        if (static_cast<uint8_t>(r.tag) == kTagServerSeq) {
            if (r.msg.size() == sizeof(uint32_t)) {
                memcpy(&seq_, r.msg.data(), sizeof(seq_));
                seq_ += kSeqMargin;
            }
            continue;
        }
        const net::Packet p = world_keep::packet(r);
        net::Reader body = net::packet_body(p);
        bool ok = false;
        switch (r.tag) {
            case Tag::ItemRecord: {
                net::ItemPutMsg m;
                if (p.type == net::Msg::ItemPut && (ok = net::read_item_put(body, m))) {
                    Scene& s = scenes_[m.scene];
                    s.items[m.pdid] = std::move(m);
                }
                break;
            }
            case Tag::ItemList: {
                net::ItemHashesMsg m;
                if (p.type == net::Msg::ItemList && (ok = net::read_item_hashes(body, m))) {
                    Scene& s = scenes_[m.scene];
                    s.listed = true;
                    s.list.insert(m.hashes.begin(), m.hashes.end());
                }
                break;
            }
            case Tag::ItemChange: {
                if (p.type == net::Msg::ItemPut) {
                    net::ItemPutMsg m;
                    if ((ok = net::read_item_put(body, m))) {
                        Scene& s = scenes_[m.scene];
                        s.gone.erase(m.pdid);
                        if (s.listed) s.list.insert(net::pdid_hash(m.pdid));
                        s.items[m.pdid] = std::move(m);
                    }
                } else if (p.type == net::Msg::ItemGone) {
                    net::ItemGoneMsg m;
                    if ((ok = net::read_item_gone(body, m))) {
                        Scene& s = scenes_[m.scene];
                        s.items.erase(m.pdid);
                        if (s.listed)
                            s.list.erase(net::pdid_hash(m.pdid));
                        else
                            s.gone[m.pdid] = std::move(m);
                    }
                }
                break;
            }
            case Tag::ObjectChange:
            case Tag::ObjectGone: {
                net::ObjectPutMsg m;
                if (p.type == net::Msg::ObjectPut && (ok = net::read_object_put(body, m)))
                    scenes_[m.scene].objects[object_key(m.object)] = std::move(m.object);
                break;
            }
            case Tag::FireChange: {
                net::FirePutMsg m;
                if (p.type == net::Msg::FirePut && (ok = net::read_fire_put(body, m))) {
                    Scene& s = scenes_[m.scene];
                    s.fires[m.id] = std::move(m);
                }
                break;
            }
            case Tag::ContainerTheirs:
            case Tag::ContainerOurs: {
                net::ContainerPutMsg m;
                if (p.type == net::Msg::ContainerPut && (ok = net::read_container_put(body, m)))
                    scenes_[m.scene].containers[m.id] = std::move(m.text);
                break;
            }
            case Tag::WaterChange: {
                net::WaterPutMsg m;
                if (p.type == net::Msg::WaterPut && (ok = net::read_water_put(body, m))) {
                    Scene& s = scenes_[m.scene];
                    s.water[m.id] = std::move(m);
                }
                break;
            }
            default:  // animals (stage 5) and anything newer: kept as it is, written back
                other_.emplace_back(static_cast<uint8_t>(r.tag), r.msg);
                ok = true;
                break;
        }
        if (!ok) ++bad;
    }
    dirty_ = false;
    logf("world: loaded %s: %s%s", file.filename().string().c_str(), describe().c_str(),
         bad ? (" (" + std::to_string(bad) + " record(s) unreadable, dropped)").c_str() : "");
    return true;
}

bool World::save(const std::filesystem::path& file) {
    std::string records;
    uint32_t count = 0;
    for (const auto& [name, s] : scenes_) {
        if (s.listed) {
            net::ItemHashesMsg m;
            m.scene = name;
            m.hashes.assign(s.list.begin(), s.list.end());
            net::Writer w(net::Msg::ItemList, net::kMaxMessage);
            net::write_item_hashes(w, m);
            add_record(records, count, Tag::ItemList, w);
        }
        for (const auto& [pdid, m] : s.items) {
            net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
            net::write_item_put(w, m);
            add_record(records, count, Tag::ItemRecord, w);
        }
        for (const auto& [pdid, m] : s.gone) {
            net::Writer w(net::Msg::ItemGone);
            net::write_item_gone(w, m);
            add_record(records, count, Tag::ItemChange, w);
        }
        for (const auto& [id, text] : s.containers) {
            net::ContainerPutMsg m;
            m.scene = name;
            m.id = id;
            m.text = text;
            net::Writer w(net::Msg::ContainerPut, net::kMaxMessage);
            net::write_container_put(w, m);
            add_record(records, count, Tag::ContainerTheirs, w);
        }
        for (const auto& [key, o] : s.objects) {
            net::ObjectPutMsg m;
            m.scene = name;
            m.object = o;
            net::Writer w(net::Msg::ObjectPut, net::kMaxMessage);
            net::write_object_put(w, m);
            add_record(records, count, Tag::ObjectChange, w);
        }
        for (const auto& [id, m] : s.fires) {
            net::Writer w(net::Msg::FirePut, net::kMaxMessage);
            net::write_fire_put(w, m);
            add_record(records, count, Tag::FireChange, w);
        }
        for (const auto& [id, m] : s.water) {
            net::Writer w(net::Msg::WaterPut, net::kMaxMessage);
            net::write_water_put(w, m);
            add_record(records, count, Tag::WaterChange, w);
        }
    }
    for (const auto& [tag, msg] : other_) {
        records += static_cast<char>(tag);
        put_u32(records, static_cast<uint32_t>(msg.size()));
        records.append(reinterpret_cast<const char*>(msg.data()), msg.size());
        ++count;
    }
    records += static_cast<char>(kTagServerSeq);
    put_u32(records, sizeof(uint32_t));
    put_u32(records, seq_);
    ++count;

    std::string data;
    put_u32(data, kFileMagic);
    put_u32(data, kFileVersion);
    put_u32(data, count);
    data += records;

    // Written whole beside the old one, then swapped in: a crash or a pulled disk leaves the old file or the new one.
    auto tmp = file;
    tmp += ".tmp";
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) {
            logf("world: couldn't write %s", tmp.string().c_str());
            return false;
        }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        logf("world: couldn't replace %s (%s)", file.string().c_str(), ec.message().c_str());
        return false;
    }
    dirty_ = false;
    return true;
}

World::Counts World::counts() const {
    Counts c;
    c.scenes = scenes_.size();
    c.other = other_.size();
    for (const auto& [name, s] : scenes_) {
        c.items += s.items.size();
        c.gone += s.gone.size();
        c.listed_scenes += s.listed ? 1 : 0;
        c.containers += s.containers.size();
        c.objects += s.objects.size();
        c.fires += s.fires.size();
        c.water += s.water.size();
    }
    return c;
}

std::string World::describe() const {
    const Counts c = counts();
    char line[320];
    snprintf(line, sizeof(line),
             "%zu scene(s) (%zu with a whole item list), %zu item(s), %zu taken, %zu container(s), %zu object(s), "
             "%zu fire(s), %zu water source(s), %zu other record(s)",
             c.scenes, c.listed_scenes, c.items, c.gone, c.containers, c.objects, c.fires, c.water, c.other);
    return line;
}

// --- the players' messages -------------------------------------------------------------------------------------------

bool World::newest(const net::Packet& p, const std::string& key, uint32_t seq) {
    if (p.from >= net::kMaxPlayers) return false;
    Seen& s = seen_[p.from];
    if (s.link != p.link) {
        s.seq.clear();
        s.link = p.link;
    }
    const auto last = s.seq.find(key);
    if (last != s.seq.end() && static_cast<int32_t>(seq - last->second) <= 0) return false;
    s.seq[key] = seq;
    return true;
}

bool World::on_packet(const net::Packet& p, double now_ms) {
    net::Reader r = net::packet_body(p);
    switch (p.type) {
        case net::Msg::ItemPut: {
            net::ItemPutMsg m;
            if (net::read_item_put(r, m)) item_put(p, std::move(m));
            return true;
        }
        case net::Msg::ItemGone: {
            net::ItemGoneMsg m;
            if (net::read_item_gone(r, m)) item_gone(p, std::move(m), now_ms);
            return true;
        }
        case net::Msg::ItemWant: {
            net::ItemHashesMsg m;
            if (net::read_item_hashes(r, m)) item_want(p, m);
            return true;
        }
        case net::Msg::ContainerPut: {
            net::ContainerPutMsg m;
            if (net::read_container_put(r, m)) container_put(p, std::move(m));
            return true;
        }
        case net::Msg::ContainerWant: {
            net::ContainerWantMsg m;
            if (net::read_container_want(r, m)) container_want(p, m);
            return true;
        }
        case net::Msg::ContainerLock: {
            net::ContainerLockMsg m;
            if (net::read_container_lock(r, m)) container_lock(p, m, now_ms);
            return true;
        }
        case net::Msg::ObjectPut: {
            net::ObjectPutMsg m;
            if (!net::read_object_put(r, m)) return true;
            const std::string key = object_key(m.object);
            if (!newest(p, "o/" + key, m.seq)) return true;
            scenes_[m.scene].objects[key] = m.object;
            dirty_ = true;
            m.seq = ++seq_;
            net::Writer w(net::Msg::ObjectPut, net::kMaxMessage);
            net::write_object_put(w, m);
            session_.send_packet_except(p.from, w, true);
            return true;
        }
        case net::Msg::FirePut: {
            net::FirePutMsg m;
            if (!net::read_fire_put(r, m) || !newest(p, "f/" + m.id, m.seq)) return true;
            m.seq = ++seq_;
            net::Writer w(net::Msg::FirePut, net::kMaxMessage);
            net::write_fire_put(w, m);
            session_.send_packet_except(p.from, w, true);
            Scene& s = scenes_[m.scene];
            s.fires[m.id] = std::move(m);
            dirty_ = true;
            return true;
        }
        case net::Msg::WaterPut: {
            net::WaterPutMsg m;
            if (!net::read_water_put(r, m) || !newest(p, "w/" + m.id, m.seq)) return true;
            m.seq = ++seq_;
            net::Writer w(net::Msg::WaterPut, net::kMaxMessage);
            net::write_water_put(w, m);
            session_.send_packet_except(p.from, w, true);
            Scene& s = scenes_[m.scene];
            s.water[m.id] = std::move(m);
            dirty_ = true;
            return true;
        }
        case net::Msg::BloodMark: {
            net::BloodMarkMsg m;
            if (!net::read_blood_mark(r, m)) return true;
            net::Writer w(net::Msg::BloodMark);
            net::write_blood_mark(w, m);
            session_.send_packet_except(p.from, w, true);
            return true;
        }
        case net::Msg::SprayMark: {
            net::SprayMarkMsg m;
            if (!net::read_spray_mark(r, m)) return true;
            net::Writer w(net::Msg::SprayMark);
            net::write_spray_mark(w, m);
            session_.send_packet_except(p.from, w, true);
            return true;
        }
        case net::Msg::HouseLights: {
            const uint8_t on = r.get<uint8_t>();
            const std::string scene = r.get_blob(64);
            if (!r.ok() || scene.empty()) return true;
            net::Writer w(net::Msg::HouseLights, 128);
            w.put<uint8_t>(on ? 1 : 0);
            w.put_blob(scene, 64);
            session_.send_packet_except(p.from, w, true);
            return true;
        }
        case net::Msg::Give: {  // as the host's game: everyone else hears; only the receiver gets the item itself
            net::GiveMsg m;
            if (!net::read_give(r, m)) return true;
            const net::Status st = session_.status();
            for (const net::PlayerInfo& pl : st.players) {
                if (!pl.linked || pl.id == 0 || pl.id == p.from) continue;
                net::GiveMsg c = m;
                if (c.op == net::GiveOp::Done && pl.id != m.receiver) c.prefab.clear(), c.record.clear();
                net::Writer w(net::Msg::Give, net::kMaxMessage);
                net::write_give(w, c);
                session_.send_packet_to(pl.id, w, true);
            }
            return true;
        }
        default:
            return false;
    }
}

void World::item_put(const net::Packet& p, net::ItemPutMsg m) {
    if (m.flags & (net::kItemNotYours | net::kItemAnswer)) return;  // a host's words; not a player's to say
    if (!newest(p, "i/" + m.pdid, m.seq)) return;
    claims_.lies_again(m.scene, m.pdid);
    m.seq = ++seq_;
    net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
    net::write_item_put(w, m);
    session_.send_packet_except(p.from, w, true);
    Scene& s = scenes_[m.scene];
    s.gone.erase(m.pdid);
    if (s.listed) s.list.insert(net::pdid_hash(m.pdid));
    s.items[m.pdid] = std::move(m);
    dirty_ = true;
}

void World::not_yours(uint8_t to, const std::string& scene, const std::string& pdid, bool tossed) {
    net::ItemGoneMsg m;
    m.scene = scene;
    m.seq = ++seq_;
    m.reason = tossed ? net::ItemGoneReason::LostToss : net::ItemGoneReason::NotYours;
    m.pdid = pdid;
    net::Writer w(net::Msg::ItemGone);
    net::write_item_gone(w, m);
    session_.send_packet_to(to, w, true);
}

void World::item_gone(const net::Packet& p, net::ItemGoneMsg m, double now_ms) {
    if (m.reason == net::ItemGoneReason::NotYours || m.reason == net::ItemGoneReason::LostToss) return;
    if (!newest(p, "i/" + m.pdid, m.seq)) return;
    if (m.reason == net::ItemGoneReason::Taken) {
        const net::TakeDecision d = claims_.take(m.scene, m.pdid, p.from, net::TakeFacts{}, now_ms);
        if (d.answer != net::TakeAnswer::Yours) {  // someone's take came first or won the coin: the others never hear
            not_yours(p.from, m.scene, m.pdid, d.tossed);
            logf("items: player %u took %s in %s %s; refused: out of their pack", p.from, short_id(m.pdid).c_str(),
                 m.scene.c_str(), d.tossed ? "at the same moment as another player and lost the coin toss" : "after another player");
            return;
        }
        if (d.loser >= 0) {
            not_yours(static_cast<uint8_t>(d.loser), m.scene, m.pdid, true);
            logf("items: player %u took %s at the same moment as player %d and won the coin toss", p.from,
                 short_id(m.pdid).c_str(), d.loser);
        }
    }
    m.seq = ++seq_;
    net::Writer w(net::Msg::ItemGone);
    net::write_item_gone(w, m);
    session_.send_packet_except(p.from, w, true);
    Scene& s = scenes_[m.scene];
    s.items.erase(m.pdid);
    if (s.listed)
        s.list.erase(net::pdid_hash(m.pdid));
    else
        s.gone[m.pdid] = std::move(m);
    dirty_ = true;
}

void World::item_want(const net::Packet& p, const net::ItemHashesMsg& m) {
    const auto sc = scenes_.find(m.scene);
    size_t sent = 0;
    if (sc != scenes_.end()) {
        const std::unordered_set<uint64_t> asked(m.hashes.begin(), m.hashes.end());
        for (const auto& [pdid, item] : sc->second.items) {
            if (!asked.count(net::pdid_hash(pdid))) continue;
            net::ItemPutMsg put = item;
            put.seq = ++seq_;
            put.flags |= net::kItemAnswer;
            net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
            net::write_item_put(w, put);
            if (!w.ok()) continue;
            session_.send_packet_to(p.from, w, true);
            ++sent;
        }
    }
    logf("items: player %u asked for %zu item(s) of %s; sent %zu", p.from, m.hashes.size(), m.scene.c_str(), sent);
}

void World::container_put(const net::Packet& p, net::ContainerPutMsg m) {
    if (m.flags & net::kContainerAnswer) return;  // a host's word
    if (!newest(p, "c/" + m.id, m.seq)) return;
    m.seq = ++seq_;
    net::Writer w(net::Msg::ContainerPut, net::kMaxMessage);
    net::write_container_put(w, m);
    session_.send_packet_except(p.from, w, true);
    scenes_[m.scene].containers[m.id] = std::move(m.text);
    dirty_ = true;
}

void World::container_want(const net::Packet& p, const net::ContainerWantMsg& m) {
    net::ContainerPutMsg a;
    a.scene = m.scene;
    a.id = m.id;
    a.seq = ++seq_;
    a.flags = net::kContainerAnswer;
    bool known = false;
    if (const auto sc = scenes_.find(m.scene); sc != scenes_.end())
        if (const auto it = sc->second.containers.find(m.id); it != sc->second.containers.end()) {
            a.text = net::strip_uniques(it->second);  // one-of-a-kind items are each game's own
            known = true;
        }
    if (a.text.empty()) a.flags |= net::kContainerUnknown;
    net::Writer w(net::Msg::ContainerPut, net::kMaxMessage);
    net::write_container_put(w, a);
    session_.send_packet_to(p.from, w, true);
    logf("containers: player %u is opening %s in %s; sent %s (%zu chars)", p.from, short_id(m.id).c_str(), m.scene.c_str(),
         known ? "what we have" : "nothing known: theirs stands", a.text.size());
}

void World::tell_lock(const std::string& key, int to) {
    const size_t bar = key.find('|');
    if (bar == std::string::npos) return;
    net::ContainerLockMsg m;
    m.scene = key.substr(0, bar);
    m.id = key.substr(bar + 1);
    m.op = net::LockOp::State;
    m.owner = locks_.owner(key);
    net::Writer w(net::Msg::ContainerLock);
    net::write_container_lock(w, m);
    if (to >= 0)
        session_.send_packet_to(static_cast<uint8_t>(to), w, true);
    else
        session_.send_packet(w, true);
}

void World::container_lock(const net::Packet& p, const net::ContainerLockMsg& m, double now_ms) {
    const std::string key = net::lock_key(m.scene, m.id);
    if (m.op == net::LockOp::Take) {
        const uint8_t before = locks_.owner(key);
        const uint8_t owner = locks_.take(key, p.from, now_ms);
        if (owner != before)
            tell_lock(key);  // to everyone, the asker included
        else
            tell_lock(key, p.from);  // taken (by whom), or theirs already: told again
    } else if (m.op == net::LockOp::Release) {
        if (locks_.release(key, p.from)) tell_lock(key);
    }
}

// --- arrivals --------------------------------------------------------------------------------------------------------

void World::send_scene(uint8_t to, const std::string& scene) {
    const auto sc = scenes_.find(scene);
    if (sc == scenes_.end()) {
        logf("world: player %u is in %s; nothing kept there yet", to, scene.c_str());
        return;
    }
    const Scene& s = sc->second;
    size_t items = 0;
    if (s.listed) {
        net::ItemHashesMsg m;
        m.scene = scene;
        m.hashes.assign(s.list.begin(), s.list.end());
        net::Writer w(net::Msg::ItemList, net::kMaxMessage);
        net::write_item_hashes(w, m);
        if (w.ok()) session_.send_packet_to(to, w, true);
        items = m.hashes.size();
    } else {
        for (const auto& [pdid, gone] : s.gone) {
            net::ItemGoneMsg m = gone;
            m.seq = ++seq_;
            net::Writer w(net::Msg::ItemGone);
            net::write_item_gone(w, m);
            session_.send_packet_to(to, w, true);
        }
        for (const auto& [pdid, item] : s.items) {
            net::ItemPutMsg m = item;
            m.seq = ++seq_;
            m.flags |= net::kItemAnswer;
            net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
            net::write_item_put(w, m);
            if (w.ok()) session_.send_packet_to(to, w, true);
        }
        items = s.items.size() + s.gone.size();
    }
    for (const auto& [key, o] : s.objects) {
        net::ObjectPutMsg m;
        m.scene = scene;
        m.seq = ++seq_;
        m.object = o;
        net::Writer w(net::Msg::ObjectPut, net::kMaxMessage);
        net::write_object_put(w, m);
        if (w.ok()) session_.send_packet_to(to, w, true);
    }
    for (const auto& [id, fire] : s.fires) {
        net::FirePutMsg m = fire;
        m.seq = ++seq_;
        net::Writer w(net::Msg::FirePut, net::kMaxMessage);
        net::write_fire_put(w, m);
        if (w.ok()) session_.send_packet_to(to, w, true);
    }
    for (const auto& [id, source] : s.water) {
        net::WaterPutMsg m = source;
        m.seq = ++seq_;
        net::Writer w(net::Msg::WaterPut, net::kMaxMessage);
        net::write_water_put(w, m);
        if (w.ok()) session_.send_packet_to(to, w, true);
    }
    logf("world: player %u is in %s; sent %s%zu item(s), %zu object(s), %zu fire(s), %zu water source(s)", to, scene.c_str(),
         s.listed ? "the whole list of " : "the changes to ", items, s.objects.size(), s.fires.size(), s.water.size());
}

void World::tick(double now_ms) {
    const net::Status st = session_.status();
    std::array<bool, net::kMaxPlayers> linked{};
    for (const net::PlayerInfo& p : st.players) {
        if (p.id == 0 || p.id >= net::kMaxPlayers || !p.linked) continue;
        linked[p.id] = true;
        Arrival& a = arrivals_[p.id];
        if (a.link != p.link) a = Arrival{p.link, {}, {}, 0};
        if (!p.caught_up) continue;  // their changes from while the link was down come first
        const auto state = session_.remote_state(p.id);
        if (!state || !state->valid || state->scene.empty()) continue;  // loading: where they arrive counts
        if (state->scene == a.scene) {
            a.pending.clear();
            continue;
        }
        if (a.pending != state->scene) {
            a.pending = state->scene;
            a.due_ms = now_ms + kArrivalDelayMs;
            continue;
        }
        if (now_ms < a.due_ms) continue;
        a.scene = a.pending;
        a.pending.clear();
        send_scene(p.id, a.scene);
    }
    for (uint8_t id = 1; id < net::kMaxPlayers; ++id) {
        if (linked[id]) continue;
        arrivals_[id] = Arrival{};
        for (const std::string& key : locks_.drop_player(id)) tell_lock(key);
    }
    if (now_ms - last_lock_tick_ >= 500) {
        last_lock_tick_ = now_ms;
        for (const std::string& key : locks_.expire(now_ms, net::kLockStaleMs, kNobody)) tell_lock(key);
    }
}

}  // namespace srv
