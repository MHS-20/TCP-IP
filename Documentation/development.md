# Development

This document covers testing, debugging and the feature set.

# Testing

    make unit    # host-only tests in tests/unit, built with ASan and UBSan
    make e2e     # tests/netns/run.sh: real TAP device in an unprivileged namespace
    make test    # both

Unit tests link single source files against stubs (see
`tests/unit/test_ip_fragment.c`, which replaces routing and ARP to capture
outgoing fragments). Set `LVLIP_E2E_VERBOSE=1` to print `tc` qdisc statistics
after each impaired run of the end-to-end suite.

# Debugging

Build Level-IP with `make debug`. It adds debug symbols, enables socket and TCP debug output, and builds with Thread Sanitizer.

## Debug Output

When built with `make debug`, `lvl-ip` becomes chatty and outputs debug statements. You can enable/disable different component debug output with macros defined in headers.

For example, enabling socket-specific output:

    make clean
    CFLAGS+=-DDEBUG_SOCKET make debug 

## Debugging Networking

Use `tcpdump` with the IP address you're using, e.g.:

    $ tcpdump -i any host 10.0.0.4 -n
    IP 10.0.0.4.12000 > 10.0.0.5.8000: Flags [S], seq 1525252, win 512, length 0
    IP 10.0.0.5.8000 > 10.0.0.4.12000: Flags [S.], seq 1332068674, ack 1525253, win 29200, options [mss 1460], length 0
    IP 10.0.0.4.12000 > 10.0.0.5.8000: Flags [.], ack 1, win 512, length 0

Together with the verbose `lvl-ip` output, you can troubleshoot behaviour and spot patterns.

## Tracing Program Code

Simply run `gdb` with your favorite interface.

Refer to https://sourceware.org/gdb/current/onlinedocs/gdb/Threads.html for debugging with threads.

## Debugging Memory Allocation and Use

Address Sanitizer is built in to newer GCC versions and is activated with `-fsanitize=address`. The unit tests use it; for the daemon, replace the sanitizer in the `debug` target.

https://github.com/google/sanitizers/wiki/AddressSanitizer

## Debugging Concurrency

Level-IP uses multiple threads with shared data structures, therefore programming errors like race conditions are easy to introduce.

Thread Sanitizer by Google is also built in to newer GCCs, which helps pinpointing concurrent access to variables without proper guards.

https://github.com/google/sanitizers/wiki/ThreadSanitizerCppManual

# Coding Style

The foremost aim of Level-IP is to be an educational project on networking. Hence, source code readability should be focused on when developing Level-IP.

TODO: Actual style guidelines, so far I have been just winging it.

# Features

The aim is an introduction to TCP/IP stacks, so readability is preferred over
performance.

## Current Features

* One interface (IP 10.0.0.4, gateway 10.0.0.5) on a TAP device
* Ethernet II frame handling
* ARP request/reply, simple caching
* IPv4: header and length validation, checksum, unique datagram ids
* IPv4 fragment reassembly (30 s timeout, overlapping fragments dropped)
* IPv4 fragmentation of oversized datagrams without DF
* ICMPv4: echo reply, checksum validation, Destination Unreachable (protocol, port)
* One static route table with default gateway
* TCP: handshake, data transfer, RFC 793 "Segment Arrives", checksum validation
* TCP: RFC 6298 retransmission timeout with Karn's algorithm, RFC 793 user timeout
* TCP: out-of-order queue and SACK blocks
* TCP: half-close with shutdown(SHUT_WR)
* TCP: RFC 6528 initial sequence numbers (keyed FNV-1a, not a cryptographic hash)
* UDP: datagram sockets, checksums, bind/connect/sendto/recvfrom, port unreachable
* Random free ephemeral ports (49152-65535) per protocol
* LD_PRELOAD shim for TCP and UDP sockets

## Known Limitations

* No TCP server side (listen/accept); bind only selects the local port
* UDP sendto waits up to 100 ms for ARP resolution while holding the socket lock
* Socket fds start at 4097 and are not reserved with the kernel
* select/ppoll are not intercepted; poll is emulated by polling every 10 ms

## Upcoming features

* TCP listen/accept
* TCP Window Management, Silly Window Syndrome Avoidance, Zero-Window Probes
* TCP Congestion Control
* ICMP errors delivered to sockets (e.g. ECONNREFUSED on connected UDP)
* Raw sockets (for arping, ping)
* 'select' socket API call
