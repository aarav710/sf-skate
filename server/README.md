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

It logs one line per event (connect, room created, join, refusal, leave, host change, rate limit) to stdout.

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

- 8 players per session, 200 sessions per relay.
- 64 KB/s per player (bursts up to 256 KB); packets over that are dropped and logged.
- Packets over 4 KB are refused (the sender gets dropped), and one IP address can hold at most 16 connections.
- A player who stops answering is dropped after 10 s; when the host leaves, the longest-connected player
  becomes host. The last player out closes the session.
- Game and relay must have the same protocol version (`PROTOCOL_VERSION` in `src/net_protocol.h`), and everyone
  in a session the same world build (the world fingerprint, which includes `GAME_BUILD`: bump that whenever
  gameplay or world generation changes).
