// The relay server: an ENet UDP host that matches players into sessions (rooms.h) and forwards their messages.
// It never loads the world. relay_main.cpp runs it as a program; tests/net_test.cpp runs it in-process.
#pragma once
#include "rooms.h"
#include <enet/enet.h>
#include <unordered_map>

namespace relay {

// ENet would accept a 32 MB packet (and buffer its fragments) per peer before the rate limiter ever sees it. The
// biggest message is a chunk of a full snapshot (MsgFullSnapshot::FULL_CHUNK); everything else is under 1.5 KB.
constexpr size_t MAX_PACKET = netp::MAX_MESSAGE;
// bytes ENet may hold per peer for packets not yet handed to us (fragments of ones still coming in, allocated whole at
// their first fragment). ENet checks it before taking a packet, so a peer can tie up at most this plus one MAX_PACKET.
constexpr size_t MAX_WAITING_DATA = 2 * MAX_PACKET;
constexpr size_t MAX_PEERS_PER_IP = 16;            // a household or a NAT'd office, not one script with 1600 peers
constexpr double FULL_ASK_EVERY = 8.0;             // s between one player's asks for a full snapshot (the game asks every 10)
// events (people dodging) a player may send: a few a minute in play, so this only stops a client that floods the room
// with them (each one goes out to up to 7 others)
constexpr double EVENT_RATE = 20, EVENT_BURST = 60;   // a second, and at once
// car hits and weather changes count in a bucket of their own, so a flood of dodges can't hold them up
constexpr double KEY_EVENT_RATE = 4, KEY_EVENT_BURST = 12;
// what the relay sends on for one player, counted once per player it goes to: a full room's host sending the
// heaviest car snapshots (~45 KB/s to each of 7) fits; a client that floods the room doesn't
constexpr double FANOUT_RATE = 384 * 1024, FANOUT_BURST = 768 * 1024;
constexpr double IDLE_DROP = 10;                   // s a connection may stay out of any session before it's closed

// Whether a raw ENet datagram (as received, before ENet looks at it) carries a fragment of a packet bigger than
// maxPacket, and from which peer slot. ENet itself just ignores such a fragment, so its sender would keep its
// connection (and its slot of MAX_PEERS_PER_IP) until it times out; the relay drops it at once instead.
bool oversizedFragment(const uint8_t* data, size_t n, size_t maxPacket, uint16_t* peerId);

class Relay {
public:
    ~Relay() { stop(); }
    // Binds UDP `port` (0 = any free port, see port()). False if ENet or the socket can't be set up.
    bool start(uint16_t port, int maxPeers = MAX_ROOMS * netp::MAX_PLAYERS);
    void stop();
    uint16_t port() const { return host_ ? host_->address.port : 0; }
    // Handles every pending event, waiting up to timeoutMs for the first one.
    void service(int timeoutMs);

    Rooms rooms;
    TryLimit tries;          // wrong codes per address
    bool log = true;         // one line per event on stdout
    double rateBytes = 64 * 1024, rateBurst = 256 * 1024;
    double fanoutBytes = FANOUT_RATE, fanoutBurst = FANOUT_BURST;
    double idleDrop = IDLE_DROP;
    uint32_t timeoutMinMs = 5000, timeoutMaxMs = 10000;   // a peer that stops answering is dropped after these (ENet's)
    // the most any one peer had waiting in ENet (MAX_WAITING_DATA's count) after a service: what a peer can tie up
    size_t maxWaitingSeen = 0;
    uint64_t oversizedDropped = 0;

private:
    struct PeerInfo {
        RateLimit rl;
        double lastDropLog = -1e9;
        uint64_t dropped = 0;
        bool wantsFull = false;   // asked the host for a full snapshot, and hasn't had it yet
        double askedAt = -1e9;    // when it last asked (one ask in FULL_ASK_EVERY, so nobody can keep the host sending them)
        RateLimit events;         // people's EVENTs (counted, not bytes)
        RateLimit keyEvents;      // CAR_HIT EVENTs and WORLD_SETTINGS (counted)
        RateLimit out;            // bytes sent on for it (a message to 7 players counts 7 times)
        int fullLeft = 0;         // chunks of the full snapshot it asked for still to pass on
        double outSince = 0;      // since when it's been in no session (connected, or its session closed)
        uint32_t ip = 0;
    };
    ENetHost* host_ = nullptr;
    bool enetInit_ = false;
    std::unordered_map<ENetPeer*, PeerInfo> peers_;

    void onReceive(ENetPeer* peer, const uint8_t* data, size_t n);
    void onGone(ENetPeer* peer, const char* why, bool clean);
    void sweep(double now);
    bool fanout(ENetPeer* peer, PeerInfo& pi, size_t bytes, size_t to, double now);
    void tellLeft(const LeaveResult& lr);
    double lastSweep_ = -1e9;
    std::vector<ENetPeer*> oversized_;
    static int interceptCb(ENetHost* host, ENetEvent* event);
    void send(ENetPeer* peer, const std::vector<uint8_t>& bytes, uint8_t channel);
    void sendError(ENetPeer* peer, uint8_t code);
    ENetPeer* peerOf(PeerKey k) const { return (ENetPeer*)(uintptr_t)k; }
    static PeerKey keyOf(ENetPeer* p) { return (PeerKey)(uintptr_t)p; }
    void logf(const char* fmt, ...)
#if defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;
};

} // namespace relay
