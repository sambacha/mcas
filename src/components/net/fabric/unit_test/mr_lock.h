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
#ifndef _TEST_MR_LOCK_H_
#define _TEST_MR_LOCK_H_

#include "delete_copy.h"
#include <cstddef> /* size_t */

class mr_lock
{
  const void *_addr;
  std::size_t _len;
  DELETE_COPY(mr_lock);
public:
  mr_lock(const void *addr, std::size_t len);
  mr_lock(mr_lock &&) noexcept;
  mr_lock &operator=(mr_lock &&) noexcept;
  ~mr_lock();
};

#endif
