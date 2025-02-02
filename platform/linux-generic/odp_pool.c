/* Copyright (c) 2013-2018, Linaro Limited
 * Copyright (c) 2019-2021, Nokia
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */

#include <odp/api/pool.h>
#include <odp/api/shared_memory.h>
#include <odp/api/align.h>
#include <odp/api/ticketlock.h>
#include <odp/api/system_info.h>
#include <odp/api/plat/thread_inlines.h>

#include <odp_pool_internal.h>
#include <odp_init_internal.h>
#include <odp_packet_internal.h>
#include <odp_packet_dpdk.h>
#include <odp_config_internal.h>
#include <odp_debug_internal.h>
#include <odp_ring_ptr_internal.h>
#include <odp_global_data.h>
#include <odp_libconfig_internal.h>
#include <odp_shm_internal.h>
#include <odp_timer_internal.h>
#include <odp_event_vector_internal.h>

#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <inttypes.h>

#include <odp/api/plat/pool_inline_types.h>
#include <odp/api/plat/ticketlock_inlines.h>
#define LOCK(a)      odp_ticketlock_lock(a)
#define UNLOCK(a)    odp_ticketlock_unlock(a)
#define LOCK_INIT(a) odp_ticketlock_init(a)

#define RING_SIZE_MIN     64
#define POOL_MAX_NUM_MIN  RING_SIZE_MIN

/* Make sure packet buffers don't cross huge page boundaries starting from this
 * page size. 2MB is typically the smallest used huge page size. */
#define FIRST_HP_SIZE (2 * 1024 * 1024)

/* Define a practical limit for contiguous memory allocations */
#define MAX_SIZE   (10 * 1024 * 1024)

ODP_STATIC_ASSERT(CONFIG_PACKET_SEG_LEN_MIN >= 256,
		  "ODP Segment size must be a minimum of 256 bytes");

ODP_STATIC_ASSERT(CONFIG_PACKET_SEG_SIZE < 0xffff,
		  "Segment size must be less than 64k (16 bit offsets)");

/* Thread local variables */
typedef struct pool_local_t {
	pool_cache_t *cache[ODP_CONFIG_POOLS];
	int thr_id;

} pool_local_t;

pool_global_t *_odp_pool_glb;
static __thread pool_local_t local;

#include <odp/visibility_begin.h>

/* Fill in pool header field offsets for inline functions */
const _odp_pool_inline_offset_t _odp_pool_inline ODP_ALIGNED_CACHE = {
	.pool_hdl          = offsetof(pool_t, pool_hdl),
	.uarea_size        = offsetof(pool_t, param_uarea_size)
};

#include <odp/visibility_end.h>

static inline odp_pool_t pool_index_to_handle(uint32_t pool_idx)
{
	return _odp_cast_scalar(odp_pool_t, pool_idx + 1);
}

static inline pool_t *pool_from_buf(odp_buffer_t buf)
{
	odp_buffer_hdr_t *buf_hdr = buf_hdl_to_hdr(buf);

	return buf_hdr->pool_ptr;
}

static inline void cache_init(pool_cache_t *cache)
{
	memset(cache, 0, sizeof(pool_cache_t));
}

static inline uint32_t cache_pop(pool_cache_t *cache,
				 odp_buffer_hdr_t *buf_hdr[], int max_num)
{
	uint32_t cache_num = cache->cache_num;
	uint32_t num_ch = max_num;
	uint32_t cache_begin;
	uint32_t i;

	/* Cache does not have enough buffers */
	if (odp_unlikely(cache_num < (uint32_t)max_num))
		num_ch = cache_num;

	/* Get buffers from the cache */
	cache_begin = cache_num - num_ch;
	for (i = 0; i < num_ch; i++)
		buf_hdr[i] = cache->buf_hdr[cache_begin + i];

	cache->cache_num = cache_num - num_ch;

	return num_ch;
}

static inline void cache_push(pool_cache_t *cache, odp_buffer_hdr_t *buf_hdr[],
			      uint32_t num)
{
	uint32_t cache_num = cache->cache_num;
	uint32_t i;

	for (i = 0; i < num; i++)
		cache->buf_hdr[cache_num + i] = buf_hdr[i];

	cache->cache_num = cache_num + num;
}

static void cache_flush(pool_cache_t *cache, pool_t *pool)
{
	odp_buffer_hdr_t *buf_hdr;
	ring_ptr_t *ring;
	uint32_t mask;

	ring = &pool->ring->hdr;
	mask = pool->ring_mask;

	while (cache_pop(cache, &buf_hdr, 1))
		ring_ptr_enq(ring, mask, buf_hdr);
}

static inline uint64_t cache_total_available(pool_t *pool)
{
	uint64_t cached = 0;
	int i;

	for (i = 0; i < ODP_THREAD_COUNT_MAX; i++)
		cached += pool->local_cache[i].cache_num;

	return cached;
}

static int read_config_file(pool_global_t *pool_glb)
{
	uint32_t local_cache_size, burst_size, align;
	const char *str;
	int val = 0;

	ODP_PRINT("Pool config:\n");

	str = "pool.local_cache_size";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	if (val > CONFIG_POOL_CACHE_MAX_SIZE || val < 0) {
		ODP_ERR("Bad value %s = %i, max %i\n", str, val,
			CONFIG_POOL_CACHE_MAX_SIZE);
		return -1;
	}

	local_cache_size = val;
	pool_glb->config.local_cache_size = local_cache_size;
	ODP_PRINT("  %s: %i\n", str, val);

	str = "pool.burst_size";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	if (val <= 0) {
		ODP_ERR("Bad value %s = %i\n", str, val);
		return -1;
	}

	burst_size = val;
	pool_glb->config.burst_size = burst_size;
	ODP_PRINT("  %s: %i\n", str, val);

	/* Check local cache size and burst size relation */
	if (local_cache_size % burst_size) {
		ODP_ERR("Pool cache size not multiple of burst size\n");
		return -1;
	}

	if (local_cache_size && (local_cache_size / burst_size < 2)) {
		ODP_ERR("Cache burst size too large compared to cache size\n");
		return -1;
	}

	str = "pool.pkt.max_num";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	if (val > CONFIG_POOL_MAX_NUM || val < POOL_MAX_NUM_MIN) {
		ODP_ERR("Bad value %s = %i\n", str, val);
		return -1;
	}

	pool_glb->config.pkt_max_num = val;
	ODP_PRINT("  %s: %i\n", str, val);

	str = "pool.pkt.max_len";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	if (val <= 0) {
		ODP_ERR("Bad value %s = %i\n", str, val);
		return -1;
	}

	pool_glb->config.pkt_max_len = val;
	ODP_PRINT("  %s: %i\n", str, val);

	str = "pool.pkt.base_align";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	align = val;
	if (val == 0)
		align = ODP_CACHE_LINE_SIZE;

	if (!CHECK_IS_POWER2(align)) {
		ODP_ERR("Not a power of two: %s = %i\n", str, val);
		return -1;
	}

	pool_glb->config.pkt_base_align = align;
	ODP_PRINT("  %s: %u\n", str, align);

	str = "pool.buf.min_align";
	if (!_odp_libconfig_lookup_int(str, &val)) {
		ODP_ERR("Config option '%s' not found.\n", str);
		return -1;
	}

	align = val;
	if (val == 0)
		align = ODP_CACHE_LINE_SIZE;

	if (!CHECK_IS_POWER2(align)) {
		ODP_ERR("Not a power of two: %s = %i\n", str, val);
		return -1;
	}

	pool_glb->config.buf_min_align = align;
	ODP_PRINT("  %s: %u\n", str, align);

	ODP_PRINT("\n");

	return 0;
}

int _odp_pool_init_global(void)
{
	uint32_t i;
	odp_shm_t shm;

	shm = odp_shm_reserve("_odp_pool_global",
			      sizeof(pool_global_t),
			      ODP_CACHE_LINE_SIZE,
			      0);

	_odp_pool_glb = odp_shm_addr(shm);

	if (_odp_pool_glb == NULL)
		return -1;

	memset(_odp_pool_glb, 0, sizeof(pool_global_t));
	_odp_pool_glb->shm = shm;

	if (read_config_file(_odp_pool_glb)) {
		odp_shm_free(shm);
		_odp_pool_glb = NULL;
		return -1;
	}

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool_t *pool = pool_entry(i);

		LOCK_INIT(&pool->lock);
		pool->pool_hdl = pool_index_to_handle(i);
		pool->pool_idx = i;
	}

	ODP_DBG("\nPool init global\n");
	ODP_DBG("  buffer_hdr_t size              %zu\n", sizeof(odp_buffer_hdr_t));
	ODP_DBG("  packet_hdr_t size              %zu\n", sizeof(odp_packet_hdr_t));
	ODP_DBG("  timeout_hdr_t size             %zu\n", sizeof(odp_timeout_hdr_t));
	ODP_DBG("  event_vector_hdr_t size        %zu\n", sizeof(odp_event_vector_hdr_t));
	ODP_DBG("  packet_hdr_t::seg_data offset  %zu\n", offsetof(odp_packet_hdr_t, seg_data));
	ODP_DBG("  packet_hdr_t::timestamp offset %zu\n", offsetof(odp_packet_hdr_t, timestamp));
	ODP_DBG("\n");
	return 0;
}

int _odp_pool_term_global(void)
{
	int i;
	pool_t *pool;
	int ret = 0;
	int rc = 0;

	if (_odp_pool_glb == NULL)
		return 0;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = pool_entry(i);

		LOCK(&pool->lock);
		if (pool->reserved) {
			ODP_ERR("Not destroyed pool: %s\n", pool->name);
			rc = -1;
		}
		UNLOCK(&pool->lock);
	}

	ret = odp_shm_free(_odp_pool_glb->shm);
	if (ret < 0) {
		ODP_ERR("SHM free failed\n");
		rc = -1;
	}

	return rc;
}

int _odp_pool_init_local(void)
{
	pool_t *pool;
	int i;
	int thr_id = odp_thread_id();

	memset(&local, 0, sizeof(pool_local_t));

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool           = pool_entry(i);
		local.cache[i] = &pool->local_cache[thr_id];
		cache_init(local.cache[i]);
	}

	local.thr_id = thr_id;
	return 0;
}

int _odp_pool_term_local(void)
{
	int i;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool_t *pool = pool_entry(i);

		cache_flush(local.cache[i], pool);
	}

	return 0;
}

static pool_t *reserve_pool(uint32_t shmflags, uint8_t pool_ext, uint32_t num)
{
	int i;
	odp_shm_t shm;
	uint32_t mem_size;
	pool_t *pool;
	char ring_name[ODP_POOL_NAME_LEN];

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = pool_entry(i);

		LOCK(&pool->lock);
		if (pool->reserved == 0) {
			pool->reserved = 1;
			UNLOCK(&pool->lock);

			memset(&pool->memset_mark, 0,
			       sizeof(pool_t) - offsetof(pool_t, memset_mark));
			sprintf(ring_name, "_odp_pool_ring_%d", i);

			/* Reserve memory for the ring, and for lookup table in case of pool ext */
			mem_size = sizeof(pool_ring_t);
			if (pool_ext)
				mem_size += num * sizeof(odp_buffer_hdr_t *);

			shm = odp_shm_reserve(ring_name, mem_size, ODP_CACHE_LINE_SIZE, shmflags);

			if (odp_unlikely(shm == ODP_SHM_INVALID)) {
				ODP_ERR("Unable to alloc pool ring %d\n", i);
				LOCK(&pool->lock);
				pool->reserved = 0;
				UNLOCK(&pool->lock);
				break;
			}

			pool->ring_shm = shm;
			pool->ring = odp_shm_addr(shm);
			pool->pool_ext = pool_ext;

			return pool;
		}
		UNLOCK(&pool->lock);
	}

	return NULL;
}

static void init_buffer_hdr(pool_t *pool, odp_buffer_hdr_t *buf_hdr, uint32_t buf_index,
			    uint32_t hdr_len, uint8_t *data_ptr, void *uarea)
{
	odp_pool_type_t type = pool->type;

	memset(buf_hdr, 0, hdr_len);

	/* Initialize buffer metadata */
	buf_hdr->index.u32    = 0;
	buf_hdr->index.pool   = pool->pool_idx;
	buf_hdr->index.buffer = buf_index;
	buf_hdr->type         = type;
	buf_hdr->event_type   = type;
	buf_hdr->pool_ptr     = pool;
	buf_hdr->uarea_addr   = uarea;
	odp_atomic_init_u32(&buf_hdr->ref_cnt, 0);

	/* Store base values for fast init */
	buf_hdr->base_data = data_ptr;
	buf_hdr->buf_end   = data_ptr + pool->seg_len + pool->tailroom;

	/* Initialize segmentation metadata */
	if (type == ODP_POOL_PACKET) {
		odp_packet_hdr_t *pkt_hdr = (void *)buf_hdr;

		pkt_hdr->seg_data  = data_ptr;
		pkt_hdr->seg_len   = pool->seg_len;
		pkt_hdr->seg_count = 1;
		pkt_hdr->seg_next  = NULL;
	}

	/* Initialize event vector metadata */
	if (type == ODP_POOL_VECTOR) {
		odp_event_vector_hdr_t *vect_hdr = (void *)buf_hdr;

		vect_hdr->size      = 0;
		buf_hdr->event_type = ODP_EVENT_PACKET_VECTOR;
	}
}

static void init_buffers(pool_t *pool)
{
	uint64_t i;
	odp_buffer_hdr_t *buf_hdr;
	odp_packet_hdr_t *pkt_hdr;
	odp_shm_info_t shm_info;
	void *addr;
	void *uarea = NULL;
	uint8_t *data;
	uint32_t offset, hdr_len;
	ring_ptr_t *ring;
	uint32_t mask;
	odp_pool_type_t type;
	uint64_t page_size;
	int skipped_blocks = 0;

	if (odp_shm_info(pool->shm, &shm_info))
		ODP_ABORT("Shm info failed\n");

	page_size = shm_info.page_size;
	ring = &pool->ring->hdr;
	mask = pool->ring_mask;
	type = pool->type;

	for (i = 0; i < pool->num + skipped_blocks ; i++) {
		int skip = 0;

		addr    = &pool->base_addr[(i * pool->block_size) +
					   pool->block_offset];
		buf_hdr = addr;
		pkt_hdr = addr;

		/* Skip packet buffers which cross huge page boundaries. Some
		 * NICs cannot handle buffers which cross page boundaries. */
		if (type == ODP_POOL_PACKET && page_size >= FIRST_HP_SIZE) {
			uint64_t first_page;
			uint64_t last_page;

			first_page = ((uint64_t)(uintptr_t)addr &
					~(page_size - 1));
			last_page = (((uint64_t)(uintptr_t)addr +
					pool->block_size - 1) &
					~(page_size - 1));
			if (last_page != first_page) {
				skipped_blocks++;
				skip = 1;
			}
		}
		if (pool->uarea_size)
			uarea = &pool->uarea_base_addr[(i - skipped_blocks) *
						       pool->uarea_size];
		data = buf_hdr->data;

		if (type == ODP_POOL_PACKET)
			data = pkt_hdr->data;

		offset = pool->headroom;

		/* move to correct align */
		while (((uintptr_t)&data[offset]) % pool->align != 0)
			offset++;

		hdr_len = (uintptr_t)data - (uintptr_t)buf_hdr;
		init_buffer_hdr(pool, buf_hdr, i, hdr_len, &data[offset], uarea);

		/* Store buffer into the global pool */
		if (!skip)
			ring_ptr_enq(ring, mask, buf_hdr);
	}
	pool->skipped_blocks = skipped_blocks;
}

static bool shm_is_from_huge_pages(odp_shm_t shm)
{
	odp_shm_info_t info;
	uint64_t huge_page_size = odp_sys_huge_page_size();

	if (huge_page_size == 0)
		return 0;

	if (odp_shm_info(shm, &info)) {
		ODP_ERR("Failed to fetch shm info\n");
		return 0;
	}

	return (info.page_size >= huge_page_size);
}

static void set_pool_name(pool_t *pool, const char *name)
{
	if (name == NULL) {
		pool->name[0] = 0;
	} else {
		strncpy(pool->name, name, ODP_POOL_NAME_LEN - 1);
		pool->name[ODP_POOL_NAME_LEN - 1] = 0;
	}
}

static void set_pool_cache_size(pool_t *pool, uint32_t cache_size)
{
	uint32_t burst_size;

	pool->cache_size = 0;
	pool->burst_size = 1;

	if (cache_size > 1) {
		cache_size = (cache_size / 2) * 2;
		burst_size = _odp_pool_glb->config.burst_size;

		if ((cache_size / burst_size) < 2)
			burst_size = cache_size / 2;

		pool->cache_size = cache_size;
		pool->burst_size = burst_size;
	}
}

static int reserve_uarea(pool_t *pool, uint32_t uarea_size, uint32_t num_pkt, uint32_t shmflags)
{
	odp_shm_t shm;
	const char *max_prefix = "pool_000_uarea_";
	int max_prefix_len = strlen(max_prefix);
	char uarea_name[ODP_POOL_NAME_LEN + max_prefix_len];

	pool->uarea_shm = ODP_SHM_INVALID;

	if (uarea_size == 0) {
		pool->param_uarea_size = 0;
		pool->uarea_size       = 0;
		pool->uarea_shm_size   = 0;
		return 0;
	}

	sprintf(uarea_name, "pool_%03i_uarea_%s", pool->pool_idx, pool->name);

	pool->param_uarea_size = uarea_size;
	pool->uarea_size       = ROUNDUP_CACHE_LINE(uarea_size);
	pool->uarea_shm_size   = num_pkt * (uint64_t)pool->uarea_size;

	shm = odp_shm_reserve(uarea_name, pool->uarea_shm_size, ODP_PAGE_SIZE, shmflags);

	if (shm == ODP_SHM_INVALID)
		return -1;

	pool->uarea_shm       = shm;
	pool->uarea_base_addr = odp_shm_addr(shm);
	return 0;
}

static odp_pool_t pool_create(const char *name, const odp_pool_param_t *params,
			      uint32_t shmflags)
{
	pool_t *pool;
	uint32_t uarea_size, headroom, tailroom;
	odp_shm_t shm;
	uint32_t seg_len, align, num, hdr_size, block_size;
	uint32_t max_len, cache_size;
	uint32_t ring_size;
	odp_pool_type_t type = params->type;
	uint32_t num_extra = 0;
	const char *max_prefix = "pool_000_";
	int max_prefix_len = strlen(max_prefix);
	char shm_name[ODP_POOL_NAME_LEN + max_prefix_len];

	align = 0;

	if (type == ODP_POOL_PACKET) {
		uint32_t align_req = params->pkt.align;

		if (align_req &&
		    (!CHECK_IS_POWER2(align_req) ||
		     align_req > _odp_pool_glb->config.pkt_base_align)) {
			ODP_ERR("Bad align requirement\n");
			return ODP_POOL_INVALID;
		}

		align = _odp_pool_glb->config.pkt_base_align;
	} else {
		if (type == ODP_POOL_BUFFER)
			align = params->buf.align;

		if (align < _odp_pool_glb->config.buf_min_align)
			align = _odp_pool_glb->config.buf_min_align;
	}

	/* Validate requested buffer alignment */
	if (align > ODP_CONFIG_BUFFER_ALIGN_MAX ||
	    align != ROUNDDOWN_POWER2(align, align)) {
		ODP_ERR("Bad align requirement\n");
		return ODP_POOL_INVALID;
	}

	headroom    = 0;
	tailroom    = 0;
	seg_len     = 0;
	max_len     = 0;
	uarea_size  = 0;
	cache_size  = 0;

	switch (type) {
	case ODP_POOL_BUFFER:
		num  = params->buf.num;
		seg_len = params->buf.size;
		cache_size = params->buf.cache_size;
		break;

	case ODP_POOL_PACKET:
		if (params->pkt.headroom > CONFIG_PACKET_HEADROOM) {
			ODP_ERR("Packet headroom size not supported\n");
			return ODP_POOL_INVALID;
		}

		seg_len = CONFIG_PACKET_MAX_SEG_LEN;
		max_len = _odp_pool_glb->config.pkt_max_len;

		if (params->pkt.len &&
		    params->pkt.len < CONFIG_PACKET_MAX_SEG_LEN)
			seg_len = params->pkt.len;
		if (params->pkt.seg_len && params->pkt.seg_len > seg_len)
			seg_len = params->pkt.seg_len;
		if (seg_len < CONFIG_PACKET_SEG_LEN_MIN)
			seg_len = CONFIG_PACKET_SEG_LEN_MIN;

		/* Make sure that at least one 'max_len' packet can fit in the
		 * pool. */
		if (params->pkt.max_len != 0)
			max_len = params->pkt.max_len;
		if ((max_len + seg_len - 1) / seg_len > PKT_MAX_SEGS)
			seg_len = (max_len + PKT_MAX_SEGS - 1) / PKT_MAX_SEGS;
		if (seg_len > CONFIG_PACKET_MAX_SEG_LEN) {
			ODP_ERR("Pool unable to store 'max_len' packet\n");
			return ODP_POOL_INVALID;
		}

		headroom    = CONFIG_PACKET_HEADROOM;
		tailroom    = CONFIG_PACKET_TAILROOM;
		num         = params->pkt.num;
		uarea_size  = params->pkt.uarea_size;
		cache_size  = params->pkt.cache_size;
		break;

	case ODP_POOL_TIMEOUT:
		num = params->tmo.num;
		cache_size = params->tmo.cache_size;
		break;

	case ODP_POOL_VECTOR:
		num = params->vector.num;
		cache_size = params->vector.cache_size;
		seg_len = params->vector.max_size * sizeof(odp_packet_t);
		break;

	default:
		ODP_ERR("Bad pool type\n");
		return ODP_POOL_INVALID;
	}

	pool = reserve_pool(shmflags, 0, num);

	if (pool == NULL) {
		ODP_ERR("No more free pools\n");
		return ODP_POOL_INVALID;
	}

	set_pool_name(pool, name);

	/* Format SHM names from prefix, pool index and pool name. */
	sprintf(shm_name,   "pool_%03i_%s", pool->pool_idx, pool->name);

	pool->type   = type;
	pool->params = *params;
	pool->block_offset = 0;

	if (type == ODP_POOL_PACKET) {
		uint32_t dpdk_obj_size;

		hdr_size = ROUNDUP_CACHE_LINE(sizeof(odp_packet_hdr_t));
		block_size = hdr_size + align + headroom + seg_len + tailroom;
		/* Calculate extra space required for storing DPDK objects and
		 * mbuf headers. NOP if no DPDK pktio used or zero-copy mode is
		 * disabled. */
		dpdk_obj_size = _odp_dpdk_pool_obj_size(pool, block_size);
		if (!dpdk_obj_size) {
			ODP_ERR("Calculating DPDK mempool obj size failed\n");
			return ODP_POOL_INVALID;
		}
		if (dpdk_obj_size != block_size) {
			shmflags |= ODP_SHM_HP;
			block_size = dpdk_obj_size;
		} else {
			block_size = ROUNDUP_CACHE_LINE(block_size);
		}
	} else {
		/* Header size is rounded up to cache line size, so the
		 * following data can be cache line aligned without extra
		 * padding. */
		uint32_t align_pad = (align > ODP_CACHE_LINE_SIZE) ?
				align - ODP_CACHE_LINE_SIZE : 0;

		if (type == ODP_POOL_BUFFER)
			hdr_size = ROUNDUP_CACHE_LINE(sizeof(odp_buffer_hdr_t));
		else if (type == ODP_POOL_TIMEOUT)
			hdr_size = ROUNDUP_CACHE_LINE(sizeof(odp_timeout_hdr_t));
		else
			hdr_size = ROUNDUP_CACHE_LINE(sizeof(odp_event_vector_hdr_t));

		block_size = ROUNDUP_CACHE_LINE(hdr_size + align_pad + seg_len);
	}

	/* Allocate extra memory for skipping packet buffers which cross huge
	 * page boundaries. */
	if (type == ODP_POOL_PACKET) {
		num_extra = ((((uint64_t)num * block_size) +
				FIRST_HP_SIZE - 1) / FIRST_HP_SIZE);
		num_extra += ((((uint64_t)num_extra * block_size) +
				FIRST_HP_SIZE - 1) / FIRST_HP_SIZE);
	}

	/* Ring size must be larger than the number of items stored */
	if (num + 1 <= RING_SIZE_MIN)
		ring_size = RING_SIZE_MIN;
	else
		ring_size = ROUNDUP_POWER2_U32(num + 1);

	pool->ring_mask      = ring_size - 1;
	pool->num            = num;
	pool->align          = align;
	pool->headroom       = headroom;
	pool->seg_len        = seg_len;
	pool->max_seg_len    = headroom + seg_len + tailroom;
	pool->max_len        = max_len;
	pool->tailroom       = tailroom;
	pool->block_size     = block_size;
	pool->shm_size       = (num + num_extra) * (uint64_t)block_size;
	pool->ext_desc       = NULL;
	pool->ext_destroy    = NULL;

	set_pool_cache_size(pool, cache_size);

	shm = odp_shm_reserve(shm_name, pool->shm_size, ODP_PAGE_SIZE,
			      shmflags);

	pool->shm = shm;

	if (shm == ODP_SHM_INVALID) {
		ODP_ERR("SHM reserve failed\n");
		goto error;
	}

	pool->mem_from_huge_pages = shm_is_from_huge_pages(pool->shm);

	pool->base_addr = odp_shm_addr(pool->shm);
	pool->max_addr  = pool->base_addr + pool->shm_size - 1;

	if (reserve_uarea(pool, uarea_size, num, shmflags)) {
		ODP_ERR("User area SHM reserve failed\n");
		goto error;
	}

	ring_ptr_init(&pool->ring->hdr);
	init_buffers(pool);

	/* Create zero-copy DPDK memory pool. NOP if zero-copy is disabled. */
	if (type == ODP_POOL_PACKET && _odp_dpdk_pool_create(pool)) {
		ODP_ERR("Creating DPDK packet pool failed\n");
		goto error;
	}

	/* Reset pool stats */
	odp_atomic_init_u64(&pool->stats.alloc_ops, 0);
	odp_atomic_init_u64(&pool->stats.alloc_fails, 0);
	odp_atomic_init_u64(&pool->stats.free_ops, 0);
	odp_atomic_init_u64(&pool->stats.cache_alloc_ops, 0);
	odp_atomic_init_u64(&pool->stats.cache_free_ops, 0);

	return pool->pool_hdl;

error:
	if (pool->shm != ODP_SHM_INVALID)
		odp_shm_free(pool->shm);

	if (pool->uarea_shm != ODP_SHM_INVALID)
		odp_shm_free(pool->uarea_shm);

	LOCK(&pool->lock);
	pool->reserved = 0;
	UNLOCK(&pool->lock);
	return ODP_POOL_INVALID;
}

static int check_params(const odp_pool_param_t *params)
{
	odp_pool_capability_t capa;
	uint32_t cache_size, num;
	int num_threads = odp_global_ro.init_param.num_control +
				odp_global_ro.init_param.num_worker;
	int cur_threads = odp_thread_count();

	if (!params || odp_pool_capability(&capa) < 0)
		return -1;

	num = 0;
	cache_size = 0;
	if (num_threads < cur_threads)
		num_threads = cur_threads;

	switch (params->type) {
	case ODP_POOL_BUFFER:
		num = params->buf.num;
		cache_size = params->buf.cache_size;

		if (params->buf.num > capa.buf.max_num) {
			ODP_ERR("buf.num too large %u\n", params->buf.num);
			return -1;
		}

		if (params->buf.size > capa.buf.max_size) {
			ODP_ERR("buf.size too large %u\n", params->buf.size);
			return -1;
		}

		if (params->buf.align > capa.buf.max_align) {
			ODP_ERR("buf.align too large %u\n", params->buf.align);
			return -1;
		}

		if (params->stats.all & ~capa.buf.stats.all) {
			ODP_ERR("Unsupported pool statistics counter\n");
			return -1;
		}

		break;

	case ODP_POOL_PACKET:
		num = params->pkt.num;
		cache_size = params->pkt.cache_size;

		if (params->pkt.num > capa.pkt.max_num) {
			ODP_ERR("pkt.num too large %u\n", params->pkt.num);
			return -1;
		}

		if (params->pkt.max_num > capa.pkt.max_num) {
			ODP_ERR("pkt.max_num too large %u\n",
				params->pkt.max_num);
			return -1;
		}

		if (params->pkt.len > capa.pkt.max_len) {
			ODP_ERR("pkt.len too large %u\n", params->pkt.len);
			return -1;
		}

		if (params->pkt.max_len > capa.pkt.max_len) {
			ODP_ERR("pkt.max_len too large %u\n",
				params->pkt.max_len);
			return -1;
		}

		if (params->pkt.seg_len > capa.pkt.max_seg_len) {
			ODP_ERR("pkt.seg_len too large %u\n",
				params->pkt.seg_len);
			return -1;
		}

		if (params->pkt.uarea_size > capa.pkt.max_uarea_size) {
			ODP_ERR("pkt.uarea_size too large %u\n",
				params->pkt.uarea_size);
			return -1;
		}

		if (params->pkt.headroom > capa.pkt.max_headroom) {
			ODP_ERR("pkt.headroom too large %u\n",
				params->pkt.headroom);
			return -1;
		}

		if (params->stats.all & ~capa.pkt.stats.all) {
			ODP_ERR("Unsupported pool statistics counter\n");
			return -1;
		}

		break;

	case ODP_POOL_TIMEOUT:
		num = params->tmo.num;
		cache_size = params->tmo.cache_size;

		if (params->tmo.num > capa.tmo.max_num) {
			ODP_ERR("tmo.num too large %u\n", params->tmo.num);
			return -1;
		}

		if (params->stats.all & ~capa.tmo.stats.all) {
			ODP_ERR("Unsupported pool statistics counter\n");
			return -1;
		}

		break;

	case ODP_POOL_VECTOR:
		num = params->vector.num;
		cache_size = params->vector.cache_size;

		if (params->vector.num == 0) {
			ODP_ERR("vector.num zero\n");
			return -1;
		}

		if (params->vector.num > capa.vector.max_num) {
			ODP_ERR("vector.num too large %u\n", params->vector.num);
			return -1;
		}

		if (params->vector.max_size == 0) {
			ODP_ERR("vector.max_size zero\n");
			return -1;
		}

		if (params->vector.max_size > capa.vector.max_size) {
			ODP_ERR("vector.max_size too large %u\n", params->vector.max_size);
			return -1;
		}

		if (params->stats.all & ~capa.vector.stats.all) {
			ODP_ERR("Unsupported pool statistics counter\n");
			return -1;
		}

		break;

	default:
		ODP_ERR("bad pool type %i\n", params->type);
		return -1;
	}

	if (cache_size > CONFIG_POOL_CACHE_MAX_SIZE) {
		ODP_ERR("Too large cache size %u\n", cache_size);
		return -1;
	}

	if (num <= (num_threads * cache_size))
		ODP_DBG("Entire pool fits into thread local caches. Pool "
			"starvation may occur if the pool is used by multiple "
			"threads.\n");

	return 0;
}

odp_pool_t odp_pool_create(const char *name, const odp_pool_param_t *params)
{
	uint32_t shm_flags = 0;

	if (check_params(params))
		return ODP_POOL_INVALID;

	if (params->type == ODP_POOL_PACKET)
		shm_flags = ODP_SHM_PROC;
	if (odp_global_ro.shm_single_va)
		shm_flags |= ODP_SHM_SINGLE_VA;

	return pool_create(name, params, shm_flags);
}

int odp_pool_destroy(odp_pool_t pool_hdl)
{
	pool_t *pool = pool_entry_from_hdl(pool_hdl);
	int i;

	if (pool == NULL)
		return -1;

	LOCK(&pool->lock);

	if (pool->reserved == 0) {
		UNLOCK(&pool->lock);
		ODP_ERR("Pool not created\n");
		return -1;
	}

	/* Destroy external DPDK mempool */
	if (pool->ext_destroy) {
		pool->ext_destroy(pool->ext_desc);
		pool->ext_destroy = NULL;
		pool->ext_desc = NULL;
	}

	/* Make sure local caches are empty */
	for (i = 0; i < ODP_THREAD_COUNT_MAX; i++)
		cache_flush(&pool->local_cache[i], pool);

	if (pool->pool_ext == 0)
		odp_shm_free(pool->shm);

	if (pool->uarea_shm != ODP_SHM_INVALID)
		odp_shm_free(pool->uarea_shm);

	pool->reserved = 0;
	odp_shm_free(pool->ring_shm);
	pool->ring = NULL;
	UNLOCK(&pool->lock);

	return 0;
}

odp_event_type_t _odp_buffer_event_type(odp_buffer_t buf)
{
	return buf_hdl_to_hdr(buf)->event_type;
}

void _odp_buffer_event_type_set(odp_buffer_t buf, int ev)
{
	buf_hdl_to_hdr(buf)->event_type = ev;
}

odp_pool_t odp_pool_lookup(const char *name)
{
	uint32_t i;
	pool_t *pool;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool = pool_entry(i);

		LOCK(&pool->lock);
		if (strcmp(name, pool->name) == 0) {
			/* found it */
			UNLOCK(&pool->lock);
			return pool->pool_hdl;
		}
		UNLOCK(&pool->lock);
	}

	return ODP_POOL_INVALID;
}

int odp_pool_info(odp_pool_t pool_hdl, odp_pool_info_t *info)
{
	pool_t *pool = pool_entry_from_hdl(pool_hdl);

	if (pool == NULL || info == NULL)
		return -1;

	memset(info, 0, sizeof(odp_pool_info_t));

	info->name = pool->name;

	if (pool->pool_ext) {
		info->pool_ext = 1;
		info->pool_ext_param = pool->ext_param;
	} else {
		info->params = pool->params;
	}

	if (pool->type == ODP_POOL_PACKET)
		info->pkt.max_num = pool->num;

	info->min_data_addr = (uintptr_t)pool->base_addr;
	info->max_data_addr = (uintptr_t)pool->max_addr;

	return 0;
}

int _odp_buffer_alloc_multi(pool_t *pool, odp_buffer_hdr_t *buf_hdr[], int max_num)
{
	uint32_t pool_idx = pool->pool_idx;
	pool_cache_t *cache = local.cache[pool_idx];
	ring_ptr_t *ring;
	odp_buffer_hdr_t *hdr;
	uint32_t mask, num_ch, num_alloc, i;
	uint32_t num_deq = 0;
	uint32_t burst_size = pool->burst_size;

	/* First pull packets from local cache */
	num_ch = cache_pop(cache, buf_hdr, max_num);

	if (CONFIG_POOL_STATISTICS && pool->params.stats.bit.cache_alloc_ops && num_ch)
		odp_atomic_inc_u64(&pool->stats.cache_alloc_ops);

	/* If needed, get more from the global pool */
	if (odp_unlikely(num_ch != (uint32_t)max_num)) {
		uint32_t burst = burst_size;
		uint32_t cache_num;

		num_deq = max_num - num_ch;
		if (odp_unlikely(num_deq > burst_size))
			burst = num_deq;

		odp_buffer_hdr_t *hdr_tmp[burst];

		ring      = &pool->ring->hdr;
		mask      = pool->ring_mask;
		burst     = ring_ptr_deq_multi(ring, mask, (void **)hdr_tmp,
					       burst);
		cache_num = burst - num_deq;

		if (CONFIG_POOL_STATISTICS) {
			if (pool->params.stats.bit.alloc_ops)
				odp_atomic_inc_u64(&pool->stats.alloc_ops);
			if (odp_unlikely(pool->params.stats.bit.alloc_fails && burst == 0))
				odp_atomic_inc_u64(&pool->stats.alloc_fails);
		}

		if (odp_unlikely(burst < num_deq)) {
			num_deq   = burst;
			cache_num = 0;
		}

		for (i = 0; i < num_deq; i++) {
			uint32_t idx = num_ch + i;

			hdr = hdr_tmp[i];
			odp_prefetch(hdr);
			buf_hdr[idx] = hdr;
		}

		/* Cache possible extra buffers. Cache is currently empty. */
		if (cache_num)
			cache_push(cache, &hdr_tmp[num_deq], cache_num);
	}

	num_alloc = num_ch + num_deq;

	return num_alloc;
}

static inline void buffer_free_to_pool(pool_t *pool,
				       odp_buffer_hdr_t *buf_hdr[], int num)
{
	uint32_t pool_idx = pool->pool_idx;
	pool_cache_t *cache = local.cache[pool_idx];
	ring_ptr_t *ring;
	uint32_t cache_num, mask;
	uint32_t cache_size = pool->cache_size;

	/* Special case of a very large free. Move directly to
	 * the global pool. */
	if (odp_unlikely(num > (int)cache_size)) {
		ring  = &pool->ring->hdr;
		mask  = pool->ring_mask;

		ring_ptr_enq_multi(ring, mask, (void **)buf_hdr, num);

		if (CONFIG_POOL_STATISTICS && pool->params.stats.bit.free_ops)
			odp_atomic_inc_u64(&pool->stats.free_ops);

		return;
	}

	/* Make room into local cache if needed. Do at least burst size
	 * transfer. */
	cache_num = cache->cache_num;

	if (odp_unlikely((int)(cache_size - cache_num) < num)) {
		int burst = pool->burst_size;

		ring  = &pool->ring->hdr;
		mask  = pool->ring_mask;

		if (odp_unlikely(num > burst))
			burst = num;
		if (odp_unlikely((uint32_t)num > cache_num))
			burst = cache_num;

		odp_buffer_hdr_t *buf_hdr[burst];

		cache_pop(cache, buf_hdr, burst);

		ring_ptr_enq_multi(ring, mask, (void **)buf_hdr, burst);
		if (CONFIG_POOL_STATISTICS && pool->params.stats.bit.free_ops)
			odp_atomic_inc_u64(&pool->stats.free_ops);
	}

	cache_push(cache, buf_hdr, num);
	if (CONFIG_POOL_STATISTICS && pool->params.stats.bit.cache_free_ops)
		odp_atomic_inc_u64(&pool->stats.cache_free_ops);
}

void _odp_buffer_free_multi(odp_buffer_hdr_t *buf_hdr[], int num_total)
{
	pool_t *pool;
	int num;
	int i;
	int first = 0;

	while (1) {
		num  = 1;
		i    = 1;
		pool = buf_hdr[first]->pool_ptr;

		/* 'num' buffers are from the same pool */
		if (num_total > 1) {
			for (i = first; i < num_total; i++)
				if (pool != buf_hdr[i]->pool_ptr)
					break;

			num = i - first;
		}

		buffer_free_to_pool(pool, &buf_hdr[first], num);

		if (i == num_total)
			return;

		first = i;
	}
}

odp_buffer_t odp_buffer_alloc(odp_pool_t pool_hdl)
{
	odp_buffer_t buf;
	pool_t *pool;
	int ret;

	ODP_ASSERT(ODP_POOL_INVALID != pool_hdl);

	pool = pool_entry_from_hdl(pool_hdl);
	ret  = _odp_buffer_alloc_multi(pool, (odp_buffer_hdr_t **)&buf, 1);

	if (odp_likely(ret == 1))
		return buf;

	return ODP_BUFFER_INVALID;
}

int odp_buffer_alloc_multi(odp_pool_t pool_hdl, odp_buffer_t buf[], int num)
{
	pool_t *pool;

	ODP_ASSERT(ODP_POOL_INVALID != pool_hdl);

	pool = pool_entry_from_hdl(pool_hdl);

	return _odp_buffer_alloc_multi(pool, (odp_buffer_hdr_t **)buf, num);
}

void odp_buffer_free(odp_buffer_t buf)
{
	_odp_buffer_free_multi((odp_buffer_hdr_t **)&buf, 1);
}

void odp_buffer_free_multi(const odp_buffer_t buf[], int num)
{
	_odp_buffer_free_multi((odp_buffer_hdr_t **)(uintptr_t)buf, num);
}

int odp_pool_capability(odp_pool_capability_t *capa)
{
	odp_pool_stats_opt_t supported_stats;
	uint32_t max_seg_len = CONFIG_PACKET_MAX_SEG_LEN;
	/* Reserve one for internal usage */
	int max_pools = ODP_CONFIG_POOLS - 1;

	memset(capa, 0, sizeof(odp_pool_capability_t));

	capa->max_pools = max_pools;

	supported_stats.all = 0;
	supported_stats.bit.available = 1;
	supported_stats.bit.alloc_ops = CONFIG_POOL_STATISTICS;
	supported_stats.bit.alloc_fails = CONFIG_POOL_STATISTICS;
	supported_stats.bit.free_ops = CONFIG_POOL_STATISTICS;
	supported_stats.bit.total_ops = 0;
	supported_stats.bit.cache_available = 1;
	supported_stats.bit.cache_alloc_ops = CONFIG_POOL_STATISTICS;
	supported_stats.bit.cache_free_ops = CONFIG_POOL_STATISTICS;

	/* Buffer pools */
	capa->buf.max_pools = max_pools;
	capa->buf.max_align = ODP_CONFIG_BUFFER_ALIGN_MAX;
	capa->buf.max_size  = MAX_SIZE;
	capa->buf.max_num   = CONFIG_POOL_MAX_NUM;
	capa->buf.min_cache_size = 0;
	capa->buf.max_cache_size = CONFIG_POOL_CACHE_MAX_SIZE;
	capa->buf.stats.all = supported_stats.all;

	/* Packet pools */
	capa->pkt.max_pools        = max_pools;
	capa->pkt.max_len          = _odp_pool_glb->config.pkt_max_len;
	capa->pkt.max_num	   = _odp_pool_glb->config.pkt_max_num;
	capa->pkt.max_align	   = _odp_pool_glb->config.pkt_base_align;
	capa->pkt.min_headroom     = CONFIG_PACKET_HEADROOM;
	capa->pkt.max_headroom     = CONFIG_PACKET_HEADROOM;
	capa->pkt.min_tailroom     = CONFIG_PACKET_TAILROOM;
	capa->pkt.max_segs_per_pkt = PKT_MAX_SEGS;
	capa->pkt.min_seg_len      = CONFIG_PACKET_SEG_LEN_MIN;
	capa->pkt.max_seg_len      = max_seg_len;
	capa->pkt.max_uarea_size   = MAX_SIZE;
	capa->pkt.min_cache_size   = 0;
	capa->pkt.max_cache_size   = CONFIG_POOL_CACHE_MAX_SIZE;
	capa->pkt.stats.all = supported_stats.all;

	/* Timeout pools */
	capa->tmo.max_pools = max_pools;
	capa->tmo.max_num   = CONFIG_POOL_MAX_NUM;
	capa->tmo.min_cache_size = 0;
	capa->tmo.max_cache_size = CONFIG_POOL_CACHE_MAX_SIZE;
	capa->tmo.stats.all = supported_stats.all;

	/* Vector pools */
	capa->vector.max_pools = max_pools;
	capa->vector.max_num   = CONFIG_POOL_MAX_NUM;
	capa->vector.max_size = CONFIG_PACKET_VECTOR_MAX_SIZE;
	capa->vector.min_cache_size = 0;
	capa->vector.max_cache_size = CONFIG_POOL_CACHE_MAX_SIZE;
	capa->vector.stats.all = supported_stats.all;
	return 0;
}

void odp_pool_print(odp_pool_t pool_hdl)
{
	pool_t *pool;

	pool = pool_entry_from_hdl(pool_hdl);

	ODP_PRINT("\nPool info\n");
	ODP_PRINT("---------\n");
	ODP_PRINT("  pool            %" PRIu64 "\n",
		  odp_pool_to_u64(pool->pool_hdl));
	ODP_PRINT("  name            %s\n", pool->name);
	ODP_PRINT("  pool type       %s\n",
		  pool->type == ODP_POOL_BUFFER ? "buffer" :
		  (pool->type == ODP_POOL_PACKET ? "packet" :
		   (pool->type == ODP_POOL_TIMEOUT ? "timeout" :
		    (pool->type == ODP_POOL_VECTOR ? "vector" :
		     "unknown"))));
	ODP_PRINT("  pool shm        %" PRIu64 "\n",
		  odp_shm_to_u64(pool->shm));
	ODP_PRINT("  user area shm   %" PRIu64 "\n",
		  odp_shm_to_u64(pool->uarea_shm));
	ODP_PRINT("  num             %u\n", pool->num);
	ODP_PRINT("  align           %u\n", pool->align);
	ODP_PRINT("  headroom        %u\n", pool->headroom);
	ODP_PRINT("  seg len         %u\n", pool->seg_len);
	ODP_PRINT("  max data len    %u\n", pool->max_len);
	ODP_PRINT("  tailroom        %u\n", pool->tailroom);
	ODP_PRINT("  block size      %u\n", pool->block_size);
	ODP_PRINT("  uarea size      %u\n", pool->uarea_size);
	ODP_PRINT("  shm size        %" PRIu64 "\n", pool->shm_size);
	ODP_PRINT("  base addr       %p\n", (void *)pool->base_addr);
	ODP_PRINT("  max addr        %p\n", (void *)pool->max_addr);
	ODP_PRINT("  uarea shm size  %" PRIu64 "\n", pool->uarea_shm_size);
	ODP_PRINT("  uarea base addr %p\n", (void *)pool->uarea_base_addr);
	ODP_PRINT("  cache size      %u\n", pool->cache_size);
	ODP_PRINT("  burst size      %u\n", pool->burst_size);
	ODP_PRINT("\n");
}

odp_pool_t odp_buffer_pool(odp_buffer_t buf)
{
	pool_t *pool = pool_from_buf(buf);

	return pool->pool_hdl;
}

void odp_pool_param_init(odp_pool_param_t *params)
{
	uint32_t default_cache_size = _odp_pool_glb->config.local_cache_size;

	memset(params, 0, sizeof(odp_pool_param_t));
	params->pkt.headroom = CONFIG_PACKET_HEADROOM;
	params->buf.cache_size = default_cache_size;
	params->pkt.cache_size = default_cache_size;
	params->tmo.cache_size = default_cache_size;
	params->vector.cache_size = default_cache_size;
}

uint64_t odp_pool_to_u64(odp_pool_t hdl)
{
	return _odp_pri(hdl);
}

unsigned int odp_pool_max_index(void)
{
	return ODP_CONFIG_POOLS - 1;
}

int odp_pool_index(odp_pool_t pool_hdl)
{
	pool_t *pool;

	ODP_ASSERT(pool_hdl != ODP_POOL_INVALID);

	pool = pool_entry_from_hdl(pool_hdl);

	return pool->pool_idx;
}

int odp_pool_stats(odp_pool_t pool_hdl, odp_pool_stats_t *stats)
{
	pool_t *pool;

	if (odp_unlikely(pool_hdl == ODP_POOL_INVALID)) {
		ODP_ERR("Invalid pool handle\n");
		return -1;
	}
	if (odp_unlikely(stats == NULL)) {
		ODP_ERR("Output buffer NULL\n");
		return -1;
	}

	pool = pool_entry_from_hdl(pool_hdl);

	memset(stats, 0, sizeof(odp_pool_stats_t));

	if (pool->params.stats.bit.available)
		stats->available = ring_ptr_len(&pool->ring->hdr);

	if (pool->params.stats.bit.alloc_ops)
		stats->alloc_ops = odp_atomic_load_u64(&pool->stats.alloc_ops);

	if (pool->params.stats.bit.alloc_fails)
		stats->alloc_fails = odp_atomic_load_u64(&pool->stats.alloc_fails);

	if (pool->params.stats.bit.free_ops)
		stats->free_ops = odp_atomic_load_u64(&pool->stats.free_ops);

	if (pool->params.stats.bit.cache_available)
		stats->cache_available = cache_total_available(pool);

	if (pool->params.stats.bit.cache_alloc_ops)
		stats->cache_alloc_ops = odp_atomic_load_u64(&pool->stats.cache_alloc_ops);

	if (pool->params.stats.bit.cache_free_ops)
		stats->cache_free_ops = odp_atomic_load_u64(&pool->stats.cache_free_ops);

	return 0;
}

int odp_pool_stats_reset(odp_pool_t pool_hdl)
{
	pool_t *pool;

	if (odp_unlikely(pool_hdl == ODP_POOL_INVALID)) {
		ODP_ERR("Invalid pool handle\n");
		return -1;
	}

	pool = pool_entry_from_hdl(pool_hdl);

	odp_atomic_store_u64(&pool->stats.alloc_ops, 0);
	odp_atomic_store_u64(&pool->stats.alloc_fails, 0);
	odp_atomic_store_u64(&pool->stats.free_ops, 0);
	odp_atomic_store_u64(&pool->stats.cache_alloc_ops, 0);
	odp_atomic_store_u64(&pool->stats.cache_free_ops, 0);

	return 0;
}

static pool_t *find_pool(odp_buffer_hdr_t *buf_hdr)
{
	int i;
	uint8_t *ptr = (uint8_t *)buf_hdr;

	for (i = 0; i < ODP_CONFIG_POOLS; i++) {
		pool_t *pool = pool_entry(i);

		if (pool->reserved == 0)
			continue;

		if (ptr >= pool->base_addr && ptr < pool->max_addr)
			return pool;
	}

	return NULL;
}

int _odp_buffer_is_valid(odp_buffer_t buf)
{
	pool_t *pool;
	odp_buffer_hdr_t *buf_hdr = buf_hdl_to_hdr(buf);

	if (buf == ODP_BUFFER_INVALID)
		return 0;

	/* Check that buffer header is from a known pool */
	pool = find_pool(buf_hdr);
	if (pool == NULL)
		return 0;

	if (pool != buf_hdr->pool_ptr)
		return 0;

	if (buf_hdr->index.buffer >= (pool->num + pool->skipped_blocks))
		return 0;

	return 1;
}

int odp_buffer_is_valid(odp_buffer_t buf)
{
	if (_odp_buffer_is_valid(buf) == 0)
		return 0;

	if (odp_event_type(odp_buffer_to_event(buf)) != ODP_EVENT_BUFFER)
		return 0;

	return 1;
}

/* No actual head pointer alignment requirement. Anyway, require even byte address. */
#define MIN_HEAD_ALIGN 2

int odp_pool_ext_capability(odp_pool_type_t type, odp_pool_ext_capability_t *capa)
{
	odp_pool_stats_opt_t supported_stats;

	if (type != ODP_POOL_PACKET)
		return -1;

	supported_stats.all = 0;

	memset(capa, 0, sizeof(odp_pool_ext_capability_t));

	capa->type           = type;
	capa->max_pools      = ODP_CONFIG_POOLS - 1;
	capa->min_cache_size = 0;
	capa->max_cache_size = CONFIG_POOL_CACHE_MAX_SIZE;
	capa->stats.all      = supported_stats.all;

	capa->pkt.max_num_buf         = _odp_pool_glb->config.pkt_max_num;
	capa->pkt.max_buf_size        = MAX_SIZE;
	capa->pkt.odp_header_size     = sizeof(odp_packet_hdr_t);
	capa->pkt.odp_trailer_size    = 0;
	capa->pkt.min_mem_align       = ODP_CACHE_LINE_SIZE;
	capa->pkt.min_buf_align       = ODP_CACHE_LINE_SIZE;
	capa->pkt.min_head_align      = MIN_HEAD_ALIGN;
	capa->pkt.buf_size_aligned    = 0;
	capa->pkt.max_headroom        = CONFIG_PACKET_HEADROOM;
	capa->pkt.max_headroom_size   = CONFIG_PACKET_HEADROOM;
	capa->pkt.max_segs_per_pkt    = PKT_MAX_SEGS;
	capa->pkt.max_uarea_size      = MAX_SIZE;

	return 0;
}

void odp_pool_ext_param_init(odp_pool_type_t type, odp_pool_ext_param_t *param)
{
	uint32_t default_cache_size = _odp_pool_glb->config.local_cache_size;

	memset(param, 0, sizeof(odp_pool_ext_param_t));

	if (type != ODP_POOL_PACKET)
		return;

	param->type         = ODP_POOL_PACKET;
	param->cache_size   = default_cache_size;
	param->pkt.headroom = CONFIG_PACKET_HEADROOM;
}

static int check_pool_ext_param(const odp_pool_ext_param_t *param)
{
	odp_pool_ext_capability_t capa;
	uint32_t head_offset = sizeof(odp_packet_hdr_t) + param->pkt.app_header_size;

	if (param->type != ODP_POOL_PACKET) {
		ODP_ERR("Pool type not supported\n");
		return -1;
	}

	if (odp_pool_ext_capability(param->type, &capa)) {
		ODP_ERR("Capa failed\n");
		return -1;
	}

	if (param->cache_size > capa.max_cache_size) {
		ODP_ERR("Too large cache size %u\n", param->cache_size);
		return -1;
	}

	if (param->stats.all != capa.stats.all) {
		ODP_ERR("Pool statistics not supported\n");
		return -1;
	}

	if (param->pkt.num_buf > capa.pkt.max_num_buf) {
		ODP_ERR("Too many packet buffers\n");
		return -1;
	}

	if (param->pkt.buf_size > capa.pkt.max_buf_size) {
		ODP_ERR("Too large packet buffer size %u\n", param->pkt.buf_size);
		return -1;
	}

	if (param->pkt.uarea_size > capa.pkt.max_uarea_size) {
		ODP_ERR("Too large user area size %u\n", param->pkt.uarea_size);
		return -1;
	}

	if (param->pkt.headroom > capa.pkt.max_headroom) {
		ODP_ERR("Too large headroom size\n");
		return -1;
	}

	if (head_offset % capa.pkt.min_head_align) {
		ODP_ERR("Head pointer not %u byte aligned\n", capa.pkt.min_head_align);
		return -1;
	}

	return 0;
}

odp_pool_t odp_pool_ext_create(const char *name, const odp_pool_ext_param_t *param)
{
	pool_t *pool;
	uint32_t ring_size;
	uint32_t num_buf = param->pkt.num_buf;
	uint32_t buf_size = param->pkt.buf_size;
	uint32_t head_offset = sizeof(odp_packet_hdr_t) + param->pkt.app_header_size;
	uint32_t headroom = param->pkt.headroom;
	uint32_t shm_flags = 0;

	if (check_pool_ext_param(param)) {
		ODP_ERR("Bad pool ext param\n");
		return ODP_POOL_INVALID;
	}

	if (odp_global_ro.shm_single_va)
		shm_flags |= ODP_SHM_SINGLE_VA;

	pool = reserve_pool(shm_flags, 1, num_buf);

	if (pool == NULL) {
		ODP_ERR("No more free pools\n");
		return ODP_POOL_INVALID;
	}

	pool->ext_param = *param;
	set_pool_name(pool, name);
	set_pool_cache_size(pool, param->cache_size);

	if (reserve_uarea(pool, param->pkt.uarea_size, num_buf, shm_flags)) {
		ODP_ERR("User area SHM reserve failed\n");
		goto error;
	}

	/* Ring size must be larger than the number of items stored */
	if (num_buf + 1 <= RING_SIZE_MIN)
		ring_size = RING_SIZE_MIN;
	else
		ring_size = ROUNDUP_POWER2_U32(num_buf + 1);

	pool->ring_mask      = ring_size - 1;
	pool->type           = param->type;
	pool->num            = num_buf;
	pool->headroom       = headroom;
	pool->tailroom       = 0;
	pool->seg_len        = buf_size - head_offset - headroom - pool->tailroom;
	pool->max_seg_len    = headroom + pool->seg_len + pool->tailroom;
	pool->max_len        = PKT_MAX_SEGS * pool->seg_len;

	ring_ptr_init(&pool->ring->hdr);

	return pool->pool_hdl;

error:
	if (pool->ring_shm != ODP_SHM_INVALID)
		odp_shm_free(pool->ring_shm);

	LOCK(&pool->lock);
	pool->reserved = 0;
	UNLOCK(&pool->lock);

	return ODP_POOL_INVALID;
}

int odp_pool_ext_populate(odp_pool_t pool_hdl, void *buf[], uint32_t buf_size, uint32_t num,
			  uint32_t flags)
{
	pool_t *pool;
	odp_buffer_hdr_t *buf_hdr;
	ring_ptr_t *ring;
	uint32_t i, ring_mask, buf_index, head_offset;
	uint32_t num_populated;
	uint8_t *data_ptr;
	uint32_t hdr_size = sizeof(odp_packet_hdr_t);
	void *uarea = NULL;

	if (pool_hdl == ODP_POOL_INVALID) {
		ODP_ERR("Bad pool handle\n");
		return -1;
	}

	pool = pool_entry_from_hdl(pool_hdl);

	if (pool->type != ODP_POOL_PACKET || pool->pool_ext == 0) {
		ODP_ERR("Bad pool type\n");
		return -1;
	}

	if (buf_size != pool->ext_param.pkt.buf_size) {
		ODP_ERR("Bad buffer size\n");
		return -1;
	}

	num_populated = pool->num_populated;

	if (num_populated + num > pool->num) {
		ODP_ERR("Trying to over populate the pool\n");
		return -1;
	}

	if ((num_populated + num == pool->num) && !(flags & ODP_POOL_POPULATE_DONE)) {
		ODP_ERR("Missing ODP_POOL_POPULATE_DONE flag\n");
		return -1;
	}

	if ((num_populated + num < pool->num) && flags) {
		ODP_ERR("Unexpected flags: 0x%x\n", flags);
		return -1;
	}

	ring = &pool->ring->hdr;
	ring_mask = pool->ring_mask;
	buf_index = pool->num_populated;
	head_offset = sizeof(odp_packet_hdr_t) + pool->ext_param.pkt.app_header_size;

	for (i = 0; i < num; i++) {
		buf_hdr = buf[i];

		if ((uintptr_t)buf_hdr & (ODP_CACHE_LINE_SIZE - 1)) {
			ODP_ERR("Bad packet buffer align: buf[%u]\n", i);
			return -1;
		}

		if (((uintptr_t)buf_hdr + head_offset) & (MIN_HEAD_ALIGN - 1)) {
			ODP_ERR("Bad head pointer align: buf[%u]\n", i);
			return -1;
		}

		if (pool->uarea_size)
			uarea = &pool->uarea_base_addr[buf_index * pool->uarea_size];

		data_ptr = (uint8_t *)buf_hdr + head_offset + pool->headroom;
		init_buffer_hdr(pool, buf_hdr, buf_index, hdr_size, data_ptr, uarea);
		pool->ring->buf_hdr_by_index[buf_index] = buf_hdr;
		buf_index++;

		ring_ptr_enq(ring, ring_mask, buf_hdr);
	}

	pool->num_populated += num;

	return 0;
}
