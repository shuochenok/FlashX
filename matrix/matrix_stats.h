#ifndef __MATRIX_STATS_H__
#define __MATRIX_STATS_H__

/*
 * Copyright 2015 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
 *
 * This file is part of FlashMatrix.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

namespace fm
{

namespace detail
{

/*
 * This class maintains the statistics of I/O and computation that occurs
 * to matrices.
 */
class matrix_stats_t
{
	std::atomic<size_t> mem_read_bytes;
	std::atomic<size_t> mem_write_bytes;
	std::atomic<size_t> EM_read_bytes;
	std::atomic<size_t> EM_write_bytes;
	std::atomic<size_t> double_multiplies;
public:
	matrix_stats_t() {
		mem_read_bytes = 0;
		mem_write_bytes = 0;
		EM_read_bytes = 0;
		EM_write_bytes = 0;
		double_multiplies = 0;
	}

	matrix_stats_t(const matrix_stats_t &stats) {
		mem_read_bytes = stats.mem_read_bytes.load();
		mem_write_bytes = stats.mem_write_bytes.load();
		EM_read_bytes = stats.EM_read_bytes.load();
		EM_write_bytes = stats.EM_write_bytes.load();
		double_multiplies = stats.double_multiplies.load();
	}

	size_t inc_read_bytes(size_t bytes, bool in_mem) {
#ifdef MATRIX_DEBUG
		if (in_mem) {
			mem_read_bytes += bytes;
			return mem_read_bytes;
		}
		else {
			EM_read_bytes += bytes;
			return EM_read_bytes;
		}
#else
		return 0;
#endif
	}

	size_t get_read_bytes(bool in_mem) const {
#ifdef MATRIX_DEBUG
		if (in_mem)
			return mem_read_bytes;
		else
			return EM_read_bytes;
#else
		return 0;
#endif
	}

	size_t inc_write_bytes(size_t bytes, bool in_mem) {
#ifdef MATRIX_DEBUG
		if (in_mem) {
			mem_write_bytes += bytes;
			return mem_write_bytes;
		}
		else {
			EM_write_bytes += bytes;
			return EM_write_bytes;
		}
#else
		return 0;
#endif
	}

	size_t get_write_bytes(bool in_mem) const {
#ifdef MATRIX_DEBUG
		if (in_mem)
			return mem_write_bytes;
		else
			return EM_write_bytes;
#else
		return 0;
#endif
	}

	size_t inc_multiplies(size_t multiplies) {
#ifdef MATRIX_DEBUG
		this->double_multiplies += multiplies;
		return double_multiplies;
#else
		return 0;
#endif
	}

	size_t get_multiplies() const {
#ifdef MATRIX_DEBUG
		return double_multiplies;
#else
		return 0;
#endif
	}

	void print_diff(const matrix_stats_t &orig) const;
};

extern matrix_stats_t matrix_stats;

}

}

#endif
