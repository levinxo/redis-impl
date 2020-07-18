DEBUG?= -g -rdynamic -ggdb 
CFLAGS?= -std=c99 -pedantic -O2 -Wall -W
CCOPT= $(CFLAGS)

OBJ = redis.o zmalloc.o

PRGNAME = redis-server

redis.o: redis.c
zmalloc.o: zmalloc.c config.h

redis-server: $(OBJ)
	$(CC) -o $(PRGNAME) $(CCOPT) $(DEBUG) $(OBJ)

.c.o:
	$(CC) -c $(CCOPT) $(DEBUG) $(COMPILE_TIME) $<

clean:
	rm -rf $(PRGNAME) *.o

