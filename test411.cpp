//=====================================================================
//
// test411.cpp - regression tests for issue #411
//
// covers:
//   1. ikcp_setmaxwaitsnd caps nsnd_buf + nsnd_que, ikcp_send returns
//      -2 (EAGAIN) once the limit is reached, and the queue recovers
//      when the limit is raised or disabled (0 = unlimited).
//   2. default behavior (no limit set) is unchanged: a black-hole peer
//      still lets the send queue grow without bound (documented in the
//      wiki: apps must throttle via ikcp_waitsnd).
//   3. dead-link handling: segments hitting dead_link are flagged via
//      kcp->state = (IUINT32)-1 for the upper layer, while remaining in
//      snd_buf (they must, for in-order delivery); memory is bounded by
//      the send window and fully reclaimed by ikcp_release.
//
// build: gcc test411.cpp -o test411 -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"
#include "ikcp.c"

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAILED: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		tests_failed++; \
	} \
} while (0)

// allocator to track live segments
static long live_count = 0;
static void *counting_malloc(size_t size) { live_count++; return malloc(size); }
static void counting_free(void *ptr) { live_count--; free(ptr); }

// a network that swallows everything: the peer never hears anything
static int blackhole_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)buf; (void)len; (void)kcp; (void)user;
	return 0;
}

// local network instance for the live-peer test (test.cpp's global
// 'vnet' stays untouched by the other tests)
static LatencySimulator *vnet2 = NULL;

// udp output routed to the local simulator instance
static int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	union { int id; void *ptr; } parameter;
	parameter.ptr = user;
	vnet2->send(parameter.id, buf, len);
	return 0;
}

//---------------------------------------------------------------------
// test: max_waitsnd caps the send queue on a dead peer (issue point 2)
//---------------------------------------------------------------------
static void test_maxwaitsnd_deadlink(void)
{
	ikcpcb *kcp;
	char msg[100];
	IUINT32 current = 0;
	int i, accepted = 0, eagain = 0;

	memset(msg, 'x', sizeof(msg));

	// track segment allocations
	ikcp_allocator(counting_malloc, counting_free);
	live_count = 0;

	kcp = ikcp_create(0x1, (void*)0);
	kcp->output = blackhole_output;
	ikcp_wndsize(kcp, 1024, 1024);
	ikcp_nodelay(kcp, 1, 10, 2, 1);
	ikcp_setmaxwaitsnd(kcp, 64);

	// pump: 100 msgs every 100ms into the void, sender stays alive 10s
	for (i = 0; i < 1000; i++) {
		int j;
		ikcp_update(kcp, current);
		if (i % 10 == 0) {
			for (j = 0; j < 100; j++) {
				int r = ikcp_send(kcp, msg, 100);
				if (r >= 0) accepted++;
				else if (r == -2) eagain++;
			}
		}
		current += 10;
	}

	// the queue is capped at exactly the limit
	CHECK(accepted == 64);
	CHECK(eagain == 10000 - 64);
	CHECK(ikcp_waitsnd(kcp) == 64);
	CHECK(kcp->nsnd_buf + kcp->nsnd_que == 64);

	// memory is bounded: 64 segments + the kcp object + its buffer
	CHECK(live_count <= 64 + 2);

	// raising the limit allows more data in
	ikcp_setmaxwaitsnd(kcp, 128);
	CHECK(ikcp_send(kcp, msg, 100) == 100);
	CHECK(ikcp_waitsnd(kcp) == 65);

	// setting 0 disables the limit entirely
	ikcp_setmaxwaitsnd(kcp, 0);
	CHECK(ikcp_send(kcp, msg, 100) == 100);
	CHECK(ikcp_waitsnd(kcp) == 66);

	// negative values also mean unlimited
	ikcp_setmaxwaitsnd(kcp, -5);
	CHECK(ikcp_send(kcp, msg, 100) == 100);
	CHECK(ikcp_waitsnd(kcp) == 67);

	ikcp_release(kcp);
	ikcp_allocator(NULL, NULL);
	live_count = 0;

	tests_run++;
	printf("test_maxwaitsnd_deadlink ok (waitsnd capped at %d)\n", 64);
}

//---------------------------------------------------------------------
// test: without a limit the old behavior is unchanged (issue point 2)
//---------------------------------------------------------------------
static void test_unlimited_default(void)
{
	ikcpcb *kcp = ikcp_create(0x1, (void*)0);
	char msg[100];
	IUINT32 current = 0;
	int i, accepted = 0;

	memset(msg, 'x', sizeof(msg));
	kcp->output = blackhole_output;
	ikcp_wndsize(kcp, 1024, 1024);
	ikcp_nodelay(kcp, 1, 10, 2, 1);

	// no ikcp_setmaxwaitsnd call: everything must be accepted
	for (i = 0; i < 1000; i++) {
		ikcp_update(kcp, current);
		if (ikcp_send(kcp, msg, 100) >= 0) accepted++;
		current += 10;
	}

	// snd_buf stays bounded by min(snd_wnd, rmt_wnd default) = 128,
	// the rest piles up in snd_queue: waitsnd == accepted
	CHECK(kcp->nsnd_buf <= 128);
	CHECK(ikcp_waitsnd(kcp) == accepted);

	ikcp_release(kcp);
	tests_run++;
	printf("test_unlimited_default ok (waitsnd=%d, unbounded queue as before)\n",
		ikcp_waitsnd(kcp));
}

//---------------------------------------------------------------------
// test: dead_link sets state, segment stays in snd_buf for retransmit
// and in-order delivery; memory reclaimed on release (issue point 3)
// note: with exponential rto backoff, 20 transmissions need ~22min
// (nodelay=1) so the test drives kcp->current directly.
//---------------------------------------------------------------------
static void test_deadlink_state(void)
{
	ikcpcb *kcp;
	char msg[100];
	IUINT32 current = 0;
	int i;

	memset(msg, 'x', sizeof(msg));

	// install the allocator first so every allocation is counted
	ikcp_allocator(counting_malloc, counting_free);
	live_count = 0;

	kcp = ikcp_create(0x1, (void*)0);
	kcp->output = blackhole_output;
	ikcp_wndsize(kcp, 32, 32);
	ikcp_nodelay(kcp, 1, 10, 2, 1);

	// fill the whole send window (32 packets)
	for (i = 0; i < 32; i++) {
		CHECK(ikcp_send(kcp, msg, 100) == 100);
	}
	ikcp_update(kcp, current);

	// let time flow far beyond dead_link=20 transmissions:
	// rto grows 1.5x per retry from 200ms, the 20th transmission
	// happens after ~22 minutes of virtual time
	for (i = 0; i < 180000; i++) {	/* 1800s = 30min */
		ikcp_update(kcp, current);
		current += 10;
	}

	// state must be flagged as dead for the upper layer
	CHECK(kcp->state == (IUINT32)-1);
	// the segments must still be in snd_buf (in-order delivery + rto)
	CHECK(kcp->nsnd_buf == 32);
	// transmission counts must have exceeded dead_link
	CHECK(kcp->xmit >= (IUINT32)(32 * 20));

	// memory stays bounded: exactly the 32 windowed segments + kcp
	// object + output buffer
	CHECK(live_count == 32 + 2);

	// all memory is reclaimed on release
	ikcp_release(kcp);
	CHECK(live_count == 0);
	ikcp_allocator(NULL, NULL);
	live_count = 0;

	tests_run++;
	printf("test_deadlink_state ok (state dead-linked, memory reclaimed)\n");
}

//---------------------------------------------------------------------
// test: with a live peer, normal sending is never blocked by the limit
// (must not regress the common path)
//---------------------------------------------------------------------
static void test_livepeer_not_blocked(void)
{
	LatencySimulator *vnet = new LatencySimulator(0, 30, 60);
	ikcpcb *kcp1 = ikcp_create(0x11223344, (void*)0);
	ikcpcb *kcp2 = ikcp_create(0x11223344, (void*)1);
	char buffer[2000];
	int hr;
	IUINT32 current = iclock();
	IUINT32 slap = current;
	IUINT32 next = 0;
	int received = 0;

	kcp1->output = udp_output;
	kcp2->output = udp_output;
	vnet2 = vnet;

	ikcp_wndsize(kcp1, 128, 128);
	ikcp_wndsize(kcp2, 128, 128);
	ikcp_nodelay(kcp1, 1, 10, 2, 1);
	ikcp_nodelay(kcp2, 1, 10, 2, 1);

	// set a limit equal to the full window: nothing should ever be
	// rejected as long as the peer keeps acking
	ikcp_setmaxwaitsnd(kcp1, 128);

	// run 3 seconds of 20ms-period messages
	{
		IUINT32 deadline = current + 3000;
		while (current < deadline) {
		isleep(1);
		current = iclock();
		ikcp_update(kcp1, current);
		ikcp_update(kcp2, current);

		for (; current >= slap; slap += 20) {
			((IUINT32*)buffer)[0] = next;
			ikcp_send(kcp1, buffer, 8);
			next++;
		}

		while ((hr = vnet->recv(1, buffer, 2000)) >= 0) {
			ikcp_input(kcp2, buffer, hr);
		}
		while ((hr = vnet->recv(0, buffer, 2000)) >= 0) {
			ikcp_input(kcp1, buffer, hr);
		}
		while ((hr = ikcp_recv(kcp2, buffer, 2000)) >= 0) {
			received++;
		}
		}
	}

	// no data was lost to the limit on a healthy link
	CHECK(received > 100);

	ikcp_release(kcp1);
	ikcp_release(kcp2);
	delete vnet;
	vnet2 = NULL;

	tests_run++;
	printf("test_livepeer_not_blocked ok (received=%d)\n", received);
}

int main(void)
{
	test_maxwaitsnd_deadlink();
	test_unlimited_default();
	test_deadlink_state();
	test_livepeer_not_blocked();

	printf("\n%d tests, %d failures\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
