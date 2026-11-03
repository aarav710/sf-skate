// The game's multiplayer client: connects to the relay, creates or joins a session, sends this player's state
// at 20 Hz and keeps the latest state of everyone else. No raylib here (the net tests link it without the game).
#pragma once
#include "net_protocol.h"
#include <memory>
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
    bool away = false;       // their connection dropped: the relay holds their place for a rejoin (shown "reconnecting")
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
// An EVENT from another player as it arrived (our clock).
struct EventIn {
    uint8_t from = 0;
    double recvAt = 0;
    netp::MsgEvent m;
};

// The client's session, from start to leave. Status goes CONNECTING (looking the relay up off the main thread, then
// connecting) -> JOINING -> IN_SESSION. A dropped connection goes RECONNECTING: it tries again (backing off, for
// REJOIN_BUDGET) with REJOIN, which gets the same id back (and `rejoined` set, so the game asks for a full snapshot).
// Anything that ends the session for good is FAILED, with `error` saying why. create/join/leave can be done any
// number of times in one run (start, leave); `session` counts the sessions, so the game can tell a new one apart.
class Net {
public:
    enum Status { OFF, CONNECTING, JOINING, IN_SESSION, RECONNECTING, FAILED };
    static constexpr double STATE_INTERVAL = 1.0 / 20, PING_INTERVAL = 1.0, CONNECT_TIMEOUT = 8.0;
    static constexpr double SILENCE = 6.0;          // s with nothing from the relay (it pongs every s): the connection is gone
    static constexpr double STALL = 1.0;            // a gap this long between services: we stalled, not the network
    static constexpr double REJOIN_BUDGET = 28.0;   // s of trying again after a drop (the relay holds the place 30)
    static constexpr double ATTEMPT_TIMEOUT = 4.0;  // s one try to connect (and rejoin) may take

    ~Net();
    // Starts creating or joining a session (leaving any session first). Never blocks: a relay given by name is looked
    // up on a worker thread. False (and FAILED) if the input is bad.
    bool start(const NetConfig& cfg, double now);
    void leave();                                   // leaves the session at once (others see LEFT straight away): OFF
    void stop() { leave(); }
    void service(double now);                       // once a frame (and during long work): never blocks
    bool stateDue(double now) const { return status == IN_SESSION && stateTimer_.due(now); }
    void sendState(const netp::PlayerState& s, double now);
    // traffic (host): a car snapshot part to the room, unreliable; a full snapshot (in chunks) to the player who asked
    void sendCarSnapshot(const netp::MsgCarSnapshot& m, double now);
    void sendFull(uint8_t to, const std::vector<uint8_t>& data, double now);
    // traffic (not the host): ask the host for a full snapshot
    void requestFull(double now);
    // something we did to the shared world, to everyone else (reliable)
    void sendEvent(const netp::MsgEvent& m, double now);
    // The session's time of day and weather. The host: these are the session's now, sent to everyone (and again to
    // each player who joins, and by whoever hosts next). Anyone else: asks the host for them; they're the session's
    // once the host sends them back (worldIn).
    void setWorld(const netp::MsgWorldSettings& w, double now);

    bool active() const { return status != OFF; }
    // in a session, connected or getting back to it (the session's world stays up while RECONNECTING)
    bool inSession() const { return status == IN_SESSION || status == RECONNECTING; }
    bool isHost() const { return inSession() && myId == hostId; }
    const RemoteInfo* remote(uint8_t id) const;
    const char* statusText() const;

    Status status = OFF;
    std::string error;           // why FAILED
    std::string code;            // the session code once in a session
    std::string myName;
    uint8_t myId = 0, hostId = 0;
    uint32_t session = 0;        // which session (counts up with each one created or joined; a rejoin keeps it)
    std::vector<RemoteInfo> remotes;
    double rttMs = -1;           // to the relay, from PING/PONG
    bool justCreated = false;    // set when CREATED arrives; main clears it after showing the code
    bool rejoined = false;       // set when a REJOIN got us back in; the caller clears it (and asks for a full snapshot)
    double endsIn = -1;          // the relay said the session closes in this many s (at endsAt, our clock)
    double endsAt = -1;
    std::vector<std::string> log;   // join/leave/host lines for the HUD or stdout, drained by the caller
    std::vector<StateIn> inbox;     // every state that arrived, oldest first, drained by the caller
    static constexpr size_t INBOX_MAX = 512;   // ~3 s of 7 players: nobody draining it can't grow it for ever
    // traffic, drained by the caller: the host's car snapshots, a full snapshot for us (only the newest is kept), and
    // (host) the players who asked for a full snapshot
    std::vector<CarSnapIn> carInbox;
    static constexpr size_t CAR_INBOX_MAX = 128;
    std::vector<FullIn> fullInbox;
    std::vector<uint8_t> fullRequests;
    std::vector<EventIn> eventInbox;    // other players' events, drained by the caller
    static constexpr size_t EVENT_INBOX_MAX = 256;
    // the session's time of day and weather: the host's (worldSet: there is one). worldIn: new ones came (from the
    // host, or as the host a request it took, from worldFrom), for the caller to show and clear
    netp::MsgWorldSettings world;
    bool worldSet = false, worldIn = false;
    uint8_t worldFrom = 0;

    // test hooks: the first try again after a drop waits this long; dropConnection() loses the connection the way a
    // network outage would (the relay isn't told), stall(s) is how a frame that long looks
    double firstRetry = 0.5;
    void dropConnection();

private:
    struct Lookup;                       // the relay's name being looked up on a worker thread
    _ENetHost* host_ = nullptr;
    _ENetPeer* peer_ = nullptr;
    bool connected_ = false;             // peer_ has connected
    NetConfig cfg_;
    std::shared_ptr<Lookup> lookup_;
    uint32_t addrHost_ = 0;              // the relay's address, once known (ENet's, network order)
    uint16_t addrPort_ = 0;
    bool haveAddr_ = false;
    double startedAt_ = 0, lastPing_ = -1e9, lastRecv_ = 0, lastService_ = -1;
    double droppedAt_ = 0, nextTry_ = 0, tryAt_ = 0;
    int tries_ = 0;
    uint64_t secret_ = 0;
    uint16_t fullSeq_ = 0;
    netp::FullAssembler full_;
    Every stateTimer_{STATE_INTERVAL};
    uint16_t seq_ = 0;
    double clockBase_ = 0;               // header times are our clock since this (kept across a rejoin)
    bool enetInit_ = false;

    void fail(const std::string& why);
    void endSession();                   // everything of the session goes (not the error)
    void closeConnection(bool tell);     // tell: a clean leave the relay hears of
    bool openConnection(double now);
    void lost(double now);
    void onConnect();
    void onPacket(const uint8_t* data, size_t n, double now);
    void send(const std::vector<uint8_t>& bytes, uint8_t channel);           // in a session only
    void sendRaw(const std::vector<uint8_t>& bytes, uint8_t channel);        // create / join / rejoin
    float clock(double now) const { return (float)(now - clockBase_); }
};

// "host:port" (or "host", port 7777). False if it doesn't parse.
bool parseHostPort(const std::string& s, std::string& host, uint16_t& port);

// Session options from the environment (SFSKATE_CREATE=1, SFSKATE_JOIN, SFSKATE_NAME, SFSKATE_RELAY), then the
// command line (--create, --join CODE, --name NAME, --relay host:port), which wins. Returns true if a session was
// asked for. Unknown arguments are left alone; a flag missing its value is reported in `err`.
bool netOptions(int argc, char** argv, NetConfig& cfg, std::string& err);
