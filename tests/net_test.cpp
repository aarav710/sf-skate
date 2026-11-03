// Multiplayer checks (`make test-net`): wire format round trips, the relay's room rules, and a relay with real
// clients over localhost UDP in one process. No raylib or world needed.
#include "net.h"
#include "../server/relay.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>

using namespace netp;

static int fails = 0;
static void check(bool ok, const char* name, const char* detail) {
    printf("%s  %-40s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) fails++;
}

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// round trip: encode, decode, and every shorter prefix and a longer buffer must be refused
template <class M>
static bool roundTrip(const M& in, M& out, size_t* size = nullptr) {
    auto bytes = pack(in, 0xFF, 0xFFFF, -1.5f);
    if (size) *size = bytes.size();
    Header h;
    if (!unpack(bytes.data(), bytes.size(), out, &h)) return false;
    if (h.type != M::TYPE || h.from != 0xFF || h.seq != 0xFFFF || h.hostTime != -1.5f) return false;
    for (size_t n = 0; n < bytes.size(); n++) { M t; if (unpack(bytes.data(), n, t)) return false; }
    auto longer = bytes; longer.push_back(0);
    M t;
    if (unpack(longer.data(), longer.size(), t)) return false;
    auto wrong = bytes; wrong[0] = (uint8_t)(M::TYPE == MSG_LEFT ? MSG_HOST_CHANGED : MSG_LEFT);
    return !unpack(wrong.data(), wrong.size(), t);
}

static bool near(float a, float b, float tol) { return fabsf(a - b) <= tol; }
static float angDiff(float a, float b) { float d = fmodf(a - b, 6.2831853f); if (d > 3.14159265f) d -= 6.2831853f; if (d < -3.14159265f) d += 6.2831853f; return fabsf(d); }

static void protocolChecks() {
    char b[256];
    {
        MsgCreate a, o; a.version = 65535; a.fingerprint = 0xFFFFFFFFu; a.name = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";  // 16 bytes
        bool ok = roundTrip(a, o) && o.version == 65535 && o.fingerprint == 0xFFFFFFFFu && o.name == a.name;
        MsgCreate e, eo; e.version = 0; e.fingerprint = 0; e.name = "";
        ok = ok && roundTrip(e, eo) && eo.version == 0 && eo.name.empty();
        auto bytes = pack(a);
        check(ok && peekVersion(bytes.data(), bytes.size()) == 65535, "proto: CREATE round trip", "max/min version, fingerprint, 16-byte name");
    }
    {
        MsgCreated a, o; a.code = "ABC234"; a.playerId = 250;
        check(roundTrip(a, o) && o.code == a.code && o.playerId == 250, "proto: CREATED round trip", "code ABC234, id 250");
    }
    {
        MsgJoin a, o; a.version = PROTOCOL_VERSION; a.fingerprint = 0x12345678; a.code = "ZZZZZZ"; a.name = "Ana";
        auto bytes = pack(a);
        check(roundTrip(a, o) && o.code == "ZZZZZZ" && o.fingerprint == 0x12345678 && o.name == "Ana" &&
              peekVersion(bytes.data(), bytes.size()) == PROTOCOL_VERSION, "proto: JOIN round trip", "version peeked before decoding");
    }
    {
        MsgWelcome a, o; a.code = "QWERTY"; a.playerId = 9; a.hostId = 1;
        bool ok = roundTrip(a, o) && o.roster.empty() && o.playerId == 9 && o.hostId == 1;
        for (int i = 0; i < MAX_PLAYERS; i++) a.roster.push_back({(uint8_t)(i + 1), std::string(i == 7 ? 16 : i, 'a' + i)});
        ok = ok && roundTrip(a, o) && o.roster == a.roster;
        // a roster claiming 9 players is refused
        auto bytes = pack(a);
        bytes[HEADER_SIZE + CODE_LEN + 2] = MAX_PLAYERS + 1;
        MsgWelcome t;
        ok = ok && !unpack(bytes.data(), bytes.size(), t);
        check(ok, "proto: WELCOME round trip", "empty and full (8) rosters, 9 refused");
    }
    {
        MsgPlayerJoined a, o; a.id = 1; a.name = "x";
        MsgLeft l, lo; l.id = 255;
        MsgHostChanged hc, ho; hc.id = 0;
        check(roundTrip(a, o) && o.id == 1 && o.name == "x" && roundTrip(l, lo) && lo.id == 255 && roundTrip(hc, ho) && ho.id == 0,
              "proto: JOINED/LEFT/HOST_CHANGED", "ids 0..255");
    }
    {
        MsgError a, o; a.code = ERR_FINGERPRINT; a.text = std::string(ERROR_TEXT_MAX, 'e');
        bool ok = roundTrip(a, o) && o.code == ERR_FINGERPRINT && o.text == a.text;
        // a name or text longer than its limit is refused on decode
        auto bytes = pack(MsgPlayerJoined{});
        bytes[HEADER_SIZE + 1] = NAME_MAX + 1;
        for (int i = 0; i <= NAME_MAX; i++) bytes.push_back('a');
        MsgPlayerJoined t;
        ok = ok && !unpack(bytes.data(), bytes.size(), t);
        check(ok, "proto: ERROR round trip, long strings", "120-char text; 17-byte name refused");
    }
    {
        MsgPing p, po; p.sentAt = 123456.789012345;
        MsgPong q, qo; q.sentAt = -0.0;
        check(roundTrip(p, po) && po.sentAt == p.sentAt && roundTrip(q, qo), "proto: PING/PONG round trip", "f64 exact");
    }
    {   // player state: size, quantization, boundaries
        MsgPlayerState a, o;
        PlayerState& s = a.s;
        s.x = 4689.123f; s.y = -12.5f; s.z = 10449.75f; s.yaw = 3.1f; s.vx = 12.34f; s.vy = -5.67f; s.vz = 0.01f;
        s.state = 2; s.vehicle = PlayerState::SEAPLANE; s.flags = 0x1F; s.rail = 65534; s.railT = 0.3333f;
        s.crouch = 0.42f; s.lean = -0.77f; s.pushPhase = 20.0f; s.pushAmt = 1; s.pitch = -0.35f; s.balance = 0.5f;
        s.flipAngle = -271.3f; s.shoveAngle = 179.99f; s.flipProg = 0.5f; s.shoveProg = -1; s.bailT = 0.37f;
        s.boardDx = 1.23f; s.boardDy = -0.4f; s.boardDz = -300.f; s.boardYaw = -3.14f; s.score = 4000000000u;
        size_t sz = 0;
        bool ok = roundTrip(a, o, &sz);
        const PlayerState& r = o.s;
        ok = ok && sz == HEADER_SIZE + PlayerState::SIZE;
        ok = ok && r.x == s.x && r.y == s.y && r.z == s.z && angDiff(r.yaw, s.yaw) < 1e-4f;
        ok = ok && near(r.vx, s.vx, 0.006f) && near(r.vy, s.vy, 0.006f) && near(r.vz, s.vz, 0.006f);
        ok = ok && r.state == 2 && r.vehicle == PlayerState::SEAPLANE && r.flags == 0x1F && r.rail == 65534 && near(r.railT, s.railT, 1e-4f);
        ok = ok && near(r.crouch, s.crouch, 0.003f) && near(r.lean, s.lean, 0.005f) && angDiff(r.pushPhase, s.pushPhase) < 0.025f;
        ok = ok && near(r.pushAmt, 1, 0) && near(r.pitch, s.pitch, 0.006f) && near(r.balance, s.balance, 0.005f);
        ok = ok && near(r.flipAngle, s.flipAngle, 0.011f) && near(r.shoveAngle, s.shoveAngle, 0.011f);   // degrees, unwrapped
        ok = ok && near(r.flipProg, 0.5f, 0.003f) && r.shoveProg == -1 && near(r.bailT, 0.37f, 0.006f);
        ok = ok && near(r.boardDx, 1.23f, 0.006f) && near(r.boardDz, -300.f, 0.006f) && angDiff(r.boardYaw, s.boardYaw) < 1e-4f;
        ok = ok && r.score == 4000000000u;
        snprintf(b, sizeof b, "%zu bytes with header, worst errors within quantization", sz);
        check(ok, "proto: PLAYER_STATE round trip", b);

        // out of range values saturate instead of wrapping; NaN goes out as 0
        PlayerState t;
        t.vx = 1000; t.vy = -1000; t.vz = NAN; t.rail = 70000; t.railT = 2; t.crouch = -1; t.lean = 5; t.pitch = -9;
        t.flipProg = 3; t.bailT = 100; t.x = NAN; t.flipAngle = -360; t.shoveAngle = 1000;
        MsgPlayerState tm, to; tm.s = t;
        ok = roundTrip(tm, to);
        const PlayerState& u = to.s;
        ok = ok && u.vx > 327 && u.vy < -327 && u.vz == 0 && u.rail == -1 && u.railT == 1 && u.crouch == 0 && u.lean == 1;
        ok = ok && near(u.pitch, -1.27f, 1e-4f) && u.flipProg == 1 && near(u.bailT, 2.55f, 1e-4f) && u.x == 0;
        ok = ok && u.flipAngle == -360 && near(u.shoveAngle, 655.34f, 1e-3f);
        // a NaN position or an unknown state in the bytes is refused
        auto bytes = pack(tm);
        float nan = NAN; memcpy(&bytes[HEADER_SIZE], &nan, 4);
        MsgPlayerState v;
        ok = ok && !unpack(bytes.data(), bytes.size(), v);
        bytes = pack(tm); bytes[HEADER_SIZE + 20] = 4;
        ok = ok && !unpack(bytes.data(), bytes.size(), v);
        check(ok, "proto: PLAYER_STATE saturates", "speeds +-327 m/s, rail out of range = none, NaN refused");
    }
    {   // a header clock that is NaN, infinite or absurd is refused, whatever the message (it feeds remote smoothing)
        MsgPlayerState m; MsgPing pg;
        bool ok = true;
        for (float bad : {(float)NAN, (float)INFINITY, -(float)INFINITY, 2e7f}) {
            auto b1 = pack(m, 1, 1, bad), b2 = pack(pg, 1, 1, bad);
            MsgPlayerState o; MsgPing po;
            ok = ok && !unpack(b1.data(), b1.size(), o) && !unpack(b2.data(), b2.size(), po);
        }
        auto good = pack(m, 1, 1, 9.9e6f);
        MsgPlayerState o;
        ok = ok && unpack(good.data(), good.size(), o);
        check(ok, "proto: header clock must be finite", "NaN, +-inf, 2e7 s refused; 9.9e6 s fine");
    }
    {   // names and codes
        bool ok = sanitizeName("Ana") == "Ana" && sanitizeName("") == "skater" && sanitizeName("   ") == "skater";
        ok = ok && sanitizeName("  Bo  ") == "Bo" && sanitizeName("a\nb\tc\x7f") == "abc";
        ok = ok && sanitizeName("abcdefghijklmnopqrstuvwxyz") == "abcdefghijklmnop";
        std::string e9; for (int i = 0; i < 9; i++) e9 += "\xC3\xA9";   // 18 bytes: cut to 8 whole characters
        ok = ok && sanitizeName(e9).size() == 16;
        ok = ok && sanitizeName("a\xC3") == "a" && sanitizeName("\xC0\xAFx") == "x" && sanitizeName("\xED\xA0\x80y") == "y";
        ok = ok && sanitizeName("\xF0\x9F\x9B\xB9 sk8") == "\xF0\x9F\x9B\xB9 sk8";
        ok = ok && normalizeCode("abc-234") == "ABC234" && normalizeCode(" QW ERTY ") == "QWERTY";
        ok = ok && normalizeCode("ABCDE0") == "" && normalizeCode("ABCDEO") == "" && normalizeCode("ABCDE1") == "" && normalizeCode("ABCDEI") == "";
        ok = ok && normalizeCode("ABCDE") == "" && normalizeCode("ABCDEFG") == "";
        std::set<char> sym(CODE_ALPHABET, CODE_ALPHABET + 32);
        ok = ok && sym.size() == 32 && !sym.count('0') && !sym.count('O') && !sym.count('1') && !sym.count('I');
        check(ok, "proto: names and codes sanitized", "UTF-8 kept whole, controls/overlong/surrogates dropped, no 0/O/1/I");
    }
    {
        std::string h; uint16_t p;
        bool ok = parseHostPort("relay.example.com:9000", h, p) && h == "relay.example.com" && p == 9000;
        ok = ok && parseHostPort("10.0.0.2", h, p) && h == "10.0.0.2" && p == 7777;
        ok = ok && !parseHostPort("x:0", h, p) && !parseHostPort("x:70000", h, p) && !parseHostPort(":7777", h, p) && !parseHostPort("x:7a", h, p);
        check(ok, "net: relay address parsing", "host:port, default port 7777, bad ports refused");
    }
    {   // 20 Hz state sends at 60 fps, at 144 fps with jitter, and after a stall
        auto count = [](double fps, double jitter, double secs, bool stall) {
            Every e(Net::STATE_INTERVAL);
            uint32_t r = 12345;
            int n = 0;
            for (double t = 100; t < 100 + secs;) {
                if (e.due(t)) { e.fired(t); n++; }
                r = r * 1664525u + 1013904223u;
                t += (1 / fps) * (1 + jitter * ((r >> 8) / 16777216.0 - 0.5));
                if (stall && t > 105 && t < 105.02) t += 3;   // a 3 s hitch: no burst afterwards
            }
            return n;
        };
        int a = count(60, 0, 10, false), c = count(144, 0.6, 10, false), d = count(60, 0, 10, true);
        snprintf(b, sizeof b, "10 s: %d sends at 60 fps, %d at jittery 144 fps; %d with a 3 s stall (want ~140)", a, c, d);
        check(abs(a - 200) <= 1 && abs(c - 200) <= 1 && abs(d - 140) <= 2, "net: state send cadence 20 Hz", b);
    }
    {   // session options: env first, flags win; nothing asked = no session
        setenv("USER", "aarav", 1);
        unsetenv("SFSKATE_CREATE"); unsetenv("SFSKATE_JOIN"); unsetenv("SFSKATE_NAME"); unsetenv("SFSKATE_RELAY");
        NetConfig c; std::string err;
        char a0[] = "sfskate", a1[] = "--create", a2[] = "--name", a3[] = "Ana", a4[] = "--relay", a5[] = "r.example:9", a6[] = "--join", a7[] = "abc234";
        char* none[] = {a0};
        bool ok = !netOptions(1, none, c, err) && err.empty() && c.name == "aarav" && c.relay == "127.0.0.1:7777";
        char* cr[] = {a0, a1, a2, a3, a4, a5};
        ok = ok && netOptions(6, cr, c, err) && c.create && c.name == "Ana" && c.relay == "r.example:9";
        setenv("SFSKATE_JOIN", "QQQQQQ", 1); setenv("SFSKATE_NAME", "Env", 1); setenv("SFSKATE_RELAY", "e.example:5", 1);
        ok = ok && netOptions(1, none, c, err) && !c.create && c.joinCode == "QQQQQQ" && c.name == "Env" && c.relay == "e.example:5";
        char* jn[] = {a0, a6, a7};
        ok = ok && netOptions(3, jn, c, err) && !c.create && c.joinCode == "abc234";
        setenv("SFSKATE_CREATE", "1", 1); unsetenv("SFSKATE_JOIN");
        ok = ok && netOptions(1, none, c, err) && c.create;
        setenv("SFSKATE_CREATE", "0", 1);
        ok = ok && !netOptions(1, none, c, err);
        char* missing[] = {a0, a6};
        ok = ok && !netOptions(2, missing, c, err) && err == "--join needs a value";
        unsetenv("SFSKATE_CREATE"); unsetenv("SFSKATE_NAME"); unsetenv("SFSKATE_RELAY");
        check(ok, "net: command line and env options", "default name from $USER, env SFSKATE_*, flags override");
    }
}

static bool validCode(const std::string& c) { return normalizeCode(c) == c; }

static void roomChecks() {
    using namespace relay;
    char b[256];
    {
        Rooms R;
        CreateResult c = R.create(1, PROTOCOL_VERSION, 77, " Ana\n");
        const Room* r = R.room(c.code);
        bool ok = !c.error && validCode(c.code) && c.id == 1 && r && r->host == 1 && r->members[0].name == "Ana";
        ok = ok && R.create(1, PROTOCOL_VERSION, 77, "again").error == ERR_BAD_REQUEST;
        ok = ok && R.create(2, PROTOCOL_VERSION + 1, 77, "old").error == ERR_VERSION && R.roomCount() == 1;
        check(ok, "rooms: create", "valid code, id 1 hosts, name sanitized; second create and wrong version refused");
    }
    {   // codes: valid alphabet, unique, room limit
        Rooms R;
        std::set<std::string> codes;
        bool ok = true;
        for (int i = 0; i < MAX_ROOMS; i++) {
            CreateResult c = R.create(100 + i, PROTOCOL_VERSION, 1, "p");
            ok = ok && !c.error && validCode(c.code);
            codes.insert(c.code);
        }
        CreateResult over = R.create(9999, PROTOCOL_VERSION, 1, "p");
        snprintf(b, sizeof b, "%zu unique of %d, room %d refused: %s", codes.size(), MAX_ROOMS, MAX_ROOMS + 1, errorText(over.error));
        check(ok && codes.size() == (size_t)MAX_ROOMS && over.error == ERR_SERVER_FULL && R.roomCount() == MAX_ROOMS, "rooms: unique codes, 200-room cap", b);
    }
    {   // a code collision draws again; a stuck generator gives up instead of looping
        std::vector<uint32_t> seq = {5, 5, 5, 7};
        size_t at = 0;
        Rooms R([&] { return seq[std::min(at++, seq.size() - 1)]; });
        CreateResult a = R.create(1, PROTOCOL_VERSION, 1, "a"), c = R.create(2, PROTOCOL_VERSION, 1, "b");
        Rooms S([] { return 42u; });
        CreateResult s1 = S.create(1, PROTOCOL_VERSION, 1, "a"), s2 = S.create(2, PROTOCOL_VERSION, 1, "b");
        bool ok = !a.error && !c.error && a.code != c.code && at == 4 && !s1.error && s2.error == ERR_SERVER_FULL;
        snprintf(b, sizeof b, "%s then %s after %zu draws; stuck rng: %s", a.code.c_str(), c.code.c_str(), at, errorText(s2.error));
        check(ok, "rooms: code collision redraws", b);
    }
    {   // join rules
        Rooms R;
        CreateResult c = R.create(1, PROTOCOL_VERSION, 77, "host");
        std::string lower = c.code;
        for (auto& ch : lower) ch = (char)tolower(ch);
        bool ok = R.join(2, c.code, PROTOCOL_VERSION + 1, 77, "v").error == ERR_VERSION;
        ok = ok && R.join(2, c.code, PROTOCOL_VERSION, 78, "f").error == ERR_FINGERPRINT;
        ok = ok && R.join(2, "AAAAAA" == c.code ? "BBBBBB" : "AAAAAA", PROTOCOL_VERSION, 77, "n").error == ERR_NO_SESSION;
        ok = ok && R.join(2, "nonsense", PROTOCOL_VERSION, 77, "n").error == ERR_NO_SESSION;
        JoinResult j = R.join(2, lower, PROTOCOL_VERSION, 77, "two");
        ok = ok && !j.error && j.id == 2 && j.host == 1 && j.others.size() == 1 && j.others[0].id == 1 && j.others[0].name == "host";
        ok = ok && j.notify.size() == 1 && j.notify[0] == 1 && R.idOf(2) == 2;
        ok = ok && R.join(2, c.code, PROTOCOL_VERSION, 77, "dup").error == ERR_BAD_REQUEST;
        int joined = 2;
        for (PeerKey p = 3; p < 20; p++) if (!R.join(p, c.code, PROTOCOL_VERSION, 77, "x").error) joined++;
        JoinResult full = R.join(50, c.code, PROTOCOL_VERSION, 77, "late");
        ok = ok && joined == MAX_PLAYERS && full.error == ERR_ROOM_FULL && R.room(c.code)->members.size() == (size_t)MAX_PLAYERS;
        snprintf(b, sizeof b, "version/fingerprint/no-session refused, lowercase code ok, %d joined then %s", joined, errorText(full.error));
        check(ok, "rooms: join checks and 8-player cap", b);
    }
    {   // leaving: ids not reused right away, host goes to the longest connected, empty room deleted
        Rooms R;
        CreateResult c = R.create(10, PROTOCOL_VERSION, 1, "A");
        R.join(20, c.code, PROTOCOL_VERSION, 1, "B");
        R.join(30, c.code, PROTOCOL_VERSION, 1, "C");
        LeaveResult lb = R.leave(20);
        bool ok = lb.wasInRoom && lb.id == 2 && !lb.newHost && !lb.roomDeleted && lb.notify.size() == 2;
        JoinResult d = R.join(40, c.code, PROTOCOL_VERSION, 1, "D");
        ok = ok && d.id == 4;
        LeaveResult la = R.leave(10);
        ok = ok && la.id == 1 && la.newHost == 3 && R.room(c.code)->host == 3 && la.notify.size() == 2;
        LeaveResult lc = R.leave(30);
        ok = ok && lc.newHost == 4 && R.room(c.code)->host == 4;
        ok = ok && !R.leave(30).wasInRoom && !R.leave(12345).wasInRoom;
        LeaveResult ld = R.leave(40);
        ok = ok && ld.roomDeleted && R.roomCount() == 0 && R.playerCount() == 0 && R.join(50, c.code, PROTOCOL_VERSION, 1, "E").error == ERR_NO_SESSION;
        snprintf(b, sizeof b, "B left (id 2), D got id %u, A left -> host %u, C left -> host %u, last out closes room", d.id, la.newHost, lc.newHost);
        check(ok, "rooms: leave, host promotion, cleanup", b);
    }
    {
        RateLimit rl; rl.rate = 1000; rl.burst = rl.tokens = 2000;
        bool ok = rl.allow(1500, 0) && !rl.allow(1000, 0) && rl.allow(500, 0) && !rl.allow(1, 0) && rl.allow(900, 1.0) && !rl.allow(200, 1.0);
        ok = ok && rl.allow(2000, 100) && !rl.allow(1, 100);   // refill is capped at the burst
        check(ok, "relay: rate limit bucket", "burst, then refill at the rate");
    }
}

// A bare ENet client, to send what a real Net wouldn't (a forged sender id).
struct RawClient {
    ENetHost* host = nullptr;
    ENetPeer* peer = nullptr;
    bool connected = false;
    std::vector<std::vector<uint8_t>> got;
    bool dropped = false;
    bool start(uint16_t port, bool quickTimeout = false) {
        host = enet_host_create(nullptr, 1, CH_COUNT, 0, 0);
        ENetAddress a; enet_address_set_host(&a, "127.0.0.1"); a.port = port;
        peer = host ? enet_host_connect(host, &a, CH_COUNT, 0) : nullptr;
        if (peer && quickTimeout) enet_peer_timeout(peer, 32, 300, 1500);
        return peer != nullptr;
    }
    void service() {
        ENetEvent ev;
        while (host && enet_host_service(host, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) connected = true;
            if (ev.type == ENET_EVENT_TYPE_DISCONNECT) { dropped = true; peer = nullptr; }
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                got.emplace_back(ev.packet->data, ev.packet->data + ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            }
        }
    }
    void send(const std::vector<uint8_t>& bytes, uint8_t ch) {
        enet_peer_send(peer, ch, enet_packet_create(bytes.data(), bytes.size(), ch == CH_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0));
        enet_host_flush(host);
    }
    ~RawClient() { if (peer) enet_peer_disconnect_now(peer, 0); if (host) enet_host_destroy(host); }
};

// The relay's ENet limits, on a relay of their own: an oversized packet is never taken in (its sender gets
// dropped) while everyone else carries on, and one address can't open more than MAX_PEERS_PER_IP connections.
static void limitChecks() {
    char b[320];
    relay::Relay rel;
    rel.log = getenv("NET_TEST_LOG") != nullptr;
    if (!rel.start(0)) { check(false, "limits: relay starts", "can't bind UDP"); return; }
    std::vector<std::unique_ptr<RawClient>> rc;
    auto pump = [&](std::function<bool()> cond, double timeout) -> double {
        double t0 = nowSec();
        while (nowSec() - t0 < timeout) {
            rel.service(1);
            for (auto& c : rc) c->service();
            if (cond()) return nowSec() - t0;
        }
        return -1;
    };
    for (int i = 0; i < 3; i++) { rc.push_back(std::make_unique<RawClient>()); rc.back()->start(rel.port(), true); }
    pump([&] { return rc[0]->connected && rc[1]->connected && rc[2]->connected; }, 10);
    MsgCreate cr; cr.name = "big";
    rc[0]->send(pack(cr), CH_RELIABLE);
    pump([&] { return !rc[0]->got.empty(); }, 10);
    MsgCreated made;
    bool created = !rc[0]->got.empty() && unpack(rc[0]->got[0].data(), rc[0]->got[0].size(), made);
    std::vector<uint8_t> fits(relay::MAX_PACKET, 0xEE), huge(256 * 1024, 0xEE);   // unknown type: ignored if it arrives
    rc[1]->send(fits, CH_RELIABLE);
    rc[0]->send(huge, CH_RELIABLE);
    double tDrop = pump([&] { return rc[0]->dropped; }, 20);
    // the relay still works for others: rc[1] (which sent a packet right at the limit) joins the room after all that
    MsgJoin j; j.code = made.code; j.name = "after";
    rc[1]->send(pack(j), CH_RELIABLE);
    double tJoin = pump([&] { return !rc[1]->got.empty(); }, 10);
    MsgWelcome w;
    bool joined = tJoin >= 0 && unpack(rc[1]->got[0].data(), rc[1]->got[0].size(), w) && !rc[1]->dropped;
    snprintf(b, sizeof b, "%zu KB packet: sender dropped after %.1fs; a %zu B one is fine and its sender then joins (%.2fs), room has %zu",
             huge.size() / 1024, tDrop, relay::MAX_PACKET, tJoin, created ? rel.rooms.playerCount() : (size_t)0);
    check(created && tDrop >= 0 && joined && !rc[2]->dropped, "limits: oversized packet refused", b);

    // (the oversized sender is still a peer on that relay until its timeout, so count on a fresh one)
    rc.clear();
    rel.stop();
    if (!rel.start(0)) { check(false, "limits: connections per address", "can't bind UDP"); return; }
    for (size_t i = 0; i < relay::MAX_PEERS_PER_IP + 1; i++) { rc.push_back(std::make_unique<RawClient>()); rc.back()->start(rel.port(), true); }
    auto settled = [&] { for (auto& c : rc) if (!c->connected && !c->dropped) return false; return true; };
    pump(settled, 20);
    int conn = 0, refused = 0;
    for (auto& c : rc) { conn += c->connected && !c->dropped; refused += !c->connected; }
    snprintf(b, sizeof b, "%zu tries from one address: %d connected, %d refused", rc.size(), conn, refused);
    check(conn == (int)relay::MAX_PEERS_PER_IP && refused == 1, "limits: connections per address", b);
}

static void liveChecks() {
    char b[320];
    relay::Relay rel;
    rel.log = getenv("NET_TEST_LOG") != nullptr;
    bool up = rel.start(0);
    snprintf(b, sizeof b, "UDP port %u", (unsigned)rel.port());
    check(up && rel.port() != 0, "live: relay starts on a free port", b);
    if (!up) return;
    const std::string addr = "127.0.0.1:" + std::to_string(rel.port());

    // every client is serviced each pass; the ones with a state send it at 20 Hz
    struct Peer { Net* net; PlayerState* st; };
    std::vector<Peer> peers;
    RawClient* raw = nullptr;
    auto pump = [&](std::function<bool()> cond, double timeout) -> double {
        double t0 = nowSec();
        while (nowSec() - t0 < timeout) {
            rel.service(1);
            double now = nowSec();
            for (auto& p : peers) {
                p.net->service(now);
                if (p.st && p.net->stateDue(now)) p.net->sendState(*p.st, now);
            }
            if (raw) raw->service();
            if (cond()) return nowSec() - t0;
        }
        return -1;
    };
    auto sees = [](const Net& n, uint8_t id, float x) {
        const RemoteInfo* r = n.remote(id);
        return r && r->hasState && r->state.x == x;
    };
    auto logged = [](const Net& n, const std::string& line) {
        for (auto& l : n.log) if (l == line) return true;
        return false;
    };

    PlayerState sa, sb;
    sa.x = 100; sa.y = 5; sa.z = 200; sa.state = 1;
    sb.x = -50; sb.y = 2; sb.z = 30;
    Net A, B;
    peers.push_back({&A, &sa});
    NetConfig ca; ca.relay = addr; ca.name = "Ana"; ca.create = true; ca.fingerprint = 42;
    A.start(ca, nowSec());
    double t = pump([&] { return A.status != Net::CONNECTING && A.status != Net::JOINING; }, 10);
    snprintf(b, sizeof b, "%s after %.2fs: code %s, id %u", A.statusText(), t, A.code.c_str(), (unsigned)A.myId);
    check(A.status == Net::IN_SESSION && validCode(A.code) && A.isHost() && A.justCreated, "live: A creates a session", b);
    if (A.status != Net::IN_SESSION) return;

    std::string typed = A.code.substr(0, 3) + "-" + A.code.substr(3);   // as a friend might type it
    for (auto& ch : typed) ch = (char)tolower(ch);
    NetConfig cb; cb.relay = addr; cb.name = "Bo"; cb.joinCode = typed; cb.fingerprint = 42;
    peers.push_back({&B, &sb});
    B.start(cb, nowSec());
    t = pump([&] { return B.status == Net::FAILED || (B.status == Net::IN_SESSION && A.remotes.size() == 1); }, 10);
    bool ok = B.status == Net::IN_SESSION && B.code == A.code && B.hostId == A.myId && !B.isHost() && B.myId != A.myId;
    ok = ok && B.remotes.size() == 1 && B.remotes[0].name == "Ana" && A.remotes.size() == 1 && A.remotes[0].name == "Bo";
    snprintf(b, sizeof b, "\"%s\" after %.2fs: B id %u, host %u, A sees %zu remote(s)", typed.c_str(), t, (unsigned)B.myId, (unsigned)B.hostId, A.remotes.size());
    check(ok, "live: B joins with the code", b);
    if (!ok) return;

    t = pump([&] { return sees(A, B.myId, -50) && sees(B, A.myId, 100); }, 10);
    ok = t >= 0 && A.remote(B.myId)->state.z == 30 && B.remote(A.myId)->state.z == 200 && B.remote(A.myId)->state.state == 1;
    sa.x = 111;   // and the next update gets through too
    double t2 = pump([&] { return sees(B, A.myId, 111); }, 10);
    double t3 = pump([&] { return A.rttMs >= 0 && B.rttMs >= 0; }, 10);
    snprintf(b, sizeof b, "both positions after %.2fs, a moved one after %.2fs, ping %.1f / %.1f ms", t, t2, A.rttMs, B.rttMs);
    check(ok && t2 >= 0 && t3 >= 0, "live: players see each other's state", b);
    {   // every state also lands in the inbox the game smooths from, with the sender's clock (rising) and ours
        bool inOk = !B.inbox.empty(), rising = true, fromA = true;
        for (size_t i = 0; i < B.inbox.size(); i++) {
            fromA = fromA && B.inbox[i].id == A.myId && B.inbox[i].recvAt > 0;
            if (i) rising = rising && B.inbox[i].sentAt > B.inbox[i - 1].sentAt;
        }
        bool last = inOk && B.inbox.back().s.x == 111;
        size_t n = B.inbox.size();
        snprintf(b, sizeof b, "%zu states, all from A %d, sender clock rising %d, newest is the moved one %d", n, fromA, rising, last);
        check(inOk && fromA && rising && last && n <= Net::INBOX_MAX, "live: timestamped states for smoothing", b);
    }

    {   // a client that claims to be A: the relay stamps its real id; a wrong version is refused
        auto rcp = std::make_unique<RawClient>();
        RawClient& rc = *rcp;
        raw = &rc;
        rc.start(rel.port());
        pump([&] { return rc.connected; }, 10);
        MsgCreate old; old.version = 999; old.name = "old";
        rc.send(pack(old), CH_RELIABLE);
        MsgError err;
        pump([&] { return !rc.got.empty(); }, 10);
        bool verOk = !rc.got.empty() && unpack(rc.got.back().data(), rc.got.back().size(), err) && err.code == ERR_VERSION;
        rc.got.clear();
        MsgJoin j; j.code = A.code; j.fingerprint = 42; j.name = "Raw";
        rc.send(pack(j), CH_RELIABLE);
        MsgWelcome w;
        pump([&] { return !rc.got.empty() && B.remotes.size() == 2; }, 10);
        bool joined = !rc.got.empty() && unpack(rc.got[0].data(), rc.got[0].size(), w);
        A.log.clear();
        peers[0].st = nullptr;   // A stays quiet, so a forged state credited to A would stick
        MsgPlayerState f; f.s.x = 7; f.s.y = 7; f.s.z = 7;
        rc.send(pack(f, A.myId, 30000), CH_STATE);
        double tf = pump([&] { return sees(B, w.playerId, 7); }, 10);
        bool forged = B.remote(A.myId) && B.remote(A.myId)->state.x == 111;
        snprintf(b, sizeof b, "version 999 -> %s; forged state shown as player %u after %.2fs, A's state untouched=%d",
                 errorText(err.code), (unsigned)w.playerId, tf, (int)forged);
        check(verOk && joined && tf >= 0 && forged, "live: relay stamps the true sender", b);
        peers[0].st = &sa;
        raw = nullptr;
        rcp.reset();   // drops the connection right away (disconnect_now)
        double tl = pump([&] { return A.remotes.size() == 1 && B.remotes.size() == 1; }, 10);
        snprintf(b, sizeof b, "both rosters back to 1 after %.2fs, A logged \"Raw left\"=%d", tl, (int)logged(A, "Raw left"));
        check(tl >= 0 && logged(A, "Raw left"), "live: LEFT on disconnect", b);
    }
    {   // joins that must fail
        Net C, D;
        NetConfig cc = cb; cc.name = "Cy"; cc.fingerprint = 43;
        NetConfig cd = cb; cd.name = "Di"; cd.joinCode = A.code == "ZZZZZZ" ? "YYYYYY" : "ZZZZZZ";
        peers.push_back({&C, nullptr});
        peers.push_back({&D, nullptr});
        C.start(cc, nowSec());
        D.start(cd, nowSec());
        pump([&] { return C.status == Net::FAILED && D.status == Net::FAILED; }, 10);
        snprintf(b, sizeof b, "other world: \"%s\"; bad code: \"%s\"; room still has %zu", C.error.c_str(), D.error.c_str(),
                 rel.rooms.room(A.code)->members.size());
        check(C.error == errorText(ERR_FINGERPRINT) && D.error == errorText(ERR_NO_SESSION) && rel.rooms.room(A.code)->members.size() == 2,
              "live: wrong world / no session refused", b);
        peers.resize(2);
    }
    // B leaves: A sees it
    B.stop();
    peers.pop_back();
    t = pump([&] { return A.remotes.empty(); }, 10);
    snprintf(b, sizeof b, "after %.2fs, logged \"Bo left\"=%d", t, (int)logged(A, "Bo left"));
    check(t >= 0 && logged(A, "Bo left"), "live: A sees B leave", b);

    // host leaves: the other player takes over
    Net E;
    NetConfig ce = cb; ce.name = "Ed";
    PlayerState se; se.x = 1;
    peers.push_back({&E, &se});
    E.start(ce, nowSec());
    pump([&] { return E.status == Net::IN_SESSION && A.remotes.size() == 1; }, 10);
    uint8_t oldHost = A.myId;
    A.stop();
    peers.erase(peers.begin());
    t = pump([&] { return E.isHost(); }, 15);
    const relay::Room* room = rel.rooms.room(E.code);
    snprintf(b, sizeof b, "after %.2fs E (id %u) hosts, roster %zu, relay host %u (was %u)", t, (unsigned)E.myId, E.remotes.size(),
             room ? (unsigned)room->host : 0u, (unsigned)oldHost);
    check(t >= 0 && E.remotes.empty() && logged(E, "You are the host now") && room && room->host == E.myId, "live: host leaves, next one hosts", b);

    E.stop();
    peers.clear();
    t = pump([&] { return rel.rooms.roomCount() == 0 && rel.rooms.playerCount() == 0; }, 10);
    check(t >= 0, "live: empty session deleted", t >= 0 ? "no rooms left" : "a room is still open");

    {   // a relay that never answers, and a bad address
        Net F, G;
        NetConfig cf; cf.relay = "127.0.0.1:" + std::to_string(rel.port() == 65535 ? 65534 : rel.port() + 1); cf.create = true;
        double t0 = nowSec();
        F.start(cf, t0);
        F.service(t0 + Net::CONNECT_TIMEOUT + 1);
        NetConfig cg; cg.relay = "host:port"; cg.create = true;
        bool started = G.start(cg, t0);
        NetConfig ch; ch.relay = addr; ch.joinCode = "nope";
        Net H;
        bool hStarted = H.start(ch, t0);
        snprintf(b, sizeof b, "\"%s\"; \"%s\"; \"%s\"", F.error.c_str(), G.error.c_str(), H.error.c_str());
        check(F.status == Net::FAILED && !started && G.status == Net::FAILED && !hStarted && H.status == Net::FAILED,
              "live: unreachable relay / bad input fail cleanly", b);
    }
}

int main() {
    protocolChecks();
    roomChecks();
    liveChecks();
    limitChecks();
    printf("\n%s (%d failed)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
