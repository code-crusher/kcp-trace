//=====================================================================
//
// test_iqqueue.cpp - iqueue 遍历宏测试
//
// 验证遍历宏的命名与 Linux 内核 list_for_each / list_for_each_entry
// 的语义一致（见 issue #462）：
//   iqueue_foreach(pos, head)                     -> list_for_each
//   iqueue_foreach_entry(pos, head, TYPE, MEMBER) -> list_for_each_entry
//
// 编译：c++ test_iqqueue.cpp -o test_iqqueue
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>

#include "ikcp.h"


struct TESTITEM
{
	int value;
	struct IQUEUEHEAD node;
};


static int failures = 0;

#define CHECK(cond) do { \
	if (!(cond)) { \
		printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
		failures++; \
	} } while (0)


static void test_foreach(void)
{
	struct IQUEUEHEAD head;
	struct TESTITEM items[4];
	struct IQUEUEHEAD *pos;
	struct TESTITEM *item;
	int i, count, sum;

	IQUEUE_INIT(&head);
	for (i = 0; i < 4; i++) {
		items[i].value = i;
		IQUEUE_INIT(&items[i].node);
		iqueue_add_tail(&items[i].node, &head);
	}

	// iqueue_foreach: 遍历原始节点，类似 list_for_each
	count = 0;
	sum = 0;
	iqueue_foreach(pos, &head) {
		item = iqueue_entry(pos, TESTITEM, node);
		CHECK(item->value == count);
		sum += item->value;
		count++;
	}
	CHECK(count == 4);
	CHECK(sum == 6);

	// iqueue_foreach_entry: 遍历包含对象，类似 list_for_each_entry
	count = 0;
	iqueue_foreach_entry(item, &head, TESTITEM, node) {
		CHECK(item->value == count);
		count++;
	}
	CHECK(count == 4);

	// 大写版本
	count = 0;
	IQUEUE_FOREACH(pos, &head) {
		CHECK(pos != &head);
		count++;
	}
	CHECK(count == 4);

	count = 0;
	IQUEUE_FOREACH_ENTRY(item, &head, TESTITEM, node) {
		CHECK(item->value == count);
		count++;
	}
	CHECK(count == 4);
}


static void test_foreach_empty(void)
{
	struct IQUEUEHEAD head;
	struct IQUEUEHEAD *pos;
	struct TESTITEM *item;
	int count = 0;

	IQUEUE_INIT(&head);

	iqueue_foreach(pos, &head) {
		count++;
	}
	CHECK(count == 0);

	iqueue_foreach_entry(item, &head, TESTITEM, node) {
		count++;
	}
	CHECK(count == 0);

	IQUEUE_FOREACH(pos, &head) {
		count++;
	}
	CHECK(count == 0);

	IQUEUE_FOREACH_ENTRY(item, &head, TESTITEM, node) {
		count++;
	}
	CHECK(count == 0);
}


int main(void)
{
	test_foreach();
	test_foreach_empty();

	if (failures != 0) {
		printf("iqueue test failed: %d failure(s)\n", failures);
		return 1;
	}

	printf("iqueue test passed\n");
	return 0;
}
