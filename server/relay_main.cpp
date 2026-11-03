// SF Skate relay: `relay [--port 7777]`. See server/README.md.
#include "relay.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static volatile sig_atomic_t gStop = 0;
static void onSignal(int) { gStop = 1; }

int main(int argc, char** argv) {
    int port = 7777;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("usage: %s [--port N]   (UDP, default 7777)\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (port <= 0 || port > 65535) { fprintf(stderr, "bad port %d\n", port); return 2; }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    relay::Relay r;
    if (!r.start((uint16_t)port)) { fprintf(stderr, "relay: can't listen on UDP %d\n", port); return 1; }
    while (!gStop) r.service(100);
    printf("relay: shutting down\n");
    return 0;
}
