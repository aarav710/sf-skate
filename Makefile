CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter
# no fused multiply-add: clang fuses a*b+c by default on Apple Silicon but not on Intel, and the world and
# traffic must come out bit-identical on both for multiplayer
CXXFLAGS += -ffp-contract=off
CXXFLAGS += $(shell pkg-config --cflags raylib)
LDFLAGS  += $(shell pkg-config --libs raylib) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo

# ENet (multiplayer): `brew install enet`. Homebrew names the pkg-config file libenet; plain -lenet if it's missing.
ENET_CFLAGS := $(shell pkg-config --cflags libenet 2>/dev/null || pkg-config --cflags enet 2>/dev/null)
ENET_LIBS   := $(shell pkg-config --libs libenet 2>/dev/null || pkg-config --libs enet 2>/dev/null || echo -lenet)
CXXFLAGS += $(ENET_CFLAGS)
LDFLAGS  += $(ENET_LIBS)
# the relay players connect to by default, e.g. make SFSKATE_RELAY=relay.example.com:7777
ifdef SFSKATE_RELAY
CXXFLAGS += -DSFSKATE_RELAY='"$(SFSKATE_RELAY)"'
endif
# net.o is rebuilt when the compiled-in relay changes (the stamp file is only rewritten when it differs)
RELAY_STAMP := build/relay-address.txt
$(shell mkdir -p build && (echo '$(SFSKATE_RELAY)' | cmp -s - $(RELAY_STAMP) || echo '$(SFSKATE_RELAY)' > $(RELAY_STAMP)))
build/net.o: $(RELAY_STAMP)

SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)

sfskate: $(OBJ)
	$(CXX) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.cpp $(wildcard src/*.h) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p build

run: sfskate
	./sfskate

clean:
	rm -rf build sfskate

.PHONY: run clean

# headless gameplay checks
SIM_SRC := tests/sim.cpp src/world_terrain.cpp src/world_city.cpp src/world_landmarks.cpp src/world_towers.cpp src/world_transit.cpp src/world_peninsula.cpp src/gondola.cpp src/mesh.cpp src/models.cpp src/instance.cpp src/player.cpp src/chase.cpp src/traffic.cpp src/jetski.cpp src/seaplane.cpp src/birds.cpp src/net_game.cpp src/remote.cpp
build/sim: $(SIM_SRC) $(wildcard src/*.h) | build
	$(CXX) $(CXXFLAGS) -o $@ $(SIM_SRC) $(LDFLAGS)

test: build/sim
	./build/sim

.PHONY: test

# multiplayer relay server (no raylib): ./build/relay --port 7777
NET_CXXFLAGS := -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Isrc -Iserver $(ENET_CFLAGS)
RELAY_SRC := server/rooms.cpp server/relay.cpp
relay: build/relay
build/relay: $(RELAY_SRC) server/relay_main.cpp $(wildcard server/*.h) src/net_protocol.h src/bytes.h | build
	$(CXX) $(NET_CXXFLAGS) -o $@ $(RELAY_SRC) server/relay_main.cpp $(ENET_LIBS)

# protocol, relay rooms, and a relay with two clients over localhost UDP
build/net_test: tests/net_test.cpp $(RELAY_SRC) src/net.cpp src/net.h $(wildcard server/*.h) src/net_protocol.h src/bytes.h | build
	$(CXX) $(NET_CXXFLAGS) -o $@ tests/net_test.cpp $(RELAY_SRC) src/net.cpp $(ENET_LIBS)

test-net: build/net_test
	./build/net_test

.PHONY: relay test-net
