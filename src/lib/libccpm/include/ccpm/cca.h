/*
   Copyright [2019,2020] [IBM Corporation]
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

#ifndef __CCPM_CRASH_CONSISTENT_ALLOCATOR_H__
#define __CCPM_CRASH_CONSISTENT_ALLOCATOR_H__

#include <ccpm/interfaces.h>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace ccpm
{
	struct area_top;
	struct cca
		: public IHeap_expandable
	{
	private:
		using top_vec_t = std::vector<std::unique_ptr<area_top>>;
		top_vec_t _top;
		top_vec_t::difference_type _last_top_allocate;
		top_vec_t::difference_type _last_top_free;
		explicit cca();

		void init(
			const region_vector_t &regions
			, ownership_callback_t resolver
			, const bool force_init
		);
	public:
		explicit cca(const region_vector_t &regions, ownership_callback_t resolver);

		explicit cca(const region_vector_t &regions);

		~cca();

		cca(const cca &) = delete;
		const cca& operator=(const cca &) = delete;

		bool reconstitute(
			const region_vector_t &regions
			, ownership_callback_t resolver
			, const bool force_init
		) override;

		status_t allocate(
			void * & ptr_
			, std::size_t bytes_
			, std::size_t // alignment
		) override;

		status_t free(
			void * & ptr_
			, std::size_t bytes_
		) override;

		void add_regions(
			const region_vector_t &
		) override;

		bool includes(
			const void *addr
		) const override;

		status_t remaining(
			std::size_t & out_size_
		) const override;

		void print(std::ostream &, const std::string &title = "cca") const;
	};
}

#endif
