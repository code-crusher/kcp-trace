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
	delete vnet;
	printf("press enter to next ...\n");
	if (isatty(0)) { char ch; scanf("%c", &ch); }
}

//=====================================================================
// 回归测试：issue #437 (ikcp_release crash in android)
//
// 两个目标：
// 1. 在发送队列满载时销毁 kcp 会话（对应
//    Session_C_Destroy -> ikcp_release 的使用场景），
//    ikcp_release 必须能安全清理所有步骤。
// 2. 分配失败（ikcp_segment_new 返回 NULL）时
//    ikcp_input 必须返回错误而不是崩溃，
//    之后的 ikcp_release 也不得崩溃。
//=====================================================================

static int fail_alloc = 0;	// 剩余可分配次数，<0 表示不限

static void* test_malloc(size_t size)
{
	if (fail_alloc == 0) return malloc(size);
	if (fail_alloc > 0) {
		fail_alloc--;
		return NULL;
	}
	return malloc(size);
}

static void test_free(void* ptr)
{
	free(ptr);
}

static int issue437_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
	(void)buf; (void)len; (void)kcp; (void)user;
	return 0;
}

// 构造一个完整的 PUSH 数据包：conv + 头部 + len 字节载荷
static int issue437_build_push(char *out, IUINT32 conv, IUINT32 sn,
	const char *payload, IUINT32 len)
{
	char *p = out;
	memcpy(p, &conv, 4); p += 4;
	*p++ = (char)81;			// cmd: IKCP_CMD_PUSH
	*p++ = (char)0;				// frg
	{ unsigned short wnd = 32; memcpy(p, &wnd, 2); p += 2; }
	{ IUINT32 v = 0; memcpy(p, &v, 4); p += 4; }		// ts
	memcpy(p, &sn, 4); p += 4;					// sn
	{ IUINT32 v = 0; memcpy(p, &v, 4); p += 4; }	// una
	memcpy(p, &len, 4); p += 4;					// len
	if (len > 0 && payload) {
		memcpy(p, payload, len); p += len;
	}
	return (int)(p - out);
}

static void test_issue_437(void)
{
	char payload[512];
	char packet[1024];
	ikcpcb *kcp;
	int i, hr;

	memset(payload, 0xa5, sizeof(payload));
	printf("==== issue #437 regression tests ====\n");

	// ---- 场景 1：队列满载时销毁会话 ----
	// 对应 android 上 Session_C_Destroy 的使用方式：
	// 创建、发送、（未完全 flush）、销毁
	for (i = 0; i < 100; i++) {
		kcp = ikcp_create(0x11223344, (void*)0);
		assert(kcp != NULL);
		kcp->output = issue437_output;
		ikcp_send(kcp, payload, (int)sizeof(payload));
		ikcp_send(kcp, payload, 300);
		if ((i & 1) == 0) ikcp_update(kcp, 1);	// 一半会话有未 flush 的发送队列
		ikcp_release(kcp);
	}
	printf("[PASS] release with queued segments: 100 cycles\n");

	// ---- 场景 2：分配失败下的 ikcp_input 与 ikcp_release ----
	kcp = ikcp_create(0x11223344, (void*)0);
	assert(kcp != NULL);
	kcp->output = issue437_output;

	// 先注入一个合法的 PUSH 包，让设备处于已知状态
	hr = issue437_build_push(packet, 0x11223344, 0, payload, 100);
	ikcp_input(kcp, packet, hr);
	ikcp_update(kcp, 1);

	// 接下来注入新的 PUSH 时强制 segment 分配失败：
	// ikcp_input 应返回负值，而不是解引用空指针
	ikcp_allocator(test_malloc, test_free);
	fail_alloc = 1;		// 下一次分配（segment）失败
	hr = issue437_build_push(packet, 0x11223344, 1, payload, 100);
	{
		int ret = ikcp_input(kcp, packet, hr);
		if (ret >= 0) {
			printf("[FAIL] ikcp_input should fail on allocation failure\n");
			exit(1);
		}
	}
	// acklist 扩容也可能失败：那条路径会 abort，
	// 这里不测该路径，只确保主路径的健壮性
	fail_alloc = 0;

	// 分配失败之后，会话仍应能正常销毁
	ikcp_release(kcp);
	printf("[PASS] input with failed allocation, then release\n");

	// 恢复默认分配器，免影响后续测试
	ikcp_allocator(NULL, NULL);

	// ---- 场景 3：非法/损坏输入下的安全性（老版本崩溃的缘由） ----
	// len 为负数或超过实际数据长度的包必须被拒绝
	kcp = ikcp_create(0x11223344, (void*)0);
	assert(kcp != NULL);
	kcp->output = issue437_output;
	{
		// len 字段伪造为巨大值
		hr = issue437_build_push(packet, 0x11223344, 0, payload, 100);
		{ IUINT32 bad = 0x7fffffff; memcpy(packet + 20, &bad, 4); }
		if (ikcp_input(kcp, packet, hr) >= 0) {
			printf("[FAIL] oversized len must be rejected\n");
			exit(1);
		}
		// len 字段伪造为高位置位（形式上为负）
		hr = issue437_build_push(packet, 0x11223344, 0, payload, 100);
		{ IUINT32 bad = 0x80000000; memcpy(packet + 20, &bad, 4); }
		if (ikcp_input(kcp, packet, hr) >= 0) {
			printf("[FAIL] negative len must be rejected\n");
			exit(1);
		}
		// conv 不匹配
		hr = issue437_build_push(packet, 0x55667788, 0, payload, 100);
		if (ikcp_input(kcp, packet, hr) >= 0) {
			printf("[FAIL] mismatched conv must be rejected\n");
			exit(1);
		}
	}
	ikcp_release(kcp);
	printf("[PASS] malformed input rejected, session released\n");

	printf("==== issue #437 regression tests passed ====\n");
}

int main()
{
	test_issue_437();	// issue #437 回归测试
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


