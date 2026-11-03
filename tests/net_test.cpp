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
        MsgCreated a, o; a.code = "ABC234"; a.playerId = 250; a.secret = 0xFEDCBA9876543210ull;
        check(roundTrip(a, o) && o.code == a.code && o.playerId == 250 && o.secret == a.secret, "proto: CREATED round trip", "code ABC234, id 250, 64-bit secret");
    }
    {
        MsgJoin a, o; a.version = PROTOCOL_VERSION; a.fingerprint = 0x12345678; a.code = "ZZZZZZ"; a.name = "Ana";
        auto bytes = pack(a);
        check(roundTrip(a, o) && o.code == "ZZZZZZ" && o.fingerprint == 0x12345678 && o.name == "Ana" &&
              peekVersion(bytes.data(), bytes.size()) == PROTOCOL_VERSION, "proto: JOIN round trip", "version peeked before decoding");
    }
    {
        MsgWelcome a, o; a.code = "QWERTY"; a.playerId = 9; a.hostId = 1; a.secret = 0x0123456789ABCDEFull;
        bool ok = roundTrip(a, o) && o.roster.empty() && o.playerId == 9 && o.hostId == 1 && o.secret == a.secret;
        for (int i = 0; i < MAX_PLAYERS; i++) a.roster.push_back({(uint8_t)(i + 1), std::string(i == 7 ? 16 : i, 'a' + i), i % 3 == 1});
        ok = ok && roundTrip(a, o) && o.roster == a.roster && o.roster[1].away && !o.roster[0].away;
        // a roster claiming 9 players is refused, and an away flag that isn't 0/1
        auto bytes = pack(a);
        bytes[HEADER_SIZE + CODE_LEN + 2 + 8] = MAX_PLAYERS + 1;
        MsgWelcome t;
        ok = ok && !unpack(bytes.data(), bytes.size(), t);
        bytes = pack(a);
        bytes[HEADER_SIZE + CODE_LEN + 2 + 8 + 2] = 2;
        ok = ok && !unpack(bytes.data(), bytes.size(), t);
        check(ok, "proto: WELCOME round trip", "empty and full (8) rosters with away flags, 9 refused, bad flag refused");
    }
    {
        MsgPlayerJoined a, o; a.id = 1; a.name = "x";
        MsgLeft l, lo; l.id = 255;
        MsgHostChanged hc, ho; hc.id = 0;
        MsgAway aw, awo; aw.id = 7; aw.away = true;
        MsgAway bk, bko; bk.id = 7; bk.away = false;
        MsgSessionEnding se, seo; se.secondsLeft = 300;
        bool ok = roundTrip(a, o) && o.id == 1 && o.name == "x" && roundTrip(l, lo) && lo.id == 255 && roundTrip(hc, ho) && ho.id == 0;
        ok = ok && roundTrip(aw, awo) && awo.id == 7 && awo.away && roundTrip(bk, bko) && !bko.away && roundTrip(se, seo) && seo.secondsLeft == 300;
        auto ab = pack(aw); ab[HEADER_SIZE + 1] = 2;
        MsgAway at;
        ok = ok && !unpack(ab.data(), ab.size(), at);
        check(ok, "proto: JOINED/LEFT/HOST_CHANGED/AWAY/ENDING", "ids 0..255, away flag 0/1 only");
    }
    {
        MsgRejoin a, o; a.version = PROTOCOL_VERSION; a.fingerprint = 99; a.code = "ABCDEF"; a.playerId = 3; a.secret = ~0ull;
        auto bytes = pack(a);
        check(roundTrip(a, o) && o.code == "ABCDEF" && o.playerId == 3 && o.secret == ~0ull && o.fingerprint == 99 &&
              peekVersion(bytes.data(), bytes.size()) == PROTOCOL_VERSION, "proto: REJOIN round trip", "code, id, secret; version peeked");
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
    {   // car and full snapshots: the car and full bytes run to the end of the message (their own formats check them)
        MsgCarSnapshot a, o; a.simTime = 1234.0625; a.hostRttMs = 80; a.part = 2; a.parts = 3; a.trains = {3, 1, 4}; a.cars = {1, 5, 9, 2, 6};
        auto bytes = pack(a);
        bool ok = unpack(bytes.data(), bytes.size(), o) && o.simTime == a.simTime && o.hostRttMs == 80 && o.part == 2 && o.parts == 3 &&
                  o.trains == a.trains && o.cars == a.cars;
        for (size_t n = 0; n < HEADER_SIZE + 14; n++) { MsgCarSnapshot t; ok = ok && !unpack(bytes.data(), n, t); }   // (no room for the trains)
        int refused = 0;
        for (double st : {-1.0, 2e7, std::nan("")}) { MsgCarSnapshot c = a; c.simTime = st; auto bb = pack(c); refused += !unpack(bb.data(), bb.size(), o); }
        for (int pp : {0, 9}) { MsgCarSnapshot c = a; c.parts = (uint8_t)pp; c.part = 0; auto bb = pack(c); refused += !unpack(bb.data(), bb.size(), o); }
        { MsgCarSnapshot c = a; c.part = 3; auto bb = pack(c); refused += !unpack(bb.data(), bb.size(), o); }
        { auto bb = bytes; bb[HEADER_SIZE + 12] = 200; refused += !unpack(bb.data(), bb.size(), o); }   // trains longer than the message
        MsgFullRequest rq, rqo;
        ok = ok && roundTrip(rq, rqo);
        snprintf(b, sizeof b, "exact; short ones refused; %d of 7 bad (time, parts, lengths) refused", refused);
        check(ok && refused == 7, "proto: CAR_SNAPSHOT round trip", b);
    }
    {   // full snapshots go in chunks of FULL_CHUNK, none over MAX_MESSAGE, put back together in any order; a chunk that
        // isn't exactly its share of the layout it claims, or claims more than MAX_SIZE, is refused
        std::vector<uint8_t> all(220 * 1024 + 123);
        for (size_t i = 0; i < all.size(); i++) all[i] = (uint8_t)(i * 7 + (i >> 11));
        auto parts = MsgFullSnapshot::split(5, 77, all);
        size_t biggest = 0;
        bool ok = parts.size() == 4;
        std::vector<std::vector<uint8_t>> wire;
        for (auto& c : parts) { wire.push_back(pack(c, 1, 0, 0)); biggest = std::max(biggest, wire.back().size()); }
        ok = ok && biggest <= MAX_MESSAGE && biggest == HEADER_SIZE + 9 + MsgFullSnapshot::FULL_CHUNK;
        FullAssembler fa;
        std::vector<uint8_t> out;
        int done = 0;
        for (int k : {2, 0, 0, 3, 1}) {   // out of order, chunk 0 twice
            MsgFullSnapshot m;
            ok = ok && unpack(wire[k].data(), wire[k].size(), m);
            done += fa.add(m, out);
        }
        ok = ok && done == 1 && out == all && !fa.active;
        // missing one: never done; a newer snapshot's chunk starts over (the old one's never mix in)
        FullAssembler fb;
        auto newer = MsgFullSnapshot::split(5, 78, all);
        int got = fb.add(parts[0], out) + fb.add(parts[1], out) + fb.add(parts[3], out);
        bool missing = got == 0 && fb.active && fb.got == 3;
        got = fb.add(newer[0], out) + fb.add(parts[2], out);   // (old chunk 2 now starts the old one over again)
        missing = missing && got == 0 && fb.snapId == 77 && fb.got == 1;
        int refused = 0;
        auto bad = [&](auto edit) { MsgFullSnapshot m = parts[1]; edit(m); auto w = pack(m); MsgFullSnapshot t; refused += !unpack(w.data(), w.size(), t); };
        bad([](MsgFullSnapshot& m) { m.data.push_back(0); });                                  // longer than its share
        bad([](MsgFullSnapshot& m) { m.data.pop_back(); });                                     // shorter
        bad([](MsgFullSnapshot& m) { m.total = MsgFullSnapshot::MAX_SIZE + 1; m.count = 9; });  // oversized
        bad([](MsgFullSnapshot& m) { m.count = 5; });                                           // count doesn't fit the total
        bad([](MsgFullSnapshot& m) { m.index = 4; });                                           // past the end
        bad([](MsgFullSnapshot& m) { m.total = 0; m.count = 0; m.index = 0; m.data.clear(); }); // empty
        // a chunk claiming a huge total can't make the assembler allocate it (add checks it too)
        MsgFullSnapshot huge = parts[0]; huge.total = 0xFFFFFFFFu;
        FullAssembler fc;
        bool capped = !fc.add(huge, out) && !fc.active && MsgFullSnapshot::split(1, 1, std::vector<uint8_t>(MsgFullSnapshot::MAX_SIZE + 1)).empty();
        snprintf(b, sizeof b, "%zu B in %zu chunks (largest message %zu B <= %zu); out of order with a repeat: whole %d; one missing: not done %d; "
                 "%d of 6 bad chunks refused; huge totals capped %d", all.size(), parts.size(), biggest, MAX_MESSAGE, (int)ok, (int)missing, refused, (int)capped);
        check(ok && missing && refused == 6 && capped, "proto: full snapshot in chunks", b);
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
    {   // world settings and events: round trips, and anything out of range refused
        MsgWorldSettings w, wo; w.preset = 2; w.flags = MsgWorldSettings::F_FOG | MsgWorldSettings::F_RAIN;
        bool ok = roundTrip(w, wo) && wo == w && wo.fog() && wo.rain();
        auto refuses = [](const std::vector<uint8_t>& bytes, auto m) { return !unpack(bytes.data(), bytes.size(), m); };
        auto wb = pack(w); wb[HEADER_SIZE] = 3; ok = ok && refuses(wb, w);
        wb = pack(w); wb[HEADER_SIZE + 1] = 4; ok = ok && refuses(wb, w);
        MsgEvent h, ho; h.kind = EVT_CAR_HIT; h.idx = 65535; h.simTime = 1e6; h.car.assign(MsgEvent::CAR_MAX, 7);
        ok = ok && roundTrip(h, ho) && ho.idx == 65535 && ho.simTime == 1e6 && ho.car == h.car;
        MsgEvent e, eo; e.kind = EVT_PED_EVADE; e.idx = 4000000; e.evade = 4; e.dirX = -1; e.dirZ = 0; e.dist = 12;
        ok = ok && roundTrip(e, eo) && eo.idx == 4000000 && eo.evade == 4 && eo.dirX == -1 && eo.dist == 12;
        MsgEvent k, ko; k.kind = EVT_PED_KNOCK; k.idx = 9;
        size_t ks = 0;
        ok = ok && roundTrip(k, ko, &ks) && ko.kind == EVT_PED_KNOCK && ko.idx == 9 && ks == HEADER_SIZE + 5;
        int refused = 0;
        for (int v = 0; v < 8; v++) {
            MsgEvent x = e;
            switch (v) {
            case 0: x.evade = 0; break;
            case 1: x.evade = 5; break;
            case 2: x.dirX = NAN; break;
            case 3: x.dirX = 0.5f; break;   // (not a unit direction)
            case 4: x.dist = -1; break;
            case 5: x.dist = INFINITY; break;
            case 6: x.kind = EVT_COUNT; break;
            default: x = h; x.simTime = NAN; break;
            }
            refused += refuses(pack(x), x);
        }
        MsgEvent big = h; big.idx = 70000; refused += refuses(pack(big), big);   // (cars are u16)
        char b[200];
        snprintf(b, sizeof b, "presets 0-2 and fog/rain flags only; car hit / dodge / knock round trips; %d of 9 bad events refused", refused);
        check(ok && refused == 9, "proto: WORLD_SETTINGS and EVENT", b);
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
    const uint16_t V = PROTOCOL_VERSION;
    {   // sessions per address
        Rooms R;
        const uint32_t ip1 = 0x0100007F, ip2 = 0x0200007F;
        int made = 0;
        for (PeerKey p = 1; p <= 6; p++) made += !R.create(p, V, 1, "x", ip1).error;
        CreateResult other = R.create(11, V, 1, "x", ip2), none = R.create(13, V, 1, "x", 0);
        R.leave(1);   // (its only player leaves: the room closes, so there's room for another)
        CreateResult again = R.create(12, V, 1, "x", ip1);
        snprintf(b, sizeof b, "%d of 6 from one address made (%s), another address fine, one closes -> another made %d", made,
                 errorText(R.create(14, V, 1, "x", ip1).error), (int)!again.error);
        check(made == MAX_ROOMS_PER_IP && !other.error && !none.error && !again.error && R.create(15, V, 1, "x", ip1).error == ERR_TOO_MANY_ROOMS,
              "rooms: sessions per address", b);
    }
    {   // a dropped player is away, their place held; the host hands over at once; back with the secret: the same id
        Rooms R;
        CreateResult c = R.create(10, V, 1, "A", 0, 0);
        R.join(20, c.code, V, 1, "B");
        R.join(30, c.code, V, 1, "C");
        LeaveResult d = R.drop(10, 100.0);
        const Room* r = R.room(c.code);
        bool ok = c.secret != 0 && d.wasInRoom && d.away && d.id == 1 && d.newHost == 2 && d.notify.size() == 2 && r->host == 2 &&
                  r->members.size() == 3 && r->find((uint8_t)1)->away() && R.playerCount() == 2 && R.idOf(10) == 0;
        int joined = 0;   // (a held place still counts: 3 + 5 fill the room)
        for (PeerKey p = 100; p < 106; p++) joined += !R.join(p, c.code, V, 1, "x").error;
        for (PeerKey p = 100; p < 105; p++) R.leave(p);
        JoinResult wrongSecret = R.rejoin(40, c.code, V, 1, 1, c.secret ^ 1, 110), wrongWorld = R.rejoin(40, c.code, V, 2, 1, c.secret, 110);
        JoinResult noRoom = R.rejoin(40, "ZZZZZZ" == c.code ? "YYYYYY" : "ZZZZZZ", V, 1, 1, c.secret, 110);
        JoinResult back = R.rejoin(40, c.code, V, 1, 1, c.secret, 110);
        ok = ok && wrongSecret.error == ERR_REJOIN_EXPIRED && wrongWorld.error == ERR_FINGERPRINT && noRoom.error == ERR_NO_SESSION;
        ok = ok && !back.error && back.id == 1 && back.host == 2 && !back.hostChanged && back.name == "A" && back.others.size() == 2 &&
             back.notify.size() == 2 && !back.replaced && R.idOf(40) == 1 && !r->find((uint8_t)1)->away();
        // the relay hadn't noticed the connection drop yet: the rejoin takes the place over from the old one
        JoinResult over = R.rejoin(50, c.code, V, 1, 1, back.secret, 120);
        ok = ok && !over.error && over.replaced == 40 && R.idOf(40) == 0 && R.idOf(50) == 1 && R.playerCount() == 3;
        snprintf(b, sizeof b, "dropped host: away, player %u hosts at once; %d more fit with its place held; wrong secret/world/code refused; back as id %u "
                 "(a member); a second rejoin replaces connection %llu", d.newHost, joined, back.id, (unsigned long long)over.replaced);
        check(ok && joined == 5, "rooms: drop, held place, rejoin", b);
    }
    {   // a held place runs out: LEFT; with everyone away the room stays till the last place runs out; the first back hosts
        Rooms R;
        R.holdTime = 30;
        CreateResult c = R.create(10, V, 1, "A", 0, 0);
        R.join(20, c.code, V, 1, "B");
        R.drop(20, 0);
        TickResult t1 = R.tick(29), t2 = R.tick(31);
        bool ok = t1.expired.empty() && t2.expired.size() == 1 && t2.expired[0].id == 2 && !t2.expired[0].away && !t2.expired[0].roomDeleted &&
                  t2.expired[0].notify.size() == 1 && R.room(c.code)->members.size() == 1;
        R.join(30, c.code, V, 1, "C");
        LeaveResult da = R.drop(10, 40), dc = R.drop(30, 41);
        ok = ok && da.newHost == 3 && !dc.newHost && R.room(c.code) && R.room(c.code)->connected() == 0;
        JoinResult ba = R.rejoin(60, c.code, V, 1, 1, c.secret, 50);
        ok = ok && !ba.error && ba.hostChanged && ba.host == 1 && R.room(c.code)->host == 1;
        R.drop(60, 55);
        TickResult t3 = R.tick(100);
        ok = ok && t3.expired.size() == 2 && t3.expired.back().roomDeleted && R.roomCount() == 0;
        JoinResult late = R.rejoin(70, c.code, V, 1, 1, c.secret, 101);
        snprintf(b, sizeof b, "B's place gone at 31 s; all away: A back hosts %d; last places run out -> room closed %d; rejoin then: %s",
                 (int)ba.hostChanged, (int)(R.roomCount() == 0), errorText(late.error));
        check(ok && late.error == ERR_NO_SESSION, "rooms: held places run out", b);
    }
    {   // the session cap: everyone told SESSION_WARN before, out at it
        Rooms R;
        R.sessionMax = 100; R.sessionWarn = 10;
        CreateResult c = R.create(10, V, 1, "A", 0, 1000);
        R.join(20, c.code, V, 1, "B");
        TickResult a1 = R.tick(1089), a2 = R.tick(1090), a3 = R.tick(1095), a4 = R.tick(1100);
        bool ok = a1.warn.empty() && a2.warn.size() == 1 && a2.warn[0].second.size() == 2 && a3.warn.empty() && a3.closed.empty() &&
                  a4.closed.size() == 1 && a4.closed[0].second.size() == 2 && R.roomCount() == 0 && R.playerCount() == 0;
        check(ok && netp::SESSION_MAX == 6 * 3600, "rooms: 6 h session cap", "warned 10 s before, closed at 100 s (test cap); default 6 h");
    }
    {   // wrong codes per address: 10 at once, then one every 3 s; other addresses unaffected; memory bounded
        TryLimit L;
        int tries = 0;
        for (int k = 0; k < 15; k++) if (L.allow(7, 0)) { tries++; L.failed(7, 0); }
        bool ok = tries == 10 && L.allow(8, 0) && !L.allow(7, 2.9) && L.allow(7, 3.1);
        TryLimit M; M.maxAddrs = 100;
        for (uint32_t ip = 1; ip <= 300; ip++) M.failed(ip, ip * 100.0);   // (old ones full again by the time they'd go)
        ok = ok && M.by.size() <= 100;
        snprintf(b, sizeof b, "%d of 15 at once, next after 3 s; 300 addresses remembered as %zu", tries, M.by.size());
        check(ok, "relay: wrong codes limited per address", b);
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
    // what came in of one type
    int count(uint8_t type) const { int n = 0; for (auto& g : got) n += peekType(g.data(), g.size()) == type; return n; }
    template <class M> bool last(M& m) const {
        for (size_t i = got.size(); i-- > 0;) if (unpack(got[i].data(), got[i].size(), m)) return true;
        return false;
    }
    ~RawClient() { if (peer) enet_peer_disconnect_now(peer, DISCONNECT_LEAVE); if (host) enet_host_destroy(host); }
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
    std::vector<uint8_t> fits(relay::MAX_PACKET, 0xEE), huge(relay::MAX_PACKET + 64 * 1024, 0xEE);   // unknown type: ignored if it arrives
    rc[1]->send(fits, CH_RELIABLE);
    rc[0]->send(huge, CH_RELIABLE);
    size_t peersBefore = rel.rooms.playerCount();
    double tDrop = pump([&] { return rc[0]->dropped; }, 20);
    bool freed = rel.oversizedDropped == 1 && rel.rooms.playerCount() == peersBefore - 1;
    // the relay still works for others: rc[1] (which sent a packet right at the limit) makes a session after all that
    MsgCreate j; j.name = "after";
    rc[1]->send(pack(j), CH_RELIABLE);
    double tJoin = pump([&] { return !rc[1]->got.empty(); }, 10);
    MsgCreated w;
    bool joined = tJoin >= 0 && unpack(rc[1]->got[0].data(), rc[1]->got[0].size(), w) && !rc[1]->dropped;
    snprintf(b, sizeof b, "%zu KB packet: sender dropped by the relay after %.2fs (its place freed %d); a %zu B one is fine and its sender then "
             "makes a session (%.2fs), players %zu", huge.size() / 1024, tDrop, (int)freed, relay::MAX_PACKET, tJoin, created ? rel.rooms.playerCount() : (size_t)0);
    check(created && tDrop >= 0 && tDrop < 1.0 && freed && joined && !rc[2]->dropped && relay::MAX_PACKET <= 66 * 1024, "limits: oversized packet refused", b);
    {   // the oversized check reads ENet's datagrams: a plain packet, fragments of one that fits, and the header flags
        std::vector<uint8_t> d = {0x80 | 0x00, 0x05, 0x12, 0x34};   // peer 5, sent time
        auto frag = [&](uint32_t total) {
            std::vector<uint8_t> f(sizeof(ENetProtocolSendFragment), 0);
            f[0] = ENET_PROTOCOL_COMMAND_SEND_FRAGMENT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
            f[offsetof(ENetProtocolSendFragment, dataLength) + 1] = 3;   // 3 bytes of data, big-endian
            for (int k = 0; k < 4; k++) f[offsetof(ENetProtocolSendFragment, totalLength) + k] = (uint8_t)(total >> (24 - 8 * k));
            f.insert(f.end(), {1, 2, 3});
            return f;
        };
        std::vector<uint8_t> ping(sizeof(ENetProtocolPing), 0);
        ping[0] = ENET_PROTOCOL_COMMAND_PING | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
        auto mk = [&](std::vector<std::vector<uint8_t>> cmds) { std::vector<uint8_t> v = d; for (auto& c : cmds) v.insert(v.end(), c.begin(), c.end()); return v; };
        uint16_t id = 0;
        auto ok1 = mk({ping, frag(1000)}), bad = mk({ping, frag(1000), frag(relay::MAX_PACKET + 1)}), cut = mk({ping, frag(relay::MAX_PACKET + 1)});
        cut.resize(cut.size() - 10);   // (cut short: not read past the end)
        bool ok = !relay::oversizedFragment(ok1.data(), ok1.size(), relay::MAX_PACKET, &id) && relay::oversizedFragment(bad.data(), bad.size(), relay::MAX_PACKET, &id) &&
                  id == 5 && !relay::oversizedFragment(cut.data(), cut.size(), relay::MAX_PACKET, &id);
        check(ok, "limits: oversized fragments found in datagrams", "a fitting fragment passes; one over MAX_PACKET after it is found (peer 5); a cut datagram isn't read past its end");
    }

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

    // a connection that never creates or joins is closed; sessions per address; wrong codes per address
    rc.clear();
    rel.stop();
    rel.idleDrop = 1.0;
    if (!rel.start(0)) { check(false, "limits: idle connections closed", "can't bind UDP"); return; }
    auto add = [&] { rc.push_back(std::make_unique<RawClient>()); rc.back()->start(rel.port()); return rc.back().get(); };
    RawClient* idle = add();
    RawClient* maker = add();
    pump([&] { return idle->connected && maker->connected; }, 10);
    MsgCreate mc; mc.name = "m";
    maker->send(pack(mc), CH_RELIABLE);
    double tIdle = pump([&] { return idle->dropped; }, 10);
    pump([&] { return false; }, 0.5);
    snprintf(b, sizeof b, "silent connection closed after %.2fs (limit %.0fs); one that made a session kept %d", tIdle, rel.idleDrop, (int)!maker->dropped);
    check(tIdle >= 0.9 && tIdle < 2.5 && !maker->dropped && maker->count(MSG_CREATED) == 1, "limits: idle connections closed", b);

    std::vector<RawClient*> makers{maker};
    for (int k = 0; k < relay::MAX_ROOMS_PER_IP; k++) makers.push_back(add());
    pump([&] { for (auto* m : makers) if (!m->connected) return false; return true; }, 10);
    for (size_t k = 1; k < makers.size(); k++) makers[k]->send(pack(mc), CH_RELIABLE);
    pump([&] { for (auto* m : makers) if (m->got.empty()) return false; return true; }, 10);
    int madeN = 0;
    MsgError over;
    for (auto* m : makers) madeN += m->count(MSG_CREATED);
    bool refusedRoom = makers.back()->last(over) && over.code == ERR_TOO_MANY_ROOMS;
    snprintf(b, sizeof b, "%zu creates from one address: %d sessions, then \"%s\"", makers.size(), madeN, refusedRoom ? over.text.c_str() : "?");
    check(madeN == relay::MAX_ROOMS_PER_IP && refusedRoom, "limits: sessions per address (live)", b);

    RawClient* guesser = add();
    pump([&] { return guesser->connected; }, 10);
    MsgCreated made0;
    unpack(maker->got[0].data(), maker->got[0].size(), made0);
    for (int k = 0; k < 14; k++) {
        MsgJoin j; j.code = std::string(5, 'Q') + CODE_ALPHABET[k]; j.name = "g";
        if (j.code == made0.code) continue;
        guesser->send(pack(j), CH_RELIABLE);
    }
    pump([&] { return guesser->count(MSG_ERROR) >= 13; }, 10);
    int noSess = 0, slow = 0;
    for (auto& g : guesser->got) { MsgError e; if (unpack(g.data(), g.size(), e)) { noSess += e.code == ERR_NO_SESSION; slow += e.code == ERR_SLOW_DOWN; } }
    // the right code from that address straight after: held back too (it's the address that's guessing)
    MsgJoin right; right.code = made0.code; right.name = "late";
    guesser->got.clear();
    guesser->send(pack(right), CH_RELIABLE);
    pump([&] { return !guesser->got.empty(); }, 10);
    MsgError e2;
    bool heldBack = guesser->last(e2) && e2.code == ERR_SLOW_DOWN;
    snprintf(b, sizeof b, "wrong codes: %d answered \"no session\", %d \"%s\"; the right one right after held back %d", noSess, slow, errorText(ERR_SLOW_DOWN), (int)heldBack);
    check(noSess == 10 && slow >= 3 && heldBack, "limits: wrong codes per address (live)", b);
    {   // a friend on that address gets back into their session all the same; a rejoin there tells nothing of codes
        MsgRejoin rj; rj.code = std::string(5, 'Q') + CODE_ALPHABET[0] == made0.code ? "ZZZZZZ" : std::string(5, 'Q') + CODE_ALPHABET[0];
        rj.playerId = 1; rj.secret = 12345;
        guesser->got.clear();
        guesser->send(pack(rj), CH_RELIABLE);
        pump([&] { return !guesser->got.empty(); }, 10);
        MsgError e3;
        bool noOracle = guesser->last(e3) && e3.code == ERR_REJOIN_EXPIRED;
        RawClient* fr = add();
        pump([&] { return fr->connected; }, 10);
        MsgRejoin mine; mine.code = made0.code; mine.playerId = made0.playerId; mine.secret = made0.secret;
        fr->send(pack(mine), CH_RELIABLE);   // (maker's place: its connection is taken over)
        pump([&] { return fr->count(MSG_WELCOME) || fr->count(MSG_ERROR); }, 10);
        MsgWelcome wb;
        bool back = fr->last(wb) && wb.playerId == made0.playerId;
        snprintf(b, sizeof b, "address out of tries: a rejoin to an unknown code says \"%s\"; the right rejoin gets player %u back %d",
                 errorText(e3.code), (unsigned)wb.playerId, (int)back);
        check(noOracle && back, "limits: rejoins on a guessing address", b);
    }

    // what's sent on counts once per player it goes to: a client flooding a room of 4 with states gets cut off at its
    // send-on budget (10 KB/s here); each car hit and weather change still goes through a flood of dodges
    rc.clear();
    rel.stop();
    rel.idleDrop = relay::IDLE_DROP;
    rel.fanoutBytes = 10 * 1024; rel.fanoutBurst = 10 * 1024;
    if (!rel.start(0)) { check(false, "limits: send-on budget", "can't bind UDP"); return; }
    RawClient* flooder = add();
    std::vector<RawClient*> ears{add(), add(), add()};
    pump([&] { for (auto& c : rc) if (!c->connected) return false; return true; }, 10);
    flooder->send(pack(mc), CH_RELIABLE);
    pump([&] { return flooder->count(MSG_CREATED) == 1; }, 10);
    MsgCreated room;
    flooder->last(room);
    for (auto* e : ears) { MsgJoin j; j.code = room.code; j.name = "ear"; e->send(pack(j), CH_RELIABLE); }
    pump([&] { for (auto* e : ears) if (!e->count(MSG_WELCOME)) return false; return true; }, 10);
    for (auto* e : ears) e->got.clear();
    double t0 = nowSec();
    MsgPlayerState ps;
    for (int k = 0; k < 400; k++) flooder->send(pack(ps, room.playerId, (uint16_t)k), CH_RELIABLE);
    pump([&] { return false; }, 1.0);
    double el = nowSec() - t0;
    int least = 1 << 30, most = 0;
    for (auto* e : ears) { least = std::min(least, e->count(MSG_PLAYER_STATE)); most = std::max(most, e->count(MSG_PLAYER_STATE)); }
    size_t per = (pack(ps).size() + 28) * 3;
    int budget = (int)((rel.fanoutBurst + rel.fanoutBytes * el) / per);
    snprintf(b, sizeof b, "400 states at once to 3 others (%zu B each to send on): %d-%d got through, budget for %.1fs ~%d", per, least, most, el, budget);
    check(least > 10 && most <= budget + 2, "limits: send-on budget counts every copy", b);
    for (auto* e : ears) e->got.clear();
    MsgEvent dodge; dodge.kind = EVT_PED_EVADE; dodge.idx = 1; dodge.evade = 1; dodge.dirX = 1; dodge.dirZ = 0; dodge.dist = 1;
    MsgEvent hit; hit.kind = EVT_CAR_HIT; hit.idx = 5; hit.simTime = 1; hit.car = {1};
    MsgWorldSettings ws; ws.preset = 1;
    rel.fanoutBytes = rel.fanoutBurst = 1e9;   // (only the event buckets now)
    pump([&] { return false; }, 0.2);
    for (int k = 0; k < 150; k++) flooder->send(pack(dodge, room.playerId, (uint16_t)(1000 + k)), CH_RELIABLE);
    flooder->send(pack(hit, room.playerId, 2000), CH_RELIABLE);
    flooder->send(pack(ws, room.playerId, 2001), CH_RELIABLE);
    pump([&] { return false; }, 0.5);
    int dodges = 0, hits = 0;
    for (auto& g : ears[0]->got) { MsgEvent m; if (unpack(g.data(), g.size(), m)) { dodges += m.kind == EVT_PED_EVADE; hits += m.kind == EVT_CAR_HIT; } }
    int sets = ears[0]->count(MSG_WORLD_SETTINGS);
    snprintf(b, sizeof b, "150 dodges then a car hit and a weather change: %d dodges, %d car hit, %d weather got through", dodges, hits, sets);
    check(dodges >= (int)relay::EVENT_BURST && dodges < 100 && hits == 1 && sets == 1, "limits: car hits and weather past a flood", b);

    // memory: a peer sending MAX_PACKET packets as fast as it can, while the relay is slow to look: what ENet holds for
    // it never gets past MAX_WAITING_DATA + one packet (was ~740 KB with whole full snapshots in one packet)
    rc.clear();
    rel.stop();
    rel.fanoutBytes = relay::FANOUT_RATE; rel.fanoutBurst = relay::FANOUT_BURST;
    if (!rel.start(0)) { check(false, "limits: memory a peer can tie up", "can't bind UDP"); return; }
    RawClient* big = add();
    pump([&] { return big->connected; }, 10);
    big->send(pack(mc), CH_RELIABLE);
    pump([&] { return big->count(MSG_CREATED) == 1; }, 10);
    rel.maxWaitingSeen = 0;
    std::vector<uint8_t> blob(relay::MAX_PACKET, 0xEE);   // (an unknown type: dropped once it's in)
    for (int round = 0; round < 6; round++) {
        for (int k = 0; k < 8; k++) big->send(blob, k % 2 ? CH_STATE : CH_RELIABLE);
        double tw = nowSec();
        while (nowSec() - tw < 0.15) { big->service(); }   // (the relay not looking meanwhile)
        pump([&] { return false; }, 0.1);
    }
    pump([&] { return false; }, 1.0);
    size_t bound = relay::MAX_WAITING_DATA + relay::MAX_PACKET;
    snprintf(b, sizeof b, "48 x %zu KB packets in bursts: at most %zu KB held for the peer (bound %zu KB); still connected %d", relay::MAX_PACKET / 1024,
             rel.maxWaitingSeen / 1024, bound / 1024, (int)!big->dropped);
    check(rel.maxWaitingSeen > 0 && rel.maxWaitingSeen <= bound && bound <= 200 * 1024, "limits: memory a peer can tie up", b);
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

    {   // shared traffic. A full snapshot (~220 KB) gets from the host to B only once B has asked for one; car snapshots
        // go from the host to everyone; a car or full snapshot from anyone else is dropped by the relay (a bare client in
        // the room, which takes whatever the relay sends it, listens)
        auto rcp = std::make_unique<RawClient>();
        RawClient& rc = *rcp;
        raw = &rc;
        rc.start(rel.port());
        pump([&] { return rc.connected; }, 10);
        MsgJoin j; j.code = A.code; j.fingerprint = 42; j.name = "Ear";
        rc.send(pack(j), CH_RELIABLE);
        MsgWelcome w;
        pump([&] { return !rc.got.empty() && A.remotes.size() == 2 && B.remotes.size() == 2; }, 10);
        bool joined = !rc.got.empty() && unpack(rc.got[0].data(), rc.got[0].size(), w);
        auto heard = [&](uint8_t type) {
            for (auto& g : rc.got) if (peekType(g.data(), g.size()) == type) return true;
            return false;
        };
        std::vector<uint8_t> data(220 * 1024);
        for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)(i * 31 + 7 + (i >> 9));
        // unasked: dropped
        A.sendFull(B.myId, data, nowSec());
        pump([&] { return false; }, 0.5);
        bool unasked = B.fullInbox.empty();
        double t0 = nowSec();
        B.requestFull(t0);
        double tReq = pump([&] { return !A.fullRequests.empty(); }, 10);
        bool asked = tReq >= 0 && A.fullRequests.size() == 1 && A.fullRequests[0] == B.myId;
        A.fullRequests.clear();
        A.sendFull(B.myId, data, nowSec());
        double tFull = pump([&] { return !B.fullInbox.empty(); }, 15);
        bool full = tFull >= 0 && B.fullInbox.size() == 1 && B.fullInbox[0].data == data && !heard(MSG_FULL_SNAPSHOT);
        B.fullInbox.clear();
        A.sendFull(B.myId, data, nowSec());   // a second one, not asked for again: dropped
        pump([&] { return false; }, 0.5);
        bool once = B.fullInbox.empty();
        B.requestFull(nowSec());   // asking again straight away: the relay holds it back (one ask in FULL_ASK_EVERY)
        pump([&] { return false; }, 0.5);
        bool throttled = A.fullRequests.empty();
        // car snapshots: a second bare client's (a non-host) never gets to anyone; the host's reaches both others
        auto rc2p = std::make_unique<RawClient>();
        RawClient& rc2 = *rc2p;
        rc2.start(rel.port());
        pump([&] { rc2.service(); return rc2.connected; }, 10);
        MsgJoin j2 = j; j2.name = "Mal";
        rc2.send(pack(j2), CH_RELIABLE);
        MsgWelcome w2;
        pump([&] { rc2.service(); return !rc2.got.empty() && A.remotes.size() == 3; }, 10);
        joined = joined && !rc2.got.empty() && unpack(rc2.got[0].data(), rc2.got[0].size(), w2);
        MsgCarSnapshot cs; cs.simTime = 1234.5; cs.trains = {1, 2, 3}; cs.cars.assign(1000, 9);
        rc2.send(pack(cs, w2.playerId, 1), CH_STATE);
        MsgFullSnapshot fs; fs.to = w.playerId; fs.data = {1, 2, 3};
        rc2.send(pack(fs, w2.playerId, 2), CH_RELIABLE);
        MsgFullRequest fr;
        rc.send(pack(fr, w.playerId, 3), CH_RELIABLE);   // (the listener is even waiting for a full snapshot)
        pump([&] { rc2.service(); return false; }, 0.5);
        bool nonHost = B.carInbox.empty() && B.fullInbox.empty() && !heard(MSG_CAR_SNAPSHOT) && !heard(MSG_FULL_SNAPSHOT);
        A.fullRequests.clear();
        cs.simTime = 99.25;
        A.sendCarSnapshot(cs, nowSec());
        double tCar = pump([&] { return !B.carInbox.empty() && heard(MSG_CAR_SNAPSHOT); }, 10);
        bool car = tCar >= 0 && B.carInbox.size() == 1 && B.carInbox[0].m.simTime == 99.25 && B.carInbox[0].m.cars == cs.cars &&
                   B.carInbox[0].m.trains == cs.trains && B.carInbox[0].recvAt > 0;
        snprintf(b, sizeof b, "unasked full dropped %d; asked after %.2fs, %zu KB full arrived intact after %.2fs, a second one dropped %d, asking again at once held back %d; "
                 "non-host car/full snapshots dropped %d; host's car snapshot to both after %.2fs",
                 (int)unasked, tReq, data.size() / 1024, tFull, (int)once, (int)throttled, (int)nonHost, tCar);
        check(joined && unasked && asked && full && once && throttled && nonHost && car, "live: car and full snapshots", b);
        B.carInbox.clear();
        raw = nullptr;
        rcp.reset();
        rc2p.reset();
        double tGone = pump([&] { return A.remotes.size() == 1 && B.remotes.size() == 1; }, 10);
        check(tGone >= 0, "live: bare clients leave", "rosters back to 1");
        A.log.clear(); B.log.clear();
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
    {   // the session's time of day and weather: the host's go to everyone; B's key press is a request the host takes and
        // sends on; a bare client's goes to the host only (never straight to the others); someone joining gets the
        // current ones. Then events: to everyone else, stamped with the sender; bad ones and floods dropped.
        auto rcp = std::make_unique<RawClient>(), lp = std::make_unique<RawClient>();
        RawClient& rc = *rcp;   // a bare client that sends
        RawClient& ear = *lp;   // and one that listens
        raw = &rc;
        rc.start(rel.port());
        pump([&] { return rc.connected; }, 10);
        MsgJoin j; j.code = A.code; j.fingerprint = 42; j.name = "Raw";
        rc.send(pack(j), CH_RELIABLE);
        MsgWelcome w;
        pump([&] { return !rc.got.empty() && A.remotes.size() == 2; }, 10);
        bool joined = !rc.got.empty() && unpack(rc.got[0].data(), rc.got[0].size(), w);
        A.worldIn = B.worldIn = false;
        MsgWorldSettings night; night.preset = 2; night.flags = MsgWorldSettings::F_RAIN;
        A.setWorld(night, nowSec());
        double t1 = pump([&] { return B.worldIn; }, 10);
        bool hostSent = t1 >= 0 && B.world == night && B.worldFrom == A.myId && A.world == night && !A.worldIn;
        B.worldIn = false;
        MsgWorldSettings fog; fog.preset = 1; fog.flags = MsgWorldSettings::F_FOG;
        B.setWorld(fog, nowSec());   // (not the host: a request)
        bool notYet = B.world == night;
        double t2 = pump([&] { return A.worldIn && B.worldIn; }, 10);
        bool request = t2 >= 0 && notYet && A.world == fog && A.worldFrom == B.myId && B.world == fog && B.worldFrom == A.myId;
        A.worldIn = B.worldIn = false;
        // the listener joins: it gets the session's settings from the host, unasked
        ear.start(rel.port());
        pump([&] { rc.service(); ear.service(); return ear.connected; }, 10);
        MsgJoin j2 = j; j2.name = "Ear";
        ear.send(pack(j2), CH_RELIABLE);
        auto settingsIn = [](RawClient& c, uint8_t from, MsgWorldSettings* out) {
            for (auto& g : c.got) {
                MsgWorldSettings m; Header h;
                if (unpack(g.data(), g.size(), m, &h) && (from == 0 || h.from == from)) { if (out) *out = m; return true; }
            }
            return false;
        };
        MsgWorldSettings got;
        double t3 = pump([&] { ear.service(); return settingsIn(ear, A.myId, &got); }, 10);
        bool joiner = t3 >= 0 && got == fog;
        // the bare client's request (forged as the host's, even): only the host gets it, and sends it on as its own
        ear.got.clear();
        MsgWorldSettings day; day.preset = 0;
        rc.send(pack(day, A.myId, 1), CH_RELIABLE);
        double t4 = pump([&] { ear.service(); return A.worldIn && B.worldIn; }, 10);
        pump([&] { ear.service(); return false; }, 0.3);
        bool direct = false;
        for (auto& g : ear.got) { Header h; MsgWorldSettings m; direct |= unpack(g.data(), g.size(), m, &h) && h.from != A.myId; }
        bool viaHost = t4 >= 0 && A.worldFrom == w.playerId && B.world == day && B.worldFrom == A.myId && settingsIn(ear, A.myId, nullptr) && !direct;
        // a bad one (preset 3) goes nowhere
        A.worldIn = B.worldIn = false;
        auto bad = pack(day, w.playerId, 2);
        bad[HEADER_SIZE] = 3;
        rc.send(bad, CH_RELIABLE);
        pump([&] { ear.service(); return false; }, 0.3);
        bool badDropped = !A.worldIn && !B.worldIn;
        snprintf(b, sizeof b, "host's to B after %.2fs %d; B's request taken and back after %.2fs %d; joiner given them %d; a bare client's "
                 "to the host only, sent on by it %d; bad one dropped %d", t1, (int)hostSent, t2, (int)request, (int)joiner, (int)viaHost, (int)badDropped);
        check(joined && hostSent && request && joiner && viaHost && badDropped, "live: world settings", b);

        // events
        A.eventInbox.clear(); B.eventInbox.clear(); ear.got.clear();
        MsgEvent pe; pe.kind = EVT_PED_EVADE; pe.idx = 1234; pe.evade = 3; pe.dirX = 0.6f; pe.dirZ = -0.8f; pe.dist = 2.5f;
        B.sendEvent(pe, nowSec());
        MsgEvent hit; hit.kind = EVT_CAR_HIT; hit.idx = 77; hit.simTime = 12.5; hit.car = {1, 2, 3, 4};
        A.sendEvent(hit, nowSec());
        auto evIn = [](RawClient& c) { int n = 0; for (auto& g : c.got) n += peekType(g.data(), g.size()) == MSG_EVENT; return n; };
        double t5 = pump([&] { ear.service(); return !A.eventInbox.empty() && !B.eventInbox.empty() && evIn(ear) == 2; }, 10);
        bool fwd = t5 >= 0 && A.eventInbox.size() == 1 && A.eventInbox[0].from == B.myId && A.eventInbox[0].m.idx == 1234 && A.eventInbox[0].m.evade == 3 &&
                   A.eventInbox[0].m.dirZ == -0.8f && B.eventInbox.size() == 1 && B.eventInbox[0].from == A.myId && B.eventInbox[0].m.car == hit.car &&
                   B.eventInbox[0].m.simTime == 12.5;
        // bad ones (a NaN direction, an unknown kind) and a flood of good ones from the bare client
        A.eventInbox.clear(); B.eventInbox.clear();
        MsgEvent nan = pe; nan.dirX = NAN;
        rc.send(pack(nan, w.playerId, 3), CH_RELIABLE);
        auto unk = pack(pe, w.playerId, 4);
        unk[HEADER_SIZE] = EVT_COUNT;
        rc.send(unk, CH_RELIABLE);
        pump([&] { ear.service(); return false; }, 0.3);
        bool badEv = A.eventInbox.empty() && B.eventInbox.empty();
        for (int k = 0; k < 300; k++) rc.send(pack(pe, w.playerId, (uint16_t)(10 + k)), CH_RELIABLE);
        pump([&] { ear.service(); return false; }, 1.0);
        size_t flood = B.eventInbox.size();
        bool limited = flood >= (size_t)relay::EVENT_BURST && flood < (size_t)relay::EVENT_BURST + 40;
        snprintf(b, sizeof b, "a dodge from B and a car hit from A to everyone else after %.2fs %d; bad ones dropped %d; 300 at once: %zu got through",
                 t5, (int)fwd, (int)badEv, flood);
        check(fwd && badEv && limited, "live: events", b);
        A.eventInbox.clear(); B.eventInbox.clear();
        raw = nullptr;
        rcp.reset();
        lp.reset();
        pump([&] { return A.remotes.size() == 1 && B.remotes.size() == 1; }, 10);
        A.log.clear(); B.log.clear();
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
        for (double tt = t0; tt < t0 + Net::CONNECT_TIMEOUT + 1 && F.status != Net::FAILED; tt += 0.1) F.service(tt);   // (frames of 0.1 s)
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

// The session over a real relay, from start to leave: a relay looked up by name, dropped connections coming back (or
// not), the host dropping, a joiner that stalls, leaving and joining another session in one run, the session cap. The
// relay notices a dropped connection in ~1 s here (5-10 s normally).
static void sessionChecks() {
    char b[480];
    relay::Relay rel;
    rel.log = getenv("NET_TEST_LOG") != nullptr;
    rel.timeoutMinMs = 500; rel.timeoutMaxMs = 1000;
    rel.rooms.maxRoomsPerIp = 100;
    rel.tries.burst = 1000;
    if (!rel.start(0)) { check(false, "session: relay starts", "can't bind UDP"); return; }
    const std::string port = std::to_string(rel.port()), addr = "127.0.0.1:" + port;
    std::vector<Net*> nets;
    std::set<Net*> frozen;   // (not serviced: a game stuck in a long frame)
    PlayerState st;
    auto pump = [&](std::function<bool()> cond, double timeout) -> double {
        double t0 = nowSec();
        while (nowSec() - t0 < timeout) {
            rel.service(1);
            double now = nowSec();
            for (Net* n : nets) {
                if (frozen.count(n)) continue;
                n->service(now);
                if (n->stateDue(now)) n->sendState(st, now);
            }
            if (cond()) return nowSec() - t0;
        }
        return -1;
    };
    auto cfg = [&](const char* name, bool create, const std::string& code = "") {
        NetConfig c; c.relay = addr; c.name = name; c.create = create; c.joinCode = code; c.fingerprint = 42;
        return c;
    };
    auto logged = [](const Net& n, const std::string& line) { for (auto& l : n.log) if (l == line) return true; return false; };
    auto inRoom = [&](Net& n) { return pump([&] { return n.status == Net::IN_SESSION || n.status == Net::FAILED; }, 10) >= 0 && n.status == Net::IN_SESSION; };

    // a relay given by name is looked up on a worker thread: start() returns at once either way
    Net A;
    nets.push_back(&A);
    NetConfig ca = cfg("Ana", true);
    ca.relay = "localhost:" + port;
    double t0 = nowSec();
    bool started = A.start(ca, t0);
    double took = nowSec() - t0;
    bool aIn = inRoom(A);
    Net bad;
    nets.push_back(&bad);
    NetConfig cbad = cfg("x", true);
    cbad.relay = "no-such-relay.invalid:7777";
    t0 = nowSec();
    bool started2 = bad.start(cbad, t0);
    double took2 = nowSec() - t0;
    double tBad = pump([&] { return bad.status == Net::FAILED; }, Net::CONNECT_TIMEOUT + 3);
    nets.pop_back();
    snprintf(b, sizeof b, "\"localhost\": start() took %.2f ms, in session %d (%s); unknown name: start() took %.2f ms, \"%s\" after %.1fs",
             took * 1000, (int)aIn, A.code.c_str(), took2 * 1000, bad.error.c_str(), tBad);
    check(started && took < 0.02 && aIn && started2 && took2 < 0.02 && tBad >= 0 && bad.error.rfind("Can't find relay", 0) == 0,
          "session: relay looked up without blocking", b);
    if (!aIn) return;

    // B drops (the relay isn't told): A sees B "reconnecting", then B is back with the same id, and can take a full snapshot
    Net B;
    B.firstRetry = 2.0;   // (so the relay has noticed first)
    nets.push_back(&B);
    B.start(cfg("Bo", false, A.code), nowSec());
    bool bIn = inRoom(B) && pump([&] { return A.remotes.size() == 1; }, 5) >= 0;
    uint8_t bId = B.myId;
    uint32_t bSess = B.session;
    A.log.clear();
    B.dropConnection();
    bool reconnecting = B.status == Net::RECONNECTING && B.inSession();
    double tAway = pump([&] { return A.remote(bId) && A.remote(bId)->away; }, 10);
    double tBack = pump([&] { return B.status == Net::IN_SESSION && A.remote(bId) && !A.remote(bId)->away; }, 15);
    bool same = B.myId == bId && B.session == bSess && B.code == A.code && B.rejoined && B.hostId == A.myId && A.remotes.size() == 1 &&
                logged(A, "Bo lost connection") && logged(A, "Bo is back");
    B.rejoined = false;
    std::vector<uint8_t> data(150 * 1024);
    for (size_t i = 0; i < data.size(); i++) data[i] = (uint8_t)(i ^ (i >> 8));
    B.requestFull(nowSec());
    double tReq = pump([&] { return !A.fullRequests.empty(); }, 5);
    A.fullRequests.clear();
    A.sendFull(bId, data, nowSec());
    double tFull = pump([&] { return !B.fullInbox.empty(); }, 10);
    bool fullOk = tFull >= 0 && B.fullInbox[0].data == data;
    B.fullInbox.clear();
    snprintf(b, sizeof b, "B dropped: reconnecting %d; A sees it away after %.2fs, back after %.2fs more, id %u kept %d; then a %zu KB full snapshot "
             "(asked after %.2fs) arrived whole %d", (int)reconnecting, tAway, tBack, (unsigned)bId, (int)same, data.size() / 1024, tReq, (int)fullOk);
    check(bIn && reconnecting && tAway >= 0 && tBack >= 0 && same && fullOk, "session: reconnect keeps the player", b);
    {   // the weather changes while B is away (the relay passes it only to those there): B gets it on coming back
        MsgWorldSettings w0; w0.preset = 0;
        A.setWorld(w0, nowSec());
        pump([&] { return B.worldSet && B.world == w0; }, 5);
        B.dropConnection();
        pump([&] { return A.remote(bId) && A.remote(bId)->away; }, 10);
        MsgWorldSettings w1; w1.preset = 2; w1.flags = MsgWorldSettings::F_RAIN;
        A.setWorld(w1, nowSec());
        B.setWorld(w0, nowSec());   // (pressed while reconnecting: goes nowhere)
        bool stale = B.world == w0;
        B.worldIn = false;
        double tW = pump([&] { return B.status == Net::IN_SESSION && B.worldIn && B.world == w1; }, 15);
        pump([&] { return false; }, 0.3);
        snprintf(b, sizeof b, "night and rain set while B was away: B had the old ones %d, has the new ones %.2fs after dropping; A's still %d",
                 (int)stale, tW, (int)(A.world == w1));
        check(stale && tW >= 0 && A.world == w1 && B.world == w1, "session: weather changed while away reaches the player", b);
        B.rejoined = false;
    }

    // the host drops: B hosts as soon as the relay notices (not after the 30 s hold), and A comes back as a member
    uint8_t aId = A.myId;
    A.firstRetry = 3.0;
    B.log.clear();
    A.dropConnection();
    double tHost = pump([&] { return B.isHost(); }, 10);
    bool aStillOut = A.status == Net::RECONNECTING;
    double tAback = pump([&] { return A.status == Net::IN_SESSION; }, 15);
    pump([&] { return B.remote(aId) && !B.remote(aId)->away; }, 5);
    const relay::Room* room = rel.rooms.room(B.code);
    bool member = A.myId == aId && !A.isHost() && A.hostId == B.myId && B.isHost() && room && room->host == B.myId && logged(B, "You are the host now");
    snprintf(b, sizeof b, "host A dropped: B hosts after %.2fs (A still reconnecting %d); A back after %.2fs more as player %u, a member %d",
             tHost, (int)aStillOut, tAback, (unsigned)A.myId, (int)member);
    check(tHost >= 0 && tHost < 3 && aStillOut && tAback >= 0 && member, "session: host drops, hands over at once", b);

    // a joiner that stalls right after joining (a slow first frame, a long load): the relay drops it, and it gets back
    // in on its own with the same id; a short stall changes nothing
    Net C;
    nets.push_back(&C);
    C.start(cfg("Cy", false, A.code), nowSec());
    bool cIn = inRoom(C);
    uint8_t cId = C.myId;
    frozen.insert(&C);
    pump([&] { return false; }, 0.3);
    frozen.clear();
    pump([&] { return false; }, 1.0);
    bool shortOk = C.status == Net::IN_SESSION && !logged(C, "Connection lost, reconnecting");
    frozen.insert(&C);
    double tFrozen = pump([&] { return B.remote(cId) && B.remote(cId)->away; }, 6);
    pump([&] { return false; }, 0.5);
    frozen.clear();
    double tCback = pump([&] { return C.status == Net::IN_SESSION && logged(C, "Reconnected") && B.remote(cId) && !B.remote(cId)->away; }, 15);
    snprintf(b, sizeof b, "0.3 s stall: nothing happens %d; a %.1f s stall: the relay drops it, and it's back by itself %.2fs later as id %u (was %u)",
             (int)shortOk, tFrozen + 0.5, tCback, (unsigned)C.myId, (unsigned)cId);
    check(cIn && shortOk && tFrozen >= 0 && tCback >= 0 && C.myId == cId, "session: a stalled joiner gets back in", b);

    // gone longer than the hold: the place is gone (LEFT for the others), and the rejoin says so
    rel.rooms.holdTime = 1.5;
    Net D;
    D.firstRetry = 5.0;
    nets.push_back(&D);
    D.start(cfg("Di", false, A.code), nowSec());
    bool dIn = inRoom(D);
    B.log.clear();
    D.dropConnection();
    double tExp = pump([&] { return D.status == Net::FAILED; }, 15);
    bool leftShown = logged(B, "Di lost connection") && logged(B, "Di left");
    // everyone gone: the session ends, and a rejoin says that instead
    Net X;
    X.firstRetry = 4.0;
    nets.push_back(&X);
    X.start(cfg("Xi", true), nowSec());
    bool xIn = inRoom(X);
    std::string xCode = X.code;
    X.dropConnection();
    double tEnd = pump([&] { return X.status == Net::FAILED; }, 15);
    rel.rooms.holdTime = netp::REJOIN_HOLD;
    snprintf(b, sizeof b, "back after the hold: \"%s\" after %.1fs (others saw lost connection, then left: %d); alone and back too late: \"%s\", room %s gone %d",
             D.error.c_str(), tExp, (int)leftShown, X.error.c_str(), xCode.c_str(), (int)!rel.rooms.room(xCode));
    check(dIn && tExp >= 0 && D.error == errorText(ERR_REJOIN_EXPIRED) && leftShown && xIn && tEnd >= 0 && X.error == "The session has ended" && !rel.rooms.room(xCode),
          "session: too late to rejoin", b);

    // leave, then another session in the same run: Y makes one (Z joins it), leaves, and joins A's; nothing of the first
    // stays, and the others see LEFT at once
    Net Y, Z;
    nets.push_back(&Y); nets.push_back(&Z);
    Y.start(cfg("Yu", true), nowSec());
    bool yIn = inRoom(Y);
    std::string y1 = Y.code;
    Z.start(cfg("Zed", false, y1), nowSec());
    bool zIn = inRoom(Z);
    pump([&] { return !Y.inbox.empty() && Y.remotes.size() == 1 && !Z.inbox.empty(); }, 5);
    MsgWorldSettings w; w.preset = 2;
    Y.setWorld(w, nowSec());
    pump([&] { return Z.worldSet; }, 5);
    uint32_t s1 = Y.session;
    Z.log.clear();
    t0 = nowSec();
    Y.leave();
    bool clean = Y.status == Net::OFF && Y.remotes.empty() && Y.inbox.empty() && Y.code.empty() && !Y.worldSet && Y.myId == 0 && !Y.inSession();
    double tLeft = pump([&] { return Z.remotes.empty(); }, 5);
    bool zHosts = Z.isHost() && logged(Z, "Yu left");
    Y.start(cfg("Yu", false, A.code), nowSec());
    bool y2 = inRoom(Y) && pump([&] { return Y.remotes.size() == 3; }, 5) >= 0;
    std::set<std::string> names;
    for (auto& r : Y.remotes) names.insert(r.name);
    bool other = Y.code == A.code && Y.session == s1 + 1 && names == std::set<std::string>{"Ana", "Bo", "Cy"} && rel.rooms.room(y1) && rel.rooms.room(y1)->members.size() == 1;
    snprintf(b, sizeof b, "Y left %s (cleared %d): Z saw it after %.3fs and hosts %d; Y then in %s (session %u -> %u) with %zu others %d",
             y1.c_str(), (int)clean, tLeft, (int)zHosts, Y.code.c_str(), s1, Y.session, Y.remotes.size(), (int)other);
    check(yIn && zIn && clean && tLeft >= 0 && tLeft < 0.3 && zHosts && y2 && other, "session: leave, then join another", b);

    // the session cap (2 s here): everyone is told before, then it ends with a clear error
    relay::Relay capped;
    capped.log = rel.log;
    capped.rooms.sessionMax = 2.0; capped.rooms.sessionWarn = 1.0;
    if (!capped.start(0)) { check(false, "session: time limit", "can't bind UDP"); return; }
    Net S;
    NetConfig cs = cfg("Sam", true);
    cs.relay = "127.0.0.1:" + std::to_string(capped.port());
    S.start(cs, nowSec());
    double tCap = -1, warnAt = -1, endsIn = -1;
    t0 = nowSec();
    while (nowSec() - t0 < 6 && S.status != Net::FAILED) {
        capped.service(1);
        S.service(nowSec());
        if (warnAt < 0 && S.endsIn > 0) { warnAt = nowSec() - t0; endsIn = S.endsIn; }
    }
    if (S.status == Net::FAILED) tCap = nowSec() - t0;
    snprintf(b, sizeof b, "warned after %.2fs (closes in %.0f s), ended after %.2fs: \"%s\"", warnAt, endsIn, tCap, S.error.c_str());
    check(warnAt > 0.5 && tCap > warnAt && S.error == errorText(ERR_SESSION_OVER), "session: time limit", b);
    S.leave();

    for (Net* n : nets) n->leave();
    pump([&] { return rel.rooms.roomCount() == 0; }, 5);
}

int main() {
    protocolChecks();
    roomChecks();
    liveChecks();
    sessionChecks();
    limitChecks();
    printf("\n%s (%d failed)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
