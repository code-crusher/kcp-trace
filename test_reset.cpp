//=====================================================================
// test_reset.cpp - regression test for issue #439
// "服务端重启后链路无法恢复" (link cannot recover after server restart)
//
// Simulates a server restart in a two-endpoint KCP link:
//  1. client and server exchange messages (works fine);
//  2. the server kcp object is destroyed and recreated with the same
//     conv (simulating a server restart);
//  3. before the fix, the client keeps sending segments with the old,
//     increasing sn while the fresh server expects sn=0, so nothing
//     ever reaches ikcp_recv - the link is dead even though UDP
//     packets keep flowing;
//  4. with ikcp_reset() called on both ends (as a heartbeat/timeout
//     layer would do), the sequence space is rebased and the link
//     recovers without recreating anything.
//=====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>

#include "ikcp.c"

struct Packet {
	IUINT32 dest; // 0 = to client, 1 = to server
	char data[2048];
	int size;
};

static std::deque<Packet> g_packets;

struct Endpoint {
	ikcpcb *kcp;
	IUINT32 id;
};

static int relay_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)kcp;
	IUINT32 from = (IUINT32)(size_t)user;
	Packet p;
	p.dest = from ^ 1;
	memcpy(p.data, buf, len);
	p.size = len;
	g_packets.push_back(p);
	return 0;
}

static Endpoint *make_endpoint(IUINT32 id, IUINT32 conv)
{
	Endpoint *e = new Endpoint;
	e->id = id;
	e->kcp = ikcp_create(conv, (void *)(size_t)id);
	e->kcp->output = relay_output;
	ikcp_nodelay(e->kcp, 1, 10, 2, 1);
	ikcp_wndsize(e->kcp, 128, 128);
	return e;
}

// drive both endpoints and the network for n milliseconds of virtual time
static IUINT32 g_now;
static void pump(Endpoint *client, Endpoint *server, int ms)
{
	IUINT32 end = g_now + (IUINT32)ms;
	while (g_now < end) {
		ikcp_update(client->kcp, g_now);
		ikcp_update(server->kcp, g_now);
		while (!g_packets.empty()) {
			Packet p = g_packets.front();
			g_packets.pop_front();
			ikcp_input(p.dest == 0 ? client->kcp : server->kcp, p.data, p.size);
		}
		g_now += 5;
	}
}

static void drain_and_count(Endpoint *e, int &messages)
{
	char buf[2048];
	while (1) {
		int n = ikcp_recv(e->kcp, buf, sizeof(buf));
		if (n < 0) break;
		messages++;
	}
}

static int test_link_works_initially(Endpoint *client, Endpoint *server)
{
	const char *msg = "hello";
	int received = 0;

	ikcp_send(client->kcp, msg, (int)strlen(msg));
	pump(client, server, 300);
	drain_and_count(server, received);
	if (received != 1) {
		fprintf(stderr, "FAIL: initial delivery, got %d messages\n", received);
		return 1;
	}
	printf("ok: initial delivery works\n");
	return 0;
}

static int test_link_broken_after_server_restart()
{
	// fresh pair
	IUINT32 conv = 0x11223344;
	Endpoint *client = make_endpoint(0, conv);
	Endpoint *server = make_endpoint(1, conv);

	// establish traffic so the client advances its send sn
	const char *msg = "ping";
	for (int i = 0; i < 5; i++) {
		ikcp_send(client->kcp, msg, (int)strlen(msg));
		pump(client, server, 100);
		int got = 0;
		drain_and_count(server, got);
	}

	// server restarts: new kcp object, same conv, state lost
	ikcp_release(server->kcp);
	server->kcp = ikcp_create(conv, (void *)(size_t)server->id);
	server->kcp->output = relay_output;
	ikcp_nodelay(server->kcp, 1, 10, 2, 1);
	ikcp_wndsize(server->kcp, 128, 128);

	// client keeps sending; data must NOT arrive at the fresh server
	ikcp_send(client->kcp, msg, (int)strlen(msg));
	pump(client, server, 500);
	int got = 0;
	drain_and_count(server, got);
	if (got != 0) {
		fprintf(stderr, "UNEXPECTED: fresh server received %d messages "
			"(sequence state unexpectedly compatible)\n", got);
		ikcp_release(client->kcp);
		ikcp_release(server->kcp);
		delete client; delete server;
		return 1;
	}
	printf("reproduced: after server restart, server receives nothing (link broken)\n");

	// fix: reset both ends (as a heartbeat/timeout layer would do)
	ikcp_reset(client->kcp);
	ikcp_reset(server->kcp);

	int sent = 0, recvd = 0;
	for (int i = 0; i < 5; i++) {
		ikcp_send(client->kcp, msg, (int)strlen(msg));
		sent++;
		pump(client, server, 100);
		int got2 = 0;
		drain_and_count(server, got2);
		recvd += got2;
	}
	// also verify the reverse direction works (server -> client)
	pump(client, server, 200);

	if (recvd == 0) {
		fprintf(stderr, "FAIL: after ikcp_reset, server still receives nothing\n");
		ikcp_release(client->kcp);
		ikcp_release(server->kcp);
		delete client; delete server;
		return 1;
	}
	printf("ok: after ikcp_reset on both ends, server received %d/%d messages\n",
			 recvd, sent);

	// verify client can also receive from server after the reset
	const char *reply = "pong";
	ikcp_send(server->kcp, reply, (int)strlen(reply));
	pump(client, server, 300);
	int got3 = 0;
	drain_and_count(client, got3);
	if (got3 != 1) {
		fprintf(stderr, "FAIL: client did not receive server's message after reset (got %d)\n", got3);
		ikcp_release(client->kcp);
		ikcp_release(server->kcp);
		delete client; delete server;
		return 1;
	}
	printf("ok: client received server's message after reset\n");

	ikcp_release(client->kcp);
	ikcp_release(server->kcp);
	delete client; delete server;
	return 0;
}

static int test_reset_keeps_unsent_data()
{
	IUINT32 conv = 0x55667788;
	Endpoint *client = make_endpoint(0, conv);
	Endpoint *server = make_endpoint(1, conv);

	// fill the send queue without flushing (never been sent)
	const char *msg = "queued";
	ikcp_send(client->kcp, msg, (int)strlen(msg));
	// reset before any flush so the segment is still in snd_queue
	ikcp_reset(client->kcp);

	// after reset, snd_nxt must be 0 and the queued packet must still be pending
	if (client->kcp->snd_nxt != 0) {
		fprintf(stderr, "FAIL: snd_nxt not rewound (got %u)\n", client->kcp->snd_nxt);
		return 1;
	}
	if (ikcp_waitsnd(client->kcp) != 1) {
		fprintf(stderr, "FAIL: unsent data lost across reset (waitsnd=%d)\n",
				ikcp_waitsnd(client->kcp));
		return 1;
	}

	pump(client, server, 300);
	int got = 0;
	drain_and_count(server, got);
	if (got != 1) {
		fprintf(stderr, "FAIL: queued data not delivered after reset (got %d)\n", got);
		return 1;
	}
	printf("ok: data queued before reset is delivered after reset\n");

	ikcp_release(client->kcp);
	ikcp_release(server->kcp);
	delete client; delete server;
	return 0;
}

int main()
{
	g_now = 100000;
	int fails = 0;

	// part 1: sanity check that a fresh pair works
	{
		Endpoint *client = make_endpoint(0, 0x11223344);
		Endpoint *server = make_endpoint(1, 0x11223344);
		fails += test_link_works_initially(client, server);
		ikcp_release(client->kcp);
		ikcp_release(server->kcp);
		delete client; delete server;
	}

	// part 2: reproduce issue #439 and verify ikcp_reset recovers the link
	fails += test_link_broken_after_server_restart();

	// part 3: unsent data survives a reset
	fails += test_reset_keeps_unsent_data();

	if (fails == 0) {
		printf("ALL RESET TESTS PASSED\n");
		return 0;
	}
	fprintf(stderr, "%d reset test(s) FAILED\n", fails);
	return 1;
}
