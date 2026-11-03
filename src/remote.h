// Other players in a multiplayer session: the states they send, smoothed into a pose drawn a little in the
// past, their colours, and what the HUD and maps need to show them. No networking here (main feeds it from Net,
// or from the SFSKATE_NET_FAKE test hook), so the tests can drive it directly.
#pragma once
#include "player.h"
#include "net_protocol.h"
#include <string>
#include <vector>

// One player's colours: clothes for the 3D skater, and a bright one for the name tag and map markers.
struct RemoteLook {
    SkaterLook skater;
    Color tag = WHITE;
};
constexpr int REMOTE_PALETTE = 8;   // = netp::MAX_PLAYERS, so everyone in a session gets their own
// The look for palette slot `slot` (0..REMOTE_PALETTE-1). The name only varies the details (skin, deck, arm wave).
RemoteLook remoteLook(int slot, const std::string& name);
// The palette slot a name asks for. Clashes are settled by assignSlots.
int preferredSlot(const std::string& name);
// Palette slots for a whole session, in id order: each player takes the slot their name asks for, or the next free
// one. Everyone in the session knows the same roster (including themselves), so all clients agree on the colours.
std::vector<int> assignSlots(const std::vector<std::pair<uint8_t, std::string>>& roster);

struct RemotePlayer {
    struct Sample { double t; netp::PlayerState s; };   // t: the sender's clock
    static constexpr double DELAY = 0.1;      // drawn this far behind the newest state
    static constexpr double EXTRAP = 0.25;    // updates late: carry on along the velocity this long, then freeze
    static constexpr float TELEPORT = 30.f;   // a jump this far between two states snaps instead of sliding
    static constexpr int MAX_SAMPLES = 32;    // 1.6 s at 20 Hz

    uint8_t id = 0;
    std::string name;
    RemoteLook look;
    std::vector<Sample> samples;   // sender time order, oldest first
    double offset = 0;             // our clock minus theirs, from the quickest packets (latency is never negative)
    bool synced = false;
    double drawOffset = 0;         // the offset drawn with: follows `offset` at most 5% faster or slower than real time
    bool drawSynced = false;
    double lastSeen = -1;          // our clock when a state last arrived
    float fade = 0;                // 0..1: grows in on join, shrinks away on leave
    bool leaving = false;
    bool away = false;             // their connection dropped: shown "reconnecting" (and out of the traffic's way) till they're back

    // set by advance()
    double drawnAt = 0;            // the sender's time the pose is from
    bool hasPose = false;
    bool frozen = false;           // no fresh state for longer than DELAY + EXTRAP
    netp::PlayerState cur;         // the smoothed state
    SkaterPose pose;

    void addSample(float sentAt, double recvAt, const netp::PlayerState& s);
    // The state at the sender's time t: interpolated between the two samples around it, extrapolated up to EXTRAP
    // past the newest (then frozen; *frozen set), the oldest before that. False when there are no samples.
    bool stateAt(double t, netp::PlayerState& out, bool* frozen = nullptr) const;
    // Moves the drawn pose on to our time `now` (dt since the last call): DELAY behind the newest state.
    void advance(double now, float dt);
    Vector3 pos() const { return {cur.x, cur.y, cur.z}; }
    bool shown() const { return hasPose && fade > 0; }
};

class Remotes {
public:
    std::vector<RemotePlayer> list;
    uint8_t myId = 0, hostId = 0;
    std::string myName;

    RemotePlayer* find(uint8_t id);
    RemotePlayer& join(uint8_t id, const std::string& name);   // someone new (or back): fades in once their first state arrives
    void leave(uint8_t id);                                    // fades out, then is dropped
    void addSample(uint8_t id, float sentAt, double recvAt, const netp::PlayerState& s);
    void assignLooks();                                        // after the roster changes
    void update(double now, float dt);
    void clear() { list.clear(); myId = hostId = 0; }
    bool empty() const { return list.empty(); }

    // the skaters, out to maxDist from the camera and not behind it
    void draw(Vector3 camPos, Vector3 camFwd, float maxDist) const;
    // the skaters into a sun shadow map, out to radius around centre
    void drawCasters(Vector3 centre, float radius) const;
};

// Turns the smoothed state into the pose drawSkater takes. On a jet ski, seaplane or train (not replicated yet)
// the skater just stands there at the sent position.
SkaterPose remotePose(const netp::PlayerState& s);
