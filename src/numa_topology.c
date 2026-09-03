#define _GNU_SOURCE
#include "../include/numa_topology.h"

#include <sched.h>

/* topology shape is process-wide... */
static bool fake_enabled = false;
static int fake_num_nodes = 1;

/* ...but "which node am I running on / allocating for" is per-thread,
   so a multi-threaded test can place each thread on a different node. */
static _Thread_local int fake_current_node = 0;
static _Thread_local int fake_alloc_node = 0;

int numa_current_node(void)
{
    if (fake_enabled)
        return fake_current_node;

#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    unsigned cpu = 0, node = 0;
    if (getcpu(&cpu, &node) == 0)
        return (int)node;
#endif
    return 0;
}

int numa_num_nodes(void)
{
    if (fake_enabled)
        return fake_num_nodes;
    return 1;
}

int numa_node_for_new_block(void)
{
    if (fake_enabled)
        return fake_alloc_node;
    return numa_current_node();
}

void numa_fake_enable(int num_nodes)
{
    fake_enabled = true;
    fake_num_nodes = (num_nodes > 0) ? num_nodes : 1;
    fake_current_node = 0;
    fake_alloc_node = 0;
}

void numa_fake_disable(void)
{
    fake_enabled = false;
    fake_num_nodes = 1;
    fake_current_node = 0;
    fake_alloc_node = 0;
}

bool numa_fake_enabled(void)
{
    return fake_enabled;
}

void numa_fake_set_current_node(int node)
{
    fake_current_node = node;
}

void numa_fake_set_alloc_node(int node)
{
    fake_alloc_node = node;
}
