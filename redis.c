#define REDIS_VERSION "1.02"

#include <stdio.h>
#include <time.h>

#include "zmalloc.h"

#define REDIS_SERVERPORT    6379

struct saveparam {
    time_t seconds;
    int changes;
};

struct redisServer {
    int port;
    int fd;
    time_t lastsave;
    char *pidfile;
    struct saveparam *saveparams;
    int saveparamslen;
};

static struct redisServer server;

static void ResetServerSaveParams() {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}

static void initServerConig() {
    server.port = REDIS_SERVERPORT;
    server.pidfile = "/var/run/redis.pid";
    server.saveparams = NULL;
    ResetServerSaveParams();
}

int main(int argc, char **argv) {
    initServerConig();
    printf("hello\n");
}

