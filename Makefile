PROJECT = heatshrink
OPTIMIZE = -O3
WARN = -Wall -Wextra -pedantic #-Werror
WARN += -Wmissing-prototypes
WARN += -Wstrict-prototypes
WARN += -Wmissing-declarations
CC=gcc

CFLAGS += -std=c99 -g ${WARN} ${OPTIMIZE}

all: heatshrink

clean:
	rm -f heatshrink \
		*.o *.core TAGS

TAGS:
	etags *.[ch]

# Installation
PREFIX ?=	/usr/local
INSTALL ?=	install
RM ?=		rm

OBJS= heatshrink.o heatshrink_decoder.o

heatshrink: ${OBJS}
	${CC} -o $@ $^ -L.

%.o: %.c
	${CC} -c -o $@ $< ${CFLAGS_STATIC}
