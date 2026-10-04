CPPFLAGS = -I include -std=gnu11 -Wall -Werror -pthread

src = $(wildcard src/*/*.c)
obj = $(patsubst src/%.c, build/%.o, $(src))
headers = $(wildcard include/*.h)
apps = apps/curl/curl apps/udp/udp

lvl-ip: $(obj)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(obj) -o lvl-ip
	@echo
	@echo "lvl-ip needs CAP_NET_ADMIN:"
	-sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip

build/%.o: src/%.c $(headers)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

debug: CFLAGS+= -DDEBUG_SOCKET -DDEBUG_TCP -g -fsanitize=thread
debug: lvl-ip

apps: $(apps)
	$(MAKE) -C tools
	$(MAKE) -C apps/curl
	$(MAKE) -C apps/curl-poll
	$(MAKE) -C apps/udp

all: lvl-ip apps

test: debug apps
	@echo
	@echo "Networking capabilites are required for test dependencies:"
	which arping | sudo xargs setcap cap_net_raw=ep
	which tc | sudo xargs setcap cap_net_admin=ep
	@echo
	cd tests && ./test-run-all

unit:
	$(MAKE) -C tests/unit run

# End-to-end over a real TAP device in an unprivileged network namespace
e2e: all
	tests/netns/run.sh

clean:
	rm -rf build lvl-ip
	$(MAKE) -C tests/unit clean

.PHONY: debug apps all test unit e2e clean
