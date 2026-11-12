// The server's world store, offline: the server and three players (real net::Sessions, as the mod uses them) over UDP
// on 127.0.0.1. Checks: a player's item / container / object / fire / water changes reach the others and are kept;
// two takes of one item (one after the other: refused; at the same moment: a coin); a container's contents on asking
// (or "nothing known"); one player at a time in a container; a player arriving in a scene gets what is kept there;
// the world survives a restart (its file), and a host's own world_keep file (a whole item list, records, changes)
// loads and is told as a list. Exit code 0 when everything held.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net/session.h"
#include "server/server.h"
#include "world_keep.h"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

constexpr uint16_t kPort = 27392;
const char* const kVersion = "test-version";

struct Player {
    explicit Player(const char* n) : name(n), session([](const std::string&) {}) {}
    std::string name;
    net::Session session;
    std::vector<net::Packet> got;
    uint32_t seq = 0;

    bool start(const std::string& scene) {
        net::Config cfg;
        cfg.role = net::Role::Client;
        cfg.transport = "udp";
        cfg.host_ip = "127.0.0.1";
        cfg.port = kPort;
        cfg.name = name;
        cfg.body = "mackenzie";
        cfg.game_version = kVersion;
        if (!session.start(cfg)) return false;
        go(scene);
        session.set_ready(true);
        return true;
    }
    void go(const std::string& scene) {
        net::StateMsg s;
        s.valid = !scene.empty();
        s.scene = scene;
        session.set_local_state(s);
    }
    uint8_t id() const { return session.status().self_id; }

    // What came of one type since the last clear(), newest last.
    template <typename M, typename ReadFn>
    std::vector<M> all(net::Msg type, ReadFn read) const {
        std::vector<M> out;
        for (const net::Packet& p : got) {
            if (p.type != type) continue;
            net::Reader r = net::packet_body(p);
            M m;
            if (read(r, m)) out.push_back(std::move(m));
        }
        return out;
    }
    std::vector<net::ItemPutMsg> puts() const { return all<net::ItemPutMsg>(net::Msg::ItemPut, net::read_item_put); }
    std::vector<net::ItemGoneMsg> gones() const { return all<net::ItemGoneMsg>(net::Msg::ItemGone, net::read_item_gone); }
    std::vector<net::ItemHashesMsg> lists() const { return all<net::ItemHashesMsg>(net::Msg::ItemList, net::read_item_hashes); }
    std::vector<net::ContainerPutMsg> containers() const {
        return all<net::ContainerPutMsg>(net::Msg::ContainerPut, net::read_container_put);
    }
    std::vector<net::ContainerLockMsg> locks() const {
        return all<net::ContainerLockMsg>(net::Msg::ContainerLock, net::read_container_lock);
    }
    std::vector<net::ObjectPutMsg> objects() const { return all<net::ObjectPutMsg>(net::Msg::ObjectPut, net::read_object_put); }
    std::vector<net::FirePutMsg> fires() const { return all<net::FirePutMsg>(net::Msg::FirePut, net::read_fire_put); }
    std::vector<net::WaterPutMsg> waters() const { return all<net::WaterPutMsg>(net::Msg::WaterPut, net::read_water_put); }

    void put_item(const std::string& scene, const std::string& pdid, const std::string& gear = "{}") {
        net::ItemPutMsg m;
        m.scene = scene;
        m.seq = ++seq;
        m.pdid = pdid;
        m.prefab = "GEAR_Hatchet";
        m.gear = gear;
        net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
        net::write_item_put(w, m);
        session.send_packet(w, true);
    }
    void take_item(const std::string& scene, const std::string& pdid) {
        net::ItemGoneMsg m;
        m.scene = scene;
        m.seq = ++seq;
        m.reason = net::ItemGoneReason::Taken;
        m.pdid = pdid;
        net::Writer w(net::Msg::ItemGone);
        net::write_item_gone(w, m);
        session.send_packet(w, true);
    }
    void want_items(const std::string& scene, const std::vector<uint64_t>& hashes) {
        net::ItemHashesMsg m;
        m.scene = scene;
        m.hashes = hashes;
        net::Writer w(net::Msg::ItemWant, net::kMaxMessage);
        net::write_item_hashes(w, m);
        session.send_packet(w, true);
    }
    void put_container(const std::string& scene, const std::string& id, const std::string& text) {
        net::ContainerPutMsg m;
        m.scene = scene;
        m.seq = ++seq;
        m.id = id;
        m.text = text;
        net::Writer w(net::Msg::ContainerPut, net::kMaxMessage);
        net::write_container_put(w, m);
        session.send_packet(w, true);
    }
    void want_container(const std::string& scene, const std::string& id) {
        net::ContainerWantMsg m;
        m.scene = scene;
        m.id = id;
        net::Writer w(net::Msg::ContainerWant);
        net::write_container_want(w, m);
        session.send_packet(w, true);
    }
    void lock(const std::string& scene, const std::string& id, net::LockOp op) {
        net::ContainerLockMsg m;
        m.scene = scene;
        m.id = id;
        m.op = op;
        net::Writer w(net::Msg::ContainerLock);
        net::write_container_lock(w, m);
        session.send_packet(w, true);
    }
    void put_object(const std::string& scene, const std::string& id) {
        net::ObjectPutMsg m;
        m.scene = scene;
        m.seq = ++seq;
        m.object.kind = net::ObjectKind::BreakDown;
        m.object.done = 1;
        m.object.id = id;
        net::Writer w(net::Msg::ObjectPut, net::kMaxMessage);
        net::write_object_put(w, m);
        session.send_packet(w, true);
    }
    void put_fire(const std::string& scene, const std::string& id, const std::string& text) {
        net::FirePutMsg m;
        m.scene = scene;
        m.seq = ++seq;
        m.id = id;
        m.text = text;
        net::Writer w(net::Msg::FirePut, net::kMaxMessage);
        net::write_fire_put(w, m);
        session.send_packet(w, true);
    }
    void put_water(const std::string& scene, const std::string& id, const std::string& text) {
        net::WaterPutMsg m;
        m.scene = scene;
        m.seq = ++seq;
        m.id = id;
        m.text = text;
        net::Writer w(net::Msg::WaterPut, net::kMaxMessage);
        net::write_water_put(w, m);
        session.send_packet(w, true);
    }
};

void pump(srv::Server& server, const std::vector<Player*>& players, double ms) {
    const double until = net::now_ms() + ms;
    while (net::now_ms() < until) {
        server.update();
        for (Player* p : players) {
            p->session.update();
            p->session.drain_packets(p->got);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void clear(const std::vector<Player*>& players) {
    for (Player* p : players) p->got.clear();
}

template <typename V, typename Pred>
int count(const V& v, Pred pred) {
    int n = 0;
    for (const auto& x : v)
        if (pred(x)) ++n;
    return n;
}

std::unique_ptr<srv::Server> start_server(const std::string& world_file) {
    auto server = std::make_unique<srv::Server>([](const std::string& line) { std::printf("  [server] %s\n", line.c_str()); });
    srv::Options opt;
    opt.bind_ip = "127.0.0.1";
    opt.port = kPort;
    opt.game_version = kVersion;
    opt.world_file = world_file;
    opt.save_every_s = 1;
    if (!server->start(opt)) return nullptr;
    return server;
}

// A host's world_keep file, as the mod writes it.
void write_host_file(const std::filesystem::path& file) {
    std::vector<world_keep::Record> records;
    {
        net::ItemHashesMsg m;  // the host last saw three items in the cabin: A, B, C
        m.scene = "CampOffice";
        for (const char* id : {"cabin-A", "cabin-B", "cabin-C"}) m.hashes.push_back(net::pdid_hash(id));
        net::Writer w(net::Msg::ItemList, net::kMaxMessage);
        net::write_item_hashes(w, m);
        records.push_back(world_keep::record(world_keep::Tag::ItemList, w));
    }
    for (const char* id : {"cabin-A", "cabin-B", "cabin-C"}) {
        net::ItemPutMsg m;
        m.scene = "CampOffice";
        m.pdid = id;
        m.prefab = "GEAR_Can";
        m.gear = std::string("{\"record\":\"") + id + "\"}";
        net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
        net::write_item_put(w, m);
        records.push_back(world_keep::record(world_keep::Tag::ItemRecord, w));
    }
    {
        net::ItemGoneMsg m;  // since then a friend took B...
        m.scene = "CampOffice";
        m.pdid = "cabin-B";
        net::Writer w(net::Msg::ItemGone);
        net::write_item_gone(w, m);
        records.push_back(world_keep::record(world_keep::Tag::ItemChange, w));
    }
    {
        net::ItemPutMsg m;  // ...and dropped D
        m.scene = "CampOffice";
        m.pdid = "cabin-D";
        m.prefab = "GEAR_Knife";
        m.gear = "{\"record\":\"cabin-D\"}";
        net::Writer w(net::Msg::ItemPut, net::kMaxMessage);
        net::write_item_put(w, m);
        records.push_back(world_keep::record(world_keep::Tag::ItemChange, w));
    }
    {
        net::ContainerPutMsg m;
        m.scene = "CampOffice";
        m.id = "cabin-locker";
        m.text = "{\"locker\":1}";
        net::Writer w(net::Msg::ContainerPut, net::kMaxMessage);
        net::write_container_put(w, m);
        records.push_back(world_keep::record(world_keep::Tag::ContainerOurs, w));
    }
    {
        world_keep::Record r;  // an animal of the host's registry: not ours to read yet, but never lost
        r.tag = world_keep::Tag::Wildlife;
        r.msg = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        records.push_back(r);
    }
    std::string data;
    auto u32 = [&](uint32_t v) { data.append(reinterpret_cast<const char*>(&v), 4); };
    u32(0x57444C54);
    u32(1);
    u32(static_cast<uint32_t>(records.size()));
    for (const world_keep::Record& r : records) {
        data += static_cast<char>(r.tag);
        u32(static_cast<uint32_t>(r.msg.size()));
        data.append(reinterpret_cast<const char*>(r.msg.data()), r.msg.size());
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

}  // namespace

int main() {
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tldserver_world_test";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string file = (dir / "world.bin").string();

    auto server = start_server(file);
    check(server != nullptr, "the server starts with no world file");
    if (!server) return 1;

    Player a("Kerel"), b("Shatsca"), c("Szymon");
    std::vector<Player*> all = {&a, &b, &c};
    check(a.start("LakeRegion") && b.start("LakeRegion") && c.start("MountainTownRegion"), "three players start");
    pump(*server, all, 2500);
    check(server->players().size() == 3, "three players are in");
    clear(all);

    // --- items: a change reaches the others, and is kept
    a.put_item("LakeRegion", "hatchet-1");
    pump(*server, all, 600);
    check(count(b.puts(), [](const auto& m) { return m.pdid == "hatchet-1" && m.scene == "LakeRegion"; }) == 1,
          "an item one player dropped reaches a player in the same scene");
    check(a.puts().empty(), "and doesn't come back to the one who dropped it");
    check(server->world().counts().items == 1, "the server keeps it");

    // --- two takes, one after the other: the second is refused
    clear(all);
    a.take_item("LakeRegion", "hatchet-1");
    pump(*server, all, 1500);
    check(count(b.gones(), [](const auto& m) { return m.pdid == "hatchet-1" && m.reason == net::ItemGoneReason::Taken; }) == 1,
          "a take reaches the others");
    clear(all);
    b.take_item("LakeRegion", "hatchet-1");
    pump(*server, all, 600);
    check(count(b.gones(), [](const auto& m) { return m.pdid == "hatchet-1" && m.reason == net::ItemGoneReason::NotYours; }) == 1,
          "a later take of the same item is refused (out of that player's pack)");
    check(a.gones().empty() && c.gones().empty(), "and the others never hear of it");
    check(server->world().counts().items == 0 && server->world().counts().gone == 1, "the server knows it's taken");

    // --- two takes at the same moment: a coin, exactly one loser
    clear(all);
    a.put_item("LakeRegion", "can-1");
    pump(*server, all, 400);
    clear(all);
    a.take_item("LakeRegion", "can-1");
    b.take_item("LakeRegion", "can-1");
    pump(*server, all, 800);
    {
        auto lost = [](const Player& p) {
            return count(p.gones(), [](const auto& m) { return m.pdid == "can-1" && m.reason == net::ItemGoneReason::LostToss; });
        };
        check(lost(a) + lost(b) == 1, "two takes at the same moment: the coin leaves exactly one without it");
    }

    // --- containers: kept, told on asking; one player at a time
    clear(all);
    a.put_container("LakeRegion", "cupboard-1", "{\"beans\":2}");
    pump(*server, all, 500);
    check(count(b.containers(), [](const auto& m) { return m.id == "cupboard-1" && m.text == "{\"beans\":2}" && m.flags == 0; }) == 1,
          "a container's change reaches the others");
    clear(all);
    c.want_container("LakeRegion", "cupboard-1");
    c.want_container("LakeRegion", "cupboard-never-seen");
    pump(*server, all, 500);
    check(count(c.containers(), [](const auto& m) {
              return m.id == "cupboard-1" && m.text == "{\"beans\":2}" && m.flags == net::kContainerAnswer;
          }) == 1,
          "a player opening a container gets its contents");
    check(count(c.containers(), [](const auto& m) {
              return m.id == "cupboard-never-seen" && m.flags == (net::kContainerAnswer | net::kContainerUnknown);
          }) == 1,
          "one the server never heard of: \"nothing known\" (theirs stands)");
    clear(all);
    a.lock("LakeRegion", "cupboard-1", net::LockOp::Take);
    pump(*server, all, 400);
    const uint8_t a_id = a.id();
    check(count(a.locks(), [&](const auto& m) { return m.owner == a_id; }) == 1 &&
              count(b.locks(), [&](const auto& m) { return m.owner == a_id; }) == 1,
          "a container being searched is that player's, and everyone hears");
    clear(all);
    b.lock("LakeRegion", "cupboard-1", net::LockOp::Take);
    pump(*server, all, 400);
    check(count(b.locks(), [&](const auto& m) { return m.owner == a_id; }) == 1, "someone else asking hears whose it is");
    clear(all);
    a.lock("LakeRegion", "cupboard-1", net::LockOp::Release);
    pump(*server, all, 400);
    check(count(b.locks(), [](const auto& m) { return m.owner == net::kNoOwner; }) == 1, "done with it: free for the others");
    clear(all);
    b.lock("LakeRegion", "cupboard-1", net::LockOp::Take);
    pump(*server, all, net::kLockStaleMs + 1500);  // ...and B never says so again
    check(count(a.locks(), [](const auto& m) { return m.owner == net::kNoOwner; }) == 1,
          "a container whose player went quiet is freed");

    // --- a scene nobody else is in: kept, and told to whoever arrives
    clear(all);
    a.go("CoastalRegion");
    pump(*server, all, 1200);
    a.put_item("CoastalRegion", "coast-rifle");
    a.put_item("CoastalRegion", "coast-flare");
    pump(*server, all, 300);
    a.take_item("CoastalRegion", "coast-flare");
    a.take_item("CoastalRegion", "coast-crate-lid");
    a.put_object("CoastalRegion", "coast-crate");
    a.put_fire("CoastalRegion", "coast-fire", "{\"burning\":true}");
    a.put_water("CoastalRegion", "coast-toilet", "{\"litres\":3}");
    pump(*server, all, 800);
    clear(all);
    c.go("CoastalRegion");
    pump(*server, all, 2000);
    check(count(c.puts(), [](const auto& m) { return m.pdid == "coast-rifle" && (m.flags & net::kItemAnswer); }) == 1,
          "arriving: the item lying there");
    check(count(c.puts(), [](const auto& m) { return m.pdid == "coast-flare"; }) == 0 &&
              count(c.gones(), [](const auto& m) { return m.pdid == "coast-flare" || m.pdid == "coast-crate-lid"; }) == 2,
          "arriving: what was taken there is gone (and isn't sent as lying there)");
    check(count(c.objects(), [](const auto& m) { return m.object.id == "coast-crate" && m.object.done == 1; }) == 1,
          "arriving: what was broken down there");
    check(count(c.fires(), [](const auto& m) { return m.id == "coast-fire" && m.text == "{\"burning\":true}"; }) == 1,
          "arriving: the fire there");
    check(count(c.waters(), [](const auto& m) { return m.id == "coast-toilet" && m.text == "{\"litres\":3}"; }) == 1,
          "arriving: the water source there");
    clear(all);
    pump(*server, all, 1500);
    check(c.puts().empty() && c.objects().empty(), "told once per arrival, not again while they stay");
    uint32_t seq_before = 0;
    {
        clear(all);
        a.put_item("CoastalRegion", "coast-seq");
        pump(*server, all, 400);
        for (const auto& m : c.puts()) seq_before = m.seq;
        check(seq_before != 0, "the server counts its own changes");
    }

    // --- the world survives a restart
    const srv::World::Counts before = server->world().counts();
    for (Player* p : all) p->session.stop();
    server->stop();
    server.reset();
    check(std::filesystem::exists(file) && !std::filesystem::exists(file + ".tmp"), "the world is in its file (no half-written one beside it)");
    server = start_server(file);
    check(server != nullptr, "the server starts again from the file");
    if (!server) return 1;
    {
        const srv::World::Counts after = server->world().counts();
        check(after.items == before.items && after.gone == before.gone && after.containers == before.containers &&
                  after.objects == before.objects && after.fires == before.fires && after.water == before.water &&
                  after.scenes == before.scenes,
              "the same world as before the restart");
    }
    Player d("Kerel");
    std::vector<Player*> second = {&d};
    check(d.start("CoastalRegion"), "a player joins the restarted server");
    pump(*server, second, 3000);
    check(count(d.puts(), [](const auto& m) { return m.pdid == "coast-rifle"; }) == 1 &&
              count(d.fires(), [](const auto& m) { return m.id == "coast-fire"; }) == 1,
          "and gets what was kept before the restart");
    {
        uint32_t seq_after = 0;
        for (const auto& m : d.puts()) seq_after = m.seq;
        check(static_cast<int32_t>(seq_after - seq_before) > 0, "the server's counter went on from before the restart");
    }
    d.session.stop();
    server->stop();
    server.reset();

    // --- a host's own world_keep file
    const std::string host_file = (dir / "host.bin").string();
    write_host_file(host_file);
    server = start_server(host_file);
    check(server != nullptr, "the server starts from a host's world_keep file");
    if (!server) return 1;
    Player e("Shatsca");
    std::vector<Player*> third = {&e};
    check(e.start("CampOffice"), "a player joins");
    pump(*server, third, 3000);
    {
        const auto lists = e.lists();
        bool right = lists.size() == 1 && lists[0].scene == "CampOffice" && lists[0].hashes.size() == 3;
        if (right)
            for (const char* id : {"cabin-A", "cabin-C", "cabin-D"})
                right = right && count(lists[0].hashes, [&](uint64_t h) { return h == net::pdid_hash(id); }) == 1;
        check(right, "a scene the host had a whole list of is told as that list: what it saw, minus what was taken since, "
                     "plus what was dropped");
        check(e.puts().empty() && e.gones().empty(), "(as a list only: the player asks for what they lack)");
    }
    e.got.clear();
    e.want_items("CampOffice", {net::pdid_hash("cabin-C"), net::pdid_hash("cabin-D"), net::pdid_hash("cabin-B")});
    e.want_container("CampOffice", "cabin-locker");
    pump(*server, third, 800);
    check(count(e.puts(), [](const auto& m) { return m.pdid == "cabin-C" && m.gear == "{\"record\":\"cabin-C\"}"; }) == 1 &&
              count(e.puts(), [](const auto& m) { return m.pdid == "cabin-D"; }) == 1 && e.puts().size() == 2,
          "asked for, the items come whole (the one taken since doesn't)");
    check(count(e.containers(), [](const auto& m) { return m.id == "cabin-locker" && m.text == "{\"locker\":1}"; }) == 1,
          "the host's containers answer too");
    e.take_item("CampOffice", "cabin-A");
    pump(*server, third, 1500);
    e.session.stop();
    server->stop();
    server.reset();
    server = start_server(host_file);
    check(server != nullptr && server->world().counts().other == 1, "records not ours to read yet (animals) are written back");
    if (server) {
        Player f("Szymon");
        std::vector<Player*> fourth = {&f};
        f.start("CampOffice");
        pump(*server, fourth, 3000);
        const auto lists = f.lists();
        check(lists.size() == 1 && lists[0].hashes.size() == 2 &&
                  count(lists[0].hashes, [](uint64_t h) { return h == net::pdid_hash("cabin-A"); }) == 0,
              "a take from a listed scene is off the list after a restart too");
        f.session.stop();
        server->stop();
    }

    // --- a file that isn't a world: the server refuses to start (and so never writes over it)
    {
        const std::string junk = (dir / "junk.bin").string();
        std::ofstream(junk, std::ios::binary) << "not a world file at all";
        auto refused = start_server(junk);
        check(refused == nullptr, "a file that isn't a world: the server doesn't start (it would write over it)");
    }

    std::filesystem::remove_all(dir, ec);
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
