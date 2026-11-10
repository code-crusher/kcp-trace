//=====================================================================
//
// test.cpp - kcp 测试用例
//
// 说明：
// gcc test.cpp -o test -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>

#include "test.h"
#include "ikcp.c"


// 模拟网络
LatencySimulator *vnet;

// 模拟网络：模拟发送一个 udp包
int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	union { int id; void *ptr; } parameter;
	parameter.ptr = user;
	vnet->send(parameter.id, buf, len);
	return 0;
}

// 测试用例
void test(int mode)
{
	// 创建模拟网络：丢包率10%，Rtt 60ms~125ms
	vnet = new LatencySimulator(10, 60, 125);

	// 创建两个端点的 kcp对象，第一个参数 conv是会话编号，同一个会话需要相同
	// 最后一个是 user参数，用来传递标识
	ikcpcb *kcp1 = ikcp_create(0x11223344, (void*)0);
	ikcpcb *kcp2 = ikcp_create(0x11223344, (void*)1);

	// 设置kcp的下层输出，这里为 udp_output，模拟udp网络输出函数
	kcp1->output = udp_output;
	kcp2->output = udp_output;

	IUINT32 current = iclock();
	IUINT32 slap = current + 20;
	IUINT32 index = 0;
	IUINT32 next = 0;
	IINT64 sumrtt = 0;
	int count = 0;
	int maxrtt = 0;

	// 配置窗口大小：平均延迟200ms，每20ms发送一个包，
	// 而考虑到丢包重发，设置最大收发窗口为128
	ikcp_wndsize(kcp1, 128, 128);
	ikcp_wndsize(kcp2, 128, 128);

	// 判断测试用例的模式
	if (mode == 0) {
		// 默认模式
		ikcp_nodelay(kcp1, 0, 10, 0, 0);
		ikcp_nodelay(kcp2, 0, 10, 0, 0);
	}
	else if (mode == 1) {
		// 普通模式，关闭流控等
		ikcp_nodelay(kcp1, 0, 10, 0, 1);
		ikcp_nodelay(kcp2, 0, 10, 0, 1);
	}	else {
		// 启动快速模式
		// 第二个参数 nodelay-启用以后若干常规加速将启动
		// 第三个参数 interval为内部处理时钟，默认设置为 10ms
		// 第四个参数 resend为快速重传指标，设置为2
		// 第五个参数 为是否禁用常规流控，这里禁止
		ikcp_nodelay(kcp1, 2, 10, 2, 1);
		ikcp_nodelay(kcp2, 2, 10, 2, 1);
		kcp1->rx_minrto = 10;
		kcp1->fastresend = 1;
	}


	char buffer[2000];
	int hr;

	IUINT32 ts1 = iclock();

	while (1) {
		isleep(1);
		current = iclock();
		ikcp_update(kcp1, iclock());
		ikcp_update(kcp2, iclock());

		// 每隔 20ms，kcp1发送数据
		for (; current >= slap; slap += 20) {
			((IUINT32*)buffer)[0] = index++;
			((IUINT32*)buffer)[1] = current;

			// 发送上层协议包
			ikcp_send(kcp1, buffer, 8);
		}

		// 处理虚拟网络：检测是否有udp包从p1->p2
		while (1) {
			hr = vnet->recv(1, buffer, 2000);
			if (hr < 0) break;
			// 如果 p2收到udp，则作为下层协议输入到kcp2
			ikcp_input(kcp2, buffer, hr);
		}

		// 处理虚拟网络：检测是否有udp包从p2->p1
		while (1) {
			hr = vnet->recv(0, buffer, 2000);
			if (hr < 0) break;
			// 如果 p1收到udp，则作为下层协议输入到kcp1
			ikcp_input(kcp1, buffer, hr);
		}

		// kcp2接收到任何包都返回回去
		while (1) {
			hr = ikcp_recv(kcp2, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			// 如果收到包就回射
			ikcp_send(kcp2, buffer, hr);
		}

		// kcp1收到kcp2的回射数据
		while (1) {
			hr = ikcp_recv(kcp1, buffer, 10);
			// 没有收到包就退出
			if (hr < 0) break;
			IUINT32 sn = *(IUINT32*)(buffer + 0);
			IUINT32 ts = *(IUINT32*)(buffer + 4);
			IUINT32 rtt = current - ts;
			
			if (sn != next) {
				// 如果收到的包不连续
				printf("ERROR sn %d<->%d\n", (int)count, (int)next);
				return;
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (IUINT32)maxrtt) maxrtt = rtt;

			printf("[RECV] mode=%d sn=%d rtt=%d\n", mode, (int)sn, (int)rtt);
		}
		if (next > 1000) break;
	}

	ts1 = iclock() - ts1;

	ikcp_release(kcp1);
	ikcp_release(kcp2);

	const char *names[3] = { "default", "normal", "fast" };
	printf("%s mode result (%dms):\n", names[mode], (int)ts1);
	printf("avgrtt=%d maxrtt=%d tx=%d\n", (int)(sumrtt / count), (int)maxrtt, (int)vnet->tx1);
	printf("press enter to next ...\n");
	fflush(stdout);
	char ch;
	// 非交互运行（管道 / CI）时 stdin 无数据，直接继续
	if (scanf("%c", &ch) != 1) {
		printf("\n");
	}
}

//---------------------------------------------------------------------
// 单元测试（issue #359）：拥塞窗口必须按被确认的分片数量增长，
// 而不是按 ikcp_input 的调用次数增长
//---------------------------------------------------------------------

// 单元测试使用的空输出函数（不经虚拟网络）
static int unit_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)buf; (void)len; (void)kcp; (void)user;
	return 0;
}

// 编码一个 IKCP_CMD_ACK 段到 buffer，返回编码长度
static int unit_encode_ack(char *buffer, IUINT32 sn, IUINT32 una, IUINT16 wnd)
{
	IUINT32 conv = 0x11223344;
	IUINT32 ts = 1000;
	IUINT32 len = 0;
	char *p = buffer;
	memcpy(p, &conv, 4); p += 4;
	*p++ = (char)IKCP_CMD_ACK;
	*p++ = 0;						// frg
	memcpy(p, &wnd, 2); p += 2;
	memcpy(p, &ts, 4); p += 4;
	memcpy(p, &sn, 4); p += 4;
	memcpy(p, &una, 4); p += 4;
	memcpy(p, &len, 4); p += 4;
	return (int)(p - buffer);
}

// 创建一个 kcp 对象，发送 segs 个分片并全部放入发送缓冲区
static ikcpcb *unit_new_kcp(int segs)
{
	ikcpcb *kcp = ikcp_create(0x11223344, (void*)0);
	char payload[100];
	int i;
	memset(payload, 'x', sizeof(payload));
	kcp->output = unit_output;
	for (i = 0; i < segs; i++) {
		ikcp_send(kcp, payload, sizeof(payload));
	}
	// 新建的 kcp 的 cwnd 为 0，flush 不会移动任何分片到 snd_buf，
	// 因此这里先给一个初始窗口
	kcp->cwnd = segs;
	ikcp_update(kcp, 1000);
	return kcp;
}

static void unit_expect(int ok, const char *what, long got, long want)
{
	if (!ok) {
		printf("unit test FAILED: %s got=%ld want=%ld\n", what, got, want);
		exit(1);
	}
}

static void test_cwnd_growth()
{
	char buffer[2048];
	int size, i;

	printf("== unit test: cwnd grows per acked segment (#359) ==\n");

	//--- 慢启动：一个数据报里捆绑 10 个 ack，cwnd 应增长 10 ---
	ikcpcb *kcp = unit_new_kcp(32);
	kcp->ssthresh = 1000;
	IUINT32 cwnd0 = kcp->cwnd;
	size = 0;
	for (i = 0; i < 10; i++) {
		size += unit_encode_ack(buffer + size, i, i + 1, 128);
	}
	ikcp_input(kcp, buffer, size);
	unit_expect(kcp->cwnd == cwnd0 + 10,
		"slow start: 10 acks in one datagram", (long)kcp->cwnd, (long)cwnd0 + 10);
	unit_expect(kcp->snd_una == 10, "slow start: snd_una", (long)kcp->snd_una, 10);
	ikcp_release(kcp);

	//--- 慢启动：单个 ack 只增长 1（保持原有行为）---
	kcp = unit_new_kcp(32);
	kcp->ssthresh = 1000;
	cwnd0 = kcp->cwnd;
	size = unit_encode_ack(buffer, 0, 1, 128);
	ikcp_input(kcp, buffer, size);
	unit_expect(kcp->cwnd == cwnd0 + 1,
		"slow start: single ack", (long)kcp->cwnd, (long)cwnd0 + 1);
	ikcp_release(kcp);

	//--- 慢启动：10 个 ack 分 10 个数据报到达与捆绑 1 个数据报等价 ---
	ikcpcb *kcp_a = unit_new_kcp(32);
	ikcpcb *kcp_b = unit_new_kcp(32);
	kcp_a->ssthresh = 1000;
	kcp_b->ssthresh = 1000;
	size = 0;
	for (i = 0; i < 10; i++) {
		size += unit_encode_ack(buffer + size, i, i + 1, 128);
	}
	ikcp_input(kcp_a, buffer, size);
	for (i = 0; i < 10; i++) {
		int one = unit_encode_ack(buffer, i, i + 1, 128);
		ikcp_input(kcp_b, buffer, one);
	}
	unit_expect(kcp_a->cwnd == kcp_b->cwnd,
		"slow start: batched == separate", (long)kcp_a->cwnd, (long)kcp_b->cwnd);
	unit_expect(kcp_a->incr == kcp_b->incr,
		"slow start: incr batched == separate", (long)kcp_a->incr, (long)kcp_b->incr);
	ikcp_release(kcp_a);
	ikcp_release(kcp_b);

	//--- 拥塞避免：批量 ack 与逐个 ack 等价，且窗口保持增长 ---
	kcp_a = unit_new_kcp(32);
	kcp_b = unit_new_kcp(32);
	kcp_a->cwnd = kcp_b->cwnd = 16;
	kcp_a->ssthresh = kcp_b->ssthresh = 16;		// cwnd == ssthresh: 进入拥塞避免
	kcp_a->incr = kcp_b->incr = 16 * kcp_a->mss;
	size = 0;
	for (i = 0; i < 10; i++) {
		size += unit_encode_ack(buffer + size, i, i + 1, 128);
	}
	ikcp_input(kcp_a, buffer, size);
	for (i = 0; i < 10; i++) {
		int one = unit_encode_ack(buffer, i, i + 1, 128);
		ikcp_input(kcp_b, buffer, one);
	}
	unit_expect(kcp_a->cwnd == kcp_b->cwnd,
		"congestion avoidance: batched == separate", (long)kcp_a->cwnd, (long)kcp_b->cwnd);
	unit_expect(kcp_a->incr == kcp_b->incr,
		"congestion avoidance: incr batched == separate", (long)kcp_a->incr, (long)kcp_b->incr);
	unit_expect(kcp_a->cwnd > 16,
		"congestion avoidance: cwnd grows", (long)kcp_a->cwnd, 17);
	ikcp_release(kcp_a);
	ikcp_release(kcp_b);

	//--- 远端窗口限流：增长不得超过 rmt_wnd ---
	kcp = unit_new_kcp(32);
	kcp->ssthresh = 1000;
	kcp->cwnd = 10;
	size = 0;
	for (i = 0; i < 10; i++) {
		size += unit_encode_ack(buffer + size, i, i + 1, 15);
	}
	ikcp_input(kcp, buffer, size);
	unit_expect(kcp->cwnd == 15, "rmt_wnd clamp: cwnd", (long)kcp->cwnd, 15);
	unit_expect(kcp->incr == 15 * kcp->mss, "rmt_wnd clamp: incr",
		(long)kcp->incr, (long)(15 * kcp->mss));
	ikcp_release(kcp);

	printf("== unit test passed ==\n");
}

int main()
{
	test_cwnd_growth();	// issue #359：cwnd 按确认分片数增长
	test(0);	// 默认模式，类似 TCP：正常模式，无快速重传，常规流控
	test(1);	// 普通模式，关闭流控等
	test(2);	// 快速模式，所有开关都打开，且关闭流控
	return 0;
}

/*
default mode result (20917ms):
avgrtt=740 maxrtt=1507

normal mode result (20131ms):
avgrtt=156 maxrtt=571

fast mode result (20207ms):
avgrtt=138 maxrtt=392
*/


