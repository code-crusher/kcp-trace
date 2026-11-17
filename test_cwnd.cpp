//=====================================================================
//
// test_cwnd.cpp - regression tests for congestion window growth
//
// Issue #436: on a LAN, a request/response workload at 33 packets/sec
// stays at 30-60ms with a few connections, but with many connections
// (and therefore a little UDP loss) latency grows past one second, while
// the same workload over TCP stays at ~30ms.
//
// Root cause: a single incoming packet normally carries ACKs for several
// segments, because one ikcp_flush() stamps every segment it sends with
// the same timestamp.  The builtin congestion control grew cwnd only once
// per ikcp_input() call instead of once per ACKed segment, so the window
// opened several times slower than TCP.  After any loss collapsed cwnd to
// 1, the send queue backed up and latency reached seconds.
//
// gcc test_cwnd.cpp -o test_cwnd -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ikcp.h"
#include "ikcp.c"


static int failures = 0;

// Sink for datagrams produced by the endpoints under test.
static int discard_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)buf; (void)len; (void)kcp; (void)user;
	return 0;
}

#define CHECK(cond, fmt, ...) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
		failures++; \
	} \
} while (0)


//---------------------------------------------------------------------
// Build one UDP datagram carrying ACKs for segments [0, count).
//
// Every ACK shares one timestamp, exactly like the ACKs produced by a
// single ikcp_flush().  `una` acknowledges the whole range at once.
//---------------------------------------------------------------------
static long build_ack_packet(char *buf, IUINT32 conv, IUINT32 una,
		IUINT32 ts, int count)
{
	char *p = buf;
	int i;
	for (i = 0; i < count; i++) {
		IKCPSEG seg;
		memset(&seg, 0, sizeof(seg));
		seg.conv = conv;
		seg.cmd = IKCP_CMD_ACK;
		seg.frg = 0;
		seg.wnd = 256;
		seg.ts = ts;
		seg.sn = (IUINT32)i;
		seg.una = una;
		seg.len = 0;
		p = ikcp_encode_seg(p, &seg);
	}
	return (long)(p - buf);
}


//---------------------------------------------------------------------
// A single datagram ACKing N segments must grow cwnd by N in slow start.
//---------------------------------------------------------------------
static void test_cwnd_grows_per_acked_segment()
{
	const int N = 8;
	char ackpkt[IKCP_OVERHEAD * 16];
	char payload[100];
	long acklen;
	IUINT32 before, after;
	ikcpcb *kcp;
	int i;

	kcp = ikcp_create(0x11223344, NULL);
	kcp->output = discard_output;

	// Fill snd_buf with N one-segment messages.  Congestion control is
	// disabled while queuing so that all of them are put in flight.
	ikcp_nodelay(kcp, 0, 10, 0, 1);
	ikcp_wndsize(kcp, 256, 256);
	memset(payload, 'x', sizeof(payload));
	for (i = 0; i < N; i++) {
		ikcp_send(kcp, payload, sizeof(payload));
	}
	kcp->updated = 1;
	ikcp_flush(kcp);
	CHECK(kcp->nsnd_buf == (IUINT32)N,
			"expected %d segments in flight, got %u", N, kcp->nsnd_buf);

	// Now enable congestion control in slow start and feed one packet
	// that ACKs all N segments.
	ikcp_nodelay(kcp, 0, 10, 0, 0);
	kcp->cwnd = 1;
	kcp->ssthresh = 128;
	kcp->incr = kcp->mss;

	before = kcp->cwnd;
	acklen = build_ack_packet(ackpkt, kcp->conv, (IUINT32)N,
			kcp->current, N);
	ikcp_input(kcp, ackpkt, acklen);
	after = kcp->cwnd;

	CHECK(after - before == (IUINT32)N,
			"cwnd grew by %u for %d ACKed segments, expected %d "
			"(grows per packet, not per segment)", after - before, N, N);
	CHECK(kcp->snd_una == (IUINT32)N,
			"expected snd_una=%d, got %u", N, kcp->snd_una);

	ikcp_release(kcp);
}


//---------------------------------------------------------------------
// Congestion avoidance must also account for every ACKed segment, not
// just the first one in the packet.
//---------------------------------------------------------------------
static void test_cwnd_growth_scales_with_acked_segments()
{
	const int N = 8;
	char ackpkt[IKCP_OVERHEAD * 16];
	char payload[100];
	IUINT32 before, after;
	ikcpcb *kcp;
	int i;

	kcp = ikcp_create(0x11223344, NULL);
	kcp->output = discard_output;
	ikcp_nodelay(kcp, 0, 10, 0, 1);
	ikcp_wndsize(kcp, 256, 256);
	memset(payload, 'x', sizeof(payload));
	for (i = 0; i < N; i++) {
		ikcp_send(kcp, payload, sizeof(payload));
	}
	kcp->updated = 1;
	ikcp_flush(kcp);

	// Congestion avoidance: cwnd is at/above ssthresh, so growth is
	// driven by the additive-increase accumulator.  Make the window small
	// enough that a single packet of N ACKs is a large fraction of it.
	ikcp_nodelay(kcp, 0, 10, 0, 0);
	kcp->cwnd = 2;
	kcp->ssthresh = 2;
	kcp->incr = kcp->cwnd * kcp->mss;

	before = kcp->cwnd;
	ikcp_input(kcp, ackpkt, build_ack_packet(ackpkt, kcp->conv,
			(IUINT32)N, kcp->current, N));
	after = kcp->cwnd;

	// With one update per packet the accumulator gains only a single
	// mss^2/incr step; with one update per ACKed segment it gains N of
	// them, so cwnd must reach the next segment boundary.
	CHECK(after > before,
			"cwnd did not grow at all for %d ACKed segments in "
			"congestion avoidance (%u -> %u)", N, before, after);

	ikcp_release(kcp);
}


//---------------------------------------------------------------------
// Integration: the workload from issue #436.
//
// A client sends a 14-byte request every 30ms and the server echoes it
// immediately over a LAN-like link (1-2ms, small random loss).  Before
// the fix a single lost packet collapsed cwnd to 1 and it recovered so
// slowly that queued requests waited seconds; now latency stays bounded.
//---------------------------------------------------------------------

struct Pkt {
	IUINT32 due;
	char data[1600];
	int len;
};

static Pkt c2s[4096], s2c[4096];
static int c2s_n, s2c_n;
static IUINT32 vnow;
static double vloss;
static unsigned vseed = 12345;
static int vlost;

static unsigned vrnd()
{
	vseed = vseed * 1103515245u + 12345u;
	return (vseed >> 8) & 0xffffff;
}

static int voutput(const char *buf, int len, ikcpcb *kcp, void *user)
{
	int side = (int)(size_t)user;
	Pkt *q, *p;
	int *qn;
	(void)kcp;

	if (vloss > 0 && (vrnd() / (double)0xffffff) < vloss) {
		vlost++;
		return 0;
	}
	if (side == 0) { q = c2s; qn = &c2s_n; }
	else { q = s2c; qn = &s2c_n; }
	p = &q[*qn];
	p->due = vnow + 1 + (vrnd() % 2);
	p->len = len;
	memcpy(p->data, buf, len);
	(*qn)++;
	return 0;
}

static void deliver(Pkt *q, int *qn, ikcpcb *kcp)
{
	int i = 0;
	while (i < *qn) {
		if ((IINT32)(vnow - q[i].due) >= 0) {
			ikcp_input(kcp, q[i].data, q[i].len);
			memmove(&q[i], &q[i + 1], (size_t)(*qn - i - 1) * sizeof(Pkt));
			(*qn)--;
		}	else {
			i++;
		}
	}
}

static int cmp_int(const void *a, const void *b)
{
	int x = *(const int*)a, y = *(const int*)b;
	return (x > y) - (x < y);
}

static void test_echo_latency_under_loss()
{
	ikcpcb *cli, *srv;
	IUINT32 t0, nextsend;
	int seq = 0, n = 0, i;
	int lat[1024];
	int p50;

	vnow = 1000000;
	vloss = 0.02;
	vlost = 0;
	c2s_n = s2c_n = 0;

	cli = ikcp_create(0x11, (void*)(size_t)0);
	srv = ikcp_create(0x11, (void*)(size_t)1);
	cli->output = voutput;
	srv->output = voutput;

	// KCP defaults, as in the report: interval 100ms, congestion
	// control and flow control enabled, no fast retransmit.
	ikcp_nodelay(cli, 0, 100, 0, 0);
	ikcp_nodelay(srv, 0, 100, 0, 0);
	ikcp_wndsize(cli, 256, 256);
	ikcp_wndsize(srv, 256, 256);

	t0 = vnow;
	nextsend = t0;

	// Send requests for 20s, then keep the link running until the tail
	// drains so that a slow-but-correct run is not mistaken for data loss.
	while (seq < 600 || c2s_n > 0 || s2c_n > 0 ||
			ikcp_waitsnd(cli) > 0 || ikcp_waitsnd(srv) > 0) {
		int sending = (IINT32)(vnow - t0) < 20000 && seq < 600;
		if (!sending && (IINT32)(vnow - t0) > 60000) break;

		while (sending && (IINT32)(vnow - nextsend) >= 0 && seq < 600) {
			IUINT32 h[2];
			h[0] = (IUINT32)seq++;
			h[1] = vnow;
			ikcp_send(cli, (const char*)h, 8);
			nextsend += 30;
		}

		ikcp_update(cli, vnow);
		ikcp_update(srv, vnow);
		deliver(c2s, &c2s_n, srv);
		deliver(s2c, &s2c_n, cli);

		{
			char buf[256];
			int hr;
			while ((hr = ikcp_recv(srv, buf, sizeof(buf))) > 0) {
				ikcp_send(srv, buf, hr);
			}
			while ((hr = ikcp_recv(cli, buf, sizeof(buf))) > 0) {
				if (n < 1024) lat[n++] = (int)(vnow - *(IUINT32*)(buf + 4));
			}
		}
		vnow++;
	}

	CHECK(vlost > 0, "test link dropped nothing, loss injection is broken");
	CHECK(n == seq, "delivered %d of %d echoes in 20s", n, seq);

	qsort(lat, (size_t)n, sizeof(int), cmp_int);
	p50 = n ? lat[n / 2] : 0;

	// Before the fix this was ~2950ms with loss; after it, ~180ms.
	CHECK(p50 < 1000, "median echo latency %dms at 2%% loss, expected "
			"under 1000ms (cwnd recovery too slow)", p50);

	printf("  echo workload: %d echoes, %d packets lost, p50=%dms\n",
			n, vlost, p50);

	ikcp_release(cli);
	ikcp_release(srv);
}


int main()
{
	printf("test_cwnd: congestion window growth (issue #436)\n");

	test_cwnd_grows_per_acked_segment();
	test_cwnd_growth_scales_with_acked_segments();
	test_echo_latency_under_loss();

	if (failures) {
		printf("test_cwnd: %d check(s) FAILED\n", failures);
		return 1;
	}
	printf("test_cwnd: all checks passed\n");
	return 0;
}
