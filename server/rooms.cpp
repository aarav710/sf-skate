#include "rooms.h"
#include <memory>
#include <random>

namespace relay {

Rooms::Rooms(std::function<uint32_t()> rng) : rng_(std::move(rng)) {
    auto dev = std::make_shared<std::random_device>();
    secretRng_ = [dev] { return (uint32_t)(*dev)(); };
    if (!rng_) rng_ = secretRng_;
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

CreateResult Rooms::create(PeerKey peer, uint16_t version, uint32_t fingerprint, const std::string& name, uint32_t ip, double now) {
    CreateResult res;
    if (version != netp::PROTOCOL_VERSION) { res.error = netp::ERR_VERSION; return res; }
    if (where_.count(peer)) { res.error = netp::ERR_BAD_REQUEST; return res; }
    if ((int)rooms_.size() >= maxRooms) { res.error = netp::ERR_SERVER_FULL; return res; }
    if (ip) {
        int mine = 0;
        for (auto& e : rooms_) mine += e.second.creatorIp == ip;
        if (mine >= maxRoomsPerIp) { res.error = netp::ERR_TOO_MANY_ROOMS; return res; }
    }
    std::string code = newCode();
    if (code.empty()) { res.error = netp::ERR_SERVER_FULL; return res; }
    Room& r = rooms_[code];
    r.code = code;
    r.fingerprint = fingerprint;
    r.creatorIp = ip;
    r.createdAt = now;
    Member m;
    m.id = takeId(r);
    m.name = netp::sanitizeName(name);
    m.peer = peer;
    m.order = ++order_;
    m.secret = newSecret();
    r.members.push_back(m);
    r.host = m.id;
    where_[peer] = code;
    res.code = code;
    res.id = m.id;
    res.secret = m.secret;
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
    if ((int)r.members.size() >= netp::MAX_PLAYERS) { res.error = netp::ERR_ROOM_FULL; return res; }   // (held places count)
    Member m;
    m.id = takeId(r);
    m.name = netp::sanitizeName(name);
    m.peer = peer;
    m.order = ++order_;
    m.secret = newSecret();
    for (auto& o : r.members) {
        res.others.push_back({o.id, o.name, o.away()});
        if (!o.away()) res.notify.push_back(o.peer);
    }
    r.members.push_back(m);
    where_[peer] = code;
    res.code = code;
    res.id = m.id;
    res.host = r.host;
    res.name = m.name;
    res.secret = m.secret;
    return res;
}

JoinResult Rooms::rejoin(PeerKey peer, const std::string& rawCode, uint16_t version, uint32_t fingerprint, uint8_t id, uint64_t secret, double now) {
    JoinResult res;
    if (version != netp::PROTOCOL_VERSION) { res.error = netp::ERR_VERSION; return res; }
    if (where_.count(peer)) { res.error = netp::ERR_BAD_REQUEST; return res; }
    std::string code = netp::normalizeCode(rawCode);
    auto it = code.empty() ? rooms_.end() : rooms_.find(code);
    if (it == rooms_.end()) { res.error = netp::ERR_NO_SESSION; return res; }
    Room& r = it->second;
    if (r.fingerprint != fingerprint) { res.error = netp::ERR_FINGERPRINT; return res; }
    Member* m = r.find(id);
    // (a wrong secret reads as an expired place: it tells a guesser nothing)
    if (!m || m->secret != secret || (m->away() && now > m->awayUntil)) { res.error = netp::ERR_REJOIN_EXPIRED; return res; }
    if (!m->away()) { res.replaced = m->peer; where_.erase(m->peer); }   // (the old connection, not noticed dropping yet)
    m->peer = peer;
    m->awayUntil = 0;
    where_[peer] = code;
    const Member* h = r.find(r.host);
    if (!h || h->away()) { r.host = id; res.hostChanged = true; }   // nobody connected hosted: they do
    for (auto& o : r.members) {
        if (o.id == id) continue;
        res.others.push_back({o.id, o.name, o.away()});
        if (!o.away()) res.notify.push_back(o.peer);
    }
    res.code = code;
    res.id = id;
    res.host = r.host;
    res.name = m->name;
    res.secret = m->secret;
    return res;
}

void Rooms::afterGone(std::map<std::string, Room>::iterator it, LeaveResult& res) {
    Room& r = it->second;
    if (r.members.empty()) { rooms_.erase(it); res.roomDeleted = true; return; }
    const Member* h = r.find(r.host);
    if (!h || h->away()) {   // the longest-connected player still here takes over (if anyone is)
        const Member* best = nullptr;
        for (auto& m : r.members) if (!m.away() && (!best || m.order < best->order)) best = &m;
        if (best && best->id != r.host) { r.host = best->id; res.newHost = best->id; }
    }
    for (auto& m : r.members) if (!m.away()) res.notify.push_back(m.peer);
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
    afterGone(it, res);
    return res;
}

LeaveResult Rooms::drop(PeerKey peer, double now) {
    LeaveResult res;
    auto w = where_.find(peer);
    if (w == where_.end()) return res;
    res.wasInRoom = true;
    res.away = true;
    res.code = w->second;
    where_.erase(w);
    auto it = rooms_.find(res.code);
    if (it == rooms_.end()) return res;
    for (auto& m : it->second.members)
        if (m.peer == peer) { res.id = m.id; m.peer = 0; m.awayUntil = now + holdTime; break; }
    afterGone(it, res);   // (the host hands over now: the session doesn't wait for them)
    return res;
}

TickResult Rooms::tick(double now) {
    TickResult out;
    for (auto it = rooms_.begin(); it != rooms_.end();) {
        Room& r = it->second;
        if (now >= r.createdAt + sessionMax) {   // the session cap: everyone out
            std::vector<PeerKey> peers;
            for (auto& m : r.members) if (!m.away()) { peers.push_back(m.peer); where_.erase(m.peer); }
            out.closed.push_back({r.code, peers});
            it = rooms_.erase(it);
            continue;
        }
        if (!r.warned && now >= r.createdAt + sessionMax - sessionWarn) {
            r.warned = true;
            std::vector<PeerKey> peers;
            for (auto& m : r.members) if (!m.away()) peers.push_back(m.peer);
            out.warn.push_back({r.code, peers});
        }
        bool deleted = false;
        for (size_t i = 0; !deleted && i < r.members.size();) {
            if (!r.members[i].away() || now <= r.members[i].awayUntil) { i++; continue; }
            LeaveResult res;
            res.wasInRoom = true;
            res.code = r.code;
            res.id = r.members[i].id;
            r.members.erase(r.members.begin() + i);
            auto cur = it++;
            afterGone(cur, res);
            deleted = res.roomDeleted;
            out.expired.push_back(res);
            if (!deleted) it = cur;   // (still here: look at the rest of it)
        }
        if (!deleted) ++it;
    }
    return out;
}

} // namespace relay
