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

struct Member {
    uint8_t id = 0;
    std::string name;
    PeerKey peer = 0;
    uint64_t order = 0;   // join order across the relay: the smallest has been connected longest
};

struct Room {
    std::string code;
    uint32_t fingerprint = 0;
    uint8_t host = 0;
    uint8_t nextId = 1;
    std::vector<Member> members;
    const Member* find(uint8_t id) const {
        for (auto& m : members) if (m.id == id) return &m;
        return nullptr;
    }
};

struct CreateResult {
    uint8_t error = netp::ERR_NONE;
    std::string code;
    uint8_t id = 0;
};

struct JoinResult {
    uint8_t error = netp::ERR_NONE;
    std::string code;
    uint8_t id = 0, host = 0;
    std::vector<netp::RosterEntry> others;   // everyone already in the room, for WELCOME
    std::vector<PeerKey> notify;             // peers to tell PLAYER_JOINED
    std::string name;                        // the joiner's sanitized name
};

struct LeaveResult {
    bool wasInRoom = false;
    std::string code;
    uint8_t id = 0;
    bool roomDeleted = false;
    uint8_t newHost = 0;                     // nonzero if the host left and someone took over
    std::vector<PeerKey> notify;             // peers still in the room
};

class Rooms {
public:
    // rng returns uniformly random 32-bit values for session codes (std::random_device by default)
    explicit Rooms(std::function<uint32_t()> rng = {});

    CreateResult create(PeerKey peer, uint16_t version, uint32_t fingerprint, const std::string& name);
    JoinResult join(PeerKey peer, const std::string& code, uint16_t version, uint32_t fingerprint, const std::string& name);
    LeaveResult leave(PeerKey peer);

    const Room* roomOf(PeerKey peer) const;
    const Room* room(const std::string& code) const;
    uint8_t idOf(PeerKey peer) const;          // 0 if not in a room
    size_t roomCount() const { return rooms_.size(); }
    size_t playerCount() const { return where_.size(); }
    int maxRooms = MAX_ROOMS;

    std::string newCode();                     // a code no current room uses ("" if it can't find one)

private:
    std::function<uint32_t()> rng_;
    std::map<std::string, Room> rooms_;
    std::unordered_map<PeerKey, std::string> where_;
    uint64_t order_ = 0;
    Room* roomOfMut(PeerKey peer);
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
