# SF Skate relay

The relay matches players into sessions by code and forwards their messages. It never loads the world, so it
runs happily on the smallest VPS (a few MB of RAM, ~1 KB/s per player today). It speaks ENet over UDP.

## Build

On Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config libenet-dev
make relay            # builds build/relay (no raylib needed)
```

On macOS: `brew install enet && make relay`.

## Run

```sh
./build/relay --port 7777
```

It logs one line per event (connect, room created, join, rejoin, refusal, leave, drop, host change, rate limit,
session limit) to stdout.

Open **UDP** port 7777 (or the one you chose) in the firewall, e.g. `sudo ufw allow 7777/udp`, and in your
cloud provider's security group. No TCP port is needed.

Players then point the game at it:

```sh
./sfskate --create --relay your.server:7777
./sfskate --join CODE --relay your.server:7777
```

or build the game with it as the default: `make SFSKATE_RELAY=your.server:7777`.

## systemd

`/etc/systemd/system/sfskate-relay.service`:

```ini
[Unit]
Description=SF Skate relay
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=/opt/sfskate/relay --port 7777
Restart=on-failure
DynamicUser=yes
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes

[Install]
WantedBy=multi-user.target
```

```sh
sudo install -D build/relay /opt/sfskate/relay
sudo systemctl daemon-reload
sudo systemctl enable --now sfskate-relay
journalctl -u sfskate-relay -f      # the event log
```

## Limits

- 8 players per session, 200 sessions per relay, 4 sessions open per IP address, 16 connections per address.
- A session lasts at most 6 hours: everyone in it is told 5 minutes before, then it closes.
- 64 KB/s per player coming in (bursts up to 256 KB), and 384 KB/s going out on their behalf, where a message sent
  on to 7 players counts 7 times (a full room's host sending the heaviest car snapshots fits). Packets over that are
  dropped and logged. The one exception is a host's full traffic snapshot for a player who asked for one: it goes
  past the limit, once per ask (at most 8 chunks).
- Events: 20 a second per player (60 at once) for people dodging; car hits and weather changes have a bucket of
  their own (4 a second, 12 at once), so a flood of the first can't hold up the others.
- Messages are at most 64 KB (a full snapshot, ~220 KB, comes in chunks). A connection sending anything bigger is
  closed at once.
- A connection that hasn't created or joined a session within 10 s is closed.
- Wrong session codes (and rejoin secrets): 10 at once per address, then one every 3 s ("Too many tries").
- Only the host's car and full snapshots are passed on; anyone else's are dropped.
- A player who leaves is gone at once. One who stops answering (noticed within 5-10 s) is shown as reconnecting:
  their place (id, colour) is held for 30 s for them to rejoin with the secret they got on joining. If they hosted,
  the longest-connected player still there hosts straight away, and they come back as a member. The last place to
  go closes the session.
- Game and relay must have the same protocol version (`PROTOCOL_VERSION` in `src/net_protocol.h`), and everyone
  in a session the same world build (the world fingerprint, which includes `GAME_BUILD`: bump that whenever
  gameplay or world generation changes).
