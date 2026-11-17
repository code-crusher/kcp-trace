//=====================================================================
//
// test_rto.cpp - RTT/RTO sampling regression tests
//
// 说明：
// 覆盖 issue #465：FEC 恢复出的原始包在重传包已被确认之后才到达接收端，
// 它带回的 ack 使用原始发送时间戳，不应再参与 RTT 采样，否则会撑大 rto。
//
// g++ test_rto.cpp -o test_rto -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <string.h>

#include "test.h"
#include "ikcp.c"


static int failures = 0;
static int checks = 0;

#define CHECK_EQ(expr, expect) do { \
		long _v = (long)(expr); \
		long _e = (long)(expect); \
		checks++; \
		if (_v != _e) { \
			failures++; \
			printf("FAIL %s:%d: %s == %ld, expect %ld\n", \
				__FILE__, __LINE__, #expr, _v, _e); \
		} \
	} while (0)


// 最近一次 output 的数据包
static char lastpkt[2048];
static int lastlen = 0;

static int capture_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)kcp; (void)user;
	if (len > (int)sizeof(lastpkt)) len = (int)sizeof(lastpkt);
	memcpy(lastpkt, buf, len);
	lastlen = len;
	return 0;
}

// 构造一个 ack 包：ts 为被确认包的发送时间戳
static void make_ack(char *buf, int *plen, IUINT32 conv, IUINT32 ts,
		IUINT32 sn, IUINT32 una)
{
	char *p = buf;
	p = ikcp_encode32u(p, conv);
	p = ikcp_encode8u(p, (IUINT8)IKCP_CMD_ACK);
	p = ikcp_encode8u(p, 0);
	p = ikcp_encode16u(p, 128);
	p = ikcp_encode32u(p, ts);
	p = ikcp_encode32u(p, sn);
	p = ikcp_encode32u(p, una);
	p = ikcp_encode32u(p, 0);
	*plen = (int)(p - buf);
}

// 取最近一次发送的包的 ts 字段（数据包和 ack 包布局相同）
static IUINT32 last_sent_ts(void)
{
	IUINT32 ts = 0;
	if (lastlen >= (int)IKCP_OVERHEAD) {
		ikcp_decode32u(lastpkt + 8, &ts);
	}
	return ts;
}

// 创建一个 nodelay 的 kcp，minrto 为 30ms
static ikcpcb* make_kcp(void)
{
	ikcpcb *kcp = ikcp_create(0x11223344, NULL);
	kcp->output = capture_output;
	ikcp_nodelay(kcp, 1, 10, 0, 1);
	ikcp_wndsize(kcp, 128, 128);
	kcp->rx_rto = 30;
	return kcp;
}

static void feed_ack(ikcpcb *kcp, IUINT32 now, IUINT32 ts, IUINT32 sn, IUINT32 una)
{
	char ack[64];
	int alen;
	ikcp_update(kcp, now);
	make_ack(ack, &alen, kcp->conv, ts, sn, una);
	ikcp_input(kcp, ack, alen);
}


// 正常的顺序 ack 必须照常参与 RTT 采样
static void test_inorder_ack_samples_rtt(void)
{
	ikcpcb *kcp = make_kcp();
	char data[8] = { 0 };

	ikcp_send(kcp, data, 8);
	ikcp_update(kcp, 0);
	CHECK_EQ(last_sent_ts(), 0);

	feed_ack(kcp, 20, 0, 0, 1);
	CHECK_EQ(kcp->rx_srtt, 20);
	CHECK_EQ(kcp->rx_rttval, 10);
	CHECK_EQ(kcp->rx_rto, 60);
	CHECK_EQ(kcp->nsnd_buf, 0);

	ikcp_release(kcp);
}


// issue #465：重传包的 ack 已到达并完成采样后，FEC 恢复出的原始包的 ack
// 带着旧时间戳到达，不得再修改 rtt/rto
static void test_stale_ack_after_retransmit_acked(void)
{
	ikcpcb *kcp = make_kcp();
	char data[8] = { 0 };
	IINT32 srtt, rttval, rto;

	ikcp_send(kcp, data, 8);
	ikcp_update(kcp, 0);
	CHECK_EQ(last_sent_ts(), 0);

	// t=30 超时重传，段的时间戳更新为 30
	ikcp_update(kcp, 30);
	CHECK_EQ(last_sent_ts(), 30);

	// t=40 收到重传包的 ack，rtt=10
	feed_ack(kcp, 40, 30, 0, 1);
	CHECK_EQ(kcp->rx_srtt, 10);
	CHECK_EQ(kcp->rx_rto, 30);
	CHECK_EQ(kcp->nsnd_buf, 0);

	srtt = kcp->rx_srtt;
	rttval = kcp->rx_rttval;
	rto = kcp->rx_rto;

	// t=110 FEC 恢复了原始包，带回 ts=0 的陈旧 ack
	feed_ack(kcp, 110, 0, 0, 1);
	CHECK_EQ(kcp->rx_srtt, srtt);
	CHECK_EQ(kcp->rx_rttval, rttval);
	CHECK_EQ(kcp->rx_rto, rto);

	ikcp_release(kcp);
}


// 重传包还在途时收到原始包的陈旧 ack，同样不得采样；
// 该 ack 仍然证明数据已送达，段会被正常确认掉，后续包的采样不受影响
static void test_stale_ack_while_retransmit_in_flight(void)
{
	ikcpcb *kcp = make_kcp();
	char data[8] = { 0 };

	ikcp_send(kcp, data, 8);
	ikcp_update(kcp, 0);
	ikcp_update(kcp, 30);	// 重传，段 ts=30

	// ts=0 的 ack 指的是已经被重传覆盖的原始包
	feed_ack(kcp, 40, 0, 0, 1);
	CHECK_EQ(kcp->rx_srtt, 0);
	CHECK_EQ(kcp->rx_rto, 30);
	CHECK_EQ(kcp->nsnd_buf, 0);

	// 后续新包的 ack 仍然正常采样
	ikcp_send(kcp, data, 8);
	ikcp_update(kcp, 50);
	feed_ack(kcp, 60, 50, 1, 2);
	CHECK_EQ(kcp->rx_srtt, 10);
	CHECK_EQ(kcp->nsnd_buf, 0);

	ikcp_release(kcp);
}


// 重复 ack（同一个包的旧 ack）不得反复采样
static void test_duplicate_ack_not_sampled(void)
{
	ikcpcb *kcp = make_kcp();
	char data[8] = { 0 };
	IINT32 srtt, rto;

	ikcp_send(kcp, data, 8);
	ikcp_update(kcp, 0);
	feed_ack(kcp, 20, 0, 0, 1);
	CHECK_EQ(kcp->rx_srtt, 20);

	srtt = kcp->rx_srtt;
	rto = kcp->rx_rto;

	// 同一 ack 在 t=200 重复到达
	feed_ack(kcp, 200, 0, 0, 1);
	CHECK_EQ(kcp->rx_srtt, srtt);
	CHECK_EQ(kcp->rx_rto, rto);

	ikcp_release(kcp);
}


int main()
{
	test_inorder_ack_samples_rtt();
	test_stale_ack_after_retransmit_acked();
	test_stale_ack_while_retransmit_in_flight();
	test_duplicate_ack_not_sampled();

	if (failures == 0) {
		printf("test_rto: all %d checks passed\n", checks);
		return 0;
	}

	printf("test_rto: %d/%d checks failed\n", failures, checks);
	return 1;
}
