/*
   Copyright [2017-2020] [IBM Corporation]
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

#include "dax_manager.h"
#include "arena.h"
#include "arena_dev.h"
#include "arena_fs.h"
#include "arena_none.h"
#include "dax_data.h"
#include "nd_utils.h"

#include <common/exceptions.h>
#include <common/fd_locked.h>
#include <common/memory_mapped.h>
#include <common/utils.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <boost/scope_exit.hpp>
#include <boost/icl/split_interval_map.hpp>
#include <gsl/pointers>
#include <experimental/filesystem>
#include <cinttypes>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>

#define DEBUG_PREFIX "dax_manager: "

static constexpr unsigned MAP_LOG_GRAIN = 21U;
static constexpr std::size_t MAP_GRAIN = std::size_t(1) << MAP_LOG_GRAIN;
static constexpr int MAP_HUGE = MAP_LOG_GRAIN << MAP_HUGE_SHIFT;

#ifndef MAP_SYNC
#define MAP_SYNC 0x80000
#endif

#ifndef MAP_SHARED_VALIDATE
#define MAP_SHARED_VALIDATE 0x03
#endif

#include <thread>
#include <sstream>

namespace fs = std::experimental::filesystem;

namespace
{
	std::set<std::string> nupm_dax_manager_mapped;
	std::mutex nupm_dax_manager_mapped_lock;

	int init_map_lock_mask()
	{
		/* env variable USE_ODP to indicate On Demand Paging may be used and therefore mapped memory need not be pinned */
		char* p = ::getenv("USE_ODP");
		bool odp = false;
		if ( p != nullptr )
		{
			errno = 0;
			odp = bool(std::strtoul(p,nullptr,10));

			auto e = errno;
			if ( e == 0 )
			{
				PLOG("USE_ODP=%d (%s on-demand paging)", int(odp), odp ? "using" : "not using");
			}
			else
			{
				PLOG("USE_ODP specification %s failed to parse: %s", p, ::strerror(e));
			}
		}
		return odp ? 0 : MAP_LOCKED;
	}
}

const int nupm::dax_manager::effective_map_locked = init_map_lock_mask();
constexpr const char *nupm::dax_manager::_cname;

nupm::path_use::path_use(path_use &&other_) noexcept
  : _path(other_._path)
{
  other_._path.clear();
}

nupm::path_use::path_use(const std::string &path_)
  : _path(path_)
{
  std::lock_guard<std::mutex> g(nupm_dax_manager_mapped_lock);
  bool inserted = nupm_dax_manager_mapped.insert(path_).second;
  if ( ! inserted )
  {
    std::ostringstream o;
    o << __func__ << ": instance already managing path (" << path_ << ")";
    throw std::range_error(o.str());
  }
  PLOG("%si%p): path: %s", __func__, static_cast<const void *>(this), _path.c_str());
}

nupm::path_use::~path_use()
{
  if ( _path.size() )
  {
    std::lock_guard<std::mutex> g(nupm_dax_manager_mapped_lock);
    nupm_dax_manager_mapped.erase(_path);
    PLOG("%s: dax mgr instance: %s", __func__, _path.c_str());
  }
}

std::pair<std::vector<::iovec>, std::size_t> get_mapping(const fs::path &path_map)
{
	/* A region must always be mapped to the same address, as MCAS
	 * MCAS software uses absolute addresses. Current design is to
	 * save this in a file extended attribute, ahtough it could be
	 * saved in a specially-named file.
	 */
	std::vector<::iovec> m;
	std::ifstream f(path_map.c_str());
	std::size_t covered = 0;
	std::uint64_t addr;
	std::size_t size;
	f >> addr >> size;
	while ( f.good() )
	{
		m.push_back(::iovec{reinterpret_cast<void *>(addr), size});
		covered += size;
		PLOG("%s %s: %p, 0x%zx", __func__, path_map.c_str(), m.back().iov_base, m.back().iov_len);
		f >> addr >> size;
	}
	return { m, covered };
}

std::vector<::iovec> get_mapping(const fs::path &path_map, const std::size_t expected_size)
{
	auto r = get_mapping(path_map);
    if ( r.second != expected_size )
	{
		std::ostringstream o;
		o << __func__ << ": map file " << path_map << std::hex << std::showbase << " expected to cover " << expected_size << " bytes, but covers " << r.second << " bytes";
		throw std::runtime_error(o.str());
	}
	return r.first;
}

void nupm::dax_manager::data_map_remove(const fs::directory_entry &e)
{
	if (
#if __cplusplus < 201703
		fs::is_regular_file(e.status())
#else
		e.is_regular_file()
#endif
	)
	{
		auto p = e.path();
		static std::set<std::string> used_extensions { ".map", ".data" };
		if ( used_extensions.count(p.extension().string()) != 0 )
		{
			CPLOG(1, "%s remove %s", __func__, p.c_str());
			std::error_code ec;
			fs::remove(p, ec);
			if ( ec.value() == 0 )
			{
				PLOG("%s: removing %s: %s", __func__, p.c_str(), ec.message().c_str());
			}
		}
	}
	else if (
#if __cplusplus < 201703
		fs::is_directory(e.status())
#else
		e.is_directory()
#endif
	)
	{
		auto p = e.path();
		std::error_code ec;
		fs::remove(p, ec);
		if ( ec.value() == 0 )
		{
			PLOG("%s: removing %s: %s", __func__, p.c_str(), ec.message().c_str());
		}
	}
}

void nupm::dax_manager::map_register(const fs::directory_entry &e)
{
	if (
#if __cplusplus < 201703
		fs::is_regular_file(e.status())
#else
		e.is_regular_file()
#endif
	)
	{
		auto p = e.path();
		if ( p.extension().string() == ".data" )
		{
			CPLOG(1, "%s %s", __func__, p.c_str());

			auto pm = p.replace_extension(".map");
			/* first: mapping. second: mapping size */
			auto r = arena_fs::get_mapping(pm);

			/* NOT CHECKED: if mapping size not equal data file size, there is an inconsistency */

			/* insert and map the file */
			auto itb =
				_mapped_spaces.insert(
					mapped_spaces::value_type(
						p.string()
						, space_registered(*this, this, p.string(), r.first)
					)
				);
			if ( ! itb.second )
			{
				throw std::domain_error("multiple instances of path " + p.string() + " in configuration");
			}
		}
	}
}

void nupm::dax_manager::files_scan(const path &p, void (dax_manager::*action)(const directory_entry &))
{
	std::error_code ec;
	auto ir = fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
	if ( ec.value() == 0 )
	{
		for ( auto e : ir )
		{
			if (
#if __cplusplus < 201703
				fs::is_directory(e.status())
#else
				e.is_directory()
#endif
			)
			{
				files_scan(e.path(), action);
			}
			(this->*action)(e);
		}
	}
}

std::unique_ptr<arena> nupm::dax_manager::make_arena_fs(
	const path &p
	, addr_t // base
	, bool force_reset
)
{
	/* No checking. Although specifying a path twice would be odd, it causes no harm.
	 * But perhaps we will scan all address maps to develop a free address interval set.
	 */
	/* For all map files in the path, add covered addresses to _address_coverage and remove from
	 * _address_fs_available
	 */
	files_scan(p, force_reset ? &dax_manager::data_map_remove : &dax_manager::map_register);
	return
		std::make_unique<arena_fs>(
			static_cast<log_source &>(*this)
			, p
		);
}

std::unique_ptr<arena> nupm::dax_manager::make_arena_none(
	const path &p
	, addr_t // base
	, bool // force_reset
)
{
	PLOG("%s: %s is unsuitable as an arena: neither a character file nor a directory", __func__, p.c_str());
	return
		std::make_unique<arena_none>(
			static_cast<log_source &>(*this)
			, p
		);
}

std::unique_ptr<arena> nupm::dax_manager::make_arena_dev(const path &p, addr_t base, bool force_reset)
{
	/* Create and insert a space_registered.
	 *   path_use : tracks usage of the path name to ensure no duplicate uses
	 *   space_opened : tracks opened file descriptors, and the iov each represents
	 *     Note: areana_fs may eventually have multiple iov's opened space.
	 *   range_use : tracks vitutal address ranges to ensure no duplicate addresses
	 *     Note: areana_fs may eventually have multiple iov's opened space.
	 */
	auto itb =
		_mapped_spaces.insert(
			mapped_spaces::value_type(
				p.string()
				, space_registered(*this, this, p.string(), base)
			)
		);
	if ( ! itb.second )
	{
		throw std::domain_error("multiple instances of path " + p.string() + " in configuration");
	}
	return
		std::make_unique<arena_dev>(
			static_cast<log_source &>(*this)
			, recover_metadata(
				itb.first->second._or._range.iov(0),
				force_reset
			)
		);
}

bool nupm::dax_manager::enter(
	common::fd_locked && fd_
	, const path & path_
	, const std::vector<::iovec> &m_
)
{
	auto itb =
		_mapped_spaces.insert(
			mapped_spaces::value_type(
				path_.string()
				, space_registered(*this, this, std::move(fd_), path_.string(), m_)
			)
		);
	if ( ! itb.second )
	{
		PLOG("%s: failed to insert %s (duplicate instance?)", __func__, path_.c_str());
	}
	return itb.second;
}

void nupm::dax_manager::remove(const path & path_)
{
	auto it = _mapped_spaces.find(path_.string());
	if ( it != _mapped_spaces.end() )
	{
		CPLOG(2, "%s: _mapped_spaces found %s at %p", __func__, path_.c_str(), static_cast<const void *>(&it->second));
	}
	else
	{
		CPLOG(2, "%s: _mapped_spaces does not contain %s", __func__, path_.c_str());
	}
	auto ct = _mapped_spaces.erase(path_.string());
	CPLOG(2, "%s: _mapped_spaces erase count %zu", __func__, ct);
}

namespace nupm
{

dax_manager::dax_manager(
  const common::log_source &ls_,
  const std::vector<config_t>& dax_configs,
  bool force_reset
)
  : common::log_source(ls_)
  , _nd()
  , _address_coverage()
  , _address_fs_available()
  /* space mapped by devdax */
  , _mapped_spaces()
  , _arenas()
  , _reentrant_lock()
{
  /* Maximum expected need is about 6 TiB (12 515GiB DIMMs */
  char *free_address_begin = reinterpret_cast<char *>(uintptr_t(1) << 40);
  auto free_address_end    = free_address_begin + (std::size_t(1) << 40);
  auto i = boost::icl::interval<char *>::right_open(free_address_begin, free_address_end);
  _address_fs_available.insert(i);

  /* set up each configuration */
  for(const auto& config: dax_configs) {

    CPLOG(0, DEBUG_PREFIX "region (%s,%lx)", config.path.c_str(), config.addr);

    /* a dax_config entry may be either devdax or fsdax.
     * If the path names a directory it is fsdax, else is it devdax.
     *
     * The startup behavior of devdax paths controlled by _mapped_spaces is:
     *   space_opened opens the path and maps the resulting fd
     * The shutdown behavior of devdax controlled by _mapped_spaces is:
     *   path_use calls nupm_dax_manager_mapped.erase(_path), a registry if files opened by this process
     *
     * The startup behavior of fsdax paths controlled by _mapped_spaces is:
     *   space_opened (None. Mapping are not attempted until open_region or create_region)
     * The shutdown behavior of devdax controlled by _mapped_spaces is:
     *   (None. Files are not opened until open_region or create_region)
     */

    path p(config.path);

    auto arena_make =
      fs::is_character_file(p) ? &dax_manager::make_arena_dev
      : fs::is_directory(p) ? &dax_manager::make_arena_fs
      : &dax_manager::make_arena_none;

    auto itc =
      _arenas.insert(
        std::make_pair(
          config.region_id
          , (this->*arena_make)(p, config.addr, force_reset)
        )
      );
    if ( ! itc.second )
    {
      throw std::domain_error("multiple instances of region " + std::to_string(config.region_id) + " in configuration");
    }
  }
}

dax_manager::~dax_manager()
{
  CPLOG(0, "%s::%s", _cname, __func__);
}

void * dax_manager::locate_free_address_range(std::size_t size_)
{
	for ( auto i : _address_fs_available )
	{
		if ( ptrdiff_t(size_) <= i.upper() - i.lower() )
		{
			return i.lower();
		}
	}
	throw std::runtime_error(__func__ + std::string(" out of address ranges"));
}

auto dax_manager::lookup_arena(arena_id_t arena_id) -> arena *
{
  auto it = _arenas.find(arena_id);
  if ( it == _arenas.end() )
  {
    throw Logic_exception("%s::%s: could not find header for region (%d)", _cname, __func__,
                        arena_id);
  }
  return it->second.get();
}

void dax_manager::debug_dump(arena_id_t arena_id)
{
  guard_t g(_reentrant_lock);
  auto it = lookup_arena(arena_id);
  it->debug_dump();
}

auto dax_manager::open_region(
  string_view name
  , unsigned arena_id
) -> std::pair<std::string, std::vector<::iovec>>
{
  guard_t           g(_reentrant_lock);
  return lookup_arena(arena_id)->region_get(name);
}

auto dax_manager::create_region(
  string_view name
  , arena_id_t arena_id
  , const size_t size
) -> std::pair<std::string, std::vector<::iovec>>
{
  guard_t           g(_reentrant_lock);
  auto arena = lookup_arena(arena_id);
  CPLOG(1, "%s: %s size %zu", __func__, name.begin(), size);
  auto r = arena->region_create(name, this, size);
  if ( r.second.empty() )
  {
    CPLOG(2, "%s: %.*s size req 0x%zx create failed", __func__, int(name.size()), name.begin(), size);
  }
  else
  {
    CPLOG(2, "%s: %.*s size req 0x%zx created at %p:%zx", __func__, int(name.size()), name.begin(), size, r.second[0].iov_base, r.second[0].iov_len);
  }
  return r;
}

void dax_manager::erase_region(string_view name, arena_id_t arena_id)
{
  guard_t           g(_reentrant_lock);
  lookup_arena(arena_id)->region_erase(name, this);
}

size_t dax_manager::get_max_available(arena_id_t arena_id)
{
  guard_t           g(_reentrant_lock);
  return lookup_arena(arena_id)->get_max_available();
}

auto dax_manager::recover_metadata(const ::iovec iov_,
                                      bool        force_rebuild) -> DM_region_header *
{
  assert(iov_.iov_base);
  DM_region_header *rh = static_cast<DM_region_header *>(iov_.iov_base);

  if ( force_rebuild || ! rh->check_magic() ) {
    PLOG("%s::%s: rebuilding.", _cname, __func__);
    rh = new (iov_.iov_base) DM_region_header(iov_.iov_len);
    PLOG("%s::%s: rebuilt.", _cname, __func__);
  }
  else {
    PLOG("%s::%s: no rebuild.", _cname, __func__);
    rh->check_undo_logs();
  }

  return rh;
}

}  // namespace nupm
