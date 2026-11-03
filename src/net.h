// The game's multiplayer client: connects to the relay, creates or joins a session, sends this player's state
// at 20 Hz and keeps the latest state of everyone else. No raylib here (the net tests link it without the game).
#pragma once
#include "net_protocol.h"
#include <string>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

struct NetConfig {
    std::string relay = "127.0.0.1:7777";   // host:port
    std::string name;
    bool create = false;
    std::string joinCode;                   // used when !create
    uint32_t fingerprint = 0;
};

// Fires every `interval` on average at whatever frame rate it is polled: the next time is advanced by the
// interval, not reset to the poll time (that would drift down to e.g. 15 Hz at 60 fps). After a long stall it
// starts afresh instead of firing a burst.
struct Every {
    double interval, next = -1e18;
    explicit Every(double i) : interval(i) {}
    bool due(double now) const { return now >= next; }
    void fired(double now) { next = now - next > interval ? now + interval : next + interval; }
    void reset() { next = -1e18; }
};

struct RemoteInfo {
    uint8_t id = 0;
    std::string name;
    netp::PlayerState state;
    bool hasState = false;
    double lastSeen = -1;    // our clock when the last state arrived
    uint16_t lastSeq = 0;
};

// One PLAYER_STATE as it arrived, for smoothing (remote.h): the sender's clock and ours.
struct StateIn {
    uint8_t id = 0;
    float sentAt = 0;     // header.hostTime: the sender's clock
    double recvAt = 0;    // our clock
    netp::PlayerState s;
};

// A CAR_SNAPSHOT from the host as it arrived (our clock), and a full snapshot meant for us.
struct CarSnapIn {
    double recvAt = 0;
    netp::MsgCarSnapshot m;
};
struct FullIn {
    double recvAt = 0;
    std::vector<uint8_t> data;   // Traffic::serializeFull
};

class Net {
public:
    enum Status { OFF, CONNECTING, JOINING, IN_SESSION, FAILED };
    static constexpr double STATE_INTERVAL = 1.0 / 20, PING_INTERVAL = 1.0, CONNECT_TIMEOUT = 8.0;

    ~Net() { stop(); }
    bool start(const NetConfig& cfg, double now);   // false (and FAILED) if the relay address is bad
    void stop();                                    // leaves the session and disconnects
    void service(double now);                       // once a frame: never blocks
    bool stateDue(double now) const { return status == IN_SESSION && stateTimer_.due(now); }
    void sendState(const netp::PlayerState& s, double now);
    // traffic (host): a car snapshot part to the room, unreliable; a full snapshot to the player who asked for it
    void sendCarSnapshot(const netp::MsgCarSnapshot& m, double now);
    void sendFull(uint8_t to, const std::vector<uint8_t>& data, double now);
    // traffic (not the host): ask the host for a full snapshot
    void requestFull(double now);

    bool active() const { return status != OFF; }
    bool isHost() const { return status == IN_SESSION && myId == hostId; }
    const RemoteInfo* remote(uint8_t id) const;
    const char* statusText() const;

    Status status = OFF;
    std::string error;           // why FAILED
    std::string code;            // the session code once in a session
    std::string myName;
    uint8_t myId = 0, hostId = 0;
    std::vector<RemoteInfo> remotes;
    double rttMs = -1;           // to the relay, from PING/PONG
    bool justCreated = false;    // set when CREATED arrives; main clears it after showing the code
    std::vector<std::string> log;   // join/leave/host lines for the HUD or stdout, drained by the caller
    std::vector<StateIn> inbox;     // every state that arrived, oldest first, drained by the caller
    static constexpr size_t INBOX_MAX = 512;   // ~3 s of 7 players: nobody draining it can't grow it for ever
    // traffic, drained by the caller: the host's car snapshots, a full snapshot for us (only the newest is kept), and
    // (host) the players who asked for a full snapshot
    std::vector<CarSnapIn> carInbox;
    static constexpr size_t CAR_INBOX_MAX = 128;
    std::vector<FullIn> fullInbox;
    std::vector<uint8_t> fullRequests;

private:
    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    NetConfig cfg_;
    double startedAt_ = 0, lastPing_ = -1e9;
    Every stateTimer_{STATE_INTERVAL};
    uint16_t seq_ = 0;
    bool enetInit_ = false;

    void fail(const std::string& why);
    void onConnect();
    void onPacket(const uint8_t* data, size_t n, double now);
    void send(const std::vector<uint8_t>& bytes, uint8_t channel);
    float clock(double now) const { return (float)(now - startedAt_); }
};

// "host:port" (or "host", port 7777). False if it doesn't parse.
bool parseHostPort(const std::string& s, std::string& host, uint16_t& port);

// Session options from the environment (SFSKATE_CREATE=1, SFSKATE_JOIN, SFSKATE_NAME, SFSKATE_RELAY), then the
// command line (--create, --join CODE, --name NAME, --relay host:port), which wins. Returns true if a session was
// asked for. Unknown arguments are left alone; a flag missing its value is reported in `err`.
bool netOptions(int argc, char** argv, NetConfig& cfg, std::string& err);
