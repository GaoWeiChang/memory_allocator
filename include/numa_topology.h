#ifndef NUMA_TOPOLOGY_H
#define NUMA_TOPOLOGY_H

#include <stdbool.h>

/*
    NUMA topology helper

    machines with a single NUMA node (e.g. most laptops) can still exercise the NUMA-aware 
    code paths by enabling a "fake" topology, which lets tests control both which node 
    "this thread" runs on and which node freshly acquired blocks get tagged with.
*/

#define NUMA_NODE_UNKNOWN (-1)

int numa_current_node(void);
int numa_num_nodes(void);
int numa_node_for_new_block(void);

/* fake topology (testing on single-node machines) */
void numa_fake_enable(int num_nodes);
void numa_fake_disable(void);
bool numa_fake_enabled(void);

/* set the node reported as current for the running thread */
void numa_fake_set_current_node(int node);

/* sets the node that newly allocated blocks (from the OS) get stamped with */
void numa_fake_set_alloc_node(int node);

#endif
