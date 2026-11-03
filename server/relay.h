// The relay server: an ENet UDP host that matches players into sessions (rooms.h) and forwards their messages.
// It never loads the world. relay_main.cpp runs it as a program; tests/net_test.cpp runs it in-process.
#pragma once
#include "rooms.h"
#include <enet/enet.h>
#include <unordered_map>

namespace relay {

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
    bool log = true;         // one line per event on stdout
    double rateBytes = 64 * 1024, rateBurst = 256 * 1024;

private:
    struct PeerInfo {
        RateLimit rl;
        double lastDropLog = -1e9;
        uint64_t dropped = 0;
    };
    ENetHost* host_ = nullptr;
    bool enetInit_ = false;
    std::unordered_map<ENetPeer*, PeerInfo> peers_;

    void onReceive(ENetPeer* peer, const uint8_t* data, size_t n);
    void onGone(ENetPeer* peer, const char* why);
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
