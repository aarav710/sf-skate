// Multiplayer wire format: message types and their packed little-endian encodings. Shared by the game
// (net.cpp) and the relay (server/), so it has no raylib or ENet dependency.
//
// Every message starts with the same 8-byte header. CREATE and JOIN put the protocol version right after it,
// and the header and ERROR layouts never change, so a relay can always read the version of an older or newer
// client and answer with an ERROR it understands.
#pragma once
#include "bytes.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace netp {

constexpr uint16_t PROTOCOL_VERSION = 1;
// Bump when gameplay or world generation changes, so mismatched builds are refused even if the world counts happen
// to agree (it goes into the world fingerprint, net_game.cpp). PROTOCOL_VERSION is for wire format changes.
constexpr uint32_t GAME_BUILD = 1;
constexpr int MAX_PLAYERS = 8;
constexpr int NAME_MAX = 16;          // bytes of UTF-8
constexpr int CODE_LEN = 6;
constexpr int ERROR_TEXT_MAX = 120;
// session codes: no 0/O or 1/I, so a code read out loud can't be mistyped
constexpr char CODE_ALPHABET[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
static_assert(sizeof(CODE_ALPHABET) == 33, "32 symbols");

// ENet channels
enum Channel : uint8_t { CH_RELIABLE = 0, CH_STATE = 1, CH_COUNT = 2 };

constexpr uint8_t FROM_RELAY = 0;     // header.from of messages the relay makes itself; player ids start at 1
constexpr uint8_t NO_PLAYER = 0;

enum MsgType : uint8_t {
    MSG_NONE = 0,
    MSG_CREATE = 1,        // client -> relay: make a session, I'm the host
    MSG_CREATED = 2,       // relay -> creator: code and your id
    MSG_JOIN = 3,          // client -> relay: join the session with this code
    MSG_WELCOME = 4,       // relay -> joiner: code, your id, host id, everyone already there
    MSG_PLAYER_JOINED = 5, // relay -> room: someone joined
    MSG_LEFT = 6,          // relay -> room: someone left (or timed out)
    MSG_HOST_CHANGED = 7,  // relay -> room: the host left, this player hosts now
    MSG_ERROR = 8,         // relay -> client: why a create/join failed
    MSG_PLAYER_STATE = 9,  // client -> relay -> room, 20 Hz, unreliable sequenced
    MSG_PING = 10,         // client -> relay, 1 Hz
    MSG_PONG = 11,         // relay -> client, echoes the ping
    // reserved for later stages (plan section 4); not encoded yet
    MSG_REJOIN = 12,
    MSG_CAR_SNAPSHOT = 13,
    MSG_FULL_SNAPSHOT = 14,
    MSG_WORLD_SETTINGS = 15,
    MSG_EVENT = 16,
    MSG_TYPE_COUNT
};

enum ErrorCode : uint8_t {
    ERR_NONE = 0,
    ERR_VERSION = 1,        // different protocol version
    ERR_FINGERPRINT = 2,    // different world (game build)
    ERR_ROOM_FULL = 3,
    ERR_NO_SESSION = 4,
    ERR_SERVER_FULL = 5,    // relay is at its room limit
    ERR_BAD_REQUEST = 6,    // malformed message, or create/join while already in a session
};

inline const char* errorText(uint8_t code) {
    switch (code) {
    case ERR_VERSION: return "Different game version";
    case ERR_FINGERPRINT: return "Different game version (world differs)";
    case ERR_ROOM_FULL: return "Session is full";
    case ERR_NO_SESSION: return "No session with that code";
    case ERR_SERVER_FULL: return "Server is full, try again later";
    case ERR_BAD_REQUEST: return "Bad request";
    default: return "Error";
    }
}

struct Header {
    uint8_t type = MSG_NONE;
    uint8_t from = 0;
    uint16_t seq = 0;
    float hostTime = 0;   // the sender's clock (session clock once Stage 3 syncs it)
};
constexpr size_t HEADER_SIZE = 8;
constexpr float MAX_HOST_TIME = 1e7f;   // seconds

inline void writeHeader(ByteWriter& w, const Header& h) { w.u8(h.type); w.u8(h.from); w.u16(h.seq); w.f32(h.hostTime); }
inline Header readHeader(ByteReader& r) {
    Header h;
    h.type = r.u8(); h.from = r.u8(); h.seq = r.u16(); h.hostTime = r.f32();
    // a clock is finite and sane (~4 months of seconds): a NaN would poison the receiver's clock offset for good
    if (!std::isfinite(h.hostTime) || std::fabs(h.hostTime) > MAX_HOST_TIME) r.bad = true;
    return h;
}

// ---------------------------------------------------------------- strings

// Keeps a player name to at most NAME_MAX bytes of valid UTF-8 with no control characters, cut on a character
// boundary, spaces trimmed; empty becomes "skater".
inline std::string sanitizeName(const std::string& in) {
    std::string out;
    size_t i = 0, n = in.size();
    while (i < n) {
        uint8_t c = (uint8_t)in[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool ok = len > 0 && i + len <= n;
        for (int k = 1; ok && k < len; k++) ok = ((uint8_t)in[i + k] >> 6) == 2;
        if (ok && len == 1 && (c < 0x20 || c == 0x7f)) ok = false;
        if (ok && len == 2 && c < 0xC2) ok = false;   // overlong
        if (ok && len == 3) {   // overlong or surrogate
            uint8_t c1 = (uint8_t)in[i + 1];
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false;
        }
        if (ok && len == 4) {   // overlong or past U+10FFFF
            uint8_t c1 = (uint8_t)in[i + 1];
            if ((c == 0xF0 && c1 < 0x90) || c > 0xF4 || (c == 0xF4 && c1 >= 0x90)) ok = false;
        }
        if (!ok) { i++; continue; }   // drop the bad byte
        if (out.size() + len > (size_t)NAME_MAX) break;
        out.append(in, i, len);
        i += len;
    }
    size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    out = a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
    return out.empty() ? std::string("skater") : out;
}

// Upper-cases a typed code and drops spaces and dashes; returns "" unless it is CODE_LEN alphabet symbols.
inline std::string normalizeCode(const std::string& in) {
    std::string out;
    for (char ch : in) {
        if (ch == ' ' || ch == '-') continue;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        bool ok = false;
        for (int k = 0; k < 32; k++) ok |= CODE_ALPHABET[k] == ch;
        if (!ok) return "";
        out += ch;
    }
    return out.size() == (size_t)CODE_LEN ? out : "";
}

inline void writeStr(ByteWriter& w, const std::string& s, size_t maxLen) {
    size_t n = std::min(s.size(), maxLen);
    w.u8((uint8_t)n);
    for (size_t i = 0; i < n; i++) w.u8((uint8_t)s[i]);
}
inline std::string readStr(ByteReader& r, size_t maxLen) {
    size_t n = r.u8();
    if (n > maxLen || n > r.left()) { r.bad = true; return ""; }
    std::string s((const char*)r.p + r.at, n);
    r.at += n;
    return s;
}
inline void writeCode(ByteWriter& w, const std::string& code) {
    for (int i = 0; i < CODE_LEN; i++) w.u8(i < (int)code.size() ? (uint8_t)code[i] : (uint8_t)'?');
}
inline std::string readCode(ByteReader& r) {
    std::string s;
    for (int i = 0; i < CODE_LEN; i++) s += (char)r.u8();
    return s;
}

// ---------------------------------------------------------------- messages
// Each has encode(ByteWriter&) and decode(ByteReader&) for the payload after the header.

struct MsgCreate {
    static constexpr uint8_t TYPE = MSG_CREATE;
    uint16_t version = PROTOCOL_VERSION;
    uint32_t fingerprint = 0;
    std::string name;
    void encode(ByteWriter& w) const { w.u16(version); w.u32(fingerprint); writeStr(w, name, NAME_MAX); }
    void decode(ByteReader& r) { version = r.u16(); fingerprint = r.u32(); name = readStr(r, NAME_MAX); }
};

struct MsgCreated {
    static constexpr uint8_t TYPE = MSG_CREATED;
    std::string code;
    uint8_t playerId = 0;
    void encode(ByteWriter& w) const { writeCode(w, code); w.u8(playerId); }
    void decode(ByteReader& r) { code = readCode(r); playerId = r.u8(); }
};

struct MsgJoin {
    static constexpr uint8_t TYPE = MSG_JOIN;
    uint16_t version = PROTOCOL_VERSION;
    uint32_t fingerprint = 0;
    std::string code;
    std::string name;
    void encode(ByteWriter& w) const { w.u16(version); w.u32(fingerprint); writeCode(w, code); writeStr(w, name, NAME_MAX); }
    void decode(ByteReader& r) { version = r.u16(); fingerprint = r.u32(); code = readCode(r); name = readStr(r, NAME_MAX); }
};

struct RosterEntry {
    uint8_t id = 0;
    std::string name;
    bool operator==(const RosterEntry& o) const { return id == o.id && name == o.name; }
};

struct MsgWelcome {
    static constexpr uint8_t TYPE = MSG_WELCOME;
    std::string code;
    uint8_t playerId = 0, hostId = 0;
    std::vector<RosterEntry> roster;   // everyone else already in the session
    void encode(ByteWriter& w) const {
        writeCode(w, code); w.u8(playerId); w.u8(hostId);
        size_t n = std::min(roster.size(), (size_t)MAX_PLAYERS);
        w.u8((uint8_t)n);
        for (size_t i = 0; i < n; i++) { w.u8(roster[i].id); writeStr(w, roster[i].name, NAME_MAX); }
    }
    void decode(ByteReader& r) {
        code = readCode(r); playerId = r.u8(); hostId = r.u8();
        int n = r.u8();
        if (n > MAX_PLAYERS) { r.bad = true; return; }
        roster.resize(n);
        for (auto& e : roster) { e.id = r.u8(); e.name = readStr(r, NAME_MAX); }
    }
};

struct MsgPlayerJoined {
    static constexpr uint8_t TYPE = MSG_PLAYER_JOINED;
    uint8_t id = 0;
    std::string name;
    void encode(ByteWriter& w) const { w.u8(id); writeStr(w, name, NAME_MAX); }
    void decode(ByteReader& r) { id = r.u8(); name = readStr(r, NAME_MAX); }
};

struct MsgLeft {
    static constexpr uint8_t TYPE = MSG_LEFT;
    uint8_t id = 0;
    void encode(ByteWriter& w) const { w.u8(id); }
    void decode(ByteReader& r) { id = r.u8(); }
};

struct MsgHostChanged {
    static constexpr uint8_t TYPE = MSG_HOST_CHANGED;
    uint8_t id = 0;
    void encode(ByteWriter& w) const { w.u8(id); }
    void decode(ByteReader& r) { id = r.u8(); }
};

struct MsgError {
    static constexpr uint8_t TYPE = MSG_ERROR;
    uint8_t code = ERR_NONE;
    std::string text;
    void encode(ByteWriter& w) const { w.u8(code); writeStr(w, text, ERROR_TEXT_MAX); }
    void decode(ByteReader& r) { code = r.u8(); text = readStr(r, ERROR_TEXT_MAX); }
};

// What another client needs to place and pose a skater (Player::draw reads exactly these), quantized:
// 52 bytes. Velocity is world space (not board speed) so a receiver can extrapolate airborne skaters too.
struct PlayerState {
    enum Vehicle : uint8_t { ON_FOOT = 0, JETSKI = 1, SEAPLANE = 2, TRAIN = 3 };
    enum Flag : uint8_t { F_GRABBING = 1, F_BOUNCING = 2, F_SPLASHED = 4, F_BOARDSLIDE = 8, F_FAKIE = 16 };
    float x = 0, y = 0, z = 0;
    float yaw = 0;                    // radians
    float vx = 0, vy = 0, vz = 0;     // m/s, +-327
    uint8_t state = 0;                // Player::State
    uint8_t vehicle = ON_FOOT;
    uint8_t flags = 0;
    int rail = -1;                    // -1 or 0..65534
    float railT = 0;                  // 0..1
    float crouch = 0;                 // 0..1
    float lean = 0;                   // -1..1
    float pushPhase = 0;              // radians, sent mod 2 pi
    float pushAmt = 0;                // 0..1
    float pitch = 0;                  // radians, +-1.27
    float balance = 0;                // -1..1
    float flipAngle = 0, shoveAngle = 0;   // DEGREES, as Player keeps them (flip -360..360: the sign tells a heelflip; shove 0..180), 0.02 deg steps, +-655
    float flipProg = -1, shoveProg = -1;   // flipT / flipDur, or -1 when not flipping
    float bailT = 0;                  // seconds, 0..2.55
    float boardDx = 0, boardDy = 0, boardDz = 0;   // board position relative to the rider (bails), +-327 m
    float boardYaw = 0;
    uint32_t score = 0;

    static constexpr size_t SIZE = 52;

    static uint16_t angle16(float a) {
        float t = a / 6.2831853f;
        t -= floorf(t);
        return (uint16_t)((uint32_t)lrintf(t * 65536.f) & 0xFFFF);
    }
    static float unangle16(uint16_t v) { float a = v * (6.2831853f / 65536.f); return a > 3.14159265f ? a - 6.2831853f : a; }
    static int16_t fix16(float v, float scale) { return (int16_t)lrintf(std::fmax(-32767.f, std::fmin(32767.f, std::isfinite(v) ? v * scale : 0.f))); }
    static uint8_t unit8(float v) { return (uint8_t)lrintf(std::fmax(0.f, std::fmin(1.f, std::isfinite(v) ? v : 0.f)) * 255.f); }
    static int8_t sunit8(float v, float scale) { return (int8_t)lrintf(std::fmax(-127.f, std::fmin(127.f, std::isfinite(v) ? v * scale : 0.f))); }
    static uint8_t prog8(float p) { return p < 0 || !std::isfinite(p) ? 255 : (uint8_t)lrintf(std::fmin(p, 1.f) * 254.f); }
    static float unprog8(uint8_t v) { return v == 255 ? -1.f : v / 254.f; }
    static float fin(float v) { return std::isfinite(v) ? v : 0.f; }

    void encode(ByteWriter& w) const {
        w.f32(fin(x)); w.f32(fin(y)); w.f32(fin(z));
        w.u16(angle16(yaw));
        w.i16(fix16(vx, 100)); w.i16(fix16(vy, 100)); w.i16(fix16(vz, 100));
        w.u8(state); w.u8(vehicle); w.u8(flags);
        w.u16(rail < 0 || rail > 65534 ? 0xFFFF : (uint16_t)rail);
        w.u16((uint16_t)lrintf(std::fmax(0.f, std::fmin(1.f, fin(railT))) * 65535.f));
        w.u8(unit8(crouch)); w.u8((uint8_t)sunit8(lean, 127)); w.u8((uint8_t)(angle16(pushPhase) >> 8)); w.u8(unit8(pushAmt));
        w.u8((uint8_t)sunit8(pitch, 100)); w.u8((uint8_t)sunit8(balance, 127));
        w.i16(fix16(flipAngle, 50)); w.i16(fix16(shoveAngle, 50));
        w.u8(prog8(flipProg)); w.u8(prog8(shoveProg));
        w.u8((uint8_t)lrintf(std::fmax(0.f, std::fmin(2.55f, fin(bailT))) * 100.f));
        w.i16(fix16(boardDx, 100)); w.i16(fix16(boardDy, 100)); w.i16(fix16(boardDz, 100));
        w.u16(angle16(boardYaw));
        w.u32(score);
    }
    void decode(ByteReader& r) {
        x = r.f32(); y = r.f32(); z = r.f32();
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) r.bad = true;
        yaw = unangle16(r.u16());
        vx = r.i16() / 100.f; vy = r.i16() / 100.f; vz = r.i16() / 100.f;
        state = r.u8(); vehicle = r.u8(); flags = r.u8();
        if (state > 3 || vehicle > 3) r.bad = true;
        uint16_t rl = r.u16(); rail = rl == 0xFFFF ? -1 : rl;
        railT = r.u16() / 65535.f;
        crouch = r.u8() / 255.f; lean = (int8_t)r.u8() / 127.f; pushPhase = unangle16((uint16_t)(r.u8() << 8)); pushAmt = r.u8() / 255.f;
        pitch = (int8_t)r.u8() / 100.f; balance = (int8_t)r.u8() / 127.f;
        flipAngle = r.i16() / 50.f; shoveAngle = r.i16() / 50.f;
        flipProg = unprog8(r.u8()); shoveProg = unprog8(r.u8());
        bailT = r.u8() / 100.f;
        boardDx = r.i16() / 100.f; boardDy = r.i16() / 100.f; boardDz = r.i16() / 100.f;
        boardYaw = unangle16(r.u16());
        score = r.u32();
    }
};

struct MsgPlayerState {
    static constexpr uint8_t TYPE = MSG_PLAYER_STATE;
    PlayerState s;
    void encode(ByteWriter& w) const { s.encode(w); }
    void decode(ByteReader& r) { s.decode(r); }
};

struct MsgPing {
    static constexpr uint8_t TYPE = MSG_PING;
    double sentAt = 0;   // the pinger's clock, echoed back in PONG
    void encode(ByteWriter& w) const { w.f64(sentAt); }
    void decode(ByteReader& r) { sentAt = r.f64(); }
};

struct MsgPong {
    static constexpr uint8_t TYPE = MSG_PONG;
    double sentAt = 0;
    void encode(ByteWriter& w) const { w.f64(sentAt); }
    void decode(ByteReader& r) { sentAt = r.f64(); }
};

// Header plus payload in one buffer.
template <class M>
std::vector<uint8_t> pack(const M& m, uint8_t from = 0, uint16_t seq = 0, float hostTime = 0) {
    std::vector<uint8_t> out;
    ByteWriter w(out);
    Header h; h.type = M::TYPE; h.from = from; h.seq = seq; h.hostTime = hostTime;
    writeHeader(w, h);
    m.encode(w);
    return out;
}

// Reads the header (into *h if given) and payload; false unless the type matches and the bytes are exactly one message.
template <class M>
bool unpack(const uint8_t* data, size_t n, M& m, Header* h = nullptr) {
    ByteReader r(data, n);
    Header hh = readHeader(r);
    if (r.bad || hh.type != M::TYPE) return false;
    m.decode(r);
    if (r.bad || r.left() != 0) return false;
    if (h) *h = hh;
    return true;
}

// The message type of a buffer, or MSG_NONE if it is too short to have a header.
inline uint8_t peekType(const uint8_t* data, size_t n) { return n >= HEADER_SIZE ? data[0] : (uint8_t)MSG_NONE; }

// The protocol version a CREATE or JOIN asks for (it always sits right after the header), or 0.
inline uint16_t peekVersion(const uint8_t* data, size_t n) {
    return n >= HEADER_SIZE + 2 ? (uint16_t)(data[HEADER_SIZE] | data[HEADER_SIZE + 1] << 8) : 0;
}

} // namespace netp
