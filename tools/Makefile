CPPFLAGS = -I ../include -std=gnu11 -Wall -Werror

all: liblevelip

debug: CFLAGS+= -DDEBUG_API
debug: liblevelip

liblevelip: liblevelip.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -shared -o liblevelip.so liblevelip.c -ldl

.PHONY:
clean:
	rm liblevelip.so*
