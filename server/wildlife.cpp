#include "server/wildlife.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

// The host's part of mod/src/wildlife_owner.inc ("host: the registry and the arbiter"), kept as close to it as a
// server without a game allows; where they differ it says so.

namespace srv {

constexpr float kTwinMetres = 150;  // a new animal this near a frozen one of its kind is that one (ingest)

namespace {

constexpr double kArbiterMs = 250;
constexpr double kControllersMs = 2000;
constexpr double kPendingMs = 3000;
constexpr double kFrozenForgetHours = 7 * 24;
constexpr double kMovedAfterHours = 1.0;
constexpr double kBlockMs = 60000;
constexpr double kWrongMs = 2000;
constexpr float kRangeMetres = 400;
constexpr size_t kKillsQueuedMax = 64;

std::string who(uint8_t player) { return player == net::kNoPlayer ? "nobody" : "player " + std::to_string(player); }

}  // namespace

Wildlife::Wildlife(Out out, LogFn log) : out_(std::move(out)), log_(std::move(log)) {}

void Wildlife::logf(const char* fmt, ...) {
    char line[640];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    log_(line);
}

uint8_t Wildlife::owner(uint32_t id) const {
    const auto it = registry_.find(id);
    return it == registry_.end() ? net::kNoPlayer : it->second.owner;
}

uint8_t Wildlife::controller(const std::string& scene) const {
    const auto it = controllers_.find(scene);
    return it == controllers_.end() ? net::kNoPlayer : it->second.player;
}

bool Wildlife::in_scene(const std::vector<Present>& players, uint8_t id, const std::string& scene) {
    for (const Present& p : players)
        if (p.id == id && p.scene == scene) return true;
    return false;
}

void Wildlife::deliver_take(const net::WildCtlMsg& t) {
    net::Writer w(net::Msg::WildCtl, net::kMaxMessage);
    net::write_wild_ctl(w, t);
    out_.to(t.to, w, true);
}

net::WildCtlMsg Wildlife::take_from_registry(const Entry& e, uint8_t to, uint16_t epoch) const {
    net::WildCtlMsg t;
    t.op = net::WildOp::Take;
    t.id = e.id;
    t.to = to;
    t.epoch = epoch;
    t.scene = e.scene;
    t.state = e.last;
    t.state.epoch = epoch;
    t.text = e.text;
    if (e.last.flags & net::kAnimalDead) {
        t.flags |= net::kTakeDead;  // a body
        t.text = e.body;            // what was harvested from it
        if (!e.body.empty()) t.flags |= net::kTakeBody;
    }
    return t;
}

// A death (or an animal gone) for the game that spawns in its scene; kept until one does.
void Wildlife::tell_controller_kill(const net::WildCtlMsg& k) {
    const auto c = controllers_.find(k.scene);
    if (c == controllers_.end() || c->second.player == net::kNoPlayer) {
        auto& q = kills_queued_[k.scene];
        if (q.size() < kKillsQueuedMax) q.push_back(k);
        return;
    }
    net::Writer w(net::Msg::WildCtl, 128);
    net::write_wild_ctl(w, k);
    out_.to(c->second.player, w, true);
}

net::WildCtlMsg Wildlife::kill_of(const Entry& e, uint8_t flags, uint8_t died_on) {
    net::WildCtlMsg k;
    k.op = net::WildOp::Kill;
    k.id = e.id;
    k.flags = flags;
    k.died_on = died_on;
    k.kind = e.last.kind;
    k.region = e.last.region;
    k.scene = e.scene;
    return k;
}

// The owner's game put it away (TLD's own reasons): gone from the registry, and from the spawning game's count.
void Wildlife::gone(uint32_t id, uint8_t from, double now) {
    const auto it = registry_.find(id);
    if (it == registry_.end() || it->second.owner != from) return;
    logf("wildlife: #%u (kind %u) is gone from %s's game (TLD put it away): forgotten", id, it->second.last.kind, who(from).c_str());
    if (from < net::kMaxPlayers && it->second.owned_ms > 0 && now - it->second.owned_ms < 20000) {
        if (now - quick_put_away_ms_[from] > 60000) quick_put_aways_[from] = 0;
        quick_put_away_ms_[from] = now;
        if (++quick_put_aways_[from] >= 2) {
            quick_put_aways_[from] = 0;
            keeps_nothing_until_[from] = now + 60000;
            logf("wildlife: %s's game puts animals away right after taking them: none handed to it for a minute", who(from).c_str());
        }
    }
    tell_controller_kill(kill_of(it->second, net::kKillGone, from));
    registry_.erase(it);
    dirty_ = true;
}

// The spawning game's regions as it reported them: kept for the next game to spawn there (and in the world file).
void Wildlife::store_regions(net::WildCtlMsg m, uint8_t from) {
    if (hours_ < 0 || m.regions.empty()) return;
    m.hours = hours_;
    RegionStore& st = regions_[m.scene];
    const bool first = st.msg.regions.empty();
    st.msg = std::move(m);
    st.reporter = from;
    dirty_ = true;
    if (first)
        logf("wildlife: %s's spawn regions in '%s' (%zu) kept for whoever spawns there next", who(from).c_str(),
             st.msg.scene.c_str(), st.msg.regions.size());
}

// A new spawning game in `scene`: the regions' state as last known (times brought up to now), and deaths meanwhile.
void Wildlife::hand_regions(const std::string& scene, uint8_t to) {
    if (to == net::kNoPlayer) return;
    if (const auto st = regions_.find(scene); st != regions_.end() && st->second.reporter != to) {
        net::WildCtlMsg m = st->second.msg;
        const float since = hours_ >= 0 && m.hours >= 0 && hours_ > m.hours ? static_cast<float>(hours_ - m.hours) : 0.0f;
        for (net::WildRegionState& g : m.regions) {
            g.next_h -= since;
            g.trap_reset_h -= since;
            g.cooldown_h -= since;
        }
        m.hours = hours_;
        logf("wildlife: %s spawns in '%s' now: %zu spawn region(s) as %s left them (%.1f h ago)", who(to).c_str(),
             scene.c_str(), m.regions.size(), who(st->second.reporter).c_str(), since);
        net::Writer w(net::Msg::WildCtl, net::kMaxMessage);
        net::write_wild_ctl(w, m);
        out_.to(to, w, true);
    }
    if (const auto q = kills_queued_.find(scene); q != kills_queued_.end()) {
        const std::vector<net::WildCtlMsg> kills = std::move(q->second);
        kills_queued_.erase(q);
        if (!kills.empty()) logf("wildlife: %zu death(s) in '%s' while nobody spawned there: to %s", kills.size(), scene.c_str(), who(to).c_str());
        for (const net::WildCtlMsg& k : kills) tell_controller_kill(k);
    }
}

// A frozen animal handed out again: as it would be by now.
void Wildlife::age_frozen(Entry& e, net::WildCtlMsg& t) {
    const double from_h = e.frozen_h;
    e.frozen_h = -1;
    if (from_h < 0 || hours_ < 0 || hours_ <= from_h || (e.last.flags & net::kAnimalDead)) return;
    const double h = hours_ - from_h;
    if ((e.last.flags & net::kAnimalBleeding) && e.last.bleed_limit > 0) {
        e.last.bleed_elapsed += static_cast<float>(h * 60.0);
        if (e.last.bleed_elapsed >= e.last.bleed_limit) {
            logf("wildlife: #%u (kind %u) bled out while nobody was near (%.1f h): its carcass lies where it was", e.id, e.last.kind, h);
            e.last.flags = static_cast<uint8_t>((e.last.flags | net::kAnimalDead) & ~net::kAnimalBleeding);
            e.last.mode = net::wild::kModeDead;
            e.last.hp = 0;
            e.last.target = net::kNoPlayer;
            e.text.clear();
            e.body.clear();
            tell_controller_kill(kill_of(e, 0, net::kNoPlayer));
            const uint8_t to = t.to;
            const uint16_t epoch = t.epoch;
            t = take_from_registry(e, to, epoch);
            return;
        }
        t.state.bleed_elapsed = e.last.bleed_elapsed;
    }
    if (h >= kMovedAfterHours) {
        t.flags |= net::kTakeMoved;
        t.hours = h;
        t.state.target = net::kNoPlayer;
        t.text.clear();  // (its old plans: where it fled to, what it stalked)
        logf("wildlife: #%u (kind %u) was left alone %.1f h: it has moved on in its spawn region", e.id, e.last.kind, h);
    }
}

void Wildlife::transfer(Entry& e, uint8_t to, const char* why, const std::vector<Present>& players, double now) {
    const uint16_t epoch = static_cast<uint16_t>(e.epoch + 1);
    logf("wildlife: #%u owner %s -> %s (%s)", e.id, who(e.owner).c_str(), who(to).c_str(), why);
    e.owned_ms = now;
    dirty_ = true;
    const bool owner_here = e.owner != net::kNoPlayer && in_scene(players, e.owner, e.scene);
    const bool was_frozen = e.frozen;
    e.frozen = false;
    if (owner_here) {
        e.pending_to = to;
        e.pending_epoch = epoch;
        e.pending_ms = now;
        net::WildCtlMsg r;
        r.op = net::WildOp::Release;
        r.id = e.id;
        r.to = to;
        r.epoch = epoch;
        net::Writer w(net::Msg::WildCtl, 64);
        net::write_wild_ctl(w, r);
        out_.to(e.owner, w, true);
        return;
    }
    // The owner isn't there to hand it over: from what we know of it.
    e.owner = to;
    e.epoch = epoch;
    e.pending_to = net::kNoPlayer;
    net::WildCtlMsg t = take_from_registry(e, to, epoch);
    if (was_frozen && (t.flags & net::kTakeDead)) t.flags |= net::kTakeOld;  // kept / left alone: not a fresh kill
    if (was_frozen) age_frozen(e, t);
    deliver_take(t);
}

void Wildlife::take(net::WildCtlMsg t, uint8_t from, double now) {
    auto it = registry_.find(t.id);
    if (it == registry_.end()) {
        Entry e;
        e.id = t.id;
        e.scene = t.scene;
        e.owner = from;
        e.last = t.state;
        e.last_ms = now;
        it = registry_.emplace(t.id, std::move(e)).first;
    }
    Entry& e = it->second;
    dirty_ = true;
    if (t.flags & net::kTakeDead) {
        // In the mod the host's game takes a body over when it is in the scene. A server never is: the owner keeps it
        // (and goes on telling us how it is, Keep); what it says here is taken as that.
        if (e.owner != from) return;
        if (t.flags & net::kTakeBody) e.body = t.text;
        e.last = t.state;
        e.last_ms = now;
        return;
    }
    if (t.flags & net::kTakeKept) {
        if (e.pending_to != net::kNoPlayer) {
            logf("wildlife: #%u stays with %s (a fight began; handover to %s called off)", t.id, who(e.owner).c_str(), who(e.pending_to).c_str());
            e.pending_to = net::kNoPlayer;
            e.track.last_switch_ms = now;
        }
        return;
    }
    if (e.pending_to == net::kNoPlayer || t.epoch != e.pending_epoch) {
        logf("wildlife: a late handover of #%u from %s (epoch %u) ignored", t.id, who(from).c_str(), t.epoch);
        return;
    }
    if (t.flags & net::kTakeNoState) {
        t.state = e.last;
        t.state.epoch = t.epoch;
        t.text = e.text;
        t.scene = e.scene;
    } else {
        if (!t.text.empty()) e.text = t.text;
        e.last = t.state;
        e.last_ms = now;
    }
    t.flags &= static_cast<uint8_t>(~net::kTakeNoState);
    t.to = e.pending_to;
    e.owner = e.pending_to;
    e.epoch = t.epoch;
    e.pending_to = net::kNoPlayer;
    deliver_take(t);
}

void Wildlife::refuse(uint32_t id, uint16_t epoch, uint8_t from, double now, uint8_t flags) {
    const auto it = registry_.find(id);
    if (it == registry_.end()) return;
    Entry& e = it->second;
    if (e.owner != from || e.epoch != epoch) return;
    if ((flags & net::kRefuseDuplicate) && (e.last.flags & net::kAnimalDead)) {
        // Shatsca 20.11: a dead body kept from before whose carcass the player's save already put back (their game
        // shares it under a new id): offered again every minute, forever (16 dead stags by the tracks). Forgotten.
        logf("wildlife: %s already has #%u's carcass (from its save): the old record forgotten", who(from).c_str(), id);
        registry_.erase(it);
        dirty_ = true;
        return;
    }
    logf("wildlife: %s couldn't take #%u; someone else will (not them for a minute)", who(from).c_str(), id);
    if (from < net::kMaxPlayers) e.blocked_until[from] = now + kBlockMs;
    e.owner = net::kNoPlayer;
}

// A frame of animals from `owner` (its own records): into the registry. Returns the records to pass on.
std::vector<net::AnimalRec> Wildlife::ingest(uint8_t owner, const std::string& scene, const std::vector<net::AnimalRec>& recs, double now) {
    std::vector<net::AnimalRec> pass;
    for (const net::AnimalRec& rec : recs) {
        auto it = registry_.find(rec.id);
        if (it == registry_.end()) {
            Entry e;
            e.id = rec.id;
            e.scene = scene;
            e.owner = owner;
            e.epoch = rec.epoch;
            it = registry_.emplace(rec.id, std::move(e)).first;
            logf("wildlife: #%u (kind %u) shared by %s, its owner", rec.id, rec.kind, who(owner).c_str());
            // Shatsca 13.11: back in a scene, TLD brought its own saved animals up again where the game came near them
            // and they were shared as new, while the same animals stood frozen here from the last visit: 9 became 18.
            // The frozen one nearest a new one of its kind (same spawn region when both say) is that same animal: it
            // goes, the game's own stays.
            if (!(rec.flags & net::kAnimalDead)) {
                uint32_t twin = 0;
                float best = kTwinMetres * kTwinMetres;
                for (const auto& [id, f] : registry_) {
                    if (id == rec.id || f.scene != scene || f.owner != net::kNoPlayer || f.pending_to != net::kNoPlayer) continue;
                    if (f.last.kind != rec.kind || (f.last.flags & net::kAnimalDead) || f.last.hp <= 0) continue;
                    if (f.last.region && rec.region && f.last.region != rec.region) continue;
                    const float dx = f.last.x - rec.x, dy = f.last.y - rec.y, dz = f.last.z - rec.z;
                    const float d = dx * dx + dy * dy + dz * dz;
                    if (d < best) best = d, twin = id;
                }
                if (twin) {
                    logf("wildlife: #%u kept frozen %.0f m from the new #%u is that same animal from the last visit: forgotten",
                         twin, std::sqrt(best), rec.id);
                    registry_.erase(twin);
                    it = registry_.find(rec.id);
                }
            }
        }
        Entry& e = it->second;
        if (e.owner != owner) {
            if (e.owner == net::kNoPlayer && e.pending_to == net::kNoPlayer && rec.epoch >= e.epoch) {
                logf("wildlife: #%u owner nobody -> %s (it runs it, nobody else did)", rec.id, who(owner).c_str());
                e.owner = owner;
                e.epoch = rec.epoch;
            } else {
                if (now - e.wrong_ms > kWrongMs) {
                    e.wrong_ms = now;
                    logf("wildlife: %s still runs #%u, which is %s's: told to let it go", who(owner).c_str(), rec.id, who(e.owner).c_str());
                    net::WildCtlMsg r;
                    r.op = net::WildOp::Release;
                    r.id = rec.id;
                    r.to = e.owner;
                    r.epoch = e.epoch;
                    net::Writer w(net::Msg::WildCtl, 64);
                    net::write_wild_ctl(w, r);
                    out_.to(owner, w, true);
                }
                continue;
            }
        }
        if (rec.epoch < e.epoch) continue;
        e.last = rec;
        e.last_ms = now;
        e.scene = scene;
        e.frozen = false;
        pass.push_back(rec);
    }
    if (!pass.empty()) dirty_ = true;
    return pass;
}

void Wildlife::arbitrate(const std::vector<Present>& players, double now) {
    if (now - last_arbiter_ < kArbiterMs) return;
    last_arbiter_ = now;
    // Who has been in which scene since when (the spawning game: the one there longest).
    for (uint8_t id = 0; id < net::kMaxPlayers; ++id) {
        std::string scene;
        for (const Present& p : players)
            if (p.id == id) scene = p.scene;
        if (scene != arrival_[id].first) arrival_[id] = {scene, now};
    }
    for (auto it = registry_.begin(); it != registry_.end();) {
        Entry& e = it->second;
        if (e.pending_to != net::kNoPlayer) {
            if (now - e.pending_ms > kPendingMs) {
                logf("wildlife: no handover of #%u from %s in time: %s takes it from what we know", e.id, who(e.owner).c_str(),
                     who(e.pending_to).c_str());
                const uint8_t to = e.pending_to;
                e.pending_to = net::kNoPlayer;
                if (in_scene(players, to, e.scene)) {
                    e.owner = to;
                    e.epoch = e.pending_epoch;
                    deliver_take(take_from_registry(e, to, e.epoch));
                } else {
                    e.owner = net::kNoPlayer;
                }
            }
            ++it;
            continue;
        }
        std::vector<net::wild::Seat> seats;
        for (const Present& p : players) {
            if (p.scene != e.scene || p.id >= net::kMaxPlayers) continue;
            if (now < e.blocked_until[p.id]) continue;
            if (p.id != e.owner && now < keeps_nothing_until_[p.id]) continue;  // (only new handovers)
            net::wild::Seat s;
            s.player = p.id;
            const float dx = e.last.x - p.x, dy = e.last.y - p.y, dz = e.last.z - p.z;
            s.metres = std::sqrt(dx * dx + dy * dy + dz * dz);
            s.looking = net::wild::looking_at(p.yaw, dx, dz, s.metres);
            s.rtt_ms = p.rtt_ms;
            s.inside = p.inside;
            seats.push_back(s);
        }
        const bool owner_here = e.owner != net::kNoPlayer && in_scene(players, e.owner, e.scene);
        // Nobody runs it (frozen, or its owner left): only someone near enough takes it; out of everyone's range it
        // stays as it was, frozen.
        bool near_one = false;
        for (const net::wild::Seat& st : seats) near_one |= st.metres <= kRangeMetres;
        const bool dead = (e.last.flags & net::kAnimalDead) != 0;
        if (seats.empty() || (!owner_here && !near_one)) {
            if (!e.frozen) {
                e.frozen = true;
                e.frozen_h = hours_;
                logf("wildlife: #%u (kind %u) frozen in '%s' at (%.0f, %.0f, %.0f), hp %.0f: nobody is %s", e.id, e.last.kind,
                     e.scene.c_str(), e.last.x, e.last.y, e.last.z, e.last.hp, seats.empty() ? "there" : "near it");
                e.owner = net::kNoPlayer;
                dirty_ = true;
            }
            if (e.frozen_h < 0 && hours_ >= 0) e.frozen_h = hours_;  // (frozen while the clock wasn't known: from now)
            if (e.frozen_h >= 0 && hours_ >= 0 && hours_ - e.frozen_h > kFrozenForgetHours) {
                logf("wildlife: #%u (kind %u) forgotten (nobody came near it for %.0f game hours)", e.id, e.last.kind, hours_ - e.frozen_h);
                if (!dead) tell_controller_kill(kill_of(e, net::kKillGone, net::kNoPlayer));
                it = registry_.erase(it);
                dirty_ = true;
                continue;
            }
            ++it;
            continue;
        }
        // (A body whose owner left: in the mod the host's game keeps it, once it is there. Here it goes to whoever is
        // near, as any animal nobody runs: decide() gives the best seat at once when the owner isn't present.)
        net::wild::Facts f;
        f.owner = e.owner;
        f.owner_present = owner_here;
        f.target = e.last.target;
        f.mode = e.last.mode;
        f.dead = dead;
        net::wild::Why why;
        const net::wild::Track before = e.track;
        const uint8_t to = net::wild::decide(e.track, f, seats, now, why);
        if (to != net::kNoPlayer && to != e.owner) {
            if (handovers_.take(now)) transfer(e, to, net::wild::why_text(why), players, now);
            else e.track = before;  // too many at once: decided again next time
        }
        ++it;
    }
    // Who spawns where.
    std::map<std::string, SceneControl> scenes;
    for (const Present& p : players) {
        if (p.id >= net::kMaxPlayers) continue;
        auto& c = scenes[p.scene];
        if (c.player == net::kNoPlayer || arrival_[p.id].second < arrival_[c.player].second ||
            (arrival_[p.id].second == arrival_[c.player].second && p.id < c.player))
            c.player = p.id;
    }
    for (auto& [scene, c] : scenes) {
        c.fresh = true;
        for (const auto& [id, e] : registry_)
            if (e.scene == scene) {
                c.fresh = false;
                break;
            }
    }
    std::string summary;
    net::WildCtlMsg m;
    m.op = net::WildOp::Controllers;
    m.per_player = static_cast<uint8_t>(per_player);
    for (const auto& [scene, c] : scenes) {
        summary += scene + "=" + std::to_string(c.player) + (c.fresh ? "*" : "") + " ";
        m.scenes.push_back({scene, c.player, static_cast<uint8_t>(c.fresh ? 1 : 0)});
    }
    std::vector<std::pair<std::string, uint8_t>> new_controllers;
    for (const auto& [scene, c] : scenes) {
        const auto old = controllers_.find(scene);
        if (old == controllers_.end() || old->second.player != c.player) new_controllers.emplace_back(scene, c.player);
    }
    controllers_.clear();
    for (const auto& [scene, c] : scenes) controllers_[scene] = c;
    const bool changed = summary != controllers_sent_;
    if (changed || now - last_controllers_ >= kControllersMs) {
        net::Writer w(net::Msg::WildCtl, net::kMaxPacket);
        net::write_wild_ctl(w, m);
        out_.all(w, changed);
        last_controllers_ = now;
        if (changed) {
            for (const auto& [scene, c] : scenes)
                logf("wildlife: %s spawns in '%s'%s", who(c.player).c_str(), scene.c_str(),
                     c.fresh ? " (nothing shared there yet: its own animals are shared)" : "");
            controllers_sent_ = summary;
        }
    }
    // A new spawning game gets the regions' state (after Controllers: it knows it spawns there by then).
    for (const auto& [scene, player] : new_controllers) hand_regions(scene, player);
}

void Wildlife::tick(const std::vector<Present>& players, double now_ms, double hours) {
    hours_ = hours;
    players_ = players;
    arbitrate(players, now_ms);
}

bool Wildlife::on_packet(const net::Packet& p, double now) {
    net::Reader r = net::packet_body(p);
    switch (p.type) {
        case net::Msg::WildFrame: {
            net::WildFrameMsg m;
            if (!net::read_wild_frame(r, m)) return true;
            m.owner = p.from;  // (each player speaks for itself)
            m.animals = ingest(p.from, m.scene, m.animals, now);
            if (!m.animals.empty()) {
                net::Writer w(net::Msg::WildFrame);
                net::write_wild_frame(w, m);
                if (w.ok()) out_.except(p.from, w, false);
            }
            return true;
        }
        case net::Msg::WildCtl: {
            net::WildCtlMsg m;
            if (!net::read_wild_ctl(r, m)) return true;
            switch (m.op) {
                case net::WildOp::Take: take(std::move(m), p.from, now); break;
                case net::WildOp::Refuse: refuse(m.id, m.epoch, p.from, now, m.flags); break;
                case net::WildOp::Gone: gone(m.id, p.from, now); break;
                case net::WildOp::Kill: tell_controller_kill(m); break;  // a death in a player's game
                case net::WildOp::Regions: store_regions(std::move(m), p.from); break;
                case net::WildOp::Keep: {  // a carcass a player's game keeps: as it is there now
                    const auto e = registry_.find(m.id);
                    if (e != registry_.end() && e->second.owner == p.from) {
                        e->second.last = m.state;
                        e->second.last_ms = now;
                        if (m.flags & net::kTakeBody) e->second.body = m.text;
                        dirty_ = true;
                    }
                    break;
                }
                default: break;  // Controllers, Release: ours to say
            }
            return true;
        }
        case net::Msg::WildSounds: {
            std::vector<net::AnimalSound> part;
            if (!net::read_sounds(r, part) || part.empty()) return true;
            net::Writer w(net::Msg::WildSounds);
            net::write_sounds(w, part);
            if (w.ok()) out_.except(p.from, w, false);
            return true;
        }
        case net::Msg::Hit: {  // a shot on an animal another game runs: to that game
            net::HitMsg h;
            if (!net::read_hit(r, h)) return true;
            const uint8_t to = owner(h.animal);
            if (to == net::kNoPlayer || to == p.from) return true;
            net::WildHitMsg m;
            m.op = net::WildHitOp::Hit;
            m.who = p.from;
            m.hit = h;
            net::Writer w(net::Msg::WildHit);
            net::write_wild_hit(w, m);
            out_.to(to, w, true);
            logf("wildlife: player %u's hit on #%u passed on to player %u, who runs it", p.from, h.animal, to);
            return true;
        }
        case net::Msg::Thrown: {  // the other games scare the animals they run
            net::ThrownMsg t;
            if (!net::read_thrown(r, t)) return true;
            net::WildHitMsg m;
            m.op = net::WildHitOp::Thrown;
            m.who = p.from;
            m.thrown = t;
            net::Writer w(net::Msg::WildHit);
            net::write_wild_hit(w, m);
            out_.except(p.from, w, true);
            return true;
        }
        case net::Msg::Carcasses: {  // the carcasses of the animals a game runs: to the others in their scene
            std::vector<net::CarcassRec> list;
            if (!net::read_carcasses(r, list)) return true;
            pass_carcasses(p.from, std::move(list), now);
            return true;
        }
        case net::Msg::Harvest: {  // a harvest at someone's copy of a carcass: to the game that runs that animal
            net::HarvestMsg h;
            if (!net::read_harvest(r, h)) return true;
            pass_harvest(p.from, h, now);
            return true;
        }
        case net::Msg::Quartered: {
            const uint32_t id = r.get<uint32_t>();
            if (!r.ok()) return true;
            const uint8_t to = owner(id);
            if (to == net::kNoPlayer || to == p.from) return true;
            net::Writer w(net::Msg::Quartered);
            w.put(id);
            out_.to(to, w, true);
            logf("wildlife: player %u quartered #%u: passed on to player %u, who runs it", p.from, id, to);
            return true;
        }
        case net::Msg::FlareShot: {
            net::FlareShotMsg m;
            if (!net::read_flare_shot(r, m)) return true;
            net::Writer w(net::Msg::FlareShot);
            net::write_flare_shot(w, m);
            out_.except(p.from, w, true);
            return true;
        }
        default:
            return false;
    }
}

void Wildlife::pass_harvest(uint8_t from, net::HarvestMsg h, double now) {
    const uint8_t to = owner(h.animal);
    if (to == net::kNoPlayer || to == from || from >= net::kMaxPlayers) {
        logf("wildlife: player %u harvested #%u, which nobody runs now: not passed on", from, h.animal);
        return;
    }
    if (harvest_seq_ == 0)  // (not 0, and not where the last run of the server began: the games keep the seqs they saw)
        harvest_seq_ = static_cast<uint32_t>(static_cast<uint64_t>(now) & 0x7FFFFFFFu) | 1u;
    PassedHarvest ph;
    ph.ours = ++harvest_seq_;
    if (ph.ours == 0) ph.ours = ++harvest_seq_;
    ph.from = from;
    ph.theirs = h.seq;
    ph.to = to;
    auto& passed = harvests_passed_[h.animal];
    passed.push_back(ph);
    if (passed.size() > 64) passed.erase(passed.begin());
    h.seq = ph.ours;
    net::Writer w(net::Msg::Harvest);
    net::write_harvest(w, h);
    out_.to(to, w, true);
    logf("wildlife: player %u harvested #%u (%.2f kg meat, %d hide, %d gut): passed on to player %u, who runs it", from,
         h.animal, h.meat_units / 1e9, h.hide, h.gut, to);
}

void Wildlife::pass_carcasses(uint8_t from, std::vector<net::CarcassRec> list, double now) {
    std::string scene;
    std::vector<net::CarcassRec> ours;
    for (net::CarcassRec& c : list) {
        const auto e = registry_.find(c.animal);
        if (e == registry_.end() || e->second.owner != from) continue;  // (not the one that runs it: an old list)
        if (scene.empty()) scene = e->second.scene;
        if (e->second.scene != scene) continue;
        // Its acks: our seqs it has counted -> each harvester's own
        auto passed = harvests_passed_.find(c.animal);
        if (passed != harvests_passed_.end()) {
            auto& v = passed->second;
            for (auto it = v.begin(); it != v.end();) {
                const bool counted = it->to != from || (c.harvest_ack && static_cast<int32_t>(c.harvest_ack - it->ours) >= 0);
                if (!counted) {
                    ++it;
                    continue;
                }
                harvest_acks_[it->from][c.animal] = it->theirs;
                it = v.erase(it);
            }
            if (v.empty()) harvests_passed_.erase(passed);
        }
        ours.push_back(std::move(c));
    }
    if (ours.empty()) return;
    const size_t per_packet = (net::kMaxPacket - 8) / net::kCarcassSize;
    for (const Present& p : players_) {
        if (p.id == from || p.id >= net::kMaxPlayers || p.scene != scene) continue;
        std::vector<net::CarcassRec> theirs = ours;
        for (net::CarcassRec& c : theirs) {
            const auto ack = harvest_acks_[p.id].find(c.animal);
            c.harvest_ack = ack == harvest_acks_[p.id].end() ? 0 : ack->second;
        }
        for (size_t at = 0; at < theirs.size(); at += per_packet) {
            const std::vector<net::CarcassRec> part(theirs.begin() + static_cast<ptrdiff_t>(at),
                                                    theirs.begin() + static_cast<ptrdiff_t>(std::min(theirs.size(), at + per_packet)));
            net::Writer w(net::Msg::Carcasses);
            net::write_carcasses(w, part);
            if (w.ok()) out_.to(p.id, w, false);
        }
    }
    if (from < net::kMaxPlayers && ours.size() != carcasses_passed_[from]) {
        carcasses_passed_[from] = ours.size();
        logf("wildlife: player %u's %zu carcass(es) in '%s' passed on to the others there", from, ours.size(), scene.c_str());
    }
}

// --- the world file --------------------------------------------------------------------------------------------------

bool Wildlife::load(bool regions, const std::vector<uint8_t>& msg) {
    net::Packet pk;
    pk.data = msg;
    net::Reader rd = net::packet_body(pk);
    net::WildCtlMsg m;
    if (msg.size() < 7 || static_cast<net::Msg>(msg[6]) != net::Msg::WildCtl || !net::read_wild_ctl(rd, m)) return false;
    if (!regions && m.op == net::WildOp::Keep) {
        if (registry_.count(m.id)) return true;
        Entry e;
        e.id = m.id;
        e.scene = m.scene;
        e.owner = net::kNoPlayer;
        e.epoch = m.epoch;
        e.last = m.state;
        e.frozen = true;
        e.frozen_h = m.hours;
        if (m.flags & net::kTakeDead) e.body = (m.flags & net::kTakeBody) ? m.text : std::string();
        else e.text = m.text;
        registry_.emplace(m.id, std::move(e));
        return true;
    }
    if (regions && m.op == net::WildOp::Regions) {
        RegionStore& st = regions_[m.scene];
        st.msg = std::move(m);
        st.reporter = net::kNoPlayer;
        return true;
    }
    return false;
}

void Wildlife::rebase(double hours) {
    for (auto& [id, e] : registry_) e.frozen_h = hours;
    for (auto& [scene, st] : regions_) st.msg.hours = hours;
}

void Wildlife::save(const std::function<void(bool regions, const net::Writer& w)>& add) const {
    for (const auto& [id, e] : registry_) {
        net::WildCtlMsg k;
        k.op = net::WildOp::Keep;
        k.id = id;
        k.epoch = e.epoch;
        const bool dead = (e.last.flags & net::kAnimalDead) != 0;
        k.flags = dead ? net::kTakeDead : 0;
        k.text = dead ? e.body : e.text;
        if (dead && !e.body.empty()) k.flags |= net::kTakeBody;
        k.hours = e.frozen && e.frozen_h >= 0 ? e.frozen_h : hours_;
        k.scene = e.scene;
        k.state = e.last;
        net::Writer w(net::Msg::WildCtl, net::kMaxMessage);
        net::write_wild_ctl(w, k);
        if (w.ok()) add(false, w);
    }
    for (const auto& [scene, st] : regions_) {
        net::Writer w(net::Msg::WildCtl, net::kMaxMessage);
        net::write_wild_ctl(w, st.msg);
        if (w.ok()) add(true, w);
    }
}

}  // namespace srv
