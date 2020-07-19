/*
 * Copyright (c) 2006-2009, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define REDIS_VERSION "1.02"

#include "fmacros.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define __USE_POSIX199309
#include <signal.h>

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif /* HAVE_BACKTRACE */

#include <stdarg.h>

#include <errno.h>

#include "ae.h"     /* Event driven programming library */
#include "sds.h"    /* Dynamic safe strings */
#include "anet.h"   /* Networking the easy way */
#include "dict.h"   /* Hash tables */
#include "adlist.h" /* Linked lists */
#include "zmalloc.h"

/* Static server configuration */
#define REDIS_SERVERPORT        6379    /* TCP port */
#define REDIS_MAXIDLETIME       (60*5)  /* default client timeout */
#define REDIS_DEFAULT_DBNUM     16
#define REDIS_CONFIGLINE_MAX    1024

/* Slave replication state - slave side */
#define REDIS_REPL_NONE 0   /* No active replication */
#define REDIS_REPL_CONNECT 1    /* Must connect to master */
#define REDIS_REPL_CONNECTED 2  /* Connected to master */

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_NOTICE 1
#define REDIS_WARNING 2

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)

/*================================= Data types ============================== */

/* A redis object, that is a type able to hold a string / list / set */
typedef struct redisObject {
    void *ptr;
    int type;
    int refcount;
} robj;

typedef struct redisDb {
    dict *dict;
    dict *expires;
    int id;
} redisDb;

/* With multiplexing we need to take per-clinet state.
 * Clients are taken in a liked list. */
typedef struct redisClient {
    int fd;
    redisDb *db;
    int dictid;
    sds querybuf;
    robj **argv;
    int argc;
    int bulklen;            /* bulk read len. -1 if not in bulk read mode */
    list *reply;
    int sentlen;
    time_t lastinteraction; /* time of the last interaction, used for timeout */
    int flags;              /* REDIS_CLOSE | REDIS_SLAVE | REDIS_MONITOR */
    int slaveseldb;         /* slave selected db, if this client is a slave */
    int authenticated;      /* when requirepass is non-NULL */
    int replstate;          /* replication state if this is a slave */
    int repldbfd;           /* replication DB file descriptor */
    long repldboff;          /* replication DB file offset */
    off_t repldbsize;       /* replication DB file size */
} redisClient;

struct saveparam {
    time_t seconds;
    int changes;
};

/* Global server state structure */
struct redisServer {
    int port;
    int fd;
    redisDb *db;
    dict *sharingpool;
    unsigned int sharingpoolsize;
    long long dirty;            /* changes to DB from the last save */
    list *clients;
    list *slaves, *monitors;
    char neterr[ANET_ERR_LEN];
    aeEventLoop *el;
    int cronloops;              /* number of times the cron function run */
    list *objfreelist;          /* A list of freed objects to avoid malloc() */
    time_t lastsave;            /* Unix time of last save succeeede */
    size_t usedmemory;             /* Used memory in megabytes */
    /* Fields used only for stats */
    time_t stat_starttime;         /* server start time */
    long long stat_numcommands;    /* number of processed commands */
    long long stat_numconnections; /* number of connections received */
    /* Configuration */
    int verbosity;
    int glueoutputbuf;
    int maxidletime;
    int dbnum;
    int daemonize;
    char *pidfile;
    int bgsaveinprogress;
    pid_t bgsavechildpid;
    struct saveparam *saveparams;
    int saveparamslen;
    char *logfile;
    char *bindaddr;
    char *dbfilename;
    char *requirepass;
    int shareobjects;
    /* Replication related */
    int isslave;
    char *masterhost;
    int masterport;
    redisClient *master;    /* client that is master for this slave */
    int replstate;
    unsigned int maxclients;
    unsigned long maxmemory;
    /* Sort parameters - qsort_r() is only available under BSD so we
     * have to take this state global, in order to pass it to sortCompare() */
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

/*================================ Prototypes =============================== */

static void setupSigSegvAction(void);

/*================================= Globals ================================= */

/* Global vars */
static struct redisServer server; /* server global state */

/*============================ Utility functions ============================ */

static void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    FILE *fp;

    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile,"a");
    if (!fp) return;

    va_start(ap, fmt);
    if (level >= server.verbosity) {
        char *c = ".-*";
        char buf[64];
        time_t now;

        now = time(NULL);
        strftime(buf,64,"%d %b %H:%M:%S",gmtime(&now));
        fprintf(fp,"%s %c ",buf,c[level]);
        vfprintf(fp, fmt, ap);
        fprintf(fp,"\n");
        fflush(fp);
    }
    va_end(ap);

    if (server.logfile) fclose(fp);
}

/* ========================= Random utility functions ======================= */

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
static void oom(const char *msg) {
    fprintf(stderr, "%s: Out of memory\n",msg);
    fflush(stderr);
    sleep(1);
    abort();
}

static void appendServerSaveParams(time_t seconds, int changes) {
    server.saveparams = zrealloc(server.saveparams,sizeof(struct saveparam)*(server.saveparamslen+1));  // sizeof: 16
    if (server.saveparams == NULL) oom("appendServerSaveParams");
    // 数组的index递增时，递增的空间大小由基础元素类型/struct 占用的大小来决定。
    server.saveparams[server.saveparamslen].seconds = seconds;
    server.saveparams[server.saveparamslen].changes = changes;
    server.saveparamslen++;
}

static void ResetServerSaveParams() {
    zfree(server.saveparams);
    server.saveparams = NULL;
    server.saveparamslen = 0;
}

static void initServerConfig() {
    server.dbnum = REDIS_DEFAULT_DBNUM;
    server.port = REDIS_SERVERPORT;
    server.verbosity = REDIS_DEBUG;
    server.maxidletime = REDIS_MAXIDLETIME;
    server.saveparams = NULL;
    server.logfile = NULL; /* NULL = log on standard output */
    server.bindaddr = NULL;
    server.glueoutputbuf = 1;
    server.daemonize = 0;
    server.pidfile = "/var/run/redis.pid";
    server.dbfilename = "dump.rdb";
    server.requirepass = NULL;
    server.shareobjects = 0;
    server.sharingpoolsize = 1024;
    server.maxclients = 0;
    server.maxmemory = 0;
    ResetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* save after 1 hour and 1 change */
    appendServerSaveParams(300,100);  /* save after 5 minutes and 100 changes */
    appendServerSaveParams(60,10000); /* save after 1 minute and 10000 changes */
    /* Replication related */
    server.isslave = 0;
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.replstate = REDIS_REPL_NONE;
}

static void initServer() {
    int j;

    signal(SIGHUP, SIG_IGN);        // 忽略terminal被关闭的信号
    signal(SIGPIPE, SIG_IGN);       // 忽略broken pipe信号
    setupSigSegvAction();
}

static int yesnotoi(char *s) {
    if (!strcasecmp(s,"yes")) return 1;
    else if (!strcasecmp(s,"no")) return 0;
    else return -1;
}

/* I agree, this is a very rudimental way to load a configuration...
   will improve later if the config gets more complex */
static void loadServerConfig(char *filename) {
    FILE *fp;
    char buf[REDIS_CONFIGLINE_MAX+1], *err = NULL;
    int linenum = 0;
    sds line = NULL;

    if (filename[0] == '-' && filename[1] == '\0')
        fp = stdin;
    else {
        if ((fp = fopen(filename, "r")) == NULL) {
            redisLog(REDIS_WARNING,"Fatal error, can't open config file");
            exit(1);
        }
    }

    while(fgets(buf,REDIS_CONFIGLINE_MAX+1,fp) != NULL) {
        // sds string的数组
        sds *argv;
        int argc, j;

        linenum++;
        line = sdsnew(buf);
        line = sdstrim(line," \t\r\n");

        /* Skip comments and blank lines*/
        if (line[0] == '#' || line[0] == '\0') {
            sdsfree(line);
            continue;
        }

        /* Split into arguments */
        argv = sdssplitlen(line,sdslen(line)," ",1,&argc);
        sdstolower(argv[0]);

        /* Execute config directives */
        if (!strcasecmp(argv[0],"timeout") && argc == 2) {
            server.maxidletime = atoi(argv[1]);
            if (server.maxidletime < 0) {
                err = "Invalid timeout value"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"port") && argc == 2) {
            server.port = atoi(argv[1]);
            if (server.port < 1 || server.port > 65535) {
                err = "Invalid port"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"bind") && argc == 2) {
            // zstrdup 复制字符串
            server.bindaddr = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"save") && argc == 3) {
            int seconds = atoi(argv[1]);
            int changes = atoi(argv[2]);
            if (seconds < 1 || changes < 0) {
                err = "Invalid save parameters"; goto loaderr;
            }
            appendServerSaveParams(seconds,changes);
        } else if (!strcasecmp(argv[0],"dir") && argc == 2) {
            if (chdir(argv[1]) == -1) {
                redisLog(REDIS_WARNING,"Can't chdir to '%s': %s",
                    argv[1], strerror(errno));
                exit(1);
            }
        } else if (!strcasecmp(argv[0],"loglevel") && argc == 2) {
            if (!strcasecmp(argv[1],"debug")) server.verbosity = REDIS_DEBUG;
            else if (!strcasecmp(argv[1],"notice")) server.verbosity = REDIS_NOTICE;
            else if (!strcasecmp(argv[1],"warning")) server.verbosity = REDIS_WARNING;
            else {
                err = "Invalid log level. Must be one of debug, notice, warning";
                goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"logfile") && argc == 2) {
            FILE *logfp;

            server.logfile = zstrdup(argv[1]);
            if (!strcasecmp(server.logfile,"stdout")) {
                zfree(server.logfile);
                server.logfile = NULL;
            }
            if (server.logfile) {
                /* Test if we are able to open the file. The server will not
                 * be able to abort just for this problem later... */
                logfp = fopen(server.logfile,"a");
                if (logfp == NULL) {
                    err = sdscatprintf(sdsempty(),
                        "Can't open the log file: %s", strerror(errno));
                    goto loaderr;
                }
                fclose(logfp);
            }
        } else if (!strcasecmp(argv[0],"databases") && argc == 2) {
            server.dbnum = atoi(argv[1]);
            if (server.dbnum < 1) {
                err = "Invalid number of databases"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"maxclients") && argc == 2) {
            server.maxclients = atoi(argv[1]);
        } else if (!strcasecmp(argv[0],"maxmemory") && argc == 2) {
            server.maxmemory = strtoll(argv[1], NULL, 10);
        } else if (!strcasecmp(argv[0],"slaveof") && argc == 3) {
            server.masterhost = sdsnew(argv[1]);
            server.masterport = atoi(argv[2]);
            server.replstate = REDIS_REPL_CONNECT;
        } else if (!strcasecmp(argv[0],"glueoutputbuf") && argc == 2) {
            if ((server.glueoutputbuf = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"shareobjects") && argc == 2) {
            if ((server.shareobjects = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"shareobjectspoolsize") && argc == 2) {
            server.sharingpoolsize = atoi(argv[1]);
            if (server.sharingpoolsize < 1) {
                err = "invalid object sharing pool size"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"daemonize") && argc == 2) {
            if ((server.daemonize = yesnotoi(argv[1])) == -1) {
                err = "argument must be 'yes' or 'no'"; goto loaderr;
            }
        } else if (!strcasecmp(argv[0],"requirepass") && argc == 2) {
          server.requirepass = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"pidfile") && argc == 2) {
          server.pidfile = zstrdup(argv[1]);
        } else if (!strcasecmp(argv[0],"dbfilename") && argc == 2) {
          server.dbfilename = zstrdup(argv[1]);
        } else {
            err = "Bad directive or wrong number of arguments"; goto loaderr;
        }
        for (j = 0; j < argc; j++)
            sdsfree(argv[j]);
        zfree(argv);
        sdsfree(line);
    }
    if (fp != stdin) fclose(fp);
    return;

loaderr:
    fprintf(stderr, "\n*** FATAL CONFIG FILE ERROR ***\n");
    fprintf(stderr, "Reading the configuration file, at line %d\n", linenum);
    fprintf(stderr, ">>> '%s'\n", line);
    fprintf(stderr, "%s\n", err);
    exit(1);
}

#ifdef HAVE_BACKTRACE
static struct redisFunctionSym symsTable[] = {
//{"freeStringObject", (unsigned long)freeStringObject},
//{"freeListObject", (unsigned long)freeListObject},
//{"freeSetObject", (unsigned long)freeSetObject},
//{"decrRefCount", (unsigned long)decrRefCount},
//{"createObject", (unsigned long)createObject},
//{"freeClient", (unsigned long)freeClient},
//{"rdbLoad", (unsigned long)rdbLoad},
//{"addReply", (unsigned long)addReply},
//{"addReplySds", (unsigned long)addReplySds},
//{"incrRefCount", (unsigned long)incrRefCount},
//{"rdbSaveBackground", (unsigned long)rdbSaveBackground},
//{"createStringObject", (unsigned long)createStringObject},
//{"replicationFeedSlaves", (unsigned long)replicationFeedSlaves},
//{"syncWithMaster", (unsigned long)syncWithMaster},
//{"tryObjectSharing", (unsigned long)tryObjectSharing},
//{"removeExpire", (unsigned long)removeExpire},
//{"expireIfNeeded", (unsigned long)expireIfNeeded},
//{"deleteIfVolatile", (unsigned long)deleteIfVolatile},
//{"deleteKey", (unsigned long)deleteKey},
//{"getExpire", (unsigned long)getExpire},
//{"setExpire", (unsigned long)setExpire},
//{"updateSlavesWaitingBgsave", (unsigned long)updateSlavesWaitingBgsave},
//{"freeMemoryIfNeeded", (unsigned long)freeMemoryIfNeeded},
//{"authCommand", (unsigned long)authCommand},
//{"pingCommand", (unsigned long)pingCommand},
//{"echoCommand", (unsigned long)echoCommand},
//{"setCommand", (unsigned long)setCommand},
//{"setnxCommand", (unsigned long)setnxCommand},
//{"getCommand", (unsigned long)getCommand},
//{"delCommand", (unsigned long)delCommand},
//{"existsCommand", (unsigned long)existsCommand},
//{"incrCommand", (unsigned long)incrCommand},
//{"decrCommand", (unsigned long)decrCommand},
//{"incrbyCommand", (unsigned long)incrbyCommand},
//{"decrbyCommand", (unsigned long)decrbyCommand},
//{"selectCommand", (unsigned long)selectCommand},
//{"randomkeyCommand", (unsigned long)randomkeyCommand},
//{"keysCommand", (unsigned long)keysCommand},
//{"dbsizeCommand", (unsigned long)dbsizeCommand},
//{"lastsaveCommand", (unsigned long)lastsaveCommand},
//{"saveCommand", (unsigned long)saveCommand},
//{"bgsaveCommand", (unsigned long)bgsaveCommand},
//{"shutdownCommand", (unsigned long)shutdownCommand},
//{"moveCommand", (unsigned long)moveCommand},
//{"renameCommand", (unsigned long)renameCommand},
//{"renamenxCommand", (unsigned long)renamenxCommand},
//{"lpushCommand", (unsigned long)lpushCommand},
//{"rpushCommand", (unsigned long)rpushCommand},
//{"lpopCommand", (unsigned long)lpopCommand},
//{"rpopCommand", (unsigned long)rpopCommand},
//{"llenCommand", (unsigned long)llenCommand},
//{"lindexCommand", (unsigned long)lindexCommand},
//{"lrangeCommand", (unsigned long)lrangeCommand},
//{"ltrimCommand", (unsigned long)ltrimCommand},
//{"typeCommand", (unsigned long)typeCommand},
//{"lsetCommand", (unsigned long)lsetCommand},
//{"saddCommand", (unsigned long)saddCommand},
//{"sremCommand", (unsigned long)sremCommand},
//{"smoveCommand", (unsigned long)smoveCommand},
//{"sismemberCommand", (unsigned long)sismemberCommand},
//{"scardCommand", (unsigned long)scardCommand},
//{"spopCommand", (unsigned long)spopCommand},
//{"sinterCommand", (unsigned long)sinterCommand},
//{"sinterstoreCommand", (unsigned long)sinterstoreCommand},
//{"sunionCommand", (unsigned long)sunionCommand},
//{"sunionstoreCommand", (unsigned long)sunionstoreCommand},
//{"sdiffCommand", (unsigned long)sdiffCommand},
//{"sdiffstoreCommand", (unsigned long)sdiffstoreCommand},
//{"syncCommand", (unsigned long)syncCommand},
//{"flushdbCommand", (unsigned long)flushdbCommand},
//{"flushallCommand", (unsigned long)flushallCommand},
//{"sortCommand", (unsigned long)sortCommand},
//{"lremCommand", (unsigned long)lremCommand},
//{"infoCommand", (unsigned long)infoCommand},
//{"mgetCommand", (unsigned long)mgetCommand},
//{"monitorCommand", (unsigned long)monitorCommand},
//{"expireCommand", (unsigned long)expireCommand},
//{"getSetCommand", (unsigned long)getSetCommand},
//{"ttlCommand", (unsigned long)ttlCommand},
//{"slaveofCommand", (unsigned long)slaveofCommand},
//{"debugCommand", (unsigned long)debugCommand},
//{"processCommand", (unsigned long)processCommand},
{"setupSigSegvAction", (unsigned long)setupSigSegvAction},
//{"readQueryFromClient", (unsigned long)readQueryFromClient},
//{"rdbRemoveTempFile", (unsigned long)rdbRemoveTempFile},
{NULL,0}
};

/* This function try to convert a pointer into a function name. It's used in
 * oreder to provide a backtrace under segmentation fault that's able to
 * display functions declared as static (otherwise the backtrace is useless). */
static char *findFuncName(void *pointer, unsigned long *offset){
    int i, ret = -1;
    unsigned long off, minoff = 0;

    /* Try to match against the Symbol with the smallest offset */
    for (i=0; symsTable[i].pointer; i++) {
        unsigned long lp = (unsigned long) pointer;

        if (lp != (unsigned long)-1 && lp >= symsTable[i].pointer) {
            off=lp-symsTable[i].pointer;
            if (ret < 0 || off < minoff) {
                minoff=off;
                ret=i;
            }
        }
    }
    if (ret == -1) return NULL;
    *offset = minoff;
    return symsTable[ret].name;
}

static void *getMcontextEip(ucontext_t *uc) {
#if defined(__FreeBSD__)
    return (void*) uc->uc_mcontext.mc_eip;
#elif defined(__dietlibc__)
    return (void*) uc->uc_mcontext.eip;
#elif defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
    return (void*) uc->uc_mcontext->__ss.__eip;
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
  #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__rip;
  #else
    return (void*) uc->uc_mcontext->__ss.__eip;
  #endif
#elif defined(__i386__) || defined(__X86_64__) /* Linux x86 */
    return (void*) uc->uc_mcontext.gregs[REG_EIP];
#elif defined(__ia64__) /* Linux IA64 */
    return (void*) uc->uc_mcontext.sc_ip;
#else
    return NULL;
#endif
}

static void segvHandler(int sig, siginfo_t *info, void *secret) {
    void *trace[100];
    char **messages = NULL;
    int i, trace_size = 0;
    unsigned long offset=0;
    time_t uptime = time(NULL)-server.stat_starttime;
    ucontext_t *uc = (ucontext_t*) secret;
    REDIS_NOTUSED(info);

    redisLog(REDIS_WARNING,
        "======= Ooops! Redis %s got signal: -%d- =======", REDIS_VERSION, sig);
    redisLog(REDIS_WARNING, "%s", sdscatprintf(sdsempty(),
        "redis_version:%s; "
        "uptime_in_seconds:%d; "
        "connected_clients:%d; "
        "connected_slaves:%d; "
        "used_memory:%zu; "
        "changes_since_last_save:%lld; "
        "bgsave_in_progress:%d; "
        "last_save_time:%d; "
        "total_connections_received:%lld; "
        "total_commands_processed:%lld; "
        "role:%s;"
        ,REDIS_VERSION,
        uptime,
        listLength(server.clients)-listLength(server.slaves),
        listLength(server.slaves),
        server.usedmemory,
        server.dirty,
        server.bgsaveinprogress,
        server.lastsave,
        server.stat_numconnections,
        server.stat_numcommands,
        server.masterhost == NULL ? "master" : "slave"
    ));

    trace_size = backtrace(trace, 100);
    /* overwrite sigaction with caller's address */
    if (getMcontextEip(uc) != NULL) {
        trace[1] = getMcontextEip(uc);
    }
    messages = backtrace_symbols(trace, trace_size);

    for (i=1; i<trace_size; ++i) {
        char *fn = findFuncName(trace[i], &offset), *p;

        p = strchr(messages[i],'+');
        if (!fn || (p && ((unsigned long)strtol(p+1,NULL,10)) < offset)) {
            redisLog(REDIS_WARNING,"%s", messages[i]);
        } else {
            redisLog(REDIS_WARNING,"%d redis-server %p %s + %d", i, trace[i], fn, (unsigned int)offset);
        }
    }
    free(messages);
    exit(0);
}

static void setupSigSegvAction(void) {
    struct sigaction act;

    sigemptyset (&act.sa_mask);
    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction
     * is used. Otherwise, sa_handler is used */
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = segvHandler;
    sigaction (SIGSEGV, &act, NULL);
    sigaction (SIGBUS, &act, NULL);
    sigaction (SIGFPE, &act, NULL);
    sigaction (SIGILL, &act, NULL);
    sigaction (SIGBUS, &act, NULL);
    return;
}
#else /* HAVE_BACKTRACE */
static void setupSigSegvAction(void) {
}
#endif /* HAVE_BACKTRACE */

/* =================================== Main! ================================ */

// **argv代表char指针的数组
int main(int argc, char **argv) {
    initServerConfig();

    if (argc == 2) {
        ResetServerSaveParams();
        loadServerConfig(argv[1]);
    } else if (argc > 2) {
        fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf]\n");
        exit(1);
    } else {
        redisLog(REDIS_WARNING,"Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
    }
    initServer();

    printf("hello\n");
}

