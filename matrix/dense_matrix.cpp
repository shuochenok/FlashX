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

#include <cblas.h>

#include "log.h"

#include "dense_matrix.h"
#include "bulk_operate.h"
#include "NUMA_dense_matrix.h"
#include "EM_dense_matrix.h"
#include "generic_type.h"
#include "rand_gen.h"
#include "one_val_matrix_store.h"
#include "local_matrix_store.h"
#include "virtual_matrix_store.h"
#include "mapply_matrix_store.h"
#include "vector.h"
#include "matrix_stats.h"

namespace fm
{

namespace detail
{

void portion_mapply_op::run(
		const std::vector<std::shared_ptr<const local_matrix_store> > &ins) const
{
	BOOST_LOG_TRIVIAL(error)
		<< "It doesn't support running on only input matrices";
	assert(0);
}

void portion_mapply_op::run(
		const std::vector<std::shared_ptr<const local_matrix_store> > &ins,
		local_matrix_store &out) const
{
	BOOST_LOG_TRIVIAL(error)
		<< "It doesn't support running on input matrices and output one matrix";
	assert(0);
}

void portion_mapply_op::run(
		const std::vector<std::shared_ptr<const local_matrix_store> > &ins,
		const std::vector<std::shared_ptr<local_matrix_store> > &outs) const
{
	BOOST_LOG_TRIVIAL(error)
		<< "It doesn't support running on input matrices and output multiple matrices";
	assert(0);
}

matrix_stats_t matrix_stats;

void matrix_stats_t::print_diff(const matrix_stats_t &orig) const
{
#ifdef MATRIX_DEBUG
	if (this->mem_read_bytes != orig.mem_read_bytes)
		BOOST_LOG_TRIVIAL(info) << "in-mem read "
			<< (this->mem_read_bytes - orig.mem_read_bytes) << " bytes";
	if (this->mem_write_bytes != orig.mem_write_bytes)
		BOOST_LOG_TRIVIAL(info) << "in-mem write "
			<< (this->mem_write_bytes - orig.mem_write_bytes) << " bytes";
	if (this->EM_read_bytes != orig.EM_read_bytes)
		BOOST_LOG_TRIVIAL(info) << "ext-mem read "
			<< (this->EM_read_bytes - orig.EM_read_bytes) << " bytes";
	if (this->EM_write_bytes != orig.EM_write_bytes)
		BOOST_LOG_TRIVIAL(info) << "ext-mem write "
			<< (this->EM_write_bytes - orig.EM_write_bytes) << " bytes";
	if (this->double_multiplies != orig.double_multiplies)
		BOOST_LOG_TRIVIAL(info) << "multiply "
			<< (this->double_multiplies - orig.double_multiplies)
			<< " double float points";
#endif
}

}

bool dense_matrix::verify_inner_prod(const dense_matrix &m,
		const bulk_operate &left_op, const bulk_operate &right_op) const
{
	if (this->get_entry_size() != left_op.left_entry_size()
			|| m.get_entry_size() != left_op.right_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The left operator isn't compatible with input matrices";
		return false;
	}

	if (left_op.output_entry_size() != right_op.left_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The type of the left operator doesn't match the right operator";
		return false;
	}

	if (right_op.left_entry_size() != right_op.right_entry_size()
			|| right_op.left_entry_size() != right_op.output_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The input and output of the right operator has different types";
		return false;
	}

	if (get_num_cols() != m.get_num_rows()) {
		BOOST_LOG_TRIVIAL(error) << "The matrix size doesn't match";
		return false;
	}
	return true;
}

bool dense_matrix::verify_mapply2(const dense_matrix &m,
			const bulk_operate &op) const
{
	if (this->get_num_rows() != m.get_num_rows()
			|| this->get_num_cols() != m.get_num_cols()) {
		BOOST_LOG_TRIVIAL(error)
			<< "two matrices in mapply2 don't have the same shape";
		return false;
	}

	if (get_entry_size() != op.left_entry_size()
			|| m.get_entry_size() != op.right_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "the element type in the matrices isn't compatible with the operator";
		return false;
	}

	return true;
}

bool dense_matrix::verify_apply(matrix_margin margin, const arr_apply_operate &op) const
{
	if (get_entry_size() != op.input_entry_size()) {
		BOOST_LOG_TRIVIAL(error)
			<< "the element type in the matrices isn't compatible with the operator";
		return false;
	}

	return true;
}

namespace
{

class double_square: public bulk_uoperate
{
public:
	virtual void runA(size_t num_eles, const void *in_arr,
			void *out_arr) const {
		long double *t_out_arr = (long double *) out_arr;
		const double *t_in_arr = (const double *) in_arr;
		for (size_t i = 0; i < num_eles; i++)
			t_out_arr[i]
				= ((long double) t_in_arr[i]) * ((long double) t_in_arr[i]);
	}
	virtual const scalar_type &get_input_type() const {
		return get_scalar_type<double>();
	}
	virtual const scalar_type &get_output_type() const {
		return get_scalar_type<long double>();
	}
};

class sum_agg: public bulk_operate
{
public:
	virtual void runAgg(size_t num_eles, const void *left_arr1,
			const void *orig, void *output) const {
		const long double *t_input = (const long double *) left_arr1;
		long double *t_output = (long double *) output;
		if (num_eles == 0)
			return;
		size_t i;
		if (orig) {
			i = 0;
			t_output[0] = *(const long double *) orig;
		}
		else {
			i = 1;
			t_output[0] = t_input[0];
		}
		for (; i < num_eles; i++)
			t_output[0] += t_input[i];
	}

	virtual void runAA(size_t num_eles, const void *left_arr,
			const void *right_arr, void *output_arr) const {
		assert(0);
	}

	virtual void runAE(size_t num_eles, const void *left_arr,
			const void *right, void *output_arr) const {
		assert(0);
	}

	virtual void runEA(size_t num_eles, const void *left,
			const void *right_arr, void *output_arr) const {
		assert(0);
	}

	virtual const scalar_type &get_left_type() const {
		return get_scalar_type<long double>();
	}

	virtual const scalar_type &get_right_type() const {
		return get_scalar_type<long double>();
	}

	virtual const scalar_type &get_output_type() const {
		return get_scalar_type<long double>();
	}
};

class double_multiply_operate: public bulk_operate
{
public:
	virtual void runAA(size_t num_eles, const void *left_arr,
			const void *right_arr, void *output_arr) const {
		const double *a = static_cast<const double *>(left_arr);
		const double *b = static_cast<const double *>(right_arr);
		long double *c = static_cast<long double *>(output_arr);
		for (size_t i = 0; i < num_eles; i++)
			c[i] = ((long double) a[i]) * ((long double) b[i]);
	}
	virtual void runAE(size_t num_eles, const void *left_arr,
			const void *right, void *output_arr) const {
		long double a = *static_cast<const double *>(right);
		const double *x = static_cast<const double *>(left_arr);
		long double *c = static_cast<long double *>(output_arr);
		for (size_t i = 0; i < num_eles; i++)
			c[i] = x[i] * a;
	}
	virtual void runEA(size_t num_eles, const void *left,
			const void *right_arr, void *output_arr) const {
		long double a = *static_cast<const double *>(left);
		const double *x = static_cast<const double *>(right_arr);
		long double *c = static_cast<long double *>(output_arr);
		for (size_t i = 0; i < num_eles; i++)
			c[i] = x[i] * a;
	}
	virtual void runAgg(size_t num_eles, const void *left_arr,
			const void *orig, void *output) const {
		assert(0);
	}

	virtual const scalar_type &get_left_type() const {
		return get_scalar_type<double>();
	}
	virtual const scalar_type &get_right_type() const {
		return get_scalar_type<double>();
	}
	virtual const scalar_type &get_output_type() const {
		return get_scalar_type<long double>();
	}
};

}

double dense_matrix::norm2() const
{
	detail::matrix_stats.inc_multiplies(get_num_rows() * get_num_cols());
	double ret = 0;
	if (get_type() == get_scalar_type<double>()) {
		dense_matrix::ptr sq_mat
			= this->sapply(bulk_uoperate::const_ptr(new double_square()));
		assert(sq_mat->get_type() == get_scalar_type<long double>());
		scalar_variable::ptr res = sq_mat->aggregate(
				bulk_operate::const_ptr(new sum_agg()));
		assert(res->get_type() == get_scalar_type<long double>());
		ret = sqrtl(*(long double *) res->get_raw());
	}
	else {
		const bulk_uoperate *op = get_type().get_basic_uops().get_op(
				basic_uops::op_idx::SQ);
		dense_matrix::ptr sq_mat = this->sapply(bulk_uoperate::conv2ptr(*op));
		scalar_variable::ptr res = sq_mat->aggregate(bulk_operate::conv2ptr(
					sq_mat->get_type().get_basic_ops().get_add()));
		res->get_type().get_basic_uops().get_op(
				basic_uops::op_idx::SQRT)->runA(1, res->get_raw(), &ret);
	}
	return ret;
}

namespace
{

template<class T>
class multiply_tall_op: public detail::portion_mapply_op
{
	detail::mem_matrix_store::const_ptr Bstore;
	std::vector<detail::local_matrix_store::ptr> Abufs;
	std::vector<detail::local_matrix_store::ptr> res_bufs;
public:
	multiply_tall_op(detail::mem_matrix_store::const_ptr Bstore,
			size_t num_threads, size_t out_num_rows,
			size_t out_num_cols): detail::portion_mapply_op(
				out_num_rows, out_num_cols, get_scalar_type<T>()) {
		this->Bstore = Bstore;
		Abufs.resize(num_threads);
		res_bufs.resize(num_threads);
	}

	virtual void run(
			const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;

	virtual detail::portion_mapply_op::const_ptr transpose() const;
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return std::string("(") + (mats[0]->get_name()
					+ "*") + Bstore->get_name() + std::string(")");
	}
};

template<class T>
class t_multiply_tall_op: public detail::portion_mapply_op
{
	multiply_tall_op<T> op;
public:
	t_multiply_tall_op(const multiply_tall_op<T> &_op): detail::portion_mapply_op(
			_op.get_out_num_cols(), _op.get_out_num_rows(),
			_op.get_output_type()), op(_op) {
	}

	virtual void run(
			const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const {
		assert(ins.size() == 1);
		std::vector<fm::detail::local_matrix_store::const_ptr> t_ins(ins.size());
		t_ins[0] = std::static_pointer_cast<const fm::detail::local_matrix_store>(
				ins[0]->transpose());
		fm::detail::local_matrix_store::ptr t_out
			= std::static_pointer_cast<fm::detail::local_matrix_store>(
					out.transpose());
		op.run(t_ins, *t_out);
	}

	virtual detail::portion_mapply_op::const_ptr transpose() const {
		return fm::detail::portion_mapply_op::const_ptr(
				new multiply_tall_op<T>(op));
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		return op.to_string(mats);
	}
};

template<class T>
detail::portion_mapply_op::const_ptr multiply_tall_op<T>::transpose() const
{
	return detail::portion_mapply_op::const_ptr(new t_multiply_tall_op<T>(*this));
}

template<class T>
void multiply_tall_op<T>::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	detail::local_matrix_store::const_ptr Astore = ins[0];
	detail::matrix_stats.inc_multiplies(
			Astore->get_num_rows() * Astore->get_num_cols() * Bstore->get_num_cols());

	const T *Amat = (const T *) Astore->get_raw_arr();
	// Let's make sure all matrices have the same data layout as the result matrix.
	if (Amat == NULL || Astore->store_layout() != out.store_layout()) {
		detail::pool_task_thread *thread = dynamic_cast<detail::pool_task_thread *>(
				thread::get_curr_thread());
		int thread_id = thread->get_pool_thread_id();
		if (Abufs[thread_id] == NULL
				|| Astore->get_num_rows() != Abufs[thread_id]->get_num_rows()
				|| Astore->get_num_cols() != Abufs[thread_id]->get_num_cols()) {
			if (out.store_layout() == matrix_layout_t::L_COL)
				const_cast<multiply_tall_op<T> *>(this)->Abufs[thread_id]
					= detail::local_matrix_store::ptr(
							new fm::detail::local_buf_col_matrix_store(0, 0,
								Astore->get_num_rows(), Astore->get_num_cols(),
								Astore->get_type(), -1));
			else
				const_cast<multiply_tall_op<T> *>(this)->Abufs[thread_id]
					= detail::local_matrix_store::ptr(
							new fm::detail::local_buf_row_matrix_store(0, 0,
								Astore->get_num_rows(), Astore->get_num_cols(),
								Astore->get_type(), -1));
		}
		Abufs[thread_id]->copy_from(*Astore);
		Amat = (const T *) Abufs[thread_id]->get_raw_arr();
	}
	const T *Bmat = (const T *) Bstore->get_raw_arr();
	assert(Bstore->store_layout() == out.store_layout());
	assert(Amat);
	assert(Bmat);

	T *res_mat = (T *) out.get_raw_arr();
	detail::local_matrix_store::ptr res_buf;
	if (res_mat == NULL) {
		detail::pool_task_thread *thread = dynamic_cast<detail::pool_task_thread *>(
				thread::get_curr_thread());
		int thread_id = thread->get_pool_thread_id();
		if (res_bufs[thread_id] == NULL
				|| out.get_num_rows() != res_bufs[thread_id]->get_num_rows()
				|| out.get_num_cols() != res_bufs[thread_id]->get_num_cols())
			const_cast<multiply_tall_op<T> *>(this)->res_bufs[thread_id]
				= detail::local_matrix_store::ptr(
					new fm::detail::local_buf_col_matrix_store(0, 0,
						out.get_num_rows(), out.get_num_cols(),
						out.get_type(), -1));
		res_buf = res_bufs[thread_id];
		res_mat = (T *) res_buf->get_raw_arr();
	}

	if (out.store_layout() == matrix_layout_t::L_COL)
		cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
				Astore->get_num_rows(), Bstore->get_num_cols(),
				Astore->get_num_cols(), 1, Amat,
				Astore->get_num_rows(), Bmat, Bstore->get_num_rows(),
				0, res_mat, out.get_num_rows());
	else
		cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
				Astore->get_num_rows(), Bstore->get_num_cols(),
				Astore->get_num_cols(), 1, Amat,
				Astore->get_num_cols(), Bmat, Bstore->get_num_cols(),
				0, res_mat, out.get_num_cols());
	if (res_buf)
		out.copy_from(*res_buf);
}

template<class T>
class multiply_wide_op: public detail::portion_mapply_op
{
	std::vector<detail::local_matrix_store::ptr> Abufs;
	std::vector<detail::local_matrix_store::ptr> Bbufs;
	std::vector<detail::local_matrix_store::ptr> res_bufs;
	size_t out_num_rows;
	size_t out_num_cols;
	matrix_layout_t Alayout;
	matrix_layout_t Blayout;
public:
	multiply_wide_op(size_t num_threads, size_t out_num_rows, size_t out_num_cols,
			matrix_layout_t required_layout): detail::portion_mapply_op(
				0, 0, get_scalar_type<T>()) {
		Abufs.resize(num_threads);
		Bbufs.resize(num_threads);
		res_bufs.resize(num_threads);
		this->out_num_rows = out_num_rows;
		this->out_num_cols = out_num_cols;
		// We need to transpose the A matrix, so we want the data in the A matrix
		// to be organized in the opposite layout to the required.
		if (required_layout == matrix_layout_t::L_COL)
			Alayout = matrix_layout_t::L_ROW;
		else
			Alayout = matrix_layout_t::L_COL;
		Blayout = required_layout;
	}

	const std::vector<detail::local_matrix_store::ptr> &get_partial_results(
			) const {
		return res_bufs;
	}

	virtual void run(
			const std::vector<detail::local_matrix_store::const_ptr> &ins) const;

	virtual detail::portion_mapply_op::const_ptr transpose() const {
		assert(0);
		return detail::portion_mapply_op::const_ptr();
	}

	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 2);
		return std::string("(") + (mats[0]->get_name()
					+ "*") + mats[1]->get_name() + std::string(")");
	}
};

template<class T>
void multiply_wide_op<T>::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins) const
{
	assert(ins.size() == 2);
	detail::pool_task_thread *thread = dynamic_cast<detail::pool_task_thread *>(
			thread::get_curr_thread());
	int thread_id = thread->get_pool_thread_id();

	detail::local_matrix_store::const_ptr Astore = ins[0];
	const T *Amat = (const T *) Astore->get_raw_arr();
	if (Amat == NULL || Astore->store_layout() != Alayout) {
		if (Abufs[thread_id] == NULL
				|| Astore->get_num_rows() != Abufs[thread_id]->get_num_rows()
				|| Astore->get_num_cols() != Abufs[thread_id]->get_num_cols()) {
			if (Alayout == matrix_layout_t::L_ROW)
				const_cast<multiply_wide_op<T> *>(this)->Abufs[thread_id]
					= detail::local_matrix_store::ptr(
							new fm::detail::local_buf_row_matrix_store(0, 0,
								Astore->get_num_rows(), Astore->get_num_cols(),
								Astore->get_type(), -1));
			else
				const_cast<multiply_wide_op<T> *>(this)->Abufs[thread_id]
					= detail::local_matrix_store::ptr(
							new fm::detail::local_buf_col_matrix_store(0, 0,
								Astore->get_num_rows(), Astore->get_num_cols(),
								Astore->get_type(), -1));
		}
		Abufs[thread_id]->copy_from(*Astore);
		Amat = (const T *) Abufs[thread_id]->get_raw_arr();
	}
	assert(Amat);

	detail::local_matrix_store::const_ptr Bstore = ins[1];
	const T *Bmat = (const T *) Bstore->get_raw_arr();
	if (Bmat == NULL || Bstore->store_layout() != Blayout) {
		if (Bbufs[thread_id] == NULL
				|| Bstore->get_num_rows() != Bbufs[thread_id]->get_num_rows()
				|| Bstore->get_num_cols() != Bbufs[thread_id]->get_num_cols()) {
			if (Blayout == matrix_layout_t::L_COL)
				const_cast<multiply_wide_op<T> *>(this)->Bbufs[thread_id]
					= detail::local_matrix_store::ptr(
							new fm::detail::local_buf_col_matrix_store(0, 0,
								Bstore->get_num_rows(), Bstore->get_num_cols(),
								Bstore->get_type(), -1));
			else
				const_cast<multiply_wide_op<T> *>(this)->Bbufs[thread_id]
					= detail::local_matrix_store::ptr(
							new fm::detail::local_buf_row_matrix_store(0, 0,
								Bstore->get_num_rows(), Bstore->get_num_cols(),
								Bstore->get_type(), -1));
		}
		Bbufs[thread_id]->copy_from(*Bstore);
		Bmat = (const T *) Bbufs[thread_id]->get_raw_arr();
	}
	assert(Bmat);

	if (res_bufs[thread_id] == NULL) {
		if (Blayout == matrix_layout_t::L_COL)
			const_cast<multiply_wide_op<T> *>(this)->res_bufs[thread_id]
				= detail::local_matrix_store::ptr(
						new fm::detail::local_buf_col_matrix_store(0, 0,
							out_num_rows, out_num_cols, get_scalar_type<T>(), -1));
		else
			const_cast<multiply_wide_op<T> *>(this)->res_bufs[thread_id]
				= detail::local_matrix_store::ptr(
						new fm::detail::local_buf_row_matrix_store(0, 0,
							out_num_rows, out_num_cols, get_scalar_type<T>(), -1));
		res_bufs[thread_id]->reset_data();
	}
	assert(res_bufs[thread_id]->store_layout() == Blayout);
	T *res_mat = (T *) res_bufs[thread_id]->get_raw_arr();
	// The A matrix is the transpose of the matrix we need. Since the A matrix
	// is stored in contiguous memory and is organized in row major, we can
	// easily interpret it as its transpose by switching its #rows and #cols.
	if (Blayout == matrix_layout_t::L_COL)
		cblas_dgemm(CblasColMajor, CblasNoTrans, CblasNoTrans,
				Astore->get_num_cols(), Bstore->get_num_cols(),
				Astore->get_num_rows(), 1, Amat,
				Astore->get_num_cols(), Bmat, Bstore->get_num_rows(),
				1, res_mat, out_num_rows);
	else
		cblas_dgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
				Astore->get_num_cols(), Bstore->get_num_cols(),
				Astore->get_num_rows(), 1, Amat,
				Astore->get_num_rows(), Bmat, Bstore->get_num_cols(),
				1, res_mat, out_num_cols);
}

}

static dense_matrix::ptr blas_multiply_tall(const dense_matrix &m1,
		const dense_matrix &m2, matrix_layout_t out_layout)
{
	if (out_layout == matrix_layout_t::L_NONE)
		out_layout = m1.store_layout();

	assert(m1.get_type() == get_scalar_type<double>());
	assert(m2.get_type() == get_scalar_type<double>());
	detail::matrix_store::const_ptr right = m2.get_raw_store();
	if (out_layout != m2.store_layout()) {
		dense_matrix::ptr tmp = m2.conv2(out_layout);
		tmp->materialize_self();
		right = tmp->get_raw_store();
	}
	if (right->is_virtual() || !right->is_in_mem() || right->get_num_nodes() > 0) {
		dense_matrix::ptr tmp = dense_matrix::create(right);
		tmp = tmp->conv_store(true, -1);
		right = tmp->get_raw_store();
	}
	assert(right->store_layout() == out_layout);
	assert(!right->is_virtual());
	assert(right->get_num_nodes() == -1);
	assert(right->is_in_mem());

	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = m1.get_raw_store();
	detail::mem_thread_pool::ptr threads
		= detail::mem_thread_pool::get_global_mem_threads();
	multiply_tall_op<double>::const_ptr mapply_op(new multiply_tall_op<double>(
				detail::mem_matrix_store::cast(right), threads->get_num_threads(),
				m1.get_num_rows(), m2.get_num_cols()));
	return dense_matrix::create(__mapply_portion_virtual(ins, mapply_op,
				out_layout));
}

static dense_matrix::ptr blas_multiply_wide(const dense_matrix &m1,
		const dense_matrix &m2, matrix_layout_t out_layout)
{
	detail::matrix_stats.inc_multiplies(
			m1.get_num_rows() * m1.get_num_cols() * m2.get_num_cols());

	matrix_layout_t required_layout;
	// If both input matrices have the same data layout, it's easy.
	if (m1.store_layout() == m2.store_layout())
		required_layout = m1.store_layout();
	// If they are different, we convert the smaller matrix.
	else if (m1.get_num_rows() * m1.get_num_cols()
			> m2.get_num_rows() * m2.get_num_cols())
		required_layout = m1.store_layout();
	else
		required_layout = m2.store_layout();
	if (out_layout == matrix_layout_t::L_NONE)
		out_layout = required_layout;

	assert(m1.get_type() == get_scalar_type<double>());
	assert(m2.get_type() == get_scalar_type<double>());

	detail::mem_thread_pool::ptr threads
		= detail::mem_thread_pool::get_global_mem_threads();
	size_t nthreads = threads->get_num_threads();

	std::vector<detail::matrix_store::const_ptr> mats(2);
	mats[0] = m1.get_data().transpose();
	assert(mats[0]);
	mats[1] = m2.get_raw_store();
	assert(mats[1]);
	size_t out_num_rows = m1.get_num_rows();
	size_t out_num_cols = m2.get_num_cols();
	std::shared_ptr<multiply_wide_op<double> > op(new multiply_wide_op<double> (
				nthreads, out_num_rows, out_num_cols, required_layout));
	__mapply_portion(mats, op, required_layout);
	std::vector<detail::local_matrix_store::ptr> local_ms
		= op->get_partial_results();
	assert(local_ms.size() == nthreads);

	// Aggregate the results from omp threads.
	// This matrix is small. We can always keep it in memory.
	detail::local_matrix_store::ptr local_res;
	if (required_layout == matrix_layout_t::L_ROW)
		local_res = detail::local_matrix_store::ptr(
				new detail::local_buf_row_matrix_store(0, 0,
					out_num_rows, out_num_cols, m1.get_type(), -1));
	else
		local_res = detail::local_matrix_store::ptr(
				new detail::local_buf_col_matrix_store(0, 0,
					out_num_rows, out_num_cols, m1.get_type(), -1));
	local_res->reset_data();
	const bulk_operate &add = get_scalar_type<double>().get_basic_ops().get_add();
	for (size_t j = 0; j < local_ms.size(); j++) {
		// It's possible that the local matrix store doesn't exist
		// because the input matrix is very small.
		if (local_ms[j])
			detail::mapply2(*local_res, *local_ms[j], add, *local_res);
	}

	detail::matrix_store::ptr res = detail::matrix_store::create(out_num_rows,
			out_num_cols, out_layout, m1.get_type(), -1, true);
	detail::local_matrix_store::ptr tmp = res->get_portion(0);
	assert(tmp->get_num_rows() == res->get_num_rows()
			&& tmp->get_num_cols() == res->get_num_cols());
	// This works for in-mem matrix. TODO Maybe it's not the best way of
	// copying data to the output matrix.
	tmp->copy_from(*local_res);
	return dense_matrix::create(res);
}

dense_matrix::ptr dense_matrix::multiply(const dense_matrix &mat,
		matrix_layout_t out_layout, bool use_blas) const
{
	if (get_type() == get_scalar_type<double>() && use_blas) {
		size_t long_dim1 = std::max(get_num_rows(), get_num_cols());
		size_t long_dim2 = std::max(mat.get_num_rows(), mat.get_num_cols());
		// We prefer to perform computation on the larger matrix.
		// If the matrix in the right operand is larger, we transpose
		// the entire computation.
		if (long_dim2 > long_dim1) {
			dense_matrix::ptr t_mat1 = this->transpose();
			dense_matrix::ptr t_mat2 = mat.transpose();
			matrix_layout_t t_layout = out_layout;
			if (t_layout == matrix_layout_t::L_ROW)
				t_layout = matrix_layout_t::L_COL;
			else if (t_layout == matrix_layout_t::L_COL)
				t_layout = matrix_layout_t::L_ROW;
			dense_matrix::ptr t_res = t_mat2->multiply(*t_mat1, t_layout,
					use_blas);
			return t_res->transpose();
		}

		if (is_wide())
			return blas_multiply_wide(*this, mat, out_layout);
		else
			return blas_multiply_tall(*this, mat, out_layout);
	}
	else if (get_type() == get_scalar_type<double>()) {
		bulk_operate::const_ptr add = bulk_operate::conv2ptr(
				get_scalar_type<long double>().get_basic_ops().get_add());
		bulk_operate::const_ptr multiply(new double_multiply_operate());
		dense_matrix::ptr res;
		if (is_wide())
			res = inner_prod(mat, multiply, add, out_layout);
		else
			res = inner_prod(mat, multiply, add, out_layout);
		assert(res->get_type() == get_scalar_type<long double>());
		dense_matrix::ptr ret = res->cast_ele_type(get_scalar_type<double>());
		return ret;
	}
	else {
		bulk_operate::const_ptr multiply = bulk_operate::conv2ptr(
				get_type().get_basic_ops().get_multiply());
		bulk_operate::const_ptr add = bulk_operate::conv2ptr(
				get_type().get_basic_ops().get_add());
		return inner_prod(mat, multiply, add, out_layout);
	}
}

namespace
{

class apply_scalar_op: public detail::portion_mapply_op
{
	scalar_variable::const_ptr var;
	bulk_operate::const_ptr op;
public:
	apply_scalar_op(scalar_variable::const_ptr var, bulk_operate::const_ptr op,
			size_t out_num_rows, size_t out_num_cols): detail::portion_mapply_op(
				out_num_rows, out_num_cols, op->get_output_type()) {
		this->var = var;
		this->op = op;
	}
	void run(const std::vector<std::shared_ptr<const detail::local_matrix_store> > &ins,
			detail::local_matrix_store &out) const;

	detail::portion_mapply_op::const_ptr transpose() const {
		return detail::portion_mapply_op::const_ptr(new apply_scalar_op(
					var, op, get_out_num_cols(), get_out_num_rows()));
	}

	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return (boost::format("apply_scalar(%1%, %2%)") % mats[0]->get_name()
				% var->get_name()).str();
	}
};

void apply_scalar_op::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 1);
	detail::matrix_stats.inc_multiplies(
			ins[0]->get_num_rows() * ins[0]->get_num_cols());

	assert(ins[0]->store_layout() == out.store_layout());
	assert(ins[0]->get_num_rows() == out.get_num_rows());
	assert(ins[0]->get_num_cols() == out.get_num_cols());
	if (ins[0]->get_raw_arr() && out.get_raw_arr())
		op->runAE(out.get_num_rows() * out.get_num_cols(),
				ins[0]->get_raw_arr(), var->get_raw(), out.get_raw_arr());
	else if (out.store_layout() == matrix_layout_t::L_COL) {
		const detail::local_col_matrix_store &col_in
			= static_cast<const detail::local_col_matrix_store &>(*ins[0]);
		detail::local_col_matrix_store &col_out
			= static_cast<detail::local_col_matrix_store &>(out);
		for (size_t i = 0; i < out.get_num_cols(); i++)
			op->runAE(out.get_num_rows(), col_in.get_col(i), var->get_raw(),
					col_out.get_col(i));
	}
	else {
		const detail::local_row_matrix_store &row_in
			= static_cast<const detail::local_row_matrix_store &>(*ins[0]);
		detail::local_row_matrix_store &row_out
			= static_cast<detail::local_row_matrix_store &>(out);
		for (size_t i = 0; i < out.get_num_rows(); i++)
			op->runAE(out.get_num_cols(), row_in.get_row(i), var->get_raw(),
					row_out.get_row(i));
	}
}

}

dense_matrix::ptr dense_matrix::apply_scalar(
		scalar_variable::const_ptr var, bulk_operate::const_ptr op) const
{
	if (get_type() != var->get_type()) {
		BOOST_LOG_TRIVIAL(error)
			<< "Can't multiply a scalar of incompatible type";
		return dense_matrix::ptr();
	}

	std::vector<detail::matrix_store::const_ptr> stores(1);
	stores[0] = store;
	detail::portion_mapply_op::const_ptr mapply_op(new apply_scalar_op(
				var, op, get_num_rows(), get_num_cols()));
	detail::matrix_store::ptr ret = __mapply_portion_virtual(stores,
			mapply_op, store_layout());
	return dense_matrix::create(ret);
}

namespace
{

/*
 * This class set elements in a container randomly.
 * set_operate can't change its own state and has to be thread-safe when
 * running on multiple threads. However, random generators aren't
 * thread-safe, so we have to create a random generator for each thread.
 */
class rand_init: public set_operate
{
public:
	enum rand_dist_type {
		NORM,
		UNIF,
		MAX_NUM,
	};
private:
	class rand_gen_wrapper {
		rand_gen::ptr gen;
	public:
		rand_gen_wrapper(rand_gen::ptr gen) {
			this->gen = gen;
		}

		rand_gen &get_gen() {
			return *gen;
		}
	};

	pthread_key_t gen_key;
	const scalar_type &type;
	const scalar_variable &var1;
	const scalar_variable &var2;
	rand_dist_type rand_dist;

	rand_gen &get_rand_gen() const {
		void *addr = pthread_getspecific(gen_key);
		if (addr == NULL) {
			if (rand_dist == rand_dist_type::NORM)
				addr = new rand_gen_wrapper(type.create_randn_gen(var1, var2));
			else if (rand_dist == rand_dist_type::UNIF)
				addr = new rand_gen_wrapper(type.create_randu_gen(var1, var2));
			else
				assert(0);
			int ret = pthread_setspecific(gen_key, addr);
			assert(ret == 0);
		}
		rand_gen_wrapper *wrapper = (rand_gen_wrapper *) addr;
		return wrapper->get_gen();
	}

	static void destroy_rand_gen(void *gen) {
		rand_gen_wrapper *wrapper = (rand_gen_wrapper *) gen;
		delete wrapper;
		printf("destroy rand gen\n");
	}
public:
	rand_init(const scalar_variable &_var1, const scalar_variable &_var2,
			rand_dist_type rand_dist): type(_var1.get_type()), var1(
				_var1), var2(_var2) {
		int ret = pthread_key_create(&gen_key, destroy_rand_gen);
		this->rand_dist = rand_dist;
		assert(ret == 0);
	}

	~rand_init() {
		pthread_key_delete(gen_key);
	}

	virtual void set(void *arr, size_t num_eles, off_t row_idx,
			off_t col_idx) const {
		get_rand_gen().gen(arr, num_eles);
	}
	virtual const scalar_type &get_type() const {
		return get_rand_gen().get_type();
	}
};

}

dense_matrix::ptr dense_matrix::_create_randu(const scalar_variable &min,
		const scalar_variable &max, size_t nrow, size_t ncol,
		matrix_layout_t layout, int num_nodes, bool in_mem,
		safs::safs_file_group::ptr group)
{
	assert(min.get_type() == max.get_type());
	detail::matrix_store::ptr store = detail::matrix_store::create(
			nrow, ncol, layout, min.get_type(), num_nodes, in_mem, group);
	store->set_data(rand_init(min, max, rand_init::rand_dist_type::UNIF));
	return dense_matrix::ptr(new dense_matrix(store));
}

dense_matrix::ptr dense_matrix::_create_randn(const scalar_variable &mean,
		const scalar_variable &var, size_t nrow, size_t ncol,
		matrix_layout_t layout, int num_nodes, bool in_mem,
		safs::safs_file_group::ptr group)
{
	assert(mean.get_type() == var.get_type());
	detail::matrix_store::ptr store = detail::matrix_store::create(
			nrow, ncol, layout, mean.get_type(), num_nodes, in_mem, group);
	store->set_data(rand_init(mean, var, rand_init::rand_dist_type::NORM));
	return dense_matrix::ptr(new dense_matrix(store));
}

dense_matrix::ptr dense_matrix::_create_const(scalar_variable::ptr val,
		size_t nrow, size_t ncol, matrix_layout_t layout, int num_nodes,
		bool in_mem, safs::safs_file_group::ptr group)
{
	detail::matrix_store::ptr store(new detail::one_val_matrix_store(
				val, nrow, ncol, layout, num_nodes));
	return dense_matrix::ptr(new dense_matrix(store));
}

void dense_matrix::materialize_self() const
{
	if (!store->is_virtual())
		return;
	const_cast<dense_matrix *>(this)->store
		= detail::virtual_matrix_store::cast(store)->materialize(
				store->is_in_mem(), store->get_num_nodes());
}

void dense_matrix::set_materialize_level(materialize_level level)
{
	const detail::virtual_matrix_store *tmp
		= dynamic_cast<const detail::virtual_matrix_store *>(store.get());
	// If the matrix isn't a virtual matrix, we don't need to materialize it.
	if (tmp == NULL)
		return;

	detail::virtual_matrix_store *tmp1
		= const_cast<detail::virtual_matrix_store *>(tmp);
	tmp1->set_materialize_level(level);
}

/********************************* mapply ************************************/

namespace detail
{

namespace
{

class mapply_task: public thread_task
{
	const std::vector<matrix_store::const_ptr> &mats;
	const std::vector<matrix_store::ptr> &out_mats;
	size_t portion_idx;
	const portion_mapply_op &op;
public:
	mapply_task(const std::vector<matrix_store::const_ptr> &_mats,
			size_t portion_idx, const portion_mapply_op &_op,
			const std::vector<matrix_store::ptr> &_out_mats): mats(
				_mats), out_mats(_out_mats), op(_op) {
		this->portion_idx = portion_idx;
	}

	void run();
};

void mapply_task::run()
{
	std::vector<detail::local_matrix_store::const_ptr> local_stores(
			mats.size());
	std::vector<detail::local_matrix_store::ptr> local_out_stores(
			out_mats.size());
	int node_id = thread::get_curr_thread()->get_node_id();
	for (size_t j = 0; j < mats.size(); j++) {
		local_stores[j] = mats[j]->get_portion(portion_idx);
		if (local_stores[j]->get_node_id() >= 0)
			assert(node_id == local_stores[j]->get_node_id());
	}
	for (size_t j = 0; j < out_mats.size(); j++) {
		local_out_stores[j] = out_mats[j]->get_portion(portion_idx);
		if (local_out_stores[j]->get_node_id() >= 0)
			assert(node_id == local_out_stores[j]->get_node_id());
	}

	if (local_out_stores.empty())
		op.run(local_stores);
	else if (local_out_stores.size() == 1)
		op.run(local_stores, *local_out_stores[0]);
	else
		op.run(local_stores, local_out_stores);
}

/*
 * This local write buffer helps to write data to part of a portion and
 * keep track of which parts are valid. It can flush data to EM matrix
 * when all parts have valid data.
 * This assumes that each part in the portion has the same size and users
 * always write data of the same size to the portion.
 */
class local_write_buffer
{
	matrix_store::ptr to_mat;
	size_t portion_start_row;
	size_t portion_start_col;
	size_t portion_num_rows;
	size_t portion_num_cols;
	local_matrix_store::ptr buf;

	// This indicates which parts of the portion has valid data.
	std::vector<bool> valid_parts;
	// This is the number of valid parts in this portion.
	size_t num_valid_parts;
	size_t min_portion_size;
	bool has_flushed;

	local_write_buffer(matrix_store::ptr to_mat, off_t global_start,
			size_t length, size_t min_portion_size);
public:
	typedef std::shared_ptr<local_write_buffer> ptr;

	static ptr create(matrix_store::ptr to_mat, off_t global_start,
			size_t length, size_t min_portion_size) {
		return ptr(new local_write_buffer(to_mat, global_start, length,
					min_portion_size));
	}

	~local_write_buffer() {
		assert(has_flushed);
	}

	bool is_all_valid() const {
		return num_valid_parts == valid_parts.size();
	}

	void flush() {
		assert(!has_flushed);
		// If the destination matrix isn't in memory, we need to write
		// data back.
		if (!to_mat->is_in_mem())
			to_mat->write_portion_async(buf, buf->get_global_start_row(),
					buf->get_global_start_col());
		has_flushed = true;
	}

	local_matrix_store::ptr set_part(size_t global_start_row,
			size_t global_start_col, size_t num_rows, size_t num_cols);
};

local_matrix_store::ptr local_write_buffer::set_part(size_t global_start_row,
		size_t global_start_col, size_t num_rows, size_t num_cols)
{
	assert(global_start_col >= (size_t) portion_start_col);
	assert(global_start_row >= (size_t) portion_start_row);
	size_t local_start;
	if (to_mat->is_wide())
		local_start = global_start_col - portion_start_col;
	else
		local_start = global_start_row - portion_start_row;
	assert(local_start % min_portion_size == 0);
	size_t part_idx = local_start / min_portion_size;
	// The part shouldn't be valid.
	assert(!valid_parts[part_idx]);
	valid_parts[part_idx] = true;
	num_valid_parts++;
	if (buf)
		return buf->get_portion(global_start_row - buf->get_global_start_row(),
				global_start_col - buf->get_global_start_col(),
				num_rows, num_cols);
	else
		// If the destination matrix is in memory, we write data to the matrix
		// directly.
		return to_mat->get_portion(global_start_row, global_start_col,
				num_rows, num_cols);
}

local_write_buffer::local_write_buffer(matrix_store::ptr to_mat,
		off_t global_start, size_t length, size_t min_portion_size)
{
	if (to_mat->is_wide()) {
		portion_start_row = 0;
		portion_start_col = global_start;
		portion_num_rows = to_mat->get_num_rows();
		portion_num_cols = length;
	}
	else {
		portion_start_row = global_start;
		portion_start_col = 0;
		portion_num_rows = length;
		portion_num_cols = to_mat->get_num_cols();
	}
	size_t num_parts = ceil(((double) length) / min_portion_size);
	valid_parts.resize(num_parts);

	this->has_flushed = false;
	this->num_valid_parts = 0;
	this->min_portion_size = min_portion_size;
	this->to_mat = to_mat;
	if (!to_mat->is_in_mem()) {
		if (to_mat->store_layout() == matrix_layout_t::L_ROW)
			buf = detail::local_matrix_store::ptr(
					new detail::local_buf_row_matrix_store(portion_start_row,
						portion_start_col, portion_num_rows, portion_num_cols,
						to_mat->get_type(), -1));
		else
			buf = detail::local_matrix_store::ptr(
					new detail::local_buf_col_matrix_store(portion_start_row,
						portion_start_col, portion_num_rows, portion_num_cols,
						to_mat->get_type(), -1));
	}
}

static size_t cal_min_portion_size(
		const std::vector<matrix_store::const_ptr> &mats1,
		const std::vector<matrix_store::ptr> &mats2)
{
	assert(mats1.size() > 0);
	if (mats1[0]->is_wide()) {
		size_t min_portion_size = mats1[0]->get_portion_size().second;
		for (size_t i = 1; i < mats1.size(); i++)
			min_portion_size = std::min(min_portion_size,
					mats1[i]->get_portion_size().second);
		for (size_t i = 0; i < mats2.size(); i++) {
			min_portion_size = std::min(min_portion_size,
					mats2[i]->get_portion_size().second);
			assert(mats2[i]->get_portion_size().second % min_portion_size == 0);
		}
		for (size_t i = 0; i < mats1.size(); i++)
			assert(mats1[i]->get_portion_size().second % min_portion_size == 0);
		return min_portion_size;
	}
	else {
		size_t min_portion_size = mats1[0]->get_portion_size().first;
		for (size_t i = 1; i < mats1.size(); i++)
			min_portion_size = std::min(min_portion_size,
					mats1[i]->get_portion_size().first);
		for (size_t i = 0; i < mats2.size(); i++) {
			min_portion_size = std::min(min_portion_size,
					mats2[i]->get_portion_size().first);
			assert(mats2[i]->get_portion_size().first % min_portion_size == 0);
		}
		for (size_t i = 0; i < mats1.size(); i++)
			assert(mats1[i]->get_portion_size().first % min_portion_size == 0);
		return min_portion_size;
	}
}

/*
 * This dispatcher issues I/O to access the same portion of all dense matrices
 * simultaneously. This may have good I/O performance, but may consume a lot
 * of memory when it runs for a large group of dense matrices.
 */
class EM_mat_mapply_par_dispatcher: public detail::EM_portion_dispatcher
{
	std::vector<matrix_store::const_ptr> mats;
	std::vector<matrix_store::ptr> res_mats;
	portion_mapply_op::const_ptr op;
	size_t min_portion_size;
public:
	EM_mat_mapply_par_dispatcher(
			const std::vector<matrix_store::const_ptr> &mats,
			const std::vector<matrix_store::ptr> &res_mats,
			portion_mapply_op::const_ptr op, size_t tot_len,
			size_t portion_size): detail::EM_portion_dispatcher(tot_len,
				portion_size) {
		this->mats = mats;
		this->res_mats = res_mats;
		this->op = op;
		this->min_portion_size = cal_min_portion_size(mats, res_mats);
	}

	virtual void create_task(off_t global_start, size_t length);
};

/*
 * This collects all the portions in a partition that are required by
 * an operation and are ready in memory.
 */
class collected_portions
{
	// The output matrices.
	std::vector<matrix_store::ptr> res_mats;
	// The portions with partial result.
	// This is only used when the operation is aggregation.
	local_matrix_store::ptr res_portion;
	// The portions with data ready.
	// This is only used when the operation isn't aggregation.
	std::vector<local_matrix_store::const_ptr> ready_portions;

	// The number of portions that are ready.
	size_t num_ready;
	// The number of portions that are required for the computation.
	size_t num_required;

	// The location of the portions.
	off_t global_start;
	size_t length;

	portion_mapply_op::const_ptr op;
public:
	typedef std::shared_ptr<collected_portions> ptr;

	collected_portions(const std::vector<matrix_store::ptr> &res_mats,
			portion_mapply_op::const_ptr op, size_t num_required,
			off_t global_start, size_t length) {
		this->res_mats = res_mats;
		this->global_start = global_start;
		this->length = length;
		this->op = op;
		this->num_required = num_required;
		this->num_ready = 0;
	}

	off_t get_global_start() const {
		return global_start;
	}

	size_t get_length() const {
		return length;
	}

	void run_all_portions();

	void add_ready_portion(local_matrix_store::const_ptr portion);

	bool is_complete() const {
		return num_required == num_ready;
	}
};

/*
 * This dispatcher accesses one portion of a dense matrix at a time in a thread,
 * although I/O is still performed asynchronously. This is particularly useful
 * when we compute on a large group of dense matrices. Users can split the large
 * group and build a hierarchy to compute on the dense matrices. In this hierarchy,
 * we can use this dispatcher to limit the matrices that are being accessed
 * at the same time to reduce memory consumption.
 */
class EM_mat_mapply_serial_dispatcher: public detail::EM_portion_dispatcher
{
	/*
	 * This contains the data structure for fetching data in a partition
	 * across all dense matrices in an operation.
	 * The first member contains the matrices whose portions haven't been
	 * fetched; the second member contains the portions that have been
	 * successfully fetched.
	 */
	typedef std::pair<std::deque<matrix_store::const_ptr>,
			collected_portions::ptr> part_state_t;

	std::vector<matrix_store::const_ptr> mats;
	std::vector<matrix_store::ptr> res_mats;

	// These maintain per-thread states.
	std::vector<part_state_t> part_states;
	portion_mapply_op::const_ptr op;
	size_t min_portion_size;
public:
	EM_mat_mapply_serial_dispatcher(
			const std::vector<matrix_store::const_ptr> &mats,
			const std::vector<matrix_store::ptr> &res_mats,
			portion_mapply_op::const_ptr op, size_t tot_len,
			size_t portion_size): detail::EM_portion_dispatcher(tot_len,
				portion_size) {
		this->mats = mats;
		this->res_mats = res_mats;
		this->op = op;
		this->min_portion_size = cal_min_portion_size(mats, res_mats);

		detail::mem_thread_pool::ptr threads
			= detail::mem_thread_pool::get_global_mem_threads();
		part_states.resize(threads->get_num_threads());
	}

	virtual void create_task(off_t global_start, size_t length);
	virtual bool issue_task();
};

class mapply_portion_compute: public portion_compute
{
	std::vector<detail::local_matrix_store::const_ptr> local_stores;
	std::vector<local_write_buffer::ptr> write_bufs;
	std::vector<matrix_store::ptr> to_mats;
	size_t num_required_reads;
	size_t num_reads;
	const portion_mapply_op &op;
public:
	mapply_portion_compute(const std::vector<local_write_buffer::ptr> &write_bufs,
			const std::vector<matrix_store::ptr> mats,
			const portion_mapply_op &_op): op(_op) {
		this->write_bufs = write_bufs;
		this->to_mats = mats;
		this->num_required_reads = 0;
		this->num_reads = 0;
	}

	void set_buf(
			const std::vector<detail::local_matrix_store::const_ptr> &stores) {
		this->local_stores = stores;
	}
	void set_EM_parts(size_t num_EM_parts) {
		this->num_required_reads = num_EM_parts;
	}

	virtual void run(char *buf, size_t size);

	void run_complete();
};

void mapply_portion_compute::run_complete()
{
	assert(!local_stores.empty());
	const detail::local_matrix_store &first_mat = *local_stores.front();
	std::vector<local_matrix_store::ptr> local_res(write_bufs.size());
	for (size_t i = 0; i < write_bufs.size(); i++) {
		size_t res_num_rows;
		size_t res_num_cols;
		if (to_mats[i]->is_wide()) {
			res_num_rows = to_mats[i]->get_num_rows();
			res_num_cols = first_mat.get_num_cols();
		}
		else {
			res_num_rows = first_mat.get_num_rows();
			res_num_cols = to_mats[i]->get_num_cols();
		}
		local_res[i] = write_bufs[i]->set_part(
				first_mat.get_global_start_row(),
				first_mat.get_global_start_col(), res_num_rows, res_num_cols);
	}
	if (local_res.empty())
		op.run(local_stores);
	else if (local_res.size() == 1)
		op.run(local_stores, *local_res[0]);
	else
		op.run(local_stores, local_res);
	for (size_t i = 0; i < write_bufs.size(); i++)
		if (write_bufs[i]->is_all_valid())
			write_bufs[i]->flush();
}

void mapply_portion_compute::run(char *buf, size_t size)
{
	assert(!local_stores.empty());
	num_reads++;
	if (num_required_reads == num_reads)
		run_complete();
}

/*
 * This method is invoked in each worker thread.
 */
void EM_mat_mapply_par_dispatcher::create_task(off_t global_start,
		size_t length)
{
	std::vector<local_write_buffer::ptr> write_bufs(res_mats.size());
	for (size_t i = 0; i < res_mats.size(); i++)
		write_bufs[i] = local_write_buffer::create(res_mats[i], global_start,
				length, min_portion_size);

	// We fetch the portions using the minimum portion size among
	// the matrices. The idea is to reduce the amount of data cached
	// in virtual matrices.
	for (size_t local_start = 0; local_start < length;
			local_start += min_portion_size) {
		size_t local_length = std::min(min_portion_size, length - local_start);
		std::vector<detail::local_matrix_store::const_ptr> local_stores(
				mats.size());
		mapply_portion_compute *mapply_compute = new mapply_portion_compute(
				write_bufs, res_mats, *op);
		mapply_portion_compute::ptr compute(mapply_compute);
		size_t num_EM_parts = 0;
		for (size_t j = 0; j < local_stores.size(); j++) {
			size_t global_start_row, global_start_col, num_rows, num_cols;
			if (mats[j]->is_wide()) {
				global_start_row = 0;
				global_start_col = global_start + local_start;
				num_rows = mats[j]->get_num_rows();
				num_cols = local_length;
			}
			else {
				global_start_row = global_start + local_start;
				global_start_col = 0;
				num_rows = local_length;
				num_cols = mats[j]->get_num_cols();
			}
			async_cres_t res = mats[j]->get_portion_async(global_start_row,
					global_start_col, num_rows, num_cols, compute);
			if (!res.first)
				num_EM_parts++;
			local_stores[j] = res.second;
		}
		mapply_compute->set_buf(local_stores);
		mapply_compute->set_EM_parts(num_EM_parts);
		// When all input parts are in memory or have been cached, we need to
		// run the portion compute manually by ourselves.
		if (num_EM_parts == 0)
			mapply_compute->run_complete();
	}
}

void collected_portions::add_ready_portion(local_matrix_store::const_ptr portion)
{
	num_ready++;
	assert(num_ready <= num_required);

	if (op->is_agg() && res_portion) {
		std::vector<local_matrix_store::const_ptr> local_stores(2);
		local_stores[0] = res_portion;
		local_stores[1] = portion;
		// We store the partial result in a single portion, regardless of
		// the number of output matrices we want to generate eventually.
		op->run(local_stores, *res_portion);
	}
	else if (op->is_agg()) {
		if (op->get_out_num_rows() > 0 && op->get_out_num_cols() > 0) {
			// Regardless of the number of output matrices we want to generate
			// eventually, we only use one matrix portion to store
			// the intermediate aggregation result.
			size_t start_row, start_col, num_rows, num_cols;
			if (res_mats.front()->is_wide()) {
				start_row = 0;
				start_col = global_start;
				num_rows = op->get_out_num_rows();
				num_cols = length;
			}
			else {
				start_row = global_start;
				start_col = 0;
				num_rows = length;
				num_cols = op->get_out_num_cols();
			}

			if (res_mats.front()->store_layout() == matrix_layout_t::L_COL)
				res_portion = local_matrix_store::ptr(
						new local_buf_col_matrix_store(start_row, start_col,
							num_rows, num_cols, op->get_output_type(),
							portion->get_node_id()));
			else
				res_portion = local_matrix_store::ptr(
						new local_buf_row_matrix_store(start_row, start_col,
							num_rows, num_cols, op->get_output_type(),
							portion->get_node_id()));

			std::vector<local_matrix_store::const_ptr> local_stores(1);
			local_stores[0] = portion;
			// We rely on the user-defined function to copy the first portion
			// to the partial result portion.
			op->run(local_stores, *res_portion);
		}
		else {
			std::vector<local_matrix_store::const_ptr> local_stores(1);
			local_stores[0] = portion;
			// We rely on the user-defined function to copy the first portion
			// to the partial result portion.
			op->run(local_stores);
		}
	}
	else {
		// This forces the portion of a mapply matrix to materialize and
		// release the data in the underlying portion.
		// TODO it's better to have a better way to materialize the mapply
		// matrix portion.
		portion->get_raw_arr();
		ready_portions.push_back(portion);
	}
}

void collected_portions::run_all_portions()
{
	assert(is_complete());
	// If this is an aggregation operation and no result portion was generated,
	// return now.
	if (op->is_agg() && res_portion == NULL)
		return;

	std::vector<local_write_buffer::ptr> write_bufs(res_mats.size());
	for (size_t i = 0; i < res_mats.size(); i++)
		write_bufs[i] = local_write_buffer::create(res_mats[i],
				global_start, length, length);
	mapply_portion_compute compute(write_bufs, res_mats, *op);

	if (res_portion) {
		std::vector<detail::local_matrix_store::const_ptr> stores(1);
		stores[0] = res_portion;
		compute.set_buf(stores);
		compute.run_complete();
		res_portion = NULL;
	}
	else {
		compute.set_buf(ready_portions);
		compute.run_complete();

		// We don't need these portions now. They should be free'd.
		ready_portions.clear();
	}
}

/*
 * This class reads the EM portions one at a time. After it reads all portions,
 * it invokes mapply_portion_compute.
 */
class serial_read_portion_compute: public portion_compute
{
	collected_portions::ptr collected;

	// The portion that is being read.
	local_matrix_store::const_ptr pending_portion;
public:
	serial_read_portion_compute(collected_portions::ptr collected) {
		this->collected = collected;
	}

	virtual void run(char *buf, size_t size) {
		collected->add_ready_portion(pending_portion);
		pending_portion = NULL;
		if (collected->is_complete())
			collected->run_all_portions();
		collected = NULL;
	}

	static void fetch_portion(std::deque<matrix_store::const_ptr> &mats,
			collected_portions::ptr collected);
};

// TODO I need to copy the vectors multiple times. Is this problematic?
void serial_read_portion_compute::fetch_portion(
		std::deque<matrix_store::const_ptr> &mats,
		collected_portions::ptr collected)
{
	serial_read_portion_compute *_compute
		= new serial_read_portion_compute(collected);
	serial_read_portion_compute::ptr compute(_compute);
	size_t global_start_row, global_start_col, num_rows, num_cols;
	while (!mats.empty()) {
		matrix_store::const_ptr mat = mats.front();
		mats.pop_front();
		if (mat->is_wide()) {
			global_start_row = 0;
			global_start_col = collected->get_global_start();
			num_rows = mat->get_num_rows();
			num_cols = collected->get_length();
		}
		else {
			global_start_row = collected->get_global_start();
			global_start_col = 0;
			num_rows = collected->get_length();
			num_cols = mat->get_num_cols();
		}
		async_cres_t res = mat->get_portion_async(global_start_row,
				global_start_col, num_rows, num_cols, compute);
		if (!res.first) {
			_compute->pending_portion = res.second;
			break;
		}
		else
			collected->add_ready_portion(res.second);
	}
	if (collected->is_complete()) {
		// All portions are ready, we can perform computation now.
		collected->run_all_portions();
	}
}

/*
 * This method is invoked in each worker thread.
 */
void EM_mat_mapply_serial_dispatcher::create_task(off_t global_start,
		size_t length)
{
	detail::pool_task_thread *curr
		= dynamic_cast<detail::pool_task_thread *>(thread::get_curr_thread());
	int thread_id = curr->get_pool_thread_id();
	assert(part_states[thread_id].first.empty());
	assert(part_states[thread_id].second == NULL);

	// Here we don't use the minimum portion size.
	collected_portions::ptr collected(new collected_portions(res_mats, op,
				mats.size(), global_start, length));
	part_states[thread_id].second = collected;
	part_states[thread_id].first.insert(part_states[thread_id].first.end(),
			mats.begin(), mats.end());
	serial_read_portion_compute::fetch_portion(part_states[thread_id].first,
			collected);
}

bool EM_mat_mapply_serial_dispatcher::issue_task()
{
	detail::pool_task_thread *curr
		= dynamic_cast<detail::pool_task_thread *>(thread::get_curr_thread());
	int thread_id = curr->get_pool_thread_id();
	// If the data of the matrices in the current partition hasn't been
	// fetched, we fetch them first.
	if (!part_states[thread_id].first.empty()) {
		assert(part_states[thread_id].second != NULL);
		serial_read_portion_compute::fetch_portion(
				part_states[thread_id].first, part_states[thread_id].second);
		return true;
	}

	// If we are ready to access the next partition, we reset the pointer
	// to the collection of the portions in the current partition.
	part_states[thread_id].second = NULL;
	return EM_portion_dispatcher::issue_task();
}

}

matrix_store::ptr __mapply_portion(
		const std::vector<matrix_store::const_ptr> &mats,
		portion_mapply_op::const_ptr op, matrix_layout_t out_layout,
		bool par_access)
{
	// As long as one of the input matrices is in external memory, we output
	// an EM matrix.
	bool out_in_mem = mats.front()->is_in_mem();
	for (size_t i = 1; i < mats.size(); i++)
		out_in_mem = out_in_mem && mats[i]->is_in_mem();

	int num_nodes = -1;
	if (out_in_mem) {
		num_nodes = mats[0]->get_num_nodes();
		for (size_t i = 1; i < mats.size(); i++)
			num_nodes = std::max(num_nodes, mats[i]->get_num_nodes());
	}
	std::vector<matrix_store::ptr> out_mats;
	if (op->get_out_num_rows() > 0 && op->get_out_num_cols() > 0) {
		detail::matrix_store::ptr res = detail::matrix_store::create(
				op->get_out_num_rows(), op->get_out_num_cols(),
				out_layout, op->get_output_type(), num_nodes, out_in_mem);
		assert(res);
		out_mats.push_back(res);
	}
	bool ret = __mapply_portion(mats, op, out_mats, par_access);
	if (ret && out_mats.size() == 1)
		return out_mats[0];
	else
		return matrix_store::ptr();
}

matrix_store::ptr __mapply_portion(
		const std::vector<matrix_store::const_ptr> &mats,
		portion_mapply_op::const_ptr op, matrix_layout_t out_layout,
		bool out_in_mem, int out_num_nodes, bool par_access)
{
	std::vector<matrix_store::ptr> out_mats;
	if (op->get_out_num_rows() > 0 && op->get_out_num_cols() > 0) {
		detail::matrix_store::ptr res = detail::matrix_store::create(
				op->get_out_num_rows(), op->get_out_num_cols(),
				out_layout, op->get_output_type(), out_num_nodes, out_in_mem);
		assert(res);
		out_mats.push_back(res);
	}
	bool ret = __mapply_portion(mats, op, out_mats, par_access);
	if (ret && out_mats.size() == 1)
		return out_mats[0];
	else
		return matrix_store::ptr();
}

// This function computes the result of mapply.
// We allow to not output a matrix.
bool __mapply_portion(
		const std::vector<matrix_store::const_ptr> &mats,
		portion_mapply_op::const_ptr op,
		const std::vector<matrix_store::ptr> &out_mats, bool par_access)
{
	bool out_in_mem;
	int out_num_nodes;
	if (out_mats.empty()) {
		out_in_mem = true;
		out_num_nodes = -1;
	}
	else {
		out_in_mem = out_mats.front()->is_in_mem();
		out_num_nodes = out_mats.front()->get_num_nodes();
		detail::matrix_stats.inc_write_bytes(
				out_mats[0]->get_num_rows() * out_mats[0]->get_num_cols()
				* out_mats[0]->get_entry_size(), out_in_mem);
	}
	for (size_t i = 1; i < out_mats.size(); i++) {
		assert(out_in_mem == out_mats[i]->is_in_mem());
		assert(out_num_nodes == out_mats[i]->get_num_nodes());
		detail::matrix_stats.inc_write_bytes(
				out_mats[i]->get_num_rows() * out_mats[i]->get_num_cols()
				* out_mats[i]->get_entry_size(), out_in_mem);
	}
	assert(mats.size() >= 1);

	bool all_in_mem = mats[0]->is_in_mem();
	size_t num_chunks = mats.front()->get_num_portions();
	std::pair<size_t, size_t> first_size = mats.front()->get_portion_size();
	size_t tot_len;
	size_t portion_size;
	if (mats.front()->is_wide()) {
		tot_len = mats.front()->get_num_cols();
		portion_size = first_size.second;
		if (op->get_out_num_cols() > 0)
			assert(op->get_out_num_cols() == mats.front()->get_num_cols());
		for (size_t i = 1; i < mats.size(); i++) {
			portion_size = std::max(portion_size,
					mats[i]->get_portion_size().second);
			assert(mats[i]->get_num_cols() == tot_len);
			all_in_mem = all_in_mem && mats[i]->is_in_mem();
		}
	}
	else {
		tot_len = mats.front()->get_num_rows();
		portion_size = first_size.first;
		if (op->get_out_num_rows() > 0)
			assert(op->get_out_num_rows() == mats.front()->get_num_rows());
		for (size_t i = 1; i < mats.size(); i++) {
			portion_size = std::max(portion_size,
					mats[i]->get_portion_size().first);
			assert(mats[i]->get_num_rows() == tot_len);
			all_in_mem = all_in_mem && mats[i]->is_in_mem();
		}
	}
	all_in_mem = all_in_mem && out_in_mem;

	if (all_in_mem) {
		detail::mem_thread_pool::ptr mem_threads
			= detail::mem_thread_pool::get_global_mem_threads();
		for (size_t i = 0; i < num_chunks; i++) {
			int node_id = -1;
			for (size_t j = 0; j < mats.size(); j++) {
				if (node_id < 0)
					node_id = mats[j]->get_portion_node_id(i);
				else if (mats[j]->get_portion_node_id(i) >= 0)
					assert(node_id == mats[j]->get_portion_node_id(i));
			}
			for (size_t j = 0; j < out_mats.size(); j++) {
				if (node_id < 0)
					node_id = out_mats[j]->get_portion_node_id(i);
				else if (out_mats[j]->get_portion_node_id(i) >= 0)
					assert(node_id == out_mats[j]->get_portion_node_id(i));
			}

			// If the local matrix portion is not assigned to any node, 
			// assign the tasks in round robin fashion.
			if (node_id < 0)
				node_id = i % mem_threads->get_num_nodes();
			mem_threads->process_task(node_id, new mapply_task(mats, i, *op,
						out_mats));
		}
		mem_threads->wait4complete();
	}
	else {
		mem_thread_pool::ptr threads = mem_thread_pool::get_global_mem_threads();
		detail::EM_portion_dispatcher::ptr dispatcher;
		if (par_access)
			dispatcher = detail::EM_portion_dispatcher::ptr(
					new EM_mat_mapply_par_dispatcher(mats, out_mats, op,
						tot_len, EM_matrix_store::CHUNK_SIZE));
		else
			dispatcher = detail::EM_portion_dispatcher::ptr(
					new EM_mat_mapply_serial_dispatcher(mats, out_mats, op,
						tot_len, EM_matrix_store::CHUNK_SIZE));
		for (size_t i = 0; i < threads->get_num_threads(); i++) {
			io_worker_task *task = new io_worker_task(dispatcher, 16);
			for (size_t j = 0; j < mats.size(); j++) {
				if (!mats[j]->is_in_mem()) {
					const EM_object *obj
						= dynamic_cast<const EM_object *>(mats[j].get());
					task->register_EM_obj(const_cast<EM_object *>(obj));
				}
			}
			for (size_t j = 0; j < out_mats.size(); j++) {
				if (!out_mats[j]->is_in_mem())
					task->register_EM_obj(
							dynamic_cast<EM_object *>(out_mats[j].get()));
			}
			threads->process_task(i % threads->get_num_nodes(), task);
		}
		threads->wait4complete();
	}
	return true;
}

matrix_store::ptr __mapply_portion_virtual(
		const std::vector<matrix_store::const_ptr> &stores,
		portion_mapply_op::const_ptr op, matrix_layout_t out_layout,
		bool par_access)
{
	mapply_matrix_store *store = new mapply_matrix_store(stores, op,
			out_layout, op->get_out_num_rows(), op->get_out_num_cols());
	store->set_par_access(par_access);
	return matrix_store::ptr(store);
}

dense_matrix::ptr mapply_portion(
		const std::vector<dense_matrix::const_ptr> &mats,
		portion_mapply_op::const_ptr op, matrix_layout_t out_layout,
		bool par_access)
{
	std::vector<matrix_store::const_ptr> stores(mats.size());
	for (size_t i = 0; i < stores.size(); i++)
		stores[i] = mats[i]->get_raw_store();
	mapply_matrix_store *store = new mapply_matrix_store(stores, op,
			out_layout, op->get_out_num_rows(), op->get_out_num_cols());
	store->set_par_access(par_access);
	matrix_store::const_ptr ret(store);
	return dense_matrix::create(ret);
}

}

/*************************** mapply applications *****************************/

///////////////////////// Scale rows and columns //////////////////////////////

namespace
{

class mapply_col_op: public detail::portion_mapply_op
{
	detail::mem_vec_store::const_ptr vals;
	bulk_operate::const_ptr op;
public:
	mapply_col_op(detail::mem_vec_store::const_ptr vals, bulk_operate::const_ptr op,
			size_t out_num_rows, size_t out_num_cols): detail::portion_mapply_op(
				out_num_rows, out_num_cols, op->get_output_type()) {
		this->vals = vals;
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const;

	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return std::string("mapply_col(") + mats[0]->get_name() + ", vec"
			+ std::string(")");
	}
};

class mapply_row_op: public detail::portion_mapply_op
{
	detail::mem_vec_store::const_ptr vals;
	bulk_operate::const_ptr op;
public:
	mapply_row_op(detail::mem_vec_store::const_ptr vals, bulk_operate::const_ptr op,
			size_t out_num_rows, size_t out_num_cols): detail::portion_mapply_op(
				out_num_rows, out_num_cols, op->get_output_type()) {
		this->vals = vals;
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const;

	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return std::string("mapply_row(") + mats[0]->get_name() + ", vec"
			+ std::string(")");
	}
};

detail::portion_mapply_op::const_ptr mapply_col_op::transpose() const
{
	return detail::portion_mapply_op::const_ptr(new mapply_row_op(vals, op,
				get_out_num_cols(), get_out_num_rows()));
}

void mapply_row_op::run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 1);
	detail::matrix_stats.inc_multiplies(
			ins[0]->get_num_rows() * ins[0]->get_num_cols());

	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	// This is a tall matrix. We divide the matrix horizontally.
	if (ins[0]->get_num_cols() == get_out_num_cols()) {
		// If we use get_raw_arr, it may not work with NUMA vector.
		const char *arr = vals->get_sub_arr(0, vals->get_length());
		assert(arr);
		local_cref_vec_store lvals(arr, 0, vals->get_length(),
				vals->get_type(), -1);
		detail::mapply_rows(*ins[0], lvals, *op, out);
	}
	// We divide the matrix vertically.
	else {
		off_t global_start = ins[0]->get_global_start_col();
		size_t len = ins[0]->get_num_cols();
		local_vec_store::const_ptr portion = vals->get_portion(global_start,
				len);
		assert(portion);
		detail::mapply_rows(*ins[0], *portion, *op, out);
	}
}

void mapply_col_op::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 1);
	detail::matrix_stats.inc_multiplies(
			ins[0]->get_num_rows() * ins[0]->get_num_cols());

	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	// This is a wide matrix. We divide the matrix vertically.
	if (ins[0]->get_num_rows() == get_out_num_rows()) {
		// If we use get_raw_arr, it may not work with NUMA vector.
		const char *arr = vals->get_sub_arr(0, vals->get_length());
		assert(arr);
		local_cref_vec_store lvals(arr, 0, vals->get_length(),
				vals->get_type(), -1);
		detail::mapply_cols(*ins[0], lvals, *op, out);
	}
	// We divide the tall matrix horizontally.
	else {
		off_t global_start = ins[0]->get_global_start_row();
		size_t len = ins[0]->get_num_rows();
		local_vec_store::const_ptr portion = vals->get_portion(global_start,
				len);
		assert(portion);
		detail::mapply_cols(*ins[0], *portion, *op, out);
	}
}

detail::portion_mapply_op::const_ptr mapply_row_op::transpose() const
{
	return detail::portion_mapply_op::const_ptr(new mapply_col_op(vals, op,
				get_out_num_cols(), get_out_num_rows()));
}

}

dense_matrix::ptr dense_matrix::mapply_cols(vector::const_ptr vals,
		bulk_operate::const_ptr op) const
{
	if (!vals->is_in_mem()) {
		BOOST_LOG_TRIVIAL(error) << "Can't scale columns with an EM vector";
		return dense_matrix::ptr();
	}
	if (get_num_rows() != vals->get_length()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The vector's length needs to equal to #rows";
		return dense_matrix::ptr();
	}
	if (get_type() != vals->get_type()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The vector needs to have the same type as the matrix";
		return dense_matrix::ptr();
	}

	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = this->get_raw_store();
	mapply_col_op::const_ptr mapply_op(new mapply_col_op(
				detail::mem_vec_store::cast(vals->get_raw_store()),
				op, get_num_rows(), get_num_cols()));
	detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
			mapply_op, this->store_layout());
	return dense_matrix::create(ret);
}

dense_matrix::ptr dense_matrix::mapply_rows(vector::const_ptr vals,
		bulk_operate::const_ptr op) const
{
	if (!vals->is_in_mem()) {
		BOOST_LOG_TRIVIAL(error) << "Can't scale rows with an EM vector";
		return dense_matrix::ptr();
	}
	if (get_num_cols() != vals->get_length()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The vector's length needs to equal to #columns";
		return dense_matrix::ptr();
	}
	if (get_type() != vals->get_type()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The vector needs to have the same type as the matrix";
		return dense_matrix::ptr();
	}

	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = this->get_raw_store();
	mapply_row_op::const_ptr mapply_op(new mapply_row_op(
				detail::mem_vec_store::cast(vals->get_raw_store()),
				op, get_num_rows(), get_num_cols()));
	detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
			mapply_op, this->store_layout());
	return dense_matrix::create(ret);
}

//////////////////////////// Cast the element types ///////////////////////////

dense_matrix::ptr dense_matrix::cast_ele_type(const scalar_type &type) const
{
	if (!require_cast(get_type(), type))
		// TODO the returned matrix may not have the specified type.
		return dense_matrix::create(get_raw_store());
	else
		return sapply(bulk_uoperate::conv2ptr(get_type().get_type_cast(type)));
}

///////////////////////////////////// mapply2 /////////////////////////////////

namespace
{

class mapply2_op: public detail::portion_mapply_op
{
	bulk_operate::const_ptr op;
public:
	mapply2_op(bulk_operate::const_ptr op, size_t out_num_rows,
			size_t out_num_cols): detail::portion_mapply_op(out_num_rows,
				out_num_cols, op->get_output_type()) {
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const {
		return portion_mapply_op::const_ptr(new mapply2_op(op,
					get_out_num_cols(), get_out_num_rows()));
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 2);
		return op->get_name() + std::string("(") + mats[0]->get_name()
			+ ", " + mats[1]->get_name() + ")";
	}
};

void mapply2_op::run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 2);
	assert(ins[0]->get_global_start_col() == ins[1]->get_global_start_col());
	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == ins[1]->get_global_start_row());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	detail::mapply2(*ins[0], *ins[1], *op, out);
}

}

dense_matrix::ptr dense_matrix::mapply2(const dense_matrix &m,
		bulk_operate::const_ptr op) const
{
	// The same shape and the same data layout.
	if (!verify_mapply2(m, *op))
		return dense_matrix::ptr();

	std::vector<detail::matrix_store::const_ptr> ins(2);
	ins[0] = this->get_raw_store();
	if (this->store_layout() == m.store_layout())
		ins[1] = m.get_raw_store();
	else {
		dense_matrix::ptr m1 = m.conv2(this->store_layout());
		ins[1] = m1->get_raw_store();
	}
	mapply2_op::const_ptr mapply_op(new mapply2_op(op, get_num_rows(),
				get_num_cols()));
	return dense_matrix::create(__mapply_portion_virtual(ins, mapply_op,
				this->store_layout()));
}

namespace
{

class sapply_op: public detail::portion_mapply_op
{
	bulk_uoperate::const_ptr op;
public:
	sapply_op(bulk_uoperate::const_ptr op, size_t out_num_rows,
			size_t out_num_cols): detail::portion_mapply_op(out_num_rows,
				out_num_cols, op->get_output_type()) {
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const {
		return portion_mapply_op::const_ptr(new sapply_op(op, get_out_num_cols(),
					get_out_num_rows()));
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return op->get_name() + std::string("(") + mats[0]->get_name() + ")";
	}
};

void sapply_op::run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 1);
	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	detail::sapply(*ins[0], *op, out);
}

}

dense_matrix::ptr dense_matrix::sapply(bulk_uoperate::const_ptr op) const
{
	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = this->get_raw_store();
	sapply_op::const_ptr mapply_op(new sapply_op(op, get_num_rows(),
				get_num_cols()));
	detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
			mapply_op, this->store_layout());
	return dense_matrix::create(ret);
}

dense_matrix::dense_matrix(size_t nrow, size_t ncol, matrix_layout_t layout,
			const scalar_type &type, int num_nodes, bool in_mem,
			safs::safs_file_group::ptr group)
{
	store = detail::matrix_store::ptr(new detail::one_val_matrix_store(
				type.create_scalar(), nrow, ncol, layout, num_nodes));
}

dense_matrix::ptr dense_matrix::create(size_t nrow, size_t ncol,
		matrix_layout_t layout, const scalar_type &type, int num_nodes,
		bool in_mem, safs::safs_file_group::ptr group)
{
	// If nothing is specified, it creates a zero matrix.
	detail::matrix_store::ptr store(new detail::one_val_matrix_store(
				type.create_scalar(), nrow, ncol, layout, num_nodes));
	return dense_matrix::ptr(new dense_matrix(store));
}

dense_matrix::ptr dense_matrix::create(size_t nrow, size_t ncol,
		matrix_layout_t layout, const scalar_type &type, const set_operate &op,
		int num_nodes, bool in_mem, safs::safs_file_group::ptr group)
{
	detail::matrix_store::ptr store = detail::matrix_store::create(
			nrow, ncol, layout, type, num_nodes, in_mem, group);
	store->set_data(op);
	return dense_matrix::ptr(new dense_matrix(store));
}

vector::ptr dense_matrix::get_col(off_t idx) const
{
	detail::vec_store::const_ptr vec = get_data().get_col_vec(idx);
	if (vec)
		return vector::create(vec);
	else
		return vector::ptr();
}

vector::ptr dense_matrix::get_row(off_t idx) const
{
	detail::vec_store::const_ptr vec = get_data().get_row_vec(idx);
	if (vec)
		return vector::create(vec);
	else
		return vector::ptr();
}

dense_matrix::ptr dense_matrix::get_cols(const std::vector<off_t> &idxs) const
{
	detail::matrix_store::const_ptr ret = get_data().get_cols(idxs);
	if (ret)
		return dense_matrix::ptr(new dense_matrix(ret));
	else
		return dense_matrix::ptr();
}

dense_matrix::ptr dense_matrix::get_rows(const std::vector<off_t> &idxs) const
{
	detail::matrix_store::const_ptr ret = get_data().get_rows(idxs);
	if (ret)
		return dense_matrix::ptr(new dense_matrix(ret));
	else
		return dense_matrix::ptr();
}

dense_matrix::ptr dense_matrix::transpose() const
{
	return dense_matrix::ptr(new dense_matrix(get_data().transpose()));
}

////////////////////////////// Inner product //////////////////////////////////

dense_matrix::ptr dense_matrix::inner_prod(const dense_matrix &m,
		bulk_operate::const_ptr left_op, bulk_operate::const_ptr right_op,
		matrix_layout_t out_layout) const
{
	if (!verify_inner_prod(m, *left_op, *right_op))
		return dense_matrix::ptr();

	size_t long_dim1 = std::max(get_num_rows(), get_num_cols());
	size_t long_dim2 = std::max(m.get_num_rows(), m.get_num_cols());
	// We prefer to perform computation on the larger matrix.
	// If the matrix in the right operand is larger, we transpose
	// the entire computation.
	if (long_dim2 > long_dim1) {
		dense_matrix::ptr t_mat1 = this->transpose();
		dense_matrix::ptr t_mat2 = m.transpose();
		matrix_layout_t t_layout = out_layout;
		if (t_layout == matrix_layout_t::L_ROW)
			t_layout = matrix_layout_t::L_COL;
		else if (t_layout == matrix_layout_t::L_COL)
			t_layout = matrix_layout_t::L_ROW;
		dense_matrix::ptr t_res = t_mat2->inner_prod(*t_mat1,
				left_op, right_op, t_layout);
		return t_res->transpose();
	}

	if (out_layout == matrix_layout_t::L_NONE) {
		if (this->store_layout() == matrix_layout_t::L_ROW)
			out_layout = matrix_layout_t::L_ROW;
		else if (this->is_wide())
			out_layout = matrix_layout_t::L_ROW;
		else
			out_layout = matrix_layout_t::L_COL;
	}

	detail::matrix_store::ptr res;
	if (is_wide())
		res = inner_prod_wide(m, left_op, right_op, out_layout);
	else
		res = inner_prod_tall(m, left_op, right_op, out_layout);

	return dense_matrix::ptr(new dense_matrix(res));
}

namespace
{

class inner_prod_tall_op: public detail::portion_mapply_op
{
	// I need to keep the right matrix to make sure the memory isn't deallocated.
	detail::matrix_store::const_ptr right;
	detail::local_matrix_store::const_ptr local_right;
	bulk_operate::const_ptr left_op;
	bulk_operate::const_ptr right_op;
public:
	inner_prod_tall_op(detail::matrix_store::const_ptr right,
			bulk_operate::const_ptr left_op, bulk_operate::const_ptr right_op,
			size_t out_num_rows, size_t out_num_cols): detail::portion_mapply_op(
				out_num_rows, out_num_cols, right_op->get_output_type()) {
		this->right = right;
		// We assume the right matrix is small, so we don't need to partition it.
		this->local_right = right->get_portion(0);
		assert(local_right->get_num_rows() == right->get_num_rows()
				&& local_right->get_num_cols() == right->get_num_cols());
		this->left_op = left_op;
		this->right_op = right_op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const;
	virtual portion_mapply_op::const_ptr transpose() const;
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return std::string("inner_prod(") + mats[0]->get_name() + ","
			+ local_right->get_name() + ")";
	}
};

class t_inner_prod_tall_op: public detail::portion_mapply_op
{
	inner_prod_tall_op op;
public:
	t_inner_prod_tall_op(const inner_prod_tall_op &_op): detail::portion_mapply_op(
			_op.get_out_num_cols(), _op.get_out_num_rows(),
			_op.get_output_type()), op(_op) {
	}

	virtual void run(
			const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const {
		assert(ins.size() == 1);
		std::vector<fm::detail::local_matrix_store::const_ptr> t_ins(ins.size());
		t_ins[0] = std::static_pointer_cast<const fm::detail::local_matrix_store>(
				ins[0]->transpose());
		fm::detail::local_matrix_store::ptr t_out
			= std::static_pointer_cast<fm::detail::local_matrix_store>(
					out.transpose());
		op.run(t_ins, *t_out);
	}

	virtual detail::portion_mapply_op::const_ptr transpose() const {
		return fm::detail::portion_mapply_op::const_ptr(
				new inner_prod_tall_op(op));
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		return op.to_string(mats);
	}
};

detail::portion_mapply_op::const_ptr inner_prod_tall_op::transpose() const
{
	return fm::detail::portion_mapply_op::const_ptr(
			new t_inner_prod_tall_op(*this));
}

void inner_prod_tall_op::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins,
		detail::local_matrix_store &out) const
{
	assert(ins.size() == 1);
	assert(ins[0]->get_global_start_col() == out.get_global_start_col());
	assert(ins[0]->get_global_start_row() == out.get_global_start_row());
	out.reset_data();
	detail::inner_prod(*ins[0], *local_right, *left_op, *right_op, out);
}

}

detail::matrix_store::ptr dense_matrix::inner_prod_tall(
		const dense_matrix &m, bulk_operate::const_ptr left_op,
		bulk_operate::const_ptr right_op, matrix_layout_t out_layout) const
{
	detail::matrix_store::const_ptr right = m.get_raw_store();
	// If the left matrix is row-major, the right matrix should be
	// column-major. When the left matrix is tall, the right matrix should
	// be small. It makes sense to convert the right matrix to column major
	// before we break up the left matrix for parallel processing.
	if (!is_wide() && this->store_layout() == matrix_layout_t::L_ROW) {
		dense_matrix::ptr tmp = m.conv2(matrix_layout_t::L_COL);
		tmp->materialize_self();
		right = tmp->get_raw_store();
	}
	if (right->is_virtual() || !right->is_in_mem() || right->get_num_nodes() > 0) {
		dense_matrix::ptr tmp = dense_matrix::create(right);
		tmp = tmp->conv_store(true, -1);
		right = tmp->get_raw_store();
	}
	assert(right->is_in_mem());
	assert(right->get_num_nodes() == -1);
	assert(!right->is_virtual());

	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = this->get_raw_store();
	inner_prod_tall_op::const_ptr mapply_op(new inner_prod_tall_op(right,
				left_op, right_op, get_num_rows(), m.get_num_cols()));
	return __mapply_portion_virtual(ins, mapply_op, out_layout);
}

namespace
{

class inner_prod_wide_op: public detail::portion_mapply_op
{
	const bulk_operate &left_op;
	const bulk_operate &right_op;
	detail::matrix_store::const_ptr res;
	std::vector<detail::local_matrix_store::ptr> local_ms;
public:
	inner_prod_wide_op(const bulk_operate &_left_op, const bulk_operate &_right_op,
			detail::matrix_store::const_ptr res,
			size_t num_threads): detail::portion_mapply_op( 0, 0,
				res->get_type()), left_op(_left_op), right_op(_right_op) {
		local_ms.resize(num_threads);
		this->res = res;
	}

	const std::vector<detail::local_matrix_store::ptr> &get_partial_results() const {
		return local_ms;
	}

	virtual void run(
			const std::vector<detail::local_matrix_store::const_ptr> &ins) const;

	virtual detail::portion_mapply_op::const_ptr transpose() const {
		// We don't need to implement this because we materialize
		// the output matrix immediately.
		assert(0);
		return detail::portion_mapply_op::const_ptr();
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return std::string("inner_prod(") + mats[0]->get_name()
			+ "," + mats[1]->get_name() + ")";
	}
};

void inner_prod_wide_op::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins) const
{
	detail::pool_task_thread *curr
		= dynamic_cast<detail::pool_task_thread *>(thread::get_curr_thread());
	int thread_id = curr->get_pool_thread_id();
	detail::local_matrix_store::ptr local_m = local_ms[thread_id];
	if (local_m == NULL) {
		int node_id = curr->get_node_id();
		if (res->store_layout() == matrix_layout_t::L_COL)
			local_m = detail::local_matrix_store::ptr(
					new detail::local_buf_col_matrix_store(0, 0,
						res->get_num_rows(), res->get_num_cols(),
						right_op.get_output_type(), node_id));
		else
			local_m = detail::local_matrix_store::ptr(
					new detail::local_buf_row_matrix_store(0, 0,
						res->get_num_rows(), res->get_num_cols(),
						right_op.get_output_type(), node_id));
		local_m->reset_data();
		assert((size_t) thread_id < local_ms.size());
		const_cast<inner_prod_wide_op *>(this)->local_ms[thread_id] = local_m;
	}
	detail::local_matrix_store::const_ptr store
		= std::static_pointer_cast<const detail::local_matrix_store>(
				ins[0]->transpose());
	detail::inner_prod(*store, *ins[1], left_op, right_op, *local_m);
}

}

detail::matrix_store::ptr dense_matrix::inner_prod_wide(
		const dense_matrix &m, bulk_operate::const_ptr left_op,
		bulk_operate::const_ptr right_op, matrix_layout_t out_layout) const
{
	// This matrix is small. We can always keep it in memory.
	detail::matrix_store::ptr res = detail::matrix_store::create(
			get_num_rows(), m.get_num_cols(), out_layout,
			right_op->get_output_type(), -1, true);

	detail::mem_thread_pool::ptr threads
		= detail::mem_thread_pool::get_global_mem_threads();
	size_t nthreads = threads->get_num_threads();

	std::vector<detail::matrix_store::const_ptr> mats(2);
	mats[0] = get_data().transpose();
	assert(mats[0]);
	mats[1] = m.get_raw_store();
	assert(mats[1]);
	std::shared_ptr<inner_prod_wide_op> op(new inner_prod_wide_op(
				*left_op, *right_op, res, nthreads));
	__mapply_portion(mats, op, out_layout);
	std::vector<detail::local_matrix_store::ptr> local_ms
		= op->get_partial_results();
	assert(local_ms.size() == nthreads);

	// Aggregate the results from omp threads.
	res->reset_data();
	detail::local_matrix_store::ptr local_res = res->get_portion(0);
	assert(local_res->get_num_rows() == res->get_num_rows()
			&& local_res->get_num_cols() == res->get_num_cols());
	for (size_t j = 0; j < local_ms.size(); j++) {
		// It's possible that the local matrix store doesn't exist
		// because the input matrix is very small.
		if (local_ms[j])
			detail::mapply2(*local_res, *local_ms[j], *right_op, *local_res);
	}
	return res;
}

////////////////////////////// Aggregation /////////////////////////////

namespace
{

/*
 * This allows us to aggregate on the shorter dimension.
 * It outputs a long vector, so the result doesn't need to be materialized
 * immediately.
 */
class matrix_short_agg_op: public detail::portion_mapply_op
{
	matrix_margin margin;
	agg_operate::const_ptr op;
public:
	matrix_short_agg_op(matrix_margin margin, agg_operate::const_ptr op,
			size_t out_num_rows, size_t out_num_cols): detail::portion_mapply_op(
				out_num_rows, out_num_cols, op->get_output_type()) {
		this->margin = margin;
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const {
		assert(ins.size() == 1);
		// The output matrix is actually a vector.
		if (out.get_num_rows() == 1) {
			assert(out.store_layout() == matrix_layout_t::L_ROW);
			local_ref_vec_store res(
					static_cast<detail::local_row_matrix_store &>(out).get_row(0),
					0, out.get_num_cols(), out.get_type(), -1);
			aggregate(*ins[0], op->get_agg(), margin, res);
		}
		else {
			assert(out.store_layout() == matrix_layout_t::L_COL);
			assert(out.get_num_cols() == 1);
			local_ref_vec_store res(
					static_cast<detail::local_col_matrix_store &>(out).get_col(0),
					0, out.get_num_rows(), out.get_type(), -1);
			aggregate(*ins[0], op->get_agg(), margin, res);
		}
	}

	virtual portion_mapply_op::const_ptr transpose() const {
		matrix_margin new_margin = (this->margin == matrix_margin::MAR_ROW ?
			matrix_margin::MAR_COL : matrix_margin::MAR_ROW);
		return portion_mapply_op::const_ptr(new matrix_short_agg_op(
					new_margin, op, get_out_num_cols(), get_out_num_rows()));
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return std::string("agg(") + mats[0]->get_name() + ")";
	}
};

/*
 * This aggregates on the longer dimension.
 * It outputs a very short vector, so the result is materialized immediately.
 */
class matrix_long_agg_op: public detail::portion_mapply_op
{
	matrix_margin margin;
	agg_operate::const_ptr op;
	// Each row stores the local aggregation results on a thread.
	detail::mem_row_matrix_store::ptr partial_res;
	std::vector<local_vec_store::ptr> local_bufs;
	// This indicates the number of times that data has been aggregated to
	// the partial results.
	std::vector<size_t> num_aggs;
public:
	matrix_long_agg_op(detail::mem_row_matrix_store::ptr partial_res,
			matrix_margin margin,
			agg_operate::const_ptr &op): detail::portion_mapply_op(
				0, 0, partial_res->get_type()) {
		this->partial_res = partial_res;
		this->margin = margin;
		this->op = op;
		local_bufs.resize(partial_res->get_num_rows());
		num_aggs.resize(partial_res->get_num_rows());
	}

	bool valid_row(size_t off) const {
		return num_aggs[off] > 0;
	}

	size_t get_num_valid_rows() const {
		size_t ret = 0;
		for (size_t i = 0; i < num_aggs.size(); i++)
			if (num_aggs[i] > 0)
				ret++;
		return ret;
	}

	virtual void run(
			const std::vector<detail::local_matrix_store::const_ptr> &ins) const;

	virtual detail::portion_mapply_op::const_ptr transpose() const {
		assert(0);
		return detail::portion_mapply_op::const_ptr();
	}

	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		return std::string();
	}
};

void matrix_long_agg_op::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins) const
{
	assert(ins.size() == 1);
	detail::pool_task_thread *thread = dynamic_cast<detail::pool_task_thread *>(
			thread::get_curr_thread());
	int thread_id = thread->get_pool_thread_id();
	if (local_bufs[thread_id] == NULL)
		const_cast<matrix_long_agg_op *>(this)->local_bufs[thread_id]
			= local_vec_store::ptr(new local_buf_vec_store(0,
						partial_res->get_num_cols(), partial_res->get_type(),
						ins[0]->get_node_id()));
	detail::aggregate(*ins[0], op->get_agg(), margin, *local_bufs[thread_id]);

	// If this is the first time, we should copy the local results to
	// the corresponding row.
	if (num_aggs[thread_id] == 0)
		memcpy(partial_res->get_row(thread_id),
				local_bufs[thread_id]->get_raw_arr(),
				partial_res->get_num_cols() * partial_res->get_entry_size());
	else
		op->get_combine().runAA(partial_res->get_num_cols(),
				partial_res->get_row(thread_id),
				local_bufs[thread_id]->get_raw_arr(),
				partial_res->get_row(thread_id));
	const_cast<matrix_long_agg_op *>(this)->num_aggs[thread_id]++;
}

}

vector::ptr aggregate(detail::matrix_store::const_ptr store,
		matrix_margin margin, agg_operate::const_ptr op)
{
	/*
	 * If we aggregate on the shorter dimension.
	 */
	if ((margin == matrix_margin::MAR_ROW && !store->is_wide())
			|| (margin == matrix_margin::MAR_COL && store->is_wide())) {
		std::vector<detail::matrix_store::const_ptr> ins(1);
		ins[0] = store;
		size_t out_num_rows;
		size_t out_num_cols;
		if (margin == matrix_margin::MAR_ROW) {
			out_num_rows = store->get_num_rows();
			out_num_cols = 1;
		}
		else {
			out_num_rows = 1;
			out_num_cols = store->get_num_cols();
		}
		matrix_short_agg_op::const_ptr agg_op(new matrix_short_agg_op(
					margin, op, out_num_rows, out_num_cols));
		matrix_layout_t output_layout = (margin == matrix_margin::MAR_ROW
				? matrix_layout_t::L_COL : matrix_layout_t::L_ROW);
		detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
				agg_op, output_layout);
		ret->materialize_self();
		// TODO if the result matrix is in external memory, getting
		// the row/column will read the entire row/column into memory.
		if (ret->get_num_cols() == 1)
			return vector::create(ret->get_col_vec(0));
		else
			return vector::create(ret->get_row_vec(0));
	}
	if (!op->has_combine()) {
		BOOST_LOG_TRIVIAL(error)
			<< "aggregation on the long dimension requires combine";
		return vector::ptr();
	}

	/*
	 * If we aggregate on the entire matrix or on the longer dimension.
	 */
	detail::mem_thread_pool::ptr threads
		= detail::mem_thread_pool::get_global_mem_threads();
	size_t num_threads = threads->get_num_threads();
	detail::mem_row_matrix_store::ptr partial_res;
	if (margin == matrix_margin::BOTH)
		partial_res = detail::mem_row_matrix_store::create(num_threads,
				1, op->get_output_type());
	// For the next two cases, I assume the partial result is small enough
	// to be kept in memory.
	else if (margin == matrix_margin::MAR_ROW)
		partial_res = detail::mem_row_matrix_store::create(num_threads,
				store->get_num_rows(), op->get_output_type());
	else if (margin == matrix_margin::MAR_COL)
		partial_res = detail::mem_row_matrix_store::create(num_threads,
				store->get_num_cols(), op->get_output_type());
	else
		// This shouldn't happen.
		assert(0);
	partial_res->reset_data();

	std::shared_ptr<matrix_long_agg_op> agg_op(new matrix_long_agg_op(
				partial_res, margin, op));
	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = store;
	__mapply_portion(ins, agg_op, matrix_layout_t::L_ROW);

	// The last step is to aggregate the partial results from all portions.
	// It runs in serial. I hope it's not a bottleneck.
	detail::local_matrix_store::const_ptr local_res;
	size_t num_valid_rows = agg_op->get_num_valid_rows();
	// If all rows are valid.
	if (num_valid_rows == partial_res->get_num_rows())
		local_res = partial_res->get_portion(
				0, 0, partial_res->get_num_rows(), partial_res->get_num_cols());
	else {
		// Otherwise, we have to pick all the valid rows out.
		detail::local_row_matrix_store::ptr tmp(
				new detail::local_buf_row_matrix_store(0, 0, num_valid_rows,
					partial_res->get_num_cols(), partial_res->get_type(), -1));
		size_t entry_size = partial_res->get_entry_size();
		size_t copy_row = 0;
		for (size_t i = 0; i < partial_res->get_num_rows(); i++)
			if (agg_op->valid_row(i)) {
				memcpy(tmp->get_row(copy_row), partial_res->get_row(i),
						partial_res->get_num_cols() * entry_size);
				copy_row++;
			}
		assert(copy_row == num_valid_rows);
		local_res = tmp;
	}
	detail::smp_vec_store::ptr res = detail::smp_vec_store::create(
			partial_res->get_num_cols(), partial_res->get_type());
	local_ref_vec_store local_vec(res->get_raw_arr(), 0, res->get_length(),
			res->get_type(), -1);
	detail::aggregate(*local_res, op->get_combine(),
			matrix_margin::MAR_COL, local_vec);
	return vector::create(res);
}

vector::ptr dense_matrix::aggregate(matrix_margin margin,
			agg_operate::const_ptr op) const
{
	if (this->get_type() != op->get_input_type()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The matrix element type is different from the operator";
		return vector::ptr();
	}
	return fm::aggregate(store, margin, op);
}

scalar_variable::ptr dense_matrix::aggregate(bulk_operate::const_ptr op) const
{
	return aggregate(agg_operate::create(op));
}

scalar_variable::ptr dense_matrix::aggregate(agg_operate::const_ptr op) const
{
	if (this->get_type() != op->get_input_type()) {
		BOOST_LOG_TRIVIAL(error)
			<< "The matrix element type is different from the operator";
		return scalar_variable::ptr();
	}
	vector::ptr res_vec = fm::aggregate(store, matrix_margin::BOTH, op);
	assert(res_vec != NULL);
	assert(res_vec->get_length() == 1);
	assert(res_vec->is_in_mem());

	scalar_variable::ptr res = op->get_output_type().create_scalar();
	res->set_raw(dynamic_cast<const detail::mem_vec_store &>(
				res_vec->get_data()).get_raw_arr(), res->get_size());
	return res;
}

namespace
{

class matrix_margin_apply_op: public detail::portion_mapply_op
{
	matrix_margin margin;
	arr_apply_operate::const_ptr op;
public:
	matrix_margin_apply_op(matrix_margin margin, arr_apply_operate::const_ptr op,
			size_t out_num_rows, size_t out_num_cols): detail::portion_mapply_op(
				out_num_rows, out_num_cols, op->get_output_type()) {
		this->margin = margin;
		this->op = op;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const {
		detail::apply(margin, *op, *ins[0], out);
	}
	virtual portion_mapply_op::const_ptr transpose() const {
		matrix_margin new_margin = this->margin == matrix_margin::MAR_ROW ?
			matrix_margin::MAR_COL : matrix_margin::MAR_ROW;
		return portion_mapply_op::const_ptr(new matrix_margin_apply_op(
					new_margin, op, get_out_num_cols(), get_out_num_rows()));
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return std::string("apply(") + mats[0]->get_name() + ")";
	}
};

}

dense_matrix::ptr dense_matrix::apply(matrix_margin margin,
		arr_apply_operate::const_ptr op) const
{
	assert(op->get_num_out_eles() > 0);
	// In these two cases, we need to convert the matrix store layout
	// before we can apply the function to the matrix.
	detail::matrix_store::const_ptr this_mat;
	if (is_wide() && store_layout() == matrix_layout_t::L_COL
			&& margin == matrix_margin::MAR_ROW) {
		dense_matrix::ptr mat = conv2(matrix_layout_t::L_ROW);
		mat->materialize_self();
		this_mat = mat->get_raw_store();
	}
	else if (!is_wide() && store_layout() == matrix_layout_t::L_ROW
			&& margin == matrix_margin::MAR_COL) {
		dense_matrix::ptr mat = conv2(matrix_layout_t::L_COL);
		mat->materialize_self();
		this_mat = mat->get_raw_store();
	}
	else
		this_mat = get_raw_store();
	assert(this_mat);

	// In these two cases, apply the function on the rows/columns in the long
	// dimension. The previous two cases are handled as one of the two cases
	// after the matrix layout conversion from the previous cases.
	if (is_wide() && this_mat->store_layout() == matrix_layout_t::L_ROW
			&& margin == matrix_margin::MAR_ROW) {
#if 0
		// In this case, it's very difficult to make it work for NUMA matrix.
		assert(get_num_nodes() == -1);
		detail::mem_row_matrix_store::const_ptr row_mat
			= detail::mem_row_matrix_store::cast(this_mat);
		detail::mem_row_matrix_store::ptr ret_mat
			= detail::mem_row_matrix_store::create(get_num_rows(),
					op->get_num_out_eles(), op->get_output_type());
		for (size_t i = 0; i < get_num_rows(); i++) {
			local_cref_vec_store in_vec(row_mat->get_row(i), 0,
					this_mat->get_num_cols(), get_type(), -1);
			local_ref_vec_store out_vec(ret_mat->get_row(i), 0,
					ret_mat->get_num_cols(), ret_mat->get_type(), -1);
			op->run(in_vec, out_vec);
		}
		return mem_dense_matrix::create(ret_mat);
#endif
		BOOST_LOG_TRIVIAL(error)
			<< "it doesn't support to apply rows on a wide matrix";
		return dense_matrix::ptr();
	}
	else if (!is_wide() && this_mat->store_layout() == matrix_layout_t::L_COL
			&& margin == matrix_margin::MAR_COL) {
#if 0
		assert(get_num_nodes() == -1);
		detail::mem_col_matrix_store::const_ptr col_mat
			= detail::mem_col_matrix_store::cast(this_mat);
		detail::mem_col_matrix_store::ptr ret_mat
			= detail::mem_col_matrix_store::create(op->get_num_out_eles(),
					get_num_cols(), op->get_output_type());
		for (size_t i = 0; i < get_num_cols(); i++) {
			local_cref_vec_store in_vec(col_mat->get_col(i), 0,
					this_mat->get_num_rows(), get_type(), -1);
			local_ref_vec_store out_vec(ret_mat->get_col(i), 0,
					ret_mat->get_num_rows(), ret_mat->get_type(), -1);
			op->run(in_vec, out_vec);
		}
		return mem_dense_matrix::create(ret_mat);
#endif
		BOOST_LOG_TRIVIAL(error)
			<< "it doesn't support to apply columns on a tall matrix";
		return dense_matrix::ptr();
	}
	// There are four cases left. In these four cases, apply the function
	// on the rows/columns in the short dimension. We can use mapply to
	// parallelize the computation here.
	else {
		std::vector<detail::matrix_store::const_ptr> ins(1);
		ins[0] = this->get_raw_store();
		size_t out_num_rows;
		size_t out_num_cols;
		if (margin == matrix_margin::MAR_ROW) {
			out_num_rows = this->get_num_rows();
			out_num_cols = op->get_num_out_eles();
		}
		else {
			out_num_rows = op->get_num_out_eles();
			out_num_cols = this->get_num_cols();
		}
		matrix_margin_apply_op::const_ptr apply_op(new matrix_margin_apply_op(
					margin, op, out_num_rows, out_num_cols));
		matrix_layout_t output_layout = (margin == matrix_margin::MAR_ROW
				? matrix_layout_t::L_ROW : matrix_layout_t::L_COL);
		detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
				apply_op, output_layout);
		return dense_matrix::create(ret);
	}
}

////////////////////// Convert the data layout of a matrix ////////////////////

namespace
{

class conv_layout_op: public detail::portion_mapply_op
{
	matrix_layout_t layout;
public:
	conv_layout_op(matrix_layout_t layout, size_t num_rows, size_t num_cols,
			const scalar_type &type): detail::portion_mapply_op(num_rows,
				num_cols, type) {
		this->layout = layout;
	}

	virtual void run(const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const {
		assert(ins.size() == 1);
		assert(ins[0]->get_global_start_col() == out.get_global_start_col());
		assert(ins[0]->get_global_start_row() == out.get_global_start_row());
		out.copy_from(*ins[0]);
	}

	virtual portion_mapply_op::const_ptr transpose() const {
		matrix_layout_t new_layout;
		if (layout == matrix_layout_t::L_COL)
			new_layout = matrix_layout_t::L_ROW;
		else
			new_layout = matrix_layout_t::L_COL;
		return detail::portion_mapply_op::const_ptr(new conv_layout_op(new_layout,
					get_out_num_cols(), get_out_num_rows(), get_output_type()));
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		assert(mats.size() == 1);
		return std::string("conv_layout(") + mats[0]->get_name() + ")";
	}
};

}

dense_matrix::ptr dense_matrix::conv2(matrix_layout_t layout) const
{
	if (store_layout() == layout)
		return dense_matrix::create(get_raw_store());

	// If the dense matrix has only one row or one column, it's very easy
	// to convert its layout. We don't need to copy data or run computation
	// at all.
	if (get_num_cols() == 1) {
		detail::vec_store::const_ptr vec = get_data().get_col_vec(0);
		return dense_matrix::create(vec->conv2mat(get_num_rows(),
					get_num_cols(), layout == matrix_layout_t::L_ROW));
	}
	else if (get_num_rows() == 1) {
		detail::vec_store::const_ptr vec = get_data().get_row_vec(0);
		return dense_matrix::create(vec->conv2mat(get_num_rows(),
					get_num_cols(), layout == matrix_layout_t::L_ROW));
	}

	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = this->get_raw_store();
	conv_layout_op::const_ptr mapply_op(new conv_layout_op(layout,
				get_num_rows(), get_num_cols(), get_type()));
	detail::matrix_store::ptr ret = __mapply_portion_virtual(ins,
			mapply_op, layout);
	return dense_matrix::create(ret);
}

vector::ptr dense_matrix::row_sum() const
{
	bulk_operate::const_ptr add
		= bulk_operate::conv2ptr(get_type().get_basic_ops().get_add());
	return fm::aggregate(store, matrix_margin::MAR_ROW, agg_operate::create(add));
}

vector::ptr dense_matrix::col_sum() const
{
	bulk_operate::const_ptr add
		= bulk_operate::conv2ptr(get_type().get_basic_ops().get_add());
	return fm::aggregate(store, matrix_margin::MAR_COL, agg_operate::create(add));
}

vector::ptr dense_matrix::row_norm2() const
{
	detail::matrix_stats.inc_multiplies(get_num_rows() * get_num_cols());

	const bulk_uoperate *op = get_type().get_basic_uops().get_op(
			basic_uops::op_idx::SQ);
	dense_matrix::ptr sq_mat = this->sapply(bulk_uoperate::conv2ptr(*op));
	vector::ptr sums = sq_mat->row_sum();
	op = get_type().get_basic_uops().get_op(basic_uops::op_idx::SQRT);
	dense_matrix::ptr sqrt_mat = sums->conv2mat(sums->get_length(), 1,
			false)->sapply(bulk_uoperate::conv2ptr(*op));
	return sqrt_mat->get_col(0);
}

vector::ptr dense_matrix::col_norm2() const
{
	detail::matrix_stats.inc_multiplies(get_num_rows() * get_num_cols());

	const bulk_uoperate *op = get_type().get_basic_uops().get_op(
			basic_uops::op_idx::SQ);
	dense_matrix::ptr sq_mat = this->sapply(bulk_uoperate::conv2ptr(*op));
	vector::ptr sums = sq_mat->col_sum();
	op = get_type().get_basic_uops().get_op(basic_uops::op_idx::SQRT);
	dense_matrix::ptr sqrt_mat = sums->conv2mat(sums->get_length(), 1,
			false)->sapply(bulk_uoperate::conv2ptr(*op));
	return sqrt_mat->get_col(0);
}

class copy_op: public detail::portion_mapply_op
{
public:
	copy_op(size_t out_num_rows, size_t out_num_cols,
			const scalar_type &out_type): detail::portion_mapply_op(
			out_num_rows, out_num_cols, out_type) {
	}

	virtual void run(
			const std::vector<detail::local_matrix_store::const_ptr> &ins,
			detail::local_matrix_store &out) const {
		assert(ins.size() == 1);
		out.copy_from(*ins[0]);
	}

	virtual detail::portion_mapply_op::const_ptr transpose() const {
		assert(0);
		return detail::portion_mapply_op::const_ptr();
	}
	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		return std::string();
	}
};

detail::matrix_store::const_ptr dense_matrix::_conv_store(bool in_mem,
		int num_nodes) const
{
	// If the current matrix is EM matrix and we want to convert it to
	// an EM matrix, don't do anything.
	if (!in_mem && !store->is_in_mem() && !store->is_virtual())
		return store;
	// If the current matrix is in-mem matrix and it stores in the same
	// number of NUMA nodes as requested, don't do anything.
	if (in_mem && store->is_in_mem() && store->get_num_nodes() == num_nodes
			&& !store->is_virtual())
		return store;

	if (store->is_virtual())
		return detail::virtual_matrix_store::cast(store)->materialize(in_mem,
				num_nodes);
	else {
		std::vector<detail::matrix_store::const_ptr> in_mats(1);
		in_mats[0] = store;
		std::vector<detail::matrix_store::ptr> out_mats(1);
		out_mats[0] = detail::matrix_store::create(get_num_rows(), get_num_cols(),
				store_layout(), get_type(), num_nodes, in_mem);

		detail::portion_mapply_op::const_ptr op(new copy_op(get_num_rows(),
					get_num_cols(), get_type()));
		bool ret = detail::__mapply_portion(in_mats, op, out_mats);
		if (ret)
			return out_mats[0];
		else
			return detail::matrix_store::const_ptr();
	}
}

dense_matrix::ptr dense_matrix::conv_store(bool in_mem, int num_nodes) const
{
	detail::matrix_store::const_ptr store = _conv_store(in_mem, num_nodes);
	if (store)
		return dense_matrix::create(store);
	else
		return dense_matrix::ptr();
}

bool dense_matrix::move_store(bool in_mem, int num_nodes) const
{
	detail::matrix_store::const_ptr store = _conv_store(in_mem, num_nodes);
	if (store == NULL) {
		BOOST_LOG_TRIVIAL(error)
			<< "can't move matrix store to another storage media";
		return false;
	}
	const_cast<dense_matrix *>(this)->store = store;
	return true;
}

dense_matrix::ptr dense_matrix::logic_not() const
{
	if (get_type() != get_scalar_type<bool>()) {
		BOOST_LOG_TRIVIAL(error) << "logic_not only works on boolean matrix";
		return dense_matrix::ptr();
	}

	bulk_uoperate::const_ptr op = bulk_uoperate::conv2ptr(
			*get_type().get_basic_uops().get_op(basic_uops::op_idx::NOT));
	return sapply(op);
}

dense_matrix::ptr dense_matrix::deep_copy() const
{
	std::vector<detail::matrix_store::const_ptr> ins(1);
	ins[0] = store;
	detail::portion_mapply_op::const_ptr op(new copy_op(get_num_rows(),
				get_num_cols(), get_type()));
	return dense_matrix::create(__mapply_portion(ins, op,
				store->store_layout()));
}

namespace
{

class groupby_row_mapply_op: public detail::portion_mapply_op
{
	// This contains a bool vector for each thread.
	// The bool vector indicates whether a label gets partially aggregated data.
	std::vector<std::vector<bool> > part_agg;
	// This contains a local matrix for each thread.
	// Each row of a local matrix contains partially aggregated data for a label.
	std::vector<detail::local_row_matrix_store::ptr> part_results;
	std::vector<bool> part_status;
	size_t num_levels;
	agg_operate::const_ptr op;
public:
	groupby_row_mapply_op(size_t num_levels,
			agg_operate::const_ptr op): detail::portion_mapply_op(0, 0,
				op->get_output_type()) {
		detail::mem_thread_pool::ptr threads
			= detail::mem_thread_pool::get_global_mem_threads();
		size_t num_threads = threads->get_num_threads();
		part_results.resize(num_threads);
		part_agg.resize(num_threads);
		part_status.resize(num_threads, true);
		this->num_levels = num_levels;
		this->op = op;
	}

	virtual detail::portion_mapply_op::const_ptr transpose() const {
		throw unsupported_exception(
				"Don't support transpose of groupby_row_mapply_op");
	}

	virtual void run(
			const std::vector<detail::local_matrix_store::const_ptr> &ins) const;

	virtual std::string to_string(
			const std::vector<detail::matrix_store::const_ptr> &mats) const {
		throw unsupported_exception(
				"Don't support to_string of groupby_row_mapply_op");
	}

	detail::matrix_store::ptr get_agg() const;
};

detail::matrix_store::ptr groupby_row_mapply_op::get_agg() const
{
	for (size_t i = 0; i < part_status.size(); i++) {
		if (!part_status[i]) {
			BOOST_LOG_TRIVIAL(error) << "groupby fails on a partition";
			return detail::matrix_store::ptr();
		}
	}
	size_t first_idx;
	for (first_idx = 0; first_idx < part_results.size(); first_idx++)
		if (part_results[first_idx] != NULL)
			break;
	assert(first_idx < part_results.size());

	size_t nrow = part_results[first_idx]->get_num_rows();
	size_t ncol = part_results[first_idx]->get_num_cols();
	const scalar_type &type = part_results[first_idx]->get_type();
	detail::mem_matrix_store::ptr res = detail::mem_matrix_store::create(nrow,
			ncol, matrix_layout_t::L_ROW, type, -1);
	for (size_t i = 0; i < res->get_num_rows(); i++) {
		memcpy(res->get_row(i), part_results[first_idx]->get_row(i),
				res->get_num_cols() * res->get_entry_size());
		for (size_t j = first_idx + 1; j < part_results.size(); j++) {
			if (part_results[j] != NULL)
				op->get_combine().runAA(res->get_num_cols(),
						part_results[j]->get_row(i), res->get_row(i),
						res->get_row(i));
		}
	}
	return res;
}

void groupby_row_mapply_op::run(
		const std::vector<detail::local_matrix_store::const_ptr> &ins) const
{
	assert(ins.size() == 2);
	detail::local_matrix_store::const_ptr labels = ins[0];
	detail::local_row_matrix_store::const_ptr in;
	if (ins[1]->store_layout() == matrix_layout_t::L_COL)
		in = std::dynamic_pointer_cast<const detail::local_row_matrix_store>(
				ins[1]->conv2(matrix_layout_t::L_ROW));
	else
		in = std::dynamic_pointer_cast<const detail::local_row_matrix_store>(
				ins[1]);
	size_t num_local_rows = in->get_num_rows();

	groupby_row_mapply_op *mutable_this = const_cast<groupby_row_mapply_op *>(
			this);
	// Prepare for the output result.
	detail::pool_task_thread *thread = dynamic_cast<detail::pool_task_thread *>(
			thread::get_curr_thread());
	int thread_id = thread->get_pool_thread_id();
	if (part_results[thread_id] == NULL) {
		assert(part_agg[thread_id].empty());
		mutable_this->part_results[thread_id] = detail::local_row_matrix_store::ptr(
				new detail::local_buf_row_matrix_store(0, 0, num_levels,
					in->get_num_cols(), op->get_output_type(), -1));
		mutable_this->part_agg[thread_id].resize(num_levels);
	}
	// If there was a failure in this thread, we don't need to perform more
	// computation.
	if (!part_status[thread_id])
		return;

	for (size_t i = 0; i < num_local_rows; i++) {
		factor_value_t label_id = labels->get<factor_value_t>(i, 0);
		if ((size_t) label_id >= part_agg[thread_id].size()) {
			mutable_this->part_status[thread_id] = false;
			break;
		}
		// If we never get partially aggregated result for a label, we should
		// copy the data to the corresponding row.
		if (!part_agg[thread_id][label_id])
			memcpy(part_results[thread_id]->get_row(label_id), in->get_row(i),
					in->get_num_cols() * in->get_entry_size());
		else
			op->get_agg().runAA(in->get_num_cols(), in->get_row(i),
					part_results[thread_id]->get_row(label_id),
					part_results[thread_id]->get_row(label_id));
		auto bit = mutable_this->part_agg[thread_id][label_id];
		bit = true;
	}
}

}

dense_matrix::ptr dense_matrix::groupby_row(factor_vector::const_ptr labels,
		agg_operate::const_ptr op) const
{
	if (is_wide()) {
		BOOST_LOG_TRIVIAL(error)
			<< "groupby_row can't run on a wide dense matrix";
		return dense_matrix::ptr();
	}
	if (labels->get_length() != get_num_rows()) {
		BOOST_LOG_TRIVIAL(error)
			<< "groupby_row: there should be the same #labels as #rows";
		return dense_matrix::ptr();
	}
	if (get_type() != op->get_input_type()) {
		BOOST_LOG_TRIVIAL(error)
			<< "groupby_row: the agg op requires diff element types";
		return dense_matrix::ptr();
	}
	if (!op->has_combine()) {
		BOOST_LOG_TRIVIAL(error) << "agg op needs to have combine";
		return dense_matrix::ptr();
	}

	std::vector<detail::matrix_store::const_ptr> mats(2);
	mats[0] = labels->get_data().conv2mat(labels->get_length(), 1, false);
	mats[1] = store;
	groupby_row_mapply_op *_groupby_op = new groupby_row_mapply_op(
			labels->get_factor().get_num_levels(), op);
	detail::portion_mapply_op::const_ptr groupby_op(_groupby_op);
	__mapply_portion(mats, groupby_op, matrix_layout_t::L_ROW);

	detail::matrix_store::ptr agg = _groupby_op->get_agg();
	if (agg == NULL)
		return dense_matrix::ptr();
	else
		return dense_matrix::create(agg);
}

dense_matrix::ptr dense_matrix::groupby_row(factor_vector::const_ptr labels,
		bulk_operate::const_ptr op) const
{
	agg_operate::const_ptr agg = agg_operate::create(op);
	if (agg == NULL)
		return dense_matrix::ptr();
	return groupby_row(labels, agg);
}

}
