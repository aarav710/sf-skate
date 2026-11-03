// The relay's session bookkeeping: rooms, codes, players, host hand-over and limits. Plain C++ (no ENet), so the
// rules can be unit tested; relay.cpp maps ENet peers to the opaque PeerKey used here.
#pragma once
#include "net_protocol.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace relay {

using PeerKey = uint64_t;

constexpr int MAX_ROOMS = 200;
constexpr int MAX_ROOMS_PER_IP = 4;        // sessions one address may have open (a household making a few, not a script)
constexpr double SESSION_WARN = 5 * 60;   // s before the session cap that everyone is told

struct Member {
    uint8_t id = 0;
    std::string name;
    PeerKey peer = 0;          // 0: away (the connection dropped), their place held until awayUntil
    uint64_t order = 0;        // join order across the relay: the smallest has been connected longest
    uint64_t secret = 0;       // for a REJOIN
    double awayUntil = 0;
    bool away() const { return peer == 0; }
};

struct Room {
    std::string code;
    uint32_t fingerprint = 0;
    uint8_t host = 0;
    uint8_t nextId = 1;
    std::vector<Member> members;   // including the away ones (they still take a place)
    uint32_t creatorIp = 0;
    double createdAt = 0;
    bool warned = false;           // told the session cap is coming
    const Member* find(uint8_t id) const {
        for (auto& m : members) if (m.id == id) return &m;
        return nullptr;
    }
    Member* find(uint8_t id) {
        for (auto& m : members) if (m.id == id) return &m;
        return nullptr;
    }
    size_t connected() const { size_t n = 0; for (auto& m : members) n += !m.away(); return n; }
};

struct CreateResult {
    uint8_t error = netp::ERR_NONE;
    std::string code;
    uint8_t id = 0;
    uint64_t secret = 0;
};

struct JoinResult {
    uint8_t error = netp::ERR_NONE;
    std::string code;
    uint8_t id = 0, host = 0;
    uint64_t secret = 0;
    std::vector<netp::RosterEntry> others;   // everyone already in the room, for WELCOME
    std::vector<PeerKey> notify;             // (connected) peers to tell PLAYER_JOINED, or on a rejoin that they're back
    std::string name;                        // the joiner's sanitized name
    PeerKey replaced = 0;                    // rejoin: the old connection the relay still had for them (to close)
    bool hostChanged = false;                // rejoin: nobody connected hosted, so they do now
};

// Someone gone for good (left, or their held place ran out), or (away) dropped and held for a rejoin.
struct LeaveResult {
    bool wasInRoom = false;
    std::string code;
    uint8_t id = 0;
    bool away = false;
    bool roomDeleted = false;
    uint8_t newHost = 0;                     // nonzero if the host left and someone took over
    std::vector<PeerKey> notify;             // connected peers still in the room
};

// What the clock did to the rooms (Rooms::tick).
struct TickResult {
    std::vector<LeaveResult> expired;                                    // held places that ran out
    std::vector<std::pair<std::string, std::vector<PeerKey>>> warn;      // sessions closing in SESSION_WARN
    std::vector<std::pair<std::string, std::vector<PeerKey>>> closed;    // sessions closed (at SESSION_MAX): their peers
};

class Rooms {
public:
    // rng returns uniformly random 32-bit values for session codes and secrets (std::random_device by default)
    explicit Rooms(std::function<uint32_t()> rng = {});

    // ip: the asker's address (rooms per address are capped); now: the relay's clock (s)
    CreateResult create(PeerKey peer, uint16_t version, uint32_t fingerprint, const std::string& name, uint32_t ip = 0, double now = 0);
    JoinResult join(PeerKey peer, const std::string& code, uint16_t version, uint32_t fingerprint, const std::string& name);
    // back after a drop: the same id, if its place is still held (or the relay hasn't noticed the drop yet) and the
    // secret matches
    JoinResult rejoin(PeerKey peer, const std::string& code, uint16_t version, uint32_t fingerprint, uint8_t id, uint64_t secret, double now);
    LeaveResult leave(PeerKey peer);                 // a clean leave: gone at once
    LeaveResult drop(PeerKey peer, double now);      // the connection dropped: away, the place held for holdTime
    TickResult tick(double now);                     // held places running out, the session cap

    const Room* roomOf(PeerKey peer) const;
    const Room* room(const std::string& code) const;
    uint8_t idOf(PeerKey peer) const;          // 0 if not in a room
    size_t roomCount() const { return rooms_.size(); }
    size_t playerCount() const { return where_.size(); }   // (connected ones)
    int maxRooms = MAX_ROOMS, maxRoomsPerIp = MAX_ROOMS_PER_IP;
    double holdTime = netp::REJOIN_HOLD, sessionMax = netp::SESSION_MAX, sessionWarn = SESSION_WARN;

    std::string newCode();                     // a code no current room uses ("" if it can't find one)

private:
    std::function<uint32_t()> rng_;
    std::map<std::string, Room> rooms_;
    std::unordered_map<PeerKey, std::string> where_;
    uint64_t order_ = 0;
    Room* roomOfMut(PeerKey peer);
    std::function<uint32_t()> secretRng_;      // (always std::random_device: a secret mustn't follow from the codes)
    uint64_t newSecret() { uint64_t a = secretRng_(); return a << 32 | secretRng_(); }
    // after `res.id` went (away or for good): host hand-over, the peers to tell, the room gone if nobody's left
    void afterGone(std::map<std::string, Room>::iterator it, LeaveResult& res);
};

// Wrong session codes (and rejoin secrets) per address: a token bucket of tries, so guessing codes takes years.
struct TryLimit {
    double rate = 1.0 / 3, burst = 10;   // a try back every 3 s, 10 at once
    size_t maxAddrs = 10000;             // addresses remembered (full ones are forgotten first)
    struct Bucket { double tokens, last; };
    std::unordered_map<uint32_t, Bucket> by;
    bool allow(uint32_t ip, double now) {   // whether this address may try now
        auto it = by.find(ip);
        return it == by.end() || std::min(burst, it->second.tokens + (now - it->second.last) * rate) >= 1;
    }
    void failed(uint32_t ip, double now) {  // a wrong code (or secret) from it
        if (by.size() >= maxAddrs && !by.count(ip))
            for (auto it = by.begin(); it != by.end();) it = it->second.tokens + (now - it->second.last) * rate >= burst ? by.erase(it) : std::next(it);
        if (by.size() >= maxAddrs && !by.count(ip)) return;   // (all busy: this one goes uncounted)
        auto ins = by.insert({ip, {burst, now}});
        Bucket& b = ins.first->second;
        b.tokens = std::min(burst, b.tokens + (now - b.last) * rate);
        b.last = now;
        b.tokens = std::max(0.0, b.tokens - 1);
    }
};

// Per-peer token bucket: `rate` bytes per second, holding up to `burst` (so a join snapshot can go through).
struct RateLimit {
    double rate = 64 * 1024, burst = 256 * 1024;
    double tokens = 256 * 1024, last = -1;
    bool allow(size_t bytes, double now) {
        if (last >= 0) tokens = std::min(burst, tokens + (now - last) * rate);
        last = now;
        if (tokens < (double)bytes) return false;
        tokens -= (double)bytes;
        return true;
    }
};

} // namespace relay
