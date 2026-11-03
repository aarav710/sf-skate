#include "net.h"
#include <enet/enet.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <thread>

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
    cfg.nameGiven = cfg.relayGiven = false;
    bool want = false;
    if (env("SFSKATE_RELAY")) cfg.relay = env("SFSKATE_RELAY"), cfg.relayGiven = true;
    if (env("SFSKATE_NAME")) cfg.name = env("SFSKATE_NAME"), cfg.nameGiven = true;
    if (env("SFSKATE_JOIN")) { cfg.joinCode = env("SFSKATE_JOIN"); want = true; }
    if (env("SFSKATE_CREATE") && strcmp(env("SFSKATE_CREATE"), "0") != 0) { cfg.create = true; want = true; }
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool takes = !strcmp(a, "--join") || !strcmp(a, "--name") || !strcmp(a, "--relay");
        if (takes && i + 1 >= argc) { err = std::string(a) + " needs a value"; return false; }
        if (!strcmp(a, "--create")) { cfg.create = true; want = true; }
        else if (!strcmp(a, "--join")) { cfg.joinCode = argv[++i]; cfg.create = false; want = true; }
        else if (!strcmp(a, "--name")) cfg.name = argv[++i], cfg.nameGiven = true;
        else if (!strcmp(a, "--relay")) cfg.relay = argv[++i], cfg.relayGiven = true;
    }
    return want;
}

const char* defaultRelay() { return SFSKATE_RELAY; }

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

struct Net::Lookup {
    std::string name;
    std::atomic<int> state{0};   // 0 looking, 1 found (addr), 2 not found
    ENetAddress addr{};
};

Net::~Net() {
    leave();
    if (enetInit_) { enet_deinitialize(); enetInit_ = false; }
}

bool Net::start(const NetConfig& cfg, double now) {
    leave();
    cfg_ = cfg;
    myName = netp::sanitizeName(cfg.name);
    startedAt_ = clockBase_ = lastService_ = lastRecv_ = now;
    stateTimer_.reset();
    lastPing_ = -1e9;
    status = CONNECTING;
    error.clear();
    if (!cfg.create && netp::normalizeCode(cfg.joinCode).empty()) { fail("Session codes are 6 letters and digits"); return false; }
    std::string h;
    uint16_t port;
    if (!parseHostPort(cfg.relay, h, port)) { fail("Bad server address \"" + cfg.relay + "\""); return false; }
    if (!enetInit_) {
        if (enet_initialize() != 0) { fail("Network setup failed"); return false; }
        enetInit_ = true;
    }
    addrPort_ = port;
    ENetAddress addr;
    if (enet_address_set_host_ip(&addr, h.c_str()) == 0) {   // a plain IP address: nothing to look up
        addrHost_ = addr.host;
        haveAddr_ = true;
        if (!openConnection(now)) { fail("Network setup failed"); return false; }
        return true;
    }
    // a name: looked up on a worker thread (a DNS lookup can take seconds), picked up by service()
    auto lk = std::make_shared<Lookup>();
    lk->name = h;
    lookup_ = lk;
    std::thread([lk] {
        ENetAddress a;
        bool ok = enet_address_set_host(&a, lk->name.c_str()) == 0;
        if (ok) lk->addr = a;
        lk->state = ok ? 1 : 2;
    }).detach();
    return true;
}

bool Net::openConnection(double now) {
    if (!host_) {
        host_ = enet_host_create(nullptr, 1, netp::CH_COUNT, 0, 0);
        if (!host_) return false;
        host_->maximumPacketSize = netp::MAX_MESSAGE;   // (ENet's own default is 32 MB)
    }
    ENetAddress addr;
    addr.host = addrHost_;
    addr.port = addrPort_;
    peer_ = enet_host_connect(host_, &addr, netp::CH_COUNT, 0);
    if (!peer_) return false;
    // (a stall of the game longer than this is caught up by a rejoin)
    enet_peer_timeout(peer_, 32, 5000, 15000);
    connected_ = false;
    tryAt_ = now;
    return true;
}

void Net::closeConnection(bool tell) {
    if (peer_) {
        // a leave tells the relay right away (others see LEFT at once) instead of letting it time us out
        if (tell && connected_) enet_peer_disconnect_now(peer_, netp::DISCONNECT_LEAVE);
        else enet_peer_reset(peer_);
        peer_ = nullptr;
    }
    connected_ = false;
    if (host_) {
        enet_host_flush(host_);
        enet_host_destroy(host_);
        host_ = nullptr;
    }
}

void Net::endSession() {
    remotes.clear();
    inbox.clear();
    carInbox.clear(); fullInbox.clear(); fullRequests.clear(); eventInbox.clear();
    full_.reset();
    worldSet = worldIn = false;
    code.clear();
    myId = hostId = 0;
    secret_ = 0;
    justCreated = rejoined = false;
    endsIn = endsAt = -1;
    rttMs = -1;
}

void Net::leave() {
    closeConnection(true);
    lookup_.reset();
    haveAddr_ = false;
    endSession();
    status = OFF;
}

void Net::fail(const std::string& why) {
    error = why;
    closeConnection(true);
    lookup_.reset();
    endSession();
    status = FAILED;
}

void Net::lost(double now) {
    closeConnection(false);
    status = RECONNECTING;
    droppedAt_ = now;
    tries_ = 0;
    nextTry_ = now + firstRetry;
    full_.reset();
    log.push_back("Connection lost, reconnecting");
}

void Net::dropConnection() {
    if (status == IN_SESSION) lost(lastService_);
}

void Net::sendCarSnapshot(const netp::MsgCarSnapshot& m, double now) {
    if (!isHost()) return;
    send(netp::pack(m, myId, seq_++, clock(now)), netp::CH_STATE);
}

void Net::sendFull(uint8_t to, const std::vector<uint8_t>& data, double now) {
    if (!isHost()) return;
    fullSeq_++;
    for (auto& c : netp::MsgFullSnapshot::split(to, fullSeq_, data)) send(netp::pack(c, myId, seq_++, clock(now)), netp::CH_RELIABLE);
}

void Net::requestFull(double now) {
    if (status != IN_SESSION || isHost()) return;
    send(netp::pack(netp::MsgFullRequest(), myId, seq_++, clock(now)), netp::CH_RELIABLE);
}

void Net::sendEvent(const netp::MsgEvent& m, double now) {
    if (status != IN_SESSION) return;
    send(netp::pack(m, myId, seq_++, clock(now)), netp::CH_RELIABLE);
}

void Net::setWorld(const netp::MsgWorldSettings& w, double now) {
    if (status != IN_SESSION) return;
    if (isHost()) { world = w; worldSet = true; }
    send(netp::pack(w, myId, seq_++, clock(now)), netp::CH_RELIABLE);   // (the relay passes a request to the host only)
}

void Net::sendRaw(const std::vector<uint8_t>& bytes, uint8_t channel) {
    if (!peer_) return;
    ENetPacket* pk = enet_packet_create(bytes.data(), bytes.size(), channel == netp::CH_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0);
    if (enet_peer_send(peer_, channel, pk) < 0) enet_packet_destroy(pk);
}

void Net::send(const std::vector<uint8_t>& bytes, uint8_t channel) {
    if (status == IN_SESSION) sendRaw(bytes, channel);   // (not while getting back in: nothing would take it)
}

void Net::onConnect() {
    if (status == RECONNECTING) {
        netp::MsgRejoin m; m.fingerprint = cfg_.fingerprint; m.code = code; m.playerId = myId; m.secret = secret_;
        sendRaw(netp::pack(m, 0, seq_++, 0), netp::CH_RELIABLE);
        return;
    }
    status = JOINING;
    if (cfg_.create) {
        netp::MsgCreate m; m.fingerprint = cfg_.fingerprint; m.name = myName;
        sendRaw(netp::pack(m, 0, seq_++, 0), netp::CH_RELIABLE);
    } else {
        netp::MsgJoin m; m.fingerprint = cfg_.fingerprint; m.code = netp::normalizeCode(cfg_.joinCode); m.name = myName;
        sendRaw(netp::pack(m, 0, seq_++, 0), netp::CH_RELIABLE);
    }
}

void Net::service(double now) {
    if (status == OFF || status == FAILED) return;
    if (lastService_ >= 0 && now - lastService_ > STALL) {
        // we were stalled (a long frame, loading): the network had no chance to answer meanwhile
        double gap = now - lastService_;
        startedAt_ += gap;
        tryAt_ += gap;
        lastRecv_ = now;
    }
    lastService_ = now;
    if (lookup_) {
        int st = lookup_->state;
        if (st == 0) {
            if (now - startedAt_ > CONNECT_TIMEOUT) fail("Can't find server \"" + lookup_->name + "\"");
            return;
        }
        if (st == 2) { fail("Can't find server \"" + lookup_->name + "\""); return; }
        addrHost_ = lookup_->addr.host;
        haveAddr_ = true;
        lookup_.reset();
        if (!openConnection(now)) { fail("Network setup failed"); return; }
    }
    ENetEvent ev;
    while (host_ && enet_host_service(host_, &ev, 0) > 0) {
        ENetHost* h = host_;
        Status was = status;
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            connected_ = true;
            lastRecv_ = now;
            onConnect();
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            lastRecv_ = now;
            onPacket(ev.packet->data, ev.packet->dataLength, now);
            enet_packet_destroy(ev.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            peer_ = nullptr;
            connected_ = false;
            if (status == IN_SESSION) lost(now);
            else if (status != RECONNECTING)   // (a try that didn't connect: the next one goes at nextTry_)
                fail(status == CONNECTING ? "Can't reach the server at " + cfg_.relay : "Lost connection to the server");
            break;
        default: break;
        }
        if (host_ != h || status != was) break;   // (the connection went, or changed: what's left is for the next frame)
    }
    switch (status) {
    case CONNECTING:
    case JOINING:
        if (now - startedAt_ > CONNECT_TIMEOUT)
            fail(status == CONNECTING ? "Can't reach the server at " + cfg_.relay : std::string("The server didn't answer"));
        break;
    case IN_SESSION:
        if (now - lastRecv_ > SILENCE) lost(now);   // (the relay pongs every second: it can't hear us, or we it)
        break;
    case RECONNECTING:
        if (now - droppedAt_ > REJOIN_BUDGET) { fail("Lost connection to the server"); break; }
        if (peer_ && now - tryAt_ > ATTEMPT_TIMEOUT) closeConnection(false);   // (this try is going nowhere)
        if (!peer_ && now >= nextTry_) {
            tries_++;
            nextTry_ = now + std::min(8.0, firstRetry * (1 << std::min(tries_, 5)));
            if (!openConnection(now)) closeConnection(false);
        }
        break;
    default: break;
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
    case CONNECTING: return "connecting to server...";
    case JOINING: return cfg_.create ? "creating session..." : "joining session...";
    case IN_SESSION: return isHost() ? "in session (host)" : "in session";
    case RECONNECTING: return "reconnecting...";
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
        code = m.code; myId = hostId = m.playerId; secret_ = m.secret;
        status = IN_SESSION;
        session++;
        justCreated = true;
        log.push_back("Session " + code + " created");
        return;
    }
    case netp::MSG_WELCOME: {
        netp::MsgWelcome m;
        if ((status != JOINING && status != RECONNECTING) || !netp::unpack(data, n, m)) return;
        if (status == RECONNECTING) {   // back in: the same id, and the roster as it is now (keeping what we had of them)
            if (m.playerId != myId || m.code != code) { fail("Couldn't get back into the session"); return; }
            std::vector<RemoteInfo> was;
            was.swap(remotes);
            for (auto& e : m.roster) {
                RemoteInfo r;
                for (auto& o : was) if (o.id == e.id) r = o;
                r.id = e.id; r.name = e.name; r.away = e.away;
                remotes.push_back(r);
            }
            hostId = m.hostId; secret_ = m.secret;
            status = IN_SESSION;
            rejoined = true;
            log.push_back("Reconnected");
            return;
        }
        code = m.code; myId = m.playerId; hostId = m.hostId; secret_ = m.secret;
        remotes.clear();
        for (auto& e : m.roster) { RemoteInfo r; r.id = e.id; r.name = e.name; r.away = e.away; remotes.push_back(r); }
        status = IN_SESSION;
        session++;
        log.push_back("Joined session " + code);
        return;
    }
    case netp::MSG_ERROR: {
        netp::MsgError m;
        if (!netp::unpack(data, n, m)) { fail("Server error"); return; }
        if (status == RECONNECTING && m.code == netp::ERR_NO_SESSION) { fail("The session has ended"); return; }
        if (status == RECONNECTING && m.code == netp::ERR_SLOW_DOWN) { closeConnection(false); return; }   // (busy: the next try goes at nextTry_)
        fail(m.text.empty() ? netp::errorText(m.code) : m.text);
        return;
    }
    case netp::MSG_AWAY: {
        netp::MsgAway m;
        if (status != IN_SESSION || !netp::unpack(data, n, m)) return;
        for (auto& r : remotes)
            if (r.id == m.id && r.away != m.away) { r.away = m.away; log.push_back(r.name + (m.away ? " lost connection" : " is back")); }
        // back: the session's weather may have changed while they were away (the relay only passes it to those there)
        if (!m.away && isHost() && worldSet) send(netp::pack(world, myId, seq_++, clock(now)), netp::CH_RELIABLE);
        return;
    }
    case netp::MSG_SESSION_ENDING: {
        netp::MsgSessionEnding m;
        if (status != IN_SESSION || !netp::unpack(data, n, m)) return;
        endsIn = m.secondsLeft;
        endsAt = now + m.secondsLeft;
        log.push_back("The session closes in " + std::to_string((m.secondsLeft + 59) / 60) + " min");
        return;
    }
    case netp::MSG_PLAYER_JOINED: {
        netp::MsgPlayerJoined m;
        if (status != IN_SESSION || !netp::unpack(data, n, m) || m.id == myId || remote(m.id)) return;
        RemoteInfo r; r.id = m.id; r.name = m.name;
        remotes.push_back(r);
        log.push_back(m.name + " joined");
        if (isHost() && worldSet) send(netp::pack(world, myId, seq_++, clock(now)), netp::CH_RELIABLE);   // (so they have it)
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
        carInbox.clear(); fullInbox.clear(); fullRequests.clear(); full_.reset();   // (the old host's, or asked of it)
        // the session's weather is ours to keep now (what the old host last sent, which we show): everyone gets it
        // again, in case one of the old host's never got there
        if (m.id == myId && worldSet) send(netp::pack(world, myId, seq_++, clock(now)), netp::CH_RELIABLE);
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
        std::vector<uint8_t> all;
        if (!full_.add(m, all)) return;   // (more chunks to come)
        fullInbox.clear();
        fullInbox.push_back({now, std::move(all)});
        return;
    }
    case netp::MSG_FULL_REQUEST: {
        netp::MsgFullRequest m;
        if (!isHost() || !netp::unpack(data, n, m, &h) || h.from == myId || !remote(h.from)) return;
        if (std::find(fullRequests.begin(), fullRequests.end(), h.from) == fullRequests.end()) fullRequests.push_back(h.from);
        return;
    }
    case netp::MSG_WORLD_SETTINGS: {
        netp::MsgWorldSettings m;
        if (status != IN_SESSION || !netp::unpack(data, n, m, &h) || h.from == myId) return;
        if (isHost()) {   // a request: taken, and sent on to everyone as the session's
            if (!remote(h.from)) return;
            setWorld(m, now);
        } else if (h.from != hostId) return;
        world = m; worldSet = true; worldIn = true; worldFrom = h.from;
        return;
    }
    case netp::MSG_EVENT: {
        EventIn in;
        if (status != IN_SESSION || !netp::unpack(data, n, in.m, &h) || h.from == myId || !remote(h.from)) return;
        in.from = h.from; in.recvAt = now;
        if (eventInbox.size() < EVENT_INBOX_MAX) eventInbox.push_back(std::move(in));
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
