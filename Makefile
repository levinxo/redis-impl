DEBUG?= -g -rdynamic -ggdb 
CFLAGS?= -std=c99 -pedantic -O2 -Wall -W
CCOPT= $(CFLAGS)

OBJ = adlist.o ae.o anet.o dict.o redis.o sds.o zmalloc.o

PRGNAME = redis-server

adlist.o: adlist.c adlist.h zmalloc.h
ae.o: ae.c ae.h zmalloc.h
anet.o: anet.c fmacros.h anet.h
redis.o: redis.c fmacros.h ae.h sds.h anet.h dict.h adlist.h zmalloc.h
dict.o: dict.c fmacros.h dict.h zmalloc.h
sds.o: sds.c sds.h zmalloc.h
zmalloc.o: zmalloc.c config.h

redis-server: $(OBJ)
	$(CC) -o $(PRGNAME) $(CCOPT) $(DEBUG) $(OBJ)

.c.o:
	$(CC) -c $(CCOPT) $(DEBUG) $(COMPILE_TIME) $<

clean:
	rm -rf $(PRGNAME) *.o

