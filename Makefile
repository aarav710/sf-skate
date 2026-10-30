CXX      ?= clang++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter
CXXFLAGS += $(shell pkg-config --cflags raylib)
LDFLAGS  += $(shell pkg-config --libs raylib) -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo

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
SIM_SRC := tests/sim.cpp src/world_terrain.cpp src/world_city.cpp src/world_landmarks.cpp src/world_towers.cpp src/world_transit.cpp src/world_peninsula.cpp src/gondola.cpp src/mesh.cpp src/models.cpp src/instance.cpp src/player.cpp src/chase.cpp
build/sim: $(SIM_SRC) $(wildcard src/*.h) | build
	$(CXX) $(CXXFLAGS) -o $@ $(SIM_SRC) $(LDFLAGS)

test: build/sim
	./build/sim

.PHONY: test
