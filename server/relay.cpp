#include "relay.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace relay {

static double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string addrText(const ENetPeer* p) {
    char ip[64] = "?";
    enet_address_get_host_ip(&p->address, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(p->address.port);
}

void Relay::logf(const char* fmt, ...) {
    if (!log) return;
    char when[32];
    time_t t = time(nullptr);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", gmtime(&t));
    printf("%s ", when);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

bool Relay::start(uint16_t port, int maxPeers) {
    if (enet_initialize() != 0) return false;
    enetInit_ = true;
    ENetAddress addr;
    addr.host = ENET_HOST_ANY;
    addr.port = port;
    host_ = enet_host_create(&addr, (size_t)maxPeers, netp::CH_COUNT, 0, 0);
    if (!host_) return false;
    host_->maximumPacketSize = MAX_PACKET;
    host_->maximumWaitingData = MAX_WAITING_DATA;
    host_->duplicatePeers = MAX_PEERS_PER_IP;
    logf("relay listening on UDP %u (protocol %u, %d rooms max)", (unsigned)host_->address.port, (unsigned)netp::PROTOCOL_VERSION, rooms.maxRooms);
    return true;
}

void Relay::stop() {
    if (host_) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    if (enetInit_) { enet_deinitialize(); enetInit_ = false; }
    peers_.clear();
    rooms = Rooms();   // every session went with the host
}

void Relay::send(ENetPeer* peer, const std::vector<uint8_t>& bytes, uint8_t channel) {
    ENetPacket* pk = enet_packet_create(bytes.data(), bytes.size(), channel == netp::CH_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (enet_peer_send(peer, channel, pk) < 0) enet_packet_destroy(pk);
}

void Relay::sendError(ENetPeer* peer, uint8_t code) {
    netp::MsgError e;
    e.code = code;
    e.text = netp::errorText(code);
    send(peer, netp::pack(e, netp::FROM_RELAY), netp::CH_RELIABLE);
}

void Relay::service(int timeoutMs) {
    if (!host_) return;
    ENetEvent ev;
    int got = enet_host_service(host_, &ev, (enet_uint32)timeoutMs);
    while (got > 0) {
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            // a dead connection is noticed after 10 s without acks
            enet_peer_timeout(ev.peer, 32, 5000, 10000);
            PeerInfo& pi = peers_[ev.peer];
            pi = PeerInfo();
            pi.rl.rate = rateBytes; pi.rl.burst = pi.rl.tokens = rateBurst;
            logf("connect %s", addrText(ev.peer).c_str());
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE:
            onReceive(ev.peer, ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            onGone(ev.peer, "disconnect");
            break;
        default: break;
        }
        got = enet_host_check_events(host_, &ev);
    }
    enet_host_flush(host_);
}

void Relay::onGone(ENetPeer* peer, const char* why) {
    // ENet reports both a clean disconnect and a timeout as DISCONNECT
    LeaveResult lr = rooms.leave(keyOf(peer));
    if (lr.wasInRoom) {
        logf("%s %s: player %u left %s%s", why, addrText(peer).c_str(), (unsigned)lr.id, lr.code.c_str(), lr.roomDeleted ? " (room closed)" : "");
        netp::MsgLeft left; left.id = lr.id;
        auto bytes = netp::pack(left, netp::FROM_RELAY);
        for (PeerKey k : lr.notify) send(peerOf(k), bytes, netp::CH_RELIABLE);
        if (lr.newHost) {
            logf("room %s: player %u is host now", lr.code.c_str(), (unsigned)lr.newHost);
            netp::MsgHostChanged hc; hc.id = lr.newHost;
            auto hb = netp::pack(hc, netp::FROM_RELAY);
            for (PeerKey k : lr.notify) send(peerOf(k), hb, netp::CH_RELIABLE);
        }
    } else {
        logf("%s %s", why, addrText(peer).c_str());
    }
    peers_.erase(peer);
    peer->data = nullptr;
}

void Relay::onReceive(ENetPeer* peer, const uint8_t* data, size_t n) {
    PeerInfo& pi = peers_[peer];
    double now = nowSec();
    PeerKey key = keyOf(peer);
    uint8_t type = netp::peekType(data, n);
    // a full snapshot from the host for someone waiting for one goes past the rate limit (it's a one-off ~220 KB, more
    // than a second's allowance); anything else that size is turned away by the checks below
    bool fullAnswer = false;
    if (type == netp::MSG_FULL_SNAPSHOT && n >= netp::HEADER_SIZE + 1) {
        const Room* r = rooms.roomOf(key);
        const Member* to = r ? r->find(data[netp::HEADER_SIZE]) : nullptr;
        auto it = to ? peers_.find(peerOf(to->peer)) : peers_.end();
        fullAnswer = it != peers_.end() && rooms.idOf(key) == r->host && to->peer != key && it->second.wantsFull;
    }
    if (!fullAnswer && !pi.rl.allow(n + 28, now)) {   // + UDP/IP overhead
        pi.dropped++;
        if (now - pi.lastDropLog > 5) {
            logf("rate limit %s: dropped %llu packets", addrText(peer).c_str(), (unsigned long long)pi.dropped);
            pi.lastDropLog = now;
        }
        return;
    }
    switch (type) {
    case netp::MSG_CREATE: {
        if (netp::peekVersion(data, n) != netp::PROTOCOL_VERSION) {
            logf("create from %s refused: protocol %u", addrText(peer).c_str(), (unsigned)netp::peekVersion(data, n));
            sendError(peer, netp::ERR_VERSION);
            return;
        }
        netp::MsgCreate m;
        if (!netp::unpack(data, n, m)) { sendError(peer, netp::ERR_BAD_REQUEST); return; }
        CreateResult cr = rooms.create(key, m.version, m.fingerprint, m.name);
        if (cr.error) {
            logf("create from %s refused: %s", addrText(peer).c_str(), netp::errorText(cr.error));
            sendError(peer, cr.error);
            return;
        }
        netp::MsgCreated c; c.code = cr.code; c.playerId = cr.id;
        send(peer, netp::pack(c, netp::FROM_RELAY), netp::CH_RELIABLE);
        logf("room %s created by %s \"%s\" (player %u, fingerprint %08x); %zu rooms", cr.code.c_str(), addrText(peer).c_str(),
             rooms.roomOf(key)->members[0].name.c_str(), (unsigned)cr.id, (unsigned)m.fingerprint, rooms.roomCount());
        return;
    }
    case netp::MSG_JOIN: {
        if (netp::peekVersion(data, n) != netp::PROTOCOL_VERSION) {
            logf("join from %s refused: protocol %u", addrText(peer).c_str(), (unsigned)netp::peekVersion(data, n));
            sendError(peer, netp::ERR_VERSION);
            return;
        }
        netp::MsgJoin m;
        if (!netp::unpack(data, n, m)) { sendError(peer, netp::ERR_BAD_REQUEST); return; }
        JoinResult jr = rooms.join(key, m.code, m.version, m.fingerprint, m.name);
        if (jr.error) {
            std::string shown = netp::normalizeCode(m.code);   // never echo raw client bytes into the log
            logf("join %s from %s refused: %s", shown.empty() ? "(bad code)" : shown.c_str(), addrText(peer).c_str(), netp::errorText(jr.error));
            sendError(peer, jr.error);
            return;
        }
        netp::MsgWelcome w; w.code = jr.code; w.playerId = jr.id; w.hostId = jr.host; w.roster = jr.others;
        send(peer, netp::pack(w, netp::FROM_RELAY), netp::CH_RELIABLE);
        netp::MsgPlayerJoined pj; pj.id = jr.id; pj.name = jr.name;
        auto bytes = netp::pack(pj, netp::FROM_RELAY);
        for (PeerKey k : jr.notify) send(peerOf(k), bytes, netp::CH_RELIABLE);
        logf("room %s: %s \"%s\" joined as player %u (%zu players)", jr.code.c_str(), addrText(peer).c_str(), jr.name.c_str(),
             (unsigned)jr.id, jr.others.size() + 1);
        return;
    }
    case netp::MSG_PLAYER_STATE: {
        const Room* r = rooms.roomOf(key);
        netp::MsgPlayerState m;
        if (!r || !netp::unpack(data, n, m)) return;
        // forward as-is, but stamped with the sender's real id: a client can't speak for someone else
        std::vector<uint8_t> bytes(data, data + n);
        bytes[1] = rooms.idOf(key);
        for (auto& mem : r->members) if (mem.peer != key) send(peerOf(mem.peer), bytes, netp::CH_STATE);
        return;
    }
    case netp::MSG_CAR_SNAPSHOT: {
        // only the host decides the traffic: anyone else's is dropped
        const Room* r = rooms.roomOf(key);
        netp::MsgCarSnapshot m;
        if (!r || rooms.idOf(key) != r->host || !netp::unpack(data, n, m)) return;
        std::vector<uint8_t> bytes(data, data + n);
        bytes[1] = r->host;
        for (auto& mem : r->members) if (mem.peer != key) send(peerOf(mem.peer), bytes, netp::CH_STATE);
        return;
    }
    case netp::MSG_FULL_REQUEST: {
        const Room* r = rooms.roomOf(key);
        netp::MsgFullRequest m;
        if (!r || rooms.idOf(key) == r->host || !netp::unpack(data, n, m)) return;
        const Member* h = r->find(r->host);
        if (!h) return;
        pi.wantsFull = true;
        send(peerOf(h->peer), netp::pack(m, rooms.idOf(key)), netp::CH_RELIABLE);
        return;
    }
    case netp::MSG_FULL_SNAPSHOT: {
        if (!fullAnswer) return;
        const Room* r = rooms.roomOf(key);
        const Member* to = r->find(data[netp::HEADER_SIZE]);
        ByteReader rd(data, n);
        netp::readHeader(rd);
        if (rd.bad) return;
        peers_.find(peerOf(to->peer))->second.wantsFull = false;
        std::vector<uint8_t> bytes(data, data + n);
        bytes[1] = r->host;
        send(peerOf(to->peer), bytes, netp::CH_RELIABLE);
        logf("room %s: full snapshot (%zu B) from the host to player %u", r->code.c_str(), n, (unsigned)to->id);
        return;
    }
    case netp::MSG_PING: {
        netp::MsgPing p;
        if (!netp::unpack(data, n, p)) return;
        netp::MsgPong q; q.sentAt = p.sentAt;
        send(peer, netp::pack(q, netp::FROM_RELAY), netp::CH_STATE);
        return;
    }
    default:
        return;   // later-stage or unknown messages: not relayed yet
    }
}

} // namespace relay
