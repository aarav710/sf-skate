#include "rooms.h"
#include <memory>
#include <random>

namespace relay {

Rooms::Rooms(std::function<uint32_t()> rng) : rng_(std::move(rng)) {
    if (!rng_) {
        auto dev = std::make_shared<std::random_device>();
        rng_ = [dev] { return (uint32_t)(*dev)(); };
    }
}

std::string Rooms::newCode() {
    for (int attempt = 0; attempt < 100; attempt++) {
        uint32_t v = rng_();   // 6 symbols x 5 bits = 30 bits
        std::string code;
        for (int i = 0; i < netp::CODE_LEN; i++, v >>= 5) code += netp::CODE_ALPHABET[v & 31];
        if (!rooms_.count(code)) return code;
    }
    return "";
}

Room* Rooms::roomOfMut(PeerKey peer) {
    auto it = where_.find(peer);
    if (it == where_.end()) return nullptr;
    auto r = rooms_.find(it->second);
    return r == rooms_.end() ? nullptr : &r->second;
}
const Room* Rooms::roomOf(PeerKey peer) const { return const_cast<Rooms*>(this)->roomOfMut(peer); }

const Room* Rooms::room(const std::string& code) const {
    auto r = rooms_.find(code);
    return r == rooms_.end() ? nullptr : &r->second;
}

uint8_t Rooms::idOf(PeerKey peer) const {
    const Room* r = roomOf(peer);
    if (r) for (auto& m : r->members) if (m.peer == peer) return m.id;
    return 0;
}

// next id after the last one handed out that nobody in the room has (1..250), so a fresh joiner doesn't take
// the id of someone who just left while their last packets may still be arriving
static uint8_t takeId(Room& r) {
    for (int k = 0; k < 250; k++) {
        uint8_t id = r.nextId;
        r.nextId = r.nextId >= 250 ? 1 : r.nextId + 1;
        if (!r.find(id)) return id;
    }
    return 0;
}

CreateResult Rooms::create(PeerKey peer, uint16_t version, uint32_t fingerprint, const std::string& name) {
    CreateResult res;
    if (version != netp::PROTOCOL_VERSION) { res.error = netp::ERR_VERSION; return res; }
    if (where_.count(peer)) { res.error = netp::ERR_BAD_REQUEST; return res; }
    if ((int)rooms_.size() >= maxRooms) { res.error = netp::ERR_SERVER_FULL; return res; }
    std::string code = newCode();
    if (code.empty()) { res.error = netp::ERR_SERVER_FULL; return res; }
    Room& r = rooms_[code];
    r.code = code;
    r.fingerprint = fingerprint;
    Member m;
    m.id = takeId(r);
    m.name = netp::sanitizeName(name);
    m.peer = peer;
    m.order = ++order_;
    r.members.push_back(m);
    r.host = m.id;
    where_[peer] = code;
    res.code = code;
    res.id = m.id;
    return res;
}

JoinResult Rooms::join(PeerKey peer, const std::string& rawCode, uint16_t version, uint32_t fingerprint, const std::string& name) {
    JoinResult res;
    if (version != netp::PROTOCOL_VERSION) { res.error = netp::ERR_VERSION; return res; }
    if (where_.count(peer)) { res.error = netp::ERR_BAD_REQUEST; return res; }
    std::string code = netp::normalizeCode(rawCode);
    auto it = code.empty() ? rooms_.end() : rooms_.find(code);
    if (it == rooms_.end()) { res.error = netp::ERR_NO_SESSION; return res; }
    Room& r = it->second;
    if (r.fingerprint != fingerprint) { res.error = netp::ERR_FINGERPRINT; return res; }
    if ((int)r.members.size() >= netp::MAX_PLAYERS) { res.error = netp::ERR_ROOM_FULL; return res; }
    Member m;
    m.id = takeId(r);
    m.name = netp::sanitizeName(name);
    m.peer = peer;
    m.order = ++order_;
    for (auto& o : r.members) { res.others.push_back({o.id, o.name}); res.notify.push_back(o.peer); }
    r.members.push_back(m);
    where_[peer] = code;
    res.code = code;
    res.id = m.id;
    res.host = r.host;
    res.name = m.name;
    return res;
}

LeaveResult Rooms::leave(PeerKey peer) {
    LeaveResult res;
    auto w = where_.find(peer);
    if (w == where_.end()) return res;
    res.wasInRoom = true;
    res.code = w->second;
    where_.erase(w);
    auto it = rooms_.find(res.code);
    if (it == rooms_.end()) return res;
    Room& r = it->second;
    for (size_t i = 0; i < r.members.size(); i++)
        if (r.members[i].peer == peer) { res.id = r.members[i].id; r.members.erase(r.members.begin() + i); break; }
    if (r.members.empty()) { rooms_.erase(it); res.roomDeleted = true; return res; }
    if (r.host == res.id) {   // the longest-connected player takes over
        const Member* best = &r.members[0];
        for (auto& m : r.members) if (m.order < best->order) best = &m;
        r.host = best->id;
        res.newHost = best->id;
    }
    for (auto& m : r.members) res.notify.push_back(m.peer);
    return res;
}

} // namespace relay
