/*
   Copyright [2017-2019] [IBM Corporation]
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at
       http://www.apache.org/licenses/LICENSE-2.0
   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/


#ifndef MCAS_HSTORE_HEAP_RC_H
#define MCAS_HSTORE_HEAP_RC_H

#include "hstore_config.h"
#include "histogram_log2.h"
#include "hop_hash_log.h"
#include "persister_nupm.h"
#include "rc_alloc_wrapper_lb.h"
#include "trace_flags.h"

#include <boost/icl/interval_set.hpp>
#if 0
#include <valgrind/memcheck.h>
#else
#define VALGRIND_CREATE_MEMPOOL(pool, x, y) do { (void) (pool); (void) (x); (void) (y); } while(0)
#define VALGRIND_DESTROY_MEMPOOL(pool) do { (void) (pool); } while(0)
#define VALGRIND_MAKE_MEM_DEFINED(pool, size) do { (void) (pool); (void) (size); } while(0)
#define VALGRIND_MAKE_MEM_UNDEFINED(pool, size) do { (void) (pool); (void) (size); } while(0)
#define VALGRIND_MEMPOOL_ALLOC(pool, addr, size) do { (void) (pool); (void) (addr); (void) (size); } while(0)
#define VALGRIND_MEMPOOL_FREE(pool, size) do { (void) (pool); (void) (size); } while(0)
#endif
#include <common/exceptions.h> /* General_exception */

#include <sys/uio.h> /* iovec */

#include <algorithm>
#include <array>
#include <cstddef> /* size_t, ptrdiff_t */
#include <memory>
#include <vector>

struct dax_manager;

namespace impl
{
	struct allocation_state_combined;
}

struct heap_rc_shared_ephemeral
	: private common::log_source
{
	using region_access = std::pair<std::string, std::vector<::iovec>>;
private:
	nupm::Rca_LB _heap;
	region_access _managed_regions;
	std::size_t _allocated;
	std::size_t _capacity;
	/* The set of reconstituted addresses. Only needed during recovery.
	 * Potentially large, so should be erased after recovery. But there
	 * is no mechanism to erase it yet.
	 */
	using alloc_set_t = boost::icl::interval_set<const char *>; /* std::byte_t in C++17 */
	alloc_set_t _reconstituted; /* std::byte_t in C++17 */
	using hist_type = util::histogram_log2<std::size_t>;
	hist_type _hist_alloc;
	hist_type _hist_inject;
	hist_type _hist_free;

	static constexpr unsigned log_min_alignment = 3U; /* log (sizeof(void *)) */
	static_assert(sizeof(void *) == 1U << log_min_alignment, "log_min_alignment does not match sizeof(void *)");
	/* Rca_LB seems not to allocate at or above about 2GiB. Limit reporting to 16 GiB. */
	static constexpr unsigned hist_report_upper_bound = 34U;

public:
	explicit heap_rc_shared_ephemeral(unsigned debug_level, const std::string & backing_file);

	void add_managed_region(const ::iovec &r_full, const ::iovec &r_heap, unsigned numa_node);
	region_access get_managed_regions() const { return _managed_regions; }

	template <bool B>
		void write_hist(const ::iovec & pool_) const
		{
			static bool suppress = false;
			if ( ! suppress )
			{
				hop_hash_log<B>::write(LOG_LOCATION, "pool ", pool_.iov_base);
				std::size_t lower_bound = 0;
				auto limit = std::min(std::size_t(hist_report_upper_bound), _hist_alloc.data().size());
				for ( unsigned i = log_min_alignment; i != limit; ++i )
				{
					const std::size_t upper_bound = 1ULL << i;
					hop_hash_log<B>::write(LOG_LOCATION
						, "[", lower_bound, "..", upper_bound, "): "
						, _hist_alloc.data()[i], " ", _hist_inject.data()[i], " ", _hist_free.data()[i]
						, " "
					);
					lower_bound = upper_bound;
				}
				suppress = true;
			}
		}

	std::size_t allocated() const {  return _allocated; }
	std::size_t capacity() const { return _capacity; };
	void inject_allocation(void *p, std::size_t sz, unsigned numa_node);
	void *allocate(std::size_t sz, unsigned numa_node, std::size_t alignment);
	void free(void *p, std::size_t sz, unsigned numa_node);
	bool is_reconstituted(const void *p) const;
};

struct heap_rc_shared
{
	using region_access = std::pair<std::string, std::vector<::iovec>>;
private:
	::iovec _pool0_full; /* entire extent of pool 0 */
	::iovec _pool0_heap; /* portion of pool 0 which can be used for the heap */
	unsigned _numa_node;
	std::size_t _more_region_uuids_size;
	std::array<std::uint64_t, 1024U> _more_region_uuids;
	std::unique_ptr<heap_rc_shared_ephemeral> _eph;
public:
	explicit heap_rc_shared(unsigned debug_level, ::iovec pool0_full, ::iovec pool0_heap, unsigned numa_node, const std::string &backing_file);
	explicit heap_rc_shared(unsigned debug_level, const std::unique_ptr<dax_manager> &dax_manager, const std::string &backing_file);
	/* allocation_state_combined offered, but not used */
	explicit heap_rc_shared(unsigned debug_level, const std::unique_ptr<dax_manager> &dax_manager, const std::string &backing_file, impl::allocation_state_combined *)
		: heap_rc_shared(debug_level, dax_manager, backing_file)
	{
	}

	heap_rc_shared(const heap_rc_shared &) = delete;
	heap_rc_shared &operator=(const heap_rc_shared &) = delete;

	~heap_rc_shared();

	static ::iovec open_region(const std::unique_ptr<dax_manager> &dax_manager, std::uint64_t uuid, unsigned numa_node);

	static void *iov_limit(const ::iovec &r);

	auto grow(
		const std::unique_ptr<dax_manager> & dax_manager
		, std::uint64_t uuid
		, std::size_t increment
	) -> std::size_t;

	void quiesce();

	void *alloc(std::size_t sz, std::size_t alignment);

	void inject_allocation(const void * p, std::size_t sz);

	void free(void *p, std::size_t sz, std::size_t alignment);

	unsigned percent_used() const {
    return _eph->capacity() == 0 ? 0xFFFFU : unsigned(_eph->allocated() * 100U / _eph->capacity());
  }

	bool is_reconstituted(const void * p) const;

	/* debug */
	unsigned numa_node() const
	{
		return _numa_node;
	}

    region_access regions() const;
};

struct heap_rc
{
private:
	heap_rc_shared *_heap;

public:
	explicit heap_rc(heap_rc_shared *area)
		: _heap(area)
	{
	}

	~heap_rc()
	{
	}

	heap_rc(const heap_rc &) noexcept = default;

	heap_rc & operator=(const heap_rc &) = default;

    static constexpr std::uint64_t magic_value = 0xc74892d72eed493a;

	heap_rc_shared *operator->() const
	{
		return _heap;
	}
};

#endif
