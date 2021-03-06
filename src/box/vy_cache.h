#ifndef INCLUDES_TARANTOOL_BOX_VY_CACHE_H
#define INCLUDES_TARANTOOL_BOX_VY_CACHE_H
/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>
#include <stdbool.h>

#include <small/rlist.h>

#include "index.h" /* enum iterator_type */
#include "vy_stmt.h" /* for comparators */
#include "vy_stmt_iterator.h" /* struct vy_stmt_iterator */
#include "vy_quota.h"
#include "small/mempool.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/**
 * A record in tuple cache
 */
struct vy_cache_entry {
	/* Cache */
	struct vy_cache *cache;
	/* Statement in cache */
	struct tuple *stmt;
	/* Link in LRU list */
	struct rlist in_lru;
	/* VY_CACHE_LEFT_LINKED and/or VY_CACHE_RIGHT_LINKED, see
	 * description of them for more information */
	uint32_t flags;
	/* Number of parts in key when the value was the first in EQ search */
	uint8_t left_boundary_level;
	/* Number of parts in key when the value was the last in EQ search */
	uint8_t right_boundary_level;
};

/**
 * Internal comparator (1) for BPS tree.
 */
static inline int
vy_cache_tree_cmp(struct vy_cache_entry *a,
		  struct vy_cache_entry *b, struct key_def *key_def)
{
	return vy_stmt_compare(a->stmt, b->stmt, key_def);
}

/**
 * Internal comparator (2) for BPS tree.
 */
static inline int
vy_cache_tree_key_cmp(struct vy_cache_entry *a,
		      const struct tuple *b, struct key_def *key_def)
{
	return vy_stmt_compare(a->stmt, b, key_def);
}

#define VY_CACHE_TREE_EXTENT_SIZE (16 * 1024)

#define BPS_TREE_NAME vy_cache_tree
#define BPS_TREE_BLOCK_SIZE 512
#define BPS_TREE_EXTENT_SIZE VY_CACHE_TREE_EXTENT_SIZE
#define BPS_TREE_COMPARE(a, b, index) vy_cache_tree_cmp(a, b, index)
#define BPS_TREE_COMPARE_KEY(a, b, index) vy_cache_tree_key_cmp(a, b, index)
#define bps_tree_elem_t struct vy_cache_entry *
#define bps_tree_key_t const struct tuple *
#define bps_tree_arg_t struct key_def *
#define BPS_TREE_NO_DEBUG

#include "salad/bps_tree.h"

#undef BPS_TREE_NAME
#undef BPS_TREE_BLOCK_SIZE
#undef BPS_TREE_EXTENT_SIZE
#undef BPS_TREE_COMPARE
#undef BPS_TREE_COMPARE_KEY
#undef bps_tree_elem_t
#undef bps_tree_key_t
#undef bps_tree_arg_t
#undef BPS_TREE_NO_DEBUG

/**
 * Environment of the cache
 */
struct vy_cache_env {
	/** Common LRU list of read cache. The first element is the newest */
	struct rlist cache_lru;
	/** Common quota for read cache */
	struct vy_quota quota;
	/** Common mempool for vy_cache_entry struct */
	struct mempool cache_entry_mempool;
	/** Number of cached tuples */
	size_t cached_count;
};

/**
 * Initialize common cache environment.
 * @param e - the environment.
 * @param slab_cache - source of memory.
 * @param mem_quota - memory limit for the cache.
 */
void
vy_cache_env_create(struct vy_cache_env *env, struct slab_cache *slab_cache,
		    uint64_t mem_quota);

/**
 * Destroy and free resources of cache environment.
 * @param e - the environment.
 */
void
vy_cache_env_destroy(struct vy_cache_env *e);

/**
 * Tuple cache (of one particular index)
 */
struct vy_cache {
	/* Key definition for tuple comparison */
	struct key_def *key_def;
	/* Tree of cache entries */
	struct vy_cache_tree cache_tree;
	/* The vesrion of state of cache_tree. Increments on every change */
	uint32_t version;
	/* Saved pointer to common cache environment */
	struct vy_cache_env *env;
};

/**
 * Allocate and initialize tuple cache.
 * @param env - pointer to common cache environment.
 * @param key_def - key definition for tuple comparison.
 * @retval - new tuple cache.
 */
struct vy_cache *
vy_cache_new(struct vy_cache_env *env, struct key_def *key_def);

/**
 * Destroy and deallocate tuple cache.
 * @param cache - pointer to tuple cache to destroy.
 */
void
vy_cache_delete(struct vy_cache *cache);

/**
 * Add a value to the cache. Can be used only if the reader read the latest
 * data (vlsn = INT64_MAX).
 * @param cache - pointer to tuple cache.
 * @param stmt - statement that was recently read and should be added to the
 * cache.
 * @param prev_stmt - previous statement that was read by the reader in one
 * sequence (by one iterator).
 * @param direction - direction in which the reader (iterator) observes data,
 *  +1 - forward, -1 - backward.
 */
void
vy_cache_add(struct vy_cache *cache, struct tuple *stmt,
	     struct tuple *prev_stmt, const struct tuple *key,
	     enum iterator_type order);

/**
 * Invalidate possibly cached value due to its overwriting
 * @param cache - pointer to tuple cache.
 * @param stmt - overwritten statement.
 */
void
vy_cache_on_write(struct vy_cache *cache, struct tuple *stmt);


/**
 * Cache iterator
 */
struct vy_cache_iterator {
	/** Parent class, must be the first member */
	struct vy_stmt_iterator base;
	/** Iterator usage statistics */
	struct vy_iterator_stat *stat;
	/* The cache */
	struct vy_cache *cache;

	/**
	 * Iterator type, that specifies direction, start position and stop
	 * criteria if the key is not specified, GT and EQ are changed to
	 * GE, LT to LE for beauty.
	 */
	enum iterator_type iterator_type;
	/* Search key data in terms of vinyl, vy_stmt_compare_raw argument */
	const struct tuple *key;
	/* LSN visibility, iterator shows values with lsn <= vlsn */
	const int64_t *vlsn;

	/* State of iterator */
	/* Current position in tree */
	struct vy_cache_tree_iterator curr_pos;
	/* stmt in current position in tree */
	struct tuple *curr_stmt;

	/* Last version of cache */
	uint32_t version;
	/* Is false until first .._get or .._next_.. method is called */
	bool search_started;
};

/**
 * Open an iterator over cache.
 * @param itr - iterator to open.
 * @param cache - the cache.
 * @param iterator_type - iterator type (EQ, GT, GE, LT, LE or ALL)
 * @param key - search key data in terms of vinyl, vy_stmt_compare argument
 * @param vlsn - LSN visibility, iterator shows values with lsn <= vlsn
 */
void
vy_cache_iterator_open(struct vy_cache_iterator *itr,
		       struct vy_iterator_stat *stat, struct vy_cache *cache,
		       enum iterator_type iterator_type,
		       const struct tuple *key, const int64_t *vlsn);

#if defined(__cplusplus)
} /* extern "C" { */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_CACHE_H */
