#include "net.h"
#include <enet/enet.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>

// the relay players use unless told otherwise: set it at build time with -DSFSKATE_RELAY='"host:port"'
#ifndef SFSKATE_RELAY
#define SFSKATE_RELAY "127.0.0.1:7777"
#endif

bool netOptions(int argc, char** argv, NetConfig& cfg, std::string& err) {
    auto env = [](const char* k) { const char* v = getenv(k); return v && *v ? v : nullptr; };
    cfg.relay = SFSKATE_RELAY;
    cfg.name = env("USER") ? env("USER") : "skater";
    cfg.create = false;
    cfg.joinCode.clear();
    bool want = false;
    if (env("SFSKATE_RELAY")) cfg.relay = env("SFSKATE_RELAY");
    if (env("SFSKATE_NAME")) cfg.name = env("SFSKATE_NAME");
    if (env("SFSKATE_JOIN")) { cfg.joinCode = env("SFSKATE_JOIN"); want = true; }
    if (env("SFSKATE_CREATE") && strcmp(env("SFSKATE_CREATE"), "0") != 0) { cfg.create = true; want = true; }
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool takes = !strcmp(a, "--join") || !strcmp(a, "--name") || !strcmp(a, "--relay");
        if (takes && i + 1 >= argc) { err = std::string(a) + " needs a value"; return false; }
        if (!strcmp(a, "--create")) { cfg.create = true; want = true; }
        else if (!strcmp(a, "--join")) { cfg.joinCode = argv[++i]; cfg.create = false; want = true; }
        else if (!strcmp(a, "--name")) cfg.name = argv[++i];
        else if (!strcmp(a, "--relay")) cfg.relay = argv[++i];
    }
    return want;
}

bool parseHostPort(const std::string& s, std::string& host, uint16_t& port) {
    host = s;
    port = 7777;
    size_t c = s.rfind(':');
    if (c != std::string::npos && s.find(':') == c) {   // one colon: host:port (several = a bare IPv6, not supported)
        host = s.substr(0, c);
        std::string p = s.substr(c + 1);
        if (p.empty() || p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
        int v = atoi(p.c_str());
        if (v <= 0 || v > 65535) return false;
        port = (uint16_t)v;
    }
    return !host.empty() && host.find(':') == std::string::npos;
}

bool Net::start(const NetConfig& cfg, double now) {
    stop();
    cfg_ = cfg;
    myName = netp::sanitizeName(cfg.name);
    startedAt_ = now;
    stateTimer_.reset();
    lastPing_ = -1e9;
    status = CONNECTING;
    error.clear();
    if (!cfg.create && netp::normalizeCode(cfg.joinCode).empty()) { fail("Session codes are 6 letters and digits"); return false; }
    std::string h;
    uint16_t port;
    if (!parseHostPort(cfg.relay, h, port)) { fail("Bad relay address \"" + cfg.relay + "\""); return false; }
    if (enet_initialize() != 0) { fail("Network setup failed"); return false; }
    enetInit_ = true;
    ENetAddress addr;
    if (enet_address_set_host(&addr, h.c_str()) != 0) { fail("Can't find relay \"" + h + "\""); return false; }
    addr.port = port;
    host_ = enet_host_create(nullptr, 1, netp::CH_COUNT, 0, 0);
    if (!host_) { fail("Network setup failed"); return false; }
    host_->maximumPacketSize = netp::MsgFullSnapshot::MAX_SIZE + 1024;   // (ENet's own default is 32 MB)
    peer_ = enet_host_connect(host_, &addr, netp::CH_COUNT, 0);
    if (!peer_) { fail("Network setup failed"); return false; }
    enet_peer_timeout(peer_, 32, 5000, 10000);
    return true;
}

void Net::stop() {
    if (peer_) {
        // tell the relay right away instead of letting it time us out
        enet_peer_disconnect_now(peer_, 0);
        peer_ = nullptr;
    }
    if (host_) {
        enet_host_flush(host_);
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    if (enetInit_) { enet_deinitialize(); enetInit_ = false; }
    if (status != FAILED) status = OFF;
    remotes.clear();
    inbox.clear();
    carInbox.clear(); fullInbox.clear(); fullRequests.clear();
    code.clear();
    myId = hostId = 0;
}

void Net::fail(const std::string& why) {
    error = why;
    if (peer_) { enet_peer_disconnect_now(peer_, 0); peer_ = nullptr; }
    status = FAILED;
    remotes.clear();
    inbox.clear();
    carInbox.clear(); fullInbox.clear(); fullRequests.clear();
}

void Net::sendCarSnapshot(const netp::MsgCarSnapshot& m, double now) {
    if (!isHost()) return;
    send(netp::pack(m, myId, seq_++, clock(now)), netp::CH_STATE);
}

void Net::sendFull(uint8_t to, const std::vector<uint8_t>& data, double now) {
    if (!isHost() || data.size() > netp::MsgFullSnapshot::MAX_SIZE) return;
    netp::MsgFullSnapshot m; m.to = to; m.data = data;
    send(netp::pack(m, myId, seq_++, clock(now)), netp::CH_RELIABLE);
}

void Net::requestFull(double now) {
    if (status != IN_SESSION || isHost()) return;
    send(netp::pack(netp::MsgFullRequest(), myId, seq_++, clock(now)), netp::CH_RELIABLE);
}

void Net::send(const std::vector<uint8_t>& bytes, uint8_t channel) {
    if (!peer_) return;
    ENetPacket* pk = enet_packet_create(bytes.data(), bytes.size(), channel == netp::CH_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (enet_peer_send(peer_, channel, pk) < 0) enet_packet_destroy(pk);
}

void Net::onConnect() {
    status = JOINING;
    if (cfg_.create) {
        netp::MsgCreate m; m.fingerprint = cfg_.fingerprint; m.name = myName;
        send(netp::pack(m, 0, seq_++, 0), netp::CH_RELIABLE);
    } else {
        netp::MsgJoin m; m.fingerprint = cfg_.fingerprint; m.code = netp::normalizeCode(cfg_.joinCode); m.name = myName;
        send(netp::pack(m, 0, seq_++, 0), netp::CH_RELIABLE);
    }
}

void Net::service(double now) {
    if (!host_ || status == FAILED) return;
    if ((status == CONNECTING || status == JOINING) && now - startedAt_ > CONNECT_TIMEOUT) {
        fail(status == CONNECTING ? "Can't reach the relay at " + cfg_.relay : std::string("The relay didn't answer"));
        return;
    }
    ENetEvent ev;
    while (host_ && enet_host_service(host_, &ev, 0) > 0) {
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT: onConnect(); break;
        case ENET_EVENT_TYPE_RECEIVE:
            onPacket(ev.packet->data, ev.packet->dataLength, now);
            enet_packet_destroy(ev.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            peer_ = nullptr;
            fail(status == CONNECTING ? "Can't reach the relay at " + cfg_.relay : "Lost connection to the relay");
            break;
        default: break;
        }
        if (status == FAILED) break;
    }
    if (status == IN_SESSION && now - lastPing_ >= PING_INTERVAL) {
        lastPing_ = now;
        netp::MsgPing p; p.sentAt = now;
        send(netp::pack(p, myId, seq_++, clock(now)), netp::CH_STATE);
    }
    if (host_) enet_host_flush(host_);
}

void Net::sendState(const netp::PlayerState& s, double now) {
    if (status != IN_SESSION) return;
    stateTimer_.fired(now);
    netp::MsgPlayerState m; m.s = s;
    send(netp::pack(m, myId, seq_++, clock(now)), netp::CH_STATE);
    if (host_) enet_host_flush(host_);
}

const RemoteInfo* Net::remote(uint8_t id) const {
    for (auto& r : remotes) if (r.id == id) return &r;
    return nullptr;
}

const char* Net::statusText() const {
    switch (status) {
    case OFF: return "offline";
    case CONNECTING: return "connecting to relay...";
    case JOINING: return cfg_.create ? "creating session..." : "joining session...";
    case IN_SESSION: return isHost() ? "in session (host)" : "in session";
    case FAILED: return error.c_str();
    }
    return "";
}

void Net::onPacket(const uint8_t* data, size_t n, double now) {
    netp::Header h;
    switch (netp::peekType(data, n)) {
    case netp::MSG_CREATED: {
        netp::MsgCreated m;
        if (status != JOINING || !netp::unpack(data, n, m)) return;
        code = m.code; myId = hostId = m.playerId;
        status = IN_SESSION;
        justCreated = true;
        log.push_back("Session " + code + " created");
        return;
    }
    case netp::MSG_WELCOME: {
        netp::MsgWelcome m;
        if (status != JOINING || !netp::unpack(data, n, m)) return;
        code = m.code; myId = m.playerId; hostId = m.hostId;
        remotes.clear();
        for (auto& e : m.roster) { RemoteInfo r; r.id = e.id; r.name = e.name; remotes.push_back(r); }
        status = IN_SESSION;
        log.push_back("Joined session " + code);
        return;
    }
    case netp::MSG_ERROR: {
        netp::MsgError m;
        if (!netp::unpack(data, n, m)) { fail("Relay error"); return; }
        fail(m.text.empty() ? netp::errorText(m.code) : m.text);
        return;
    }
    case netp::MSG_PLAYER_JOINED: {
        netp::MsgPlayerJoined m;
        if (status != IN_SESSION || !netp::unpack(data, n, m) || m.id == myId || remote(m.id)) return;
        RemoteInfo r; r.id = m.id; r.name = m.name;
        remotes.push_back(r);
        log.push_back(m.name + " joined");
        return;
    }
    case netp::MSG_LEFT: {
        netp::MsgLeft m;
        if (status != IN_SESSION || !netp::unpack(data, n, m)) return;
        for (size_t i = 0; i < remotes.size(); i++)
            if (remotes[i].id == m.id) { log.push_back(remotes[i].name + " left"); remotes.erase(remotes.begin() + i); break; }
        return;
    }
    case netp::MSG_HOST_CHANGED: {
        netp::MsgHostChanged m;
        if (status != IN_SESSION || !netp::unpack(data, n, m)) return;
        hostId = m.id;
        carInbox.clear(); fullInbox.clear(); fullRequests.clear();   // (the old host's, or asked of it)
        const RemoteInfo* r = remote(m.id);
        log.push_back(m.id == myId ? std::string("You are the host now") : (r ? r->name : std::string("someone")) + " is the host now");
        return;
    }
    case netp::MSG_PLAYER_STATE: {
        netp::MsgPlayerState m;
        if (status != IN_SESSION || !netp::unpack(data, n, m, &h)) return;
        for (auto& r : remotes) {
            if (r.id != h.from) continue;
            if (r.hasState && (int16_t)(h.seq - r.lastSeq) <= 0) return;   // older than what we have
            r.state = m.s; r.hasState = true; r.lastSeen = now; r.lastSeq = h.seq;
            if (inbox.size() < INBOX_MAX) inbox.push_back({h.from, h.hostTime, now, m.s});
            return;
        }
        return;   // not (or no longer) in the roster
    }
    case netp::MSG_CAR_SNAPSHOT: {
        // only the host's (the relay checks too); unreliable sequenced, so ENet already dropped any older than the last
        CarSnapIn in;
        if (status != IN_SESSION || isHost() || !netp::unpack(data, n, in.m, &h) || h.from != hostId) return;
        in.recvAt = now;
        if (carInbox.size() < CAR_INBOX_MAX) carInbox.push_back(std::move(in));
        return;
    }
    case netp::MSG_FULL_SNAPSHOT: {
        netp::MsgFullSnapshot m;
        if (status != IN_SESSION || isHost() || !netp::unpack(data, n, m, &h) || h.from != hostId || m.to != myId) return;
        fullInbox.clear();
        fullInbox.push_back({now, std::move(m.data)});
        return;
    }
    case netp::MSG_FULL_REQUEST: {
        netp::MsgFullRequest m;
        if (!isHost() || !netp::unpack(data, n, m, &h) || h.from == myId || !remote(h.from)) return;
        if (std::find(fullRequests.begin(), fullRequests.end(), h.from) == fullRequests.end()) fullRequests.push_back(h.from);
        return;
    }
    case netp::MSG_PONG: {
        netp::MsgPong m;
        if (!netp::unpack(data, n, m) || m.sentAt > now) return;
        rttMs = (now - m.sentAt) * 1000;
        return;
    }
    default: return;
    }
}
