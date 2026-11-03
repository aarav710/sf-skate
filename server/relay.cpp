#include "relay.h"
#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstddef>
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

bool oversizedFragment(const uint8_t* data, size_t n, size_t maxPacket, uint16_t* peerId) {
    // ENet's wire format (enet/protocol.h, network byte order): a 2-byte peer id with flags, the sent time if its
    // flag is set, then commands, each a fixed size (enet_protocol_command_size) plus the data of the send commands
    auto be16 = [&](size_t at) { return (uint16_t)(data[at] << 8 | data[at + 1]); };
    auto be32 = [&](size_t at) { return (uint32_t)data[at] << 24 | (uint32_t)data[at + 1] << 16 | (uint32_t)data[at + 2] << 8 | data[at + 3]; };
    if (n < 2) return false;
    uint16_t pid = be16(0);
    if (pid & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED) return false;   // (nobody here compresses)
    size_t at = (pid & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME) ? sizeof(ENetProtocolHeader) : offsetof(ENetProtocolHeader, sentTime);
    pid &= ~(ENET_PROTOCOL_HEADER_FLAG_MASK | ENET_PROTOCOL_HEADER_SESSION_MASK);
    while (at + sizeof(ENetProtocolCommandHeader) <= n) {
        uint8_t cmd = data[at] & ENET_PROTOCOL_COMMAND_MASK;
        size_t size = enet_protocol_command_size(data[at]);
        if (cmd == ENET_PROTOCOL_COMMAND_NONE || cmd >= ENET_PROTOCOL_COMMAND_COUNT || size == 0 || at + size > n) return false;
        size_t extra = 0;
        switch (cmd) {
        case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
        case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
            if (be32(at + offsetof(ENetProtocolSendFragment, totalLength)) > maxPacket) { *peerId = pid; return true; }
            extra = be16(at + offsetof(ENetProtocolSendFragment, dataLength));
            break;
        case ENET_PROTOCOL_COMMAND_SEND_RELIABLE: extra = be16(at + offsetof(ENetProtocolSendReliable, dataLength)); break;
        case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE: extra = be16(at + offsetof(ENetProtocolSendUnreliable, dataLength)); break;
        case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED: extra = be16(at + offsetof(ENetProtocolSendUnsequenced, dataLength)); break;
        default: break;
        }
        at += size + extra;
    }
    return false;
}

// the relay being serviced right now (ENet's intercept callback has no user pointer)
static thread_local Relay* tlRelay = nullptr;

int Relay::interceptCb(ENetHost* host, ENetEvent*) {
    uint16_t id;
    if (!tlRelay || !oversizedFragment(host->receivedData, host->receivedDataLength, host->maximumPacketSize, &id) || id >= host->peerCount) return 0;
    ENetPeer* p = &host->peers[id];
    // (only from the address that peer is on: a forged datagram can't get someone else dropped)
    if (p->state != ENET_PEER_STATE_CONNECTED || p->address.host != host->receivedAddress.host || p->address.port != host->receivedAddress.port) return 0;
    if (std::find(tlRelay->oversized_.begin(), tlRelay->oversized_.end(), p) == tlRelay->oversized_.end()) tlRelay->oversized_.push_back(p);
    return 1;   // (dropped: ENet never sees it)
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
    host_->intercept = interceptCb;
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
    oversized_.clear();
    Rooms fresh;
    fresh.maxRooms = rooms.maxRooms; fresh.maxRoomsPerIp = rooms.maxRoomsPerIp;
    fresh.holdTime = rooms.holdTime; fresh.sessionMax = rooms.sessionMax; fresh.sessionWarn = rooms.sessionWarn;
    rooms = fresh;   // every session went with the host (the settings stay)
    tries = TryLimit();
}

void Relay::send(ENetPeer* peer, const std::vector<uint8_t>& bytes, uint8_t channel) {
    if (!peer) return;
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
    tlRelay = this;
    ENetEvent ev;
    int got = enet_host_service(host_, &ev, (enet_uint32)timeoutMs);
    while (got > 0) {
        // what ENet holds for this peer right now (with the packet it just handed us)
        if (ev.peer) maxWaitingSeen = std::max(maxWaitingSeen, ev.peer->totalWaitingData + (ev.type == ENET_EVENT_TYPE_RECEIVE ? ev.packet->dataLength : 0));
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            enet_peer_timeout(ev.peer, 32, timeoutMinMs, timeoutMaxMs);
            PeerInfo& pi = peers_[ev.peer];
            pi = PeerInfo();
            pi.rl.rate = rateBytes; pi.rl.burst = pi.rl.tokens = rateBurst;
            pi.events.rate = EVENT_RATE; pi.events.burst = pi.events.tokens = EVENT_BURST;
            pi.keyEvents.rate = KEY_EVENT_RATE; pi.keyEvents.burst = pi.keyEvents.tokens = KEY_EVENT_BURST;
            pi.out.rate = fanoutBytes; pi.out.burst = pi.out.tokens = fanoutBurst;
            pi.outSince = nowSec();
            pi.ip = ev.peer->address.host;
            logf("connect %s", addrText(ev.peer).c_str());
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE:
            if (peers_.count(ev.peer)) onReceive(ev.peer, ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            // a leave says so in the disconnect's data; anything else (a timeout) holds the player's place
            if (peers_.count(ev.peer)) onGone(ev.peer, ev.data == netp::DISCONNECT_LEAVE ? "leave" : "dropped", ev.data == netp::DISCONNECT_LEAVE);
            break;
        default: break;
        }
        got = enet_host_check_events(host_, &ev);
    }
    // senders of oversized packets: gone at once, freeing their connection
    for (ENetPeer* p : oversized_) {
        if (!peers_.count(p)) continue;
        oversizedDropped++;
        onGone(p, "oversized packet from", true);
        enet_peer_disconnect_now(p, 0);
    }
    oversized_.clear();
    double now = nowSec();
    if (now - lastSweep_ >= 0.25) { lastSweep_ = now; sweep(now); }
    enet_host_flush(host_);
    tlRelay = nullptr;
}

void Relay::tellLeft(const LeaveResult& lr) {
    if (lr.away) {
        netp::MsgAway a; a.id = lr.id; a.away = true;
        auto bytes = netp::pack(a, netp::FROM_RELAY);
        for (PeerKey k : lr.notify) send(peerOf(k), bytes, netp::CH_RELIABLE);
    } else {
        netp::MsgLeft left; left.id = lr.id;
        auto bytes = netp::pack(left, netp::FROM_RELAY);
        for (PeerKey k : lr.notify) send(peerOf(k), bytes, netp::CH_RELIABLE);
    }
    if (lr.newHost) {
        logf("room %s: player %u is host now", lr.code.c_str(), (unsigned)lr.newHost);
        // asks the old host never answered: whoever made them asks the new one (straight away)
        for (PeerKey k : lr.notify) {
            auto it = peers_.find(peerOf(k));
            if (it != peers_.end()) { it->second.wantsFull = false; it->second.fullLeft = 0; it->second.askedAt = -1e9; }
        }
        netp::MsgHostChanged hc; hc.id = lr.newHost;
        auto hb = netp::pack(hc, netp::FROM_RELAY);
        for (PeerKey k : lr.notify) send(peerOf(k), hb, netp::CH_RELIABLE);
    }
}

void Relay::onGone(ENetPeer* peer, const char* why, bool clean) {
    LeaveResult lr = clean ? rooms.leave(keyOf(peer)) : rooms.drop(keyOf(peer), nowSec());
    if (lr.wasInRoom) {
        logf("%s %s: player %u %s %s%s", why, addrText(peer).c_str(), (unsigned)lr.id, lr.away ? "away from" : "left", lr.code.c_str(),
             lr.roomDeleted ? " (room closed)" : lr.away ? " (place held)" : "");
        tellLeft(lr);
    } else {
        logf("%s %s", why, addrText(peer).c_str());
    }
    peers_.erase(peer);
    peer->data = nullptr;
}

void Relay::sweep(double now) {
    TickResult tr = rooms.tick(now);
    for (const LeaveResult& lr : tr.expired) {
        logf("room %s: player %u's held place ran out%s", lr.code.c_str(), (unsigned)lr.id, lr.roomDeleted ? " (room closed)" : "");
        tellLeft(lr);
    }
    for (auto& w : tr.warn) {
        logf("room %s: closes in %.0f s (session limit)", w.first.c_str(), rooms.sessionWarn);
        netp::MsgSessionEnding m; m.secondsLeft = (uint16_t)std::min(65535.0, rooms.sessionWarn);
        auto bytes = netp::pack(m, netp::FROM_RELAY);
        for (PeerKey k : w.second) send(peerOf(k), bytes, netp::CH_RELIABLE);
    }
    for (auto& c : tr.closed) {
        logf("room %s: closed at the session limit", c.first.c_str());
        for (PeerKey k : c.second) {
            sendError(peerOf(k), netp::ERR_SESSION_OVER);
            auto it = peers_.find(peerOf(k));
            if (it != peers_.end()) it->second.outSince = now;
        }
    }
    // connections in no session for idleDrop: closed (a client that never creates or joins, or stays on after)
    std::vector<ENetPeer*> idle;
    for (auto& e : peers_)
        if (!rooms.roomOf(keyOf(e.first)) && now - e.second.outSince > idleDrop) idle.push_back(e.first);
    for (ENetPeer* p : idle) {
        logf("idle %s: closed", addrText(p).c_str());
        peers_.erase(p);
        p->data = nullptr;
        enet_peer_disconnect_now(p, 0);
    }
}

bool Relay::fanout(ENetPeer* peer, PeerInfo& pi, size_t bytes, size_t to, double now) {
    if (to == 0 || pi.out.allow((bytes + 28) * to, now)) return true;
    pi.dropped++;
    if (now - pi.lastDropLog > 5) {
        logf("send-on limit %s: dropped %llu packets", addrText(peer).c_str(), (unsigned long long)pi.dropped);
        pi.lastDropLog = now;
    }
    return false;
}

void Relay::onReceive(ENetPeer* peer, const uint8_t* data, size_t n) {
    PeerInfo& pi = peers_[peer];
    double now = nowSec();
    PeerKey key = keyOf(peer);
    uint8_t type = netp::peekType(data, n);
    // a full snapshot chunk from the host for someone waiting for one goes past the rate limit (the whole snapshot is a
    // one-off ~220 KB, more than a second's allowance); anything else that size is turned away by the checks below
    bool fullAnswer = false;
    netp::MsgFullSnapshot full;
    if (type == netp::MSG_FULL_SNAPSHOT && n >= netp::HEADER_SIZE + 1) {
        const Room* r = rooms.roomOf(key);
        const Member* to = r ? r->find(data[netp::HEADER_SIZE]) : nullptr;
        auto it = to && !to->away() ? peers_.find(peerOf(to->peer)) : peers_.end();
        fullAnswer = it != peers_.end() && rooms.idOf(key) == r->host && to->peer != key && it->second.wantsFull && it->second.fullLeft > 0 &&
                     netp::unpack(data, n, full);
    }
    // (a chunk nobody's waiting for, from a host whose player has gone or got one already, is dropped below: it doesn't
    // eat into the host's allowance for what it sends next)
    if (type == netp::MSG_FULL_SNAPSHOT && !fullAnswer) return;
    if (!fullAnswer && !pi.rl.allow(n + 28, now)) {   // + UDP/IP overhead
        pi.dropped++;
        if (now - pi.lastDropLog > 5) {
            logf("rate limit %s: dropped %llu packets", addrText(peer).c_str(), (unsigned long long)pi.dropped);
            pi.lastDropLog = now;
        }
        return;
    }
    // everyone else in the sender's room (connected), the ones `only` lets through
    auto others = [&](const Room* r, auto only) {
        std::vector<ENetPeer*> v;
        for (auto& mem : r->members) if (!mem.away() && mem.peer != key && only(mem)) v.push_back(peerOf(mem.peer));
        return v;
    };
    auto all = [](const Member&) { return true; };
    auto refuseVersion = [&](const char* what) {
        if (netp::peekVersion(data, n) == netp::PROTOCOL_VERSION) return false;
        logf("%s from %s refused: protocol %u", what, addrText(peer).c_str(), (unsigned)netp::peekVersion(data, n));
        sendError(peer, netp::ERR_VERSION);
        return true;
    };
    switch (type) {
    case netp::MSG_CREATE: {
        if (refuseVersion("create")) return;
        netp::MsgCreate m;
        if (!netp::unpack(data, n, m)) { sendError(peer, netp::ERR_BAD_REQUEST); return; }
        CreateResult cr = rooms.create(key, m.version, m.fingerprint, m.name, pi.ip, now);
        if (cr.error) {
            logf("create from %s refused: %s", addrText(peer).c_str(), netp::errorText(cr.error));
            sendError(peer, cr.error);
            return;
        }
        netp::MsgCreated c; c.code = cr.code; c.playerId = cr.id; c.secret = cr.secret;
        send(peer, netp::pack(c, netp::FROM_RELAY), netp::CH_RELIABLE);
        logf("room %s created by %s \"%s\" (player %u, fingerprint %08x); %zu rooms", cr.code.c_str(), addrText(peer).c_str(),
             rooms.roomOf(key)->members[0].name.c_str(), (unsigned)cr.id, (unsigned)m.fingerprint, rooms.roomCount());
        return;
    }
    case netp::MSG_JOIN:
    case netp::MSG_REJOIN: {
        bool re = type == netp::MSG_REJOIN;
        if (refuseVersion(re ? "rejoin" : "join")) return;
        netp::MsgJoin mj;
        netp::MsgRejoin mr;
        if (re ? !netp::unpack(data, n, mr) : !netp::unpack(data, n, mj)) { sendError(peer, netp::ERR_BAD_REQUEST); return; }
        const std::string& rawCode = re ? mr.code : mj.code;
        std::string shown = netp::normalizeCode(rawCode);   // never echo raw client bytes into the log
        // (guessing codes) A rejoin goes through anyway: it carries a 64-bit secret, and a player on the same address as
        // a guesser mustn't lose their session. With the address out of tries, a failed one only says "expired", so it
        // can't be used to tell which codes exist.
        bool outOfTries = !tries.allow(pi.ip, now);
        if (outOfTries && !re) {
            logf("%s %s from %s refused: too many tries", re ? "rejoin" : "join", shown.empty() ? "(bad code)" : shown.c_str(), addrText(peer).c_str());
            sendError(peer, netp::ERR_SLOW_DOWN);
            return;
        }
        JoinResult jr = re ? rooms.rejoin(key, mr.code, mr.version, mr.fingerprint, mr.playerId, mr.secret, now)
                           : rooms.join(key, mj.code, mj.version, mj.fingerprint, mj.name);
        if (jr.error) {
            if (jr.error == netp::ERR_NO_SESSION || jr.error == netp::ERR_REJOIN_EXPIRED) tries.failed(pi.ip, now);
            if (outOfTries && jr.error == netp::ERR_NO_SESSION) jr.error = netp::ERR_REJOIN_EXPIRED;
            logf("%s %s from %s refused: %s", re ? "rejoin" : "join", shown.empty() ? "(bad code)" : shown.c_str(), addrText(peer).c_str(), netp::errorText(jr.error));
            sendError(peer, jr.error);
            return;
        }
        if (jr.replaced) {   // its old connection, still open here: closed
            ENetPeer* old = peerOf(jr.replaced);
            peers_.erase(old);
            old->data = nullptr;
            enet_peer_disconnect_now(old, 0);
        }
        netp::MsgWelcome w; w.code = jr.code; w.playerId = jr.id; w.hostId = jr.host; w.secret = jr.secret; w.roster = jr.others;
        send(peer, netp::pack(w, netp::FROM_RELAY), netp::CH_RELIABLE);
        if (re) {
            netp::MsgAway back; back.id = jr.id; back.away = false;
            auto bytes = netp::pack(back, netp::FROM_RELAY);
            for (PeerKey k : jr.notify) send(peerOf(k), bytes, netp::CH_RELIABLE);
            if (jr.hostChanged) {
                netp::MsgHostChanged hc; hc.id = jr.id;
                auto hb = netp::pack(hc, netp::FROM_RELAY);
                for (PeerKey k : jr.notify) send(peerOf(k), hb, netp::CH_RELIABLE);
            }
            logf("room %s: %s is player %u again%s", jr.code.c_str(), addrText(peer).c_str(), (unsigned)jr.id, jr.hostChanged ? " (and hosts)" : "");
        } else {
            netp::MsgPlayerJoined pj; pj.id = jr.id; pj.name = jr.name;
            auto bytes = netp::pack(pj, netp::FROM_RELAY);
            for (PeerKey k : jr.notify) send(peerOf(k), bytes, netp::CH_RELIABLE);
            logf("room %s: %s \"%s\" joined as player %u (%zu players)", jr.code.c_str(), addrText(peer).c_str(), jr.name.c_str(),
                 (unsigned)jr.id, jr.others.size() + 1);
        }
        return;
    }
    case netp::MSG_PLAYER_STATE: {
        const Room* r = rooms.roomOf(key);
        netp::MsgPlayerState m;
        if (!r || !netp::unpack(data, n, m)) return;
        // forward as-is, but stamped with the sender's real id: a client can't speak for someone else
        auto to = others(r, all);
        if (!fanout(peer, pi, n, to.size(), now)) return;
        std::vector<uint8_t> bytes(data, data + n);
        bytes[1] = rooms.idOf(key);
        for (ENetPeer* p : to) send(p, bytes, netp::CH_STATE);
        return;
    }
    case netp::MSG_CAR_SNAPSHOT: {
        // only the host decides the traffic: anyone else's is dropped
        const Room* r = rooms.roomOf(key);
        netp::MsgCarSnapshot m;
        if (!r || rooms.idOf(key) != r->host || !netp::unpack(data, n, m)) return;
        auto to = others(r, all);
        if (!fanout(peer, pi, n, to.size(), now)) return;
        std::vector<uint8_t> bytes(data, data + n);
        bytes[1] = r->host;
        for (ENetPeer* p : to) send(p, bytes, netp::CH_STATE);
        return;
    }
    case netp::MSG_FULL_REQUEST: {
        const Room* r = rooms.roomOf(key);
        netp::MsgFullRequest m;
        if (!r || rooms.idOf(key) == r->host || !netp::unpack(data, n, m)) return;
        const Member* h = r->find(r->host);
        // one at a time, and not too often: each one is ~220 KB of the host's upload
        // (an ask the host hasn't answered in 30 s is given up on)
        if (!h || h->away() || now - pi.askedAt < (pi.wantsFull ? 30.0 : FULL_ASK_EVERY)) return;
        pi.wantsFull = true;
        pi.fullLeft = netp::MsgFullSnapshot::MAX_CHUNKS;
        pi.askedAt = now;
        send(peerOf(h->peer), netp::pack(m, rooms.idOf(key)), netp::CH_RELIABLE);
        return;
    }
    case netp::MSG_FULL_SNAPSHOT: {
        if (!fullAnswer) return;
        const netp::MsgFullSnapshot& m = full;
        const Room* r = rooms.roomOf(key);
        const Member* to = r->find(m.to);
        PeerInfo& ti = peers_.find(peerOf(to->peer))->second;
        // one snapshot's worth of chunks per ask (MAX_CHUNKS at most): done at its last one
        if (--ti.fullLeft <= 0 || m.index == m.count - 1) { ti.wantsFull = false; ti.fullLeft = 0; }
        std::vector<uint8_t> bytes(data, data + n);
        bytes[1] = r->host;
        send(peerOf(to->peer), bytes, netp::CH_RELIABLE);
        if (m.index == m.count - 1)
            logf("room %s: full snapshot (%u B in %d chunks) from the host to player %u", r->code.c_str(), (unsigned)m.total, (int)m.count, (unsigned)to->id);
        return;
    }
    case netp::MSG_WORLD_SETTINGS: {
        // the host's go to everyone; anyone else's are requests, for the host only (it decides, and sends them on)
        const Room* r = rooms.roomOf(key);
        netp::MsgWorldSettings m;
        if (!r || !netp::unpack(data, n, m) || !pi.keyEvents.allow(1, now)) return;
        uint8_t id = rooms.idOf(key);
        auto to = others(r, [&](const Member& mem) { return id == r->host || mem.id == r->host; });
        std::vector<uint8_t> bytes(data, data + n);
        bytes[1] = id;
        for (ENetPeer* p : to) send(p, bytes, netp::CH_RELIABLE);
        return;
    }
    case netp::MSG_EVENT: {
        const Room* r = rooms.roomOf(key);
        netp::MsgEvent m;
        if (!r || !netp::unpack(data, n, m)) return;
        if (!(m.kind == netp::EVT_CAR_HIT ? pi.keyEvents : pi.events).allow(1, now)) return;
        auto to = others(r, all);
        if (!fanout(peer, pi, n, to.size(), now)) return;
        std::vector<uint8_t> bytes(data, data + n);
        bytes[1] = rooms.idOf(key);
        for (ENetPeer* p : to) send(p, bytes, netp::CH_RELIABLE);
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
