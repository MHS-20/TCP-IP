CPPFLAGS = -I ../../include -std=gnu11 -Wall -Werror -pthread -g -fsanitize=address,undefined

tests = test_ip_frag test_ip_fragment

test_ip_frag: test_ip_frag.c ../../src/net/ip_frag.c ../../src/core/skbuff.c
	$(CC) $(CPPFLAGS) $^ -o $@

test_ip_fragment: test_ip_fragment.c ../../src/net/ip_output.c ../../src/net/ip_frag.c ../../src/core/skbuff.c ../../src/core/utils.c
	$(CC) $(CPPFLAGS) $^ -o $@

run: $(tests)
	@for t in $(tests); do ./$$t || exit 1; done

clean:
	rm -f $(tests)

.PHONY: run clean
