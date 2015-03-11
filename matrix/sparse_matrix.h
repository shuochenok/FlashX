#ifndef __SPARSE_MATRIX_H__
#define __SPARSE_MATRIX_H__

/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
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

#include <memory>

#include "FGlib.h"
#include "matrix_io.h"
#include "io_interface.h"
#include "mem_vector.h"

namespace fm
{

class compute_task
{
public:
	typedef std::shared_ptr<compute_task> ptr;

	virtual ~compute_task() {
	}

	virtual void run(char *buf, size_t size) = 0;
	virtual safs::io_request get_request() const = 0;
};

class task_creator
{
public:
	typedef std::shared_ptr<task_creator> ptr;

	virtual compute_task::ptr create(const matrix_io &) const = 0;
};

/*
 * This task performs computation on a sparse matrix in the FlashGraph format.
 */
class fg_row_compute_task: public compute_task
{
	matrix_io io;
	off_t off;
	char *buf;
	size_t buf_size;
public:
	fg_row_compute_task(const matrix_io &_io): io(_io) {
		off_t orig_off = io.get_loc().get_offset();
		off = ROUND_PAGE(orig_off);
		buf_size = ROUNDUP_PAGE(orig_off - off + io.get_size());
		buf = (char *) valloc(buf_size);
	}

	~fg_row_compute_task() {
		free(buf);
	}
	virtual void run(char *buf, size_t size);
	virtual void run_on_row(const fg::ext_mem_undirected_vertex &v) = 0;
	virtual safs::io_request get_request() const {
		return safs::io_request(buf, safs::data_loc_t(io.get_loc().get_file_id(),
					off), buf_size, READ);
	}
};

/*
 * This task performs matrix vector multiplication on a sparse matrix
 * in the FlashGraph format.
 */
template<class T>
class fg_row_multiply_task: public fg_row_compute_task
{
	const type_mem_vector<T> &input;
	type_mem_vector<T> &output;
public:
	fg_row_multiply_task(const type_mem_vector<T> &_input, type_mem_vector<T> &_output,
			const matrix_io &_io): fg_row_compute_task(_io), input(
				_input), output(_output) {
	}

	void run_on_row(const fg::ext_mem_undirected_vertex &v);
};

template<class T>
void fg_row_multiply_task<T>::run_on_row(const fg::ext_mem_undirected_vertex &v)
{
	T res = 0;
	for (size_t i = 0; i < v.get_num_edges(); i++) {
		fg::vertex_id_t id = v.get_neighbor(i);
		res += input.get(id);
	}
	output.set(v.get_id(), res);
}

/*
 * This task performs matrix vector multiplication on a sparse matrix in
 * a native format with 2D partitioning.
 */
template<class T>
class block_multiply_task: public compute_task
{
	matrix_io io;
	block_2d_size block_size;
	off_t off;
	char *buf;
	size_t buf_size;

	const type_mem_vector<T> &input;
	type_mem_vector<T> &output;

	void run_on_row_part(const sparse_row_part &rpart, size_t start_row_idx,
			size_t start_col_idx) {
		size_t row_idx = start_row_idx + rpart.get_rel_row_idx();
		size_t num_non_zeros = rpart.get_num_non_zeros();
		T sum = 0;
		for (size_t i = 0; i < num_non_zeros; i++) {
			size_t col_idx = start_col_idx + rpart.get_rel_col_idx(i);
			sum += input.get(col_idx);
		}
		output.set(row_idx, output.get(row_idx) + sum);
	}

	void run_on_block(const sparse_block_2d &block) {
		row_part_iterator it = block.get_iterator();
		size_t start_col_idx
			= block.get_block_col_idx() * block_size.get_num_cols();
		size_t start_row_idx
			= block.get_block_row_idx() * block_size.get_num_rows();
		while (it.has_next())
			run_on_row_part(it.next(), start_row_idx, start_col_idx);
	}
public:
	block_multiply_task(const type_mem_vector<T> &_input,
			type_mem_vector<T> &_output, const matrix_io &_io,
			const block_2d_size &_block_size): io(_io), block_size(
				_block_size), input(_input), output(_output) {
		off_t orig_off = io.get_loc().get_offset();
		off = ROUND_PAGE(orig_off);
		buf_size = ROUNDUP_PAGE(orig_off - off + io.get_size());
		buf = (char *) valloc(buf_size);
	}

	~block_multiply_task() {
		free(buf);
	}

	virtual void run(char *buf, size_t size) {
		block_row_iterator it((const sparse_block_2d *) buf,
				(const sparse_block_2d *) (buf + size));
		while (it.has_next())
			run_on_block(it.next());
	}

	virtual safs::io_request get_request() const {
		return safs::io_request(buf, safs::data_loc_t(io.get_loc().get_file_id(),
					off), buf_size, READ);
	}
};

template<class T>
class fg_row_multiply_creator: public task_creator
{
	const type_mem_vector<T> &input;
	type_mem_vector<T> &output;

	fg_row_multiply_creator(const type_mem_vector<T> &_input,
			type_mem_vector<T> &_output): input(_input), output(_output) {
	}
public:
	static task_creator::ptr create(const type_mem_vector<T> &_input,
			type_mem_vector<T> &_output) {
		return task_creator::ptr(new fg_row_multiply_creator<T>(_input, _output));
	}

	virtual compute_task::ptr create(const matrix_io &io) const {
		return compute_task::ptr(new fg_row_multiply_task<T>(input, output, io));
	}
};

template<class T>
class b2d_multiply_creator: public task_creator
{
	const type_mem_vector<T> &input;
	type_mem_vector<T> &output;
	block_2d_size block_size;

	b2d_multiply_creator(const type_mem_vector<T> &_input,
			type_mem_vector<T> &_output, const block_2d_size &_block_size): input(
				_input), output(_output), block_size(_block_size) {
	}
public:
	static task_creator::ptr create(const type_mem_vector<T> &_input,
			type_mem_vector<T> &_output, const block_2d_size &_block_size) {
		return task_creator::ptr(new b2d_multiply_creator<T>(_input, _output,
					_block_size));
	}

	virtual compute_task::ptr create(const matrix_io &io) const {
		return compute_task::ptr(new block_multiply_task<T>(input, output,
					io, block_size));
	}
};

/*
 * This is a base class for a sparse matrix. It provides a set of functions
 * to perform computation on the sparse matrix. Currently, it has matrix
 * vector multiplication and matrix matrix multiplication.
 * We assume the sparse matrix is stored in external memory. If the matrix
 * is in memory, we can use in_mem_io to access the sparse matrix in memory
 * while reusing all the existing code for computation.
 */
class sparse_matrix
{
	// Whether the matrix is represented by the FlashGraph format.
	bool is_fg;
	size_t nrows;
	size_t ncols;
	bool symmetric;
	// If the matrix is stored in the native format and is partitioned
	// in two dimensions, we need to know the block size.
	block_2d_size block_size;
protected:
	// This constructor is used for the sparse matrix stored
	// in the FlashGraph format.
	sparse_matrix(size_t num_vertices, bool symmetric) {
		this->nrows = num_vertices;
		this->ncols = num_vertices;
		this->symmetric = symmetric;
		this->is_fg = true;
	}

	sparse_matrix(size_t nrows, size_t ncols, bool symmetric,
			const block_2d_size &_block_size): block_size(_block_size) {
		this->symmetric = symmetric;
		this->is_fg = false;
		this->nrows = nrows;
		this->ncols = ncols;
	}
public:
	typedef std::shared_ptr<sparse_matrix> ptr;

	virtual ~sparse_matrix() {
	}

	/*
	 * This creates a sparse matrix sotred in the FlashGraph format.
	 */
	static ptr create(fg::FG_graph::ptr);
	/*
	 * This create a symmetric sparse matrix partitioned in 2D dimensions.
	 */
	static ptr create(SpM_2d_index::ptr, SpM_2d_storage::ptr);
	/*
	 * This creates an asymmetric sparse matrix partitioned in 2D dimensions.
	 */
	static ptr create(SpM_2d_index::ptr index, SpM_2d_storage::ptr mat,
			SpM_2d_index::ptr t_index, SpM_2d_storage::ptr t_mat);

	/*
	 * When customizing computatin on a sparse matrix (regardless of
	 * the storage format and the computation), we need to define two things:
	 * how data is accessed and what computation runs on the data fetched
	 * from the external memory. matrix_io_generator defines data access
	 * and compute_task defines the computation.
	 */
	void compute(task_creator::ptr creator) const;
	/*
	 * This method defines how data in the matrix is accessed.
	 * Since we need to perform computation in parallel, we need to have
	 * a matrix I/O generator for each thread.
	 */
	virtual void init_io_gens(
			std::vector<matrix_io_generator::ptr> &io_gens) const = 0;

	virtual safs::file_io_factory::shared_ptr get_io_factory() const = 0;

	size_t get_num_rows() const {
		return nrows;
	}

	size_t get_num_cols() const {
		return ncols;
	}

	bool is_symmetric() const {
		return symmetric;
	}

	virtual void transpose() = 0;

	template<class T>
	task_creator::ptr get_multiply_creator(type_mem_vector<T> &in,
			type_mem_vector<T> &out) const {
		if (is_fg)
			return fg_row_multiply_creator<T>::create(in, out);
		else
			return b2d_multiply_creator<T>::create(in, out, block_size);
	}

	template<class T>
	typename type_mem_vector<T>::ptr multiply(typename type_mem_vector<T>::ptr in) const {
		if (in->get_length() != ncols) {
			BOOST_LOG_TRIVIAL(error) << boost::format(
					"the input vector has wrong length %1%. matrix ncols: %2%")
				% in->get_length() % ncols;
			return typename type_mem_vector<T>::ptr();
		}
		else {
			typename type_mem_vector<T>::ptr ret = type_mem_vector<T>::create(nrows);
			compute(get_multiply_creator<T>(*in, *ret));
			return ret;
		}
	}

	template<class T>
	dense_matrix::ptr multiply(dense_matrix::ptr in) const {
		if (in->get_num_rows() != ncols) {
			BOOST_LOG_TRIVIAL(error) << boost::format(
					"the input matrix has wrong #rows %1%. matrix ncols: %2%")
				% in->get_num_rows() % ncols;
			return dense_matrix::ptr();
		}
		else if (!in->is_in_mem()) {
			BOOST_LOG_TRIVIAL(error) << "SpMM doesn't support EM dense matrix";
			return dense_matrix::ptr();
		}
		else if (in->store_layout() == matrix_layout_t::L_ROW) {
			BOOST_LOG_TRIVIAL(error)
				<< "SpMM doesn't support row-wise dense matrix";
			return dense_matrix::ptr();
		}
		else {
			size_t ncol = in->get_num_cols();
			mem_col_dense_matrix::ptr col_m = mem_col_dense_matrix::cast(in);
			mem_col_dense_matrix::ptr ret = mem_col_dense_matrix::create(
					get_num_rows(), ncol, sizeof(T));
			std::vector<off_t> col_idx(1);
			for (size_t i = 0; i < ncol; i++) {
				col_idx[0] = i;
				typename type_mem_vector<T>::ptr in_col = type_mem_vector<T>::create(
						mem_dense_matrix::cast(col_m->get_cols(col_idx)));
				typename type_mem_vector<T>::ptr out_col = type_mem_vector<T>::create(
						mem_dense_matrix::cast(ret->get_cols(col_idx)));
				compute(get_multiply_creator<T>(*in_col, *out_col));
			}
			return ret;
		}
	}
};

void init_flash_matrix(config_map::ptr configs);
void destroy_flash_matrix();

}

#endif
