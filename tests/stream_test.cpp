//=====================================================================
//
// stream_test.cpp - tests for ikcp_send fragment limits
//
// Issue #431: ikcp_send rejected buffers larger than 127 MSS even in
// stream mode, although stream mode never groups fragments into
// messages (every segment is sent with frg == 0).
//
// Build & run: make test
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <string>
#include <vector>

#include "ikcp.h"

// ikcp.c keeps this private, it is 128 by default
#define WND_RCV 128

static int g_failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
		g_failures++; \
	} \
} while (0)


//---------------------------------------------------------------------
// a minimal in-memory UDP link, optionally lossy / reordering
//---------------------------------------------------------------------
struct Link
{
	std::deque<std::string> packets;
	int loss_percent;		// deterministic drop rate, 0 = reliable
	unsigned seed;

	Link(int loss = 0, unsigned seed = 1)
		: loss_percent(loss), seed(seed) {}

	bool drop()
	{
		if (loss_percent <= 0) return false;
		seed = seed * 1103515245u + 12345u;
		return (int)((seed >> 16) % 100) < loss_percent;
	}
};

static int link_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	Link *link = (Link*)user;
	(void)kcp;
	if (!link->drop()) {
		link->packets.push_back(std::string(buf, len));
	}
	return 0;
}

static void link_deliver(Link *link, ikcpcb *kcp)
{
	while (!link->packets.empty()) {
		std::string pkt = link->packets.front();
		link->packets.pop_front();
		ikcp_input(kcp, pkt.data(), (long)pkt.size());
	}
}


//---------------------------------------------------------------------
// send a large buffer in stream mode and verify it is delivered intact
//---------------------------------------------------------------------
static void test_stream_large_send()
{
	printf("[TEST] stream mode accepts more than 127 MSS in one call\n");

	const IUINT32 conv = 0x11223344;
	Link down, up;

	ikcpcb *kcp1 = ikcp_create(conv, &down);
	ikcpcb *kcp2 = ikcp_create(conv, &up);
	ikcp_setoutput(kcp1, link_output);
	ikcp_setoutput(kcp2, link_output);

	kcp1->stream = 1;
	kcp2->stream = 1;
	ikcp_nodelay(kcp1, 0, 10, 0, 1);
	ikcp_nodelay(kcp2, 0, 10, 0, 1);
	ikcp_wndsize(kcp1, 128, 128);
	ikcp_wndsize(kcp2, 128, 128);

	int mss = (int)kcp1->mss;
	int total = mss * 300 + 17;		// far beyond WND_RCV * MSS

	std::vector<char> src(total);
	for (int i = 0; i < total; i++) {
		src[i] = (char)(i * 31 + 7);
	}

	// a single call must queue all the data in stream mode
	int sent = ikcp_send(kcp1, src.data(), total);
	CHECK(sent == total);
	CHECK((int)kcp1->nsnd_que == (total + mss - 1) / mss);

	// pump the link until everything has been received
	std::vector<char> dst;
	dst.reserve(total);
	std::vector<char> buf(mss);
	IUINT32 now = 0;
	int iter = 0;

	while ((int)dst.size() < total && iter < 20000) {
		now += 10;
		iter++;

		ikcp_update(kcp1, now);
		link_deliver(&down, kcp2);
		ikcp_update(kcp2, now);
		link_deliver(&up, kcp1);

		for (;;) {
			int hr = ikcp_recv(kcp2, buf.data(), (int)buf.size());
			if (hr < 0) break;
			dst.insert(dst.end(), buf.begin(), buf.begin() + hr);
		}
	}

	CHECK((int)dst.size() == total);
	if ((int)dst.size() == total) {
		CHECK(memcmp(src.data(), dst.data(), total) == 0);
	}

	ikcp_release(kcp1);
	ikcp_release(kcp2);
}


//---------------------------------------------------------------------
// multiple sends, including one larger than the old 127 MSS limit
//---------------------------------------------------------------------
static void test_stream_multiple_sends()
{
	printf("[TEST] stream mode handles mixed send sizes\n");

	const IUINT32 conv = 0x55667788;
	Link down, up;

	ikcpcb *kcp1 = ikcp_create(conv, &down);
	ikcpcb *kcp2 = ikcp_create(conv, &up);
	ikcp_setoutput(kcp1, link_output);
	ikcp_setoutput(kcp2, link_output);

	kcp1->stream = 1;
	kcp2->stream = 1;
	ikcp_nodelay(kcp1, 0, 10, 0, 1);
	ikcp_nodelay(kcp2, 0, 10, 0, 1);
	ikcp_wndsize(kcp1, 128, 128);
	ikcp_wndsize(kcp2, 128, 128);

	int mss = (int)kcp1->mss;
	const int chunks[] = { 1, 100, mss, mss + 1, mss * 200, 3, mss * 5 };
	const int nchunks = (int)(sizeof(chunks) / sizeof(chunks[0]));

	std::vector<char> src;
	for (int c = 0; c < nchunks; c++) {
		std::vector<char> chunk(chunks[c]);
		for (int i = 0; i < chunks[c]; i++) {
			chunk[i] = (char)(src.size() + i);
		}
		int sent = ikcp_send(kcp1, chunk.data(), chunks[c]);
		CHECK(sent == chunks[c]);
		src.insert(src.end(), chunk.begin(), chunk.end());
	}

	int total = (int)src.size();
	std::vector<char> dst;
	dst.reserve(total);
	std::vector<char> buf(mss);
	IUINT32 now = 0;
	int iter = 0;

	while ((int)dst.size() < total && iter < 20000) {
		now += 10;
		iter++;

		ikcp_update(kcp1, now);
		link_deliver(&down, kcp2);
		ikcp_update(kcp2, now);
		link_deliver(&up, kcp1);

		for (;;) {
			int hr = ikcp_recv(kcp2, buf.data(), (int)buf.size());
			if (hr < 0) break;
			dst.insert(dst.end(), buf.begin(), buf.begin() + hr);
		}
	}

	CHECK((int)dst.size() == total);
	if ((int)dst.size() == total) {
		CHECK(memcmp(src.data(), dst.data(), total) == 0);
	}

	ikcp_release(kcp1);
	ikcp_release(kcp2);
}


//---------------------------------------------------------------------
// a large stream must survive packet loss and retransmission
//---------------------------------------------------------------------
static void test_stream_large_send_lossy()
{
	printf("[TEST] stream mode large send survives packet loss\n");

	const IUINT32 conv = 0x13572468;
	Link down(10, 12345);		// 10% loss
	Link up(10, 54321);

	ikcpcb *kcp1 = ikcp_create(conv, &down);
	ikcpcb *kcp2 = ikcp_create(conv, &up);
	ikcp_setoutput(kcp1, link_output);
	ikcp_setoutput(kcp2, link_output);

	kcp1->stream = 1;
	kcp2->stream = 1;
	ikcp_nodelay(kcp1, 2, 10, 2, 1);
	ikcp_nodelay(kcp2, 2, 10, 2, 1);
	ikcp_wndsize(kcp1, 256, 256);
	ikcp_wndsize(kcp2, 256, 256);

	int mss = (int)kcp1->mss;
	int total = mss * 400 + 3;

	std::vector<char> src(total);
	for (int i = 0; i < total; i++) {
		src[i] = (char)(i * 17 + 3);
	}

	CHECK(ikcp_send(kcp1, src.data(), total) == total);

	std::vector<char> dst;
	dst.reserve(total);
	std::vector<char> buf(mss);
	IUINT32 now = 0;
	int iter = 0;

	while ((int)dst.size() < total && iter < 60000) {
		now += 10;
		iter++;

		ikcp_update(kcp1, now);
		link_deliver(&down, kcp2);
		ikcp_update(kcp2, now);
		link_deliver(&up, kcp1);

		for (;;) {
			int hr = ikcp_recv(kcp2, buf.data(), (int)buf.size());
			if (hr < 0) break;
			dst.insert(dst.end(), buf.begin(), buf.begin() + hr);
		}
	}

	CHECK((int)dst.size() == total);
	if ((int)dst.size() == total) {
		CHECK(memcmp(src.data(), dst.data(), total) == 0);
	}

	ikcp_release(kcp1);
	ikcp_release(kcp2);
}


//---------------------------------------------------------------------
// message mode must keep the one-message-per-window restriction
//---------------------------------------------------------------------
static void test_message_mode_limit()
{
	printf("[TEST] message mode keeps the 127 MSS per-call limit\n");

	ikcpcb *kcp = ikcp_create(0x99aabbcc, NULL);
	int mss = (int)kcp->mss;

	std::vector<char> buf(mss * WND_RCV, 0);

	kcp->stream = 0;
	CHECK(ikcp_send(kcp, buf.data(), mss * ((int)WND_RCV - 1))
		== mss * ((int)WND_RCV - 1));
	CHECK(ikcp_send(kcp, buf.data(), mss * (int)WND_RCV) == -2);

	ikcp_release(kcp);
}


//---------------------------------------------------------------------
// zero-length sends are still accepted in stream mode
//---------------------------------------------------------------------
static void test_stream_empty_send()
{
	printf("[TEST] stream mode accepts zero-length send\n");

	ikcpcb *kcp = ikcp_create(0xdeadbeef, NULL);
	kcp->stream = 1;

	CHECK(ikcp_send(kcp, NULL, 0) == 0);
	CHECK(kcp->nsnd_que == 0);

	// an empty send must not disturb queued data either
	std::vector<char> data(100, 'x');
	CHECK(ikcp_send(kcp, data.data(), 100) == 100);
	CHECK(ikcp_send(kcp, NULL, 0) == 0);
	CHECK(kcp->nsnd_que == 1);

	ikcp_release(kcp);
}


int main()
{
	test_stream_large_send();
	test_stream_multiple_sends();
	test_stream_large_send_lossy();
	test_message_mode_limit();
	test_stream_empty_send();

	if (g_failures != 0) {
		printf("\n%d check(s) FAILED\n", g_failures);
		return 1;
	}

	printf("\nall tests passed\n");
	return 0;
}
