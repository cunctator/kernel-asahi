// SPDX-License-Identifier: GPL-2.0
/*
 * KUnit tests for devres_node_remove().
 *
 * The return value of devres_node_remove() decides who owns a node. The Rust
 * Devres<T> container drops the reference held by the device's list only if
 * the function returns true, so it must return false for a node that devres
 * has already taken off the list in order to release it.
 */

#include <kunit/device.h>
#include <kunit/test.h>

#include <linux/device.h>

#include "../base.h"

struct test_node {
	struct devres_node node;
	int nr_released;
	int nr_freed;
	bool remove_in_release;
	bool removed_in_release;
};

static void test_node_release(struct device *dev, struct devres_node *node)
{
	struct test_node *tn = container_of(node, struct test_node, node);

	tn->nr_released++;
	if (tn->remove_in_release)
		tn->removed_in_release = devres_node_remove(dev, node);
}

static void test_node_free(struct devres_node *node)
{
	struct test_node *tn = container_of(node, struct test_node, node);

	tn->nr_freed++;
}

/* The memory is owned by the test, so nodes stay valid after being "freed". */
static struct test_node *test_node_alloc(struct kunit *test)
{
	struct test_node *tn;

	tn = kunit_kzalloc(test, sizeof(*tn), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tn);

	devres_node_init(&tn->node, test_node_release, test_node_free);
	devres_set_node_dbginfo(&tn->node, "test_node", sizeof(*tn));

	return tn;
}

static struct device *test_device(struct kunit *test)
{
	struct device *dev;

	dev = kunit_device_register(test, "devres-node-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);

	return dev;
}

static void remove_never_added_test(struct kunit *test)
{
	struct device *dev = test_device(test);
	struct test_node *tn = test_node_alloc(test);

	KUNIT_EXPECT_FALSE(test, devres_node_remove(dev, &tn->node));
}

/* A removed node must not be released, and its neighbours must be. */
static void remove_added_test(struct kunit *test)
{
	struct device *dev = test_device(test);
	struct test_node *first = test_node_alloc(test);
	struct test_node *middle = test_node_alloc(test);
	struct test_node *last = test_node_alloc(test);

	devres_node_add(dev, &first->node);
	devres_node_add(dev, &middle->node);
	devres_node_add(dev, &last->node);

	KUNIT_EXPECT_TRUE(test, devres_node_remove(dev, &middle->node));
	KUNIT_EXPECT_FALSE(test, devres_node_remove(dev, &middle->node));

	devres_release_all(dev);

	KUNIT_EXPECT_EQ(test, middle->nr_released, 0);
	KUNIT_EXPECT_EQ(test, middle->nr_freed, 0);
	KUNIT_EXPECT_EQ(test, first->nr_released, 1);
	KUNIT_EXPECT_EQ(test, first->nr_freed, 1);
	KUNIT_EXPECT_EQ(test, last->nr_released, 1);
	KUNIT_EXPECT_EQ(test, last->nr_freed, 1);
}

/*
 * While devres_release_all() is running, the node is on a private list and its
 * entry is not empty. This is the state that a concurrent Devres<T> drop sees.
 */
static void remove_during_release_all_test(struct kunit *test)
{
	struct device *dev = test_device(test);
	struct test_node *first = test_node_alloc(test);
	struct test_node *middle = test_node_alloc(test);
	struct test_node *last = test_node_alloc(test);

	middle->remove_in_release = true;
	middle->removed_in_release = true;

	devres_node_add(dev, &first->node);
	devres_node_add(dev, &middle->node);
	devres_node_add(dev, &last->node);

	devres_release_all(dev);

	KUNIT_EXPECT_FALSE(test, middle->removed_in_release);
	KUNIT_EXPECT_EQ(test, first->nr_released, 1);
	KUNIT_EXPECT_EQ(test, middle->nr_released, 1);
	KUNIT_EXPECT_EQ(test, last->nr_released, 1);
	KUNIT_EXPECT_EQ(test, first->nr_freed, 1);
	KUNIT_EXPECT_EQ(test, middle->nr_freed, 1);
	KUNIT_EXPECT_EQ(test, last->nr_freed, 1);
}

/*
 * The private list is gone once devres_release_all() has returned, but nothing
 * reinitializes the entry, so it still looks like it is on a list.
 */
static void remove_after_release_all_test(struct kunit *test)
{
	struct device *dev = test_device(test);
	struct test_node *tn = test_node_alloc(test);

	devres_node_add(dev, &tn->node);
	devres_release_all(dev);

	KUNIT_EXPECT_FALSE(test, devres_node_remove(dev, &tn->node));
	KUNIT_EXPECT_EQ(test, tn->nr_released, 1);
	KUNIT_EXPECT_EQ(test, tn->nr_freed, 1);
}

/* Releasing a group must affect the nodes in it, and only those. */
static void remove_during_release_group_test(struct kunit *test)
{
	struct device *dev = test_device(test);
	struct test_node *outside = test_node_alloc(test);
	struct test_node *inside = test_node_alloc(test);
	void *group;

	inside->remove_in_release = true;
	inside->removed_in_release = true;

	devres_node_add(dev, &outside->node);

	group = devres_open_group(dev, NULL, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, group);
	devres_node_add(dev, &inside->node);
	devres_close_group(dev, group);

	KUNIT_EXPECT_EQ(test, devres_release_group(dev, group), 1);

	KUNIT_EXPECT_FALSE(test, inside->removed_in_release);
	KUNIT_EXPECT_EQ(test, inside->nr_released, 1);
	KUNIT_EXPECT_EQ(test, inside->nr_freed, 1);
	KUNIT_EXPECT_FALSE(test, devres_node_remove(dev, &inside->node));

	KUNIT_EXPECT_EQ(test, outside->nr_released, 0);
	KUNIT_EXPECT_TRUE(test, devres_node_remove(dev, &outside->node));
	KUNIT_EXPECT_EQ(test, outside->nr_released, 0);
}

static struct kunit_case devres_node_test_cases[] = {
	KUNIT_CASE(remove_never_added_test),
	KUNIT_CASE(remove_added_test),
	KUNIT_CASE(remove_during_release_all_test),
	KUNIT_CASE(remove_after_release_all_test),
	KUNIT_CASE(remove_during_release_group_test),
	{}
};

static struct kunit_suite devres_node_test_suite = {
	.name = "devres_node",
	.test_cases = devres_node_test_cases,
};

kunit_test_suite(devres_node_test_suite);

MODULE_DESCRIPTION("KUnit tests for devres_node_remove()");
MODULE_LICENSE("GPL");
