#include <errno.h>

#include "associative_cache.h"

#ifdef STATISTICS
volatile int avail_cells;
volatile int num_wait_unused;
volatile int lock_contentions;
#endif

int end_evicts = 0;
int middle_evicts = 0;

/*
 * remove the idx'th element in the queue.
 * idx is the logical position in the queue,
 * instead of the physical index in the buffer.
 */
template<class T, int SIZE>
void generic_queue<T, SIZE>::remove(int idx) {
	assert(idx < num);
	/* the first element in the queue. */
	if (idx == 0) {
		pop_front();
	}
	/* the last element in the queue. */
	else if (idx == num - 1){
		num--;
	}
	/*
	 * in the middle.
	 * now we need to move data.
	 */
	else {
		T tmp[num];
		T *p = tmp;
		/* if the end of the queue is physically behind the start */
		if (start + num <= SIZE) {
			/* copy elements in front of the removed element. */
			memcpy(p, &buf[start], sizeof(T) * idx);
			p += idx;
			/* copy elements behind the removed element. */
			memcpy(p, &buf[start + idx + 1], sizeof(T) * (num - idx - 1));
		}
		/* 
		 * the removed element is between the first element
		 * and the end of the buffer.
		 */
		else if (idx + start < SIZE) {
			/* copy elements in front of the removed element. */
			memcpy(p, &buf[start], sizeof(T) * idx);
			p += idx;
			/*
			 * copy elements behind the removed element
			 * and before the end of the buffer.
			 */
			memcpy(p, &buf[start + idx + 1], sizeof(T) * (SIZE - start - idx - 1));
			p += (SIZE - start - idx - 1);
			/* copy the remaining elements in the beginning of the buffer. */
			memcpy(p, buf, sizeof(T) * (num - (SIZE - start)));
		}
		/*
		 * the removed element is between the beginning of the buffer
		 * and the last element.
		 */
		else {
			/* copy elements between the first element and the end of the buffer. */
			memcpy(p, &buf[start], sizeof(T) * (SIZE - start));
			p += (SIZE - start);
			/* copy elements between the beginning of the buffer and the removed element. */
			idx = (idx + start) % SIZE;
			memcpy(p, buf, sizeof(T) * idx);
			p += idx;
			/* copy elements after the removed element and before the last element */
			memcpy(p, &buf[idx + 1], sizeof(T) * ((start + num) % SIZE - idx - 1));
		}
		memcpy(buf, tmp, sizeof(T) * (num - 1));
		start = 0;
		num--;
	}
}

hash_cell::hash_cell(associative_cache *cache, long hash) {
	this->hash = hash;
	overflow = false;
	pthread_spin_init(&_lock, PTHREAD_PROCESS_PRIVATE);
	this->table = cache;
	char *pages[CELL_SIZE];
	if (!table->get_manager()->get_free_pages(CELL_SIZE, pages, cache))
		throw oom_exception();
	buf.set_pages(pages);
}

/**
 * rehash the pages in the current cell 
 * to the expanded cell.
 */
void hash_cell::rehash(hash_cell *expanded) {
	pthread_spin_lock(&_lock);
	pthread_spin_lock(&expanded->_lock);
	for (int i = 0, j = 0; i < CELL_SIZE; i++) {
		thread_safe_page *pg = buf.get_page(i);
		int hash1 = table->hash1_locked(pg->get_offset());
		/*
		 * It's possible that a page is in a wrong cell.
		 * It's likely because the page is added to the cell 
		 * right when `level' is increased.
		 * But the case is rare, so we can just simple ignore
		 * the case. It doesn't affect the correctness of 
		 * the implementation. The only penalty is that
		 * we might get a cache miss.
		 * Since the page is in a wrong cell, it won't be 
		 * accessed any more, so we should shorten the time
		 * it gets evicted by setting its hit to 1.
		 */
		if (hash1 != expanded->hash) {
			pg->set_hits(1);
			continue;
		}
		/* 
		 * if the two hash values don't match,
		 * it means the page is mapped to the expanded cell.
		 * we exchange the pages in the two cells.
		 */
		if (this->hash != hash1) {
			thread_safe_page *expanded_pg = expanded->buf.get_page(j);
			/* 
			 * we have to make sure no other threads are using them
			 * before we can exchange them.
			 * If the pages are in use, skip them.
			 */
			if (pg->get_ref()) {
				continue;
			}
			/* 
			 * the page in the expanded cell shouldn't
			 * have been initialized.
			 */
			assert(!expanded_pg->initialized());

			thread_safe_page tmp = *expanded_pg;
			*expanded_pg = *pg;
			*pg = tmp;
			j++;
		}
	}
	pthread_spin_unlock(&expanded->_lock);
	pthread_spin_unlock(&_lock);
	overflow = false;
}

/**
 * search for a page with the offset.
 * If the page doesn't exist, return an empty page.
 */
page *hash_cell::search(off_t off, off_t &old_off) {
	thread_safe_page *ret = NULL;
#ifndef STATISTICS
	pthread_spin_lock(&_lock);
#else
	if (pthread_spin_trylock(&_lock) == EBUSY) {
		__sync_fetch_and_add(&lock_contentions, 1);
		pthread_spin_lock(&_lock);
	}
#endif

	for (int i = 0; i < CELL_SIZE; i++) {
		if (buf.get_page(i)->get_offset() == off) {
			ret = buf.get_page(i);
			break;
		}
	}
	if (ret == NULL) {
		ret = get_empty_page();
		old_off = ret->get_offset();
		/*
		 * I have to change the offset in the spinlock,
		 * to make sure when the spinlock is unlocked, 
		 * the page can be seen by others even though
		 * it might not have data ready.
		 */
		ret->set_offset(off);
#ifdef USE_SHADOW_PAGE
		shadow_page shadow_pg = shadow.search(off);
		/*
		 * if the page has been seen before,
		 * we should set the hits info.
		 */
		if (shadow_pg.is_valid())
			ret->set_hits(shadow_pg.get_hits());
#endif
	}
#ifdef USE_LRU
	else {
		/* move the page to the end of the pos vector. */
		int pos = buf.get_idx(ret);
		for (std::vector<int>::iterator it = pos_vec.begin();
				it != pos_vec.end(); it++) {
			if (*it == pos) {
				pos_vec.erase(it);
				break;
			}
		}
		pos_vec.push_back(pos);
	}
#endif
	/* it's possible that the data in the page isn't ready */
	ret->inc_ref();
	if (ret->get_hits() == 0xff) {
		buf.scale_down_hits();
#ifdef USE_SHADOW_PAGE
		shadow.scale_down_hits();
#endif
	}
	ret->hit();
	pthread_spin_unlock(&_lock);
	return ret;
}

/* this function has to be called with lock held */
thread_safe_page *hash_cell::get_empty_page() {
	thread_safe_page *ret = NULL;

	int min_hits;
	bool expanded = false;
search_again:
	min_hits = 0x7fffffff;
	do {
		int num_io_pending = 0;
		for (int i = 0; i < CELL_SIZE; i++) {
			thread_safe_page *pg = buf.get_page(i);
			if (pg->get_ref()) {
				if (pg->is_io_pending())
					num_io_pending++;
				continue;
			}

			/* 
			 * refcnt only increases within the lock of the cell,
			 * so if the page's refcnt is 0 above,
			 * it'll be always 0 within the lock.
			 */

			if (min_hits > pg->get_hits()) {
				min_hits = pg->get_hits();
				ret = pg;
			}

			/* 
			 * if a page hasn't been accessed before,
			 * it's a completely new page, just use it.
			 */
			if (min_hits == 0)
				break;
		}
		if (num_io_pending == CELL_SIZE) {
			printf("all pages are at io pending\n");
			// TODO do something...
			// maybe we should use pthread_wait
		}
		/* it happens when all pages in the cell is used currently. */
	} while (ret == NULL);

	/*
	 * the selected page got hit before,
	 * we should expand the hash table
	 * if we haven't done it before.
	 */
	if (min_hits) {
		overflow = true;
		long table_size = table->size();
		long average_size = table->get_manager()->average_cache_size();
		if (table_size < average_size && !expanded) {
			pthread_spin_unlock(&_lock);
			if (table->expand(this)) {
				throw expand_exception();
			}
			pthread_spin_lock(&_lock);
			expanded = true;
			goto search_again;
		}
	}

	/* we record the hit info of the page in the shadow cell. */
#ifdef USE_SHADOW_PAGE
	if (ret->get_hits() > 0)
		shadow.add(shadow_page(*ret));
#endif

	ret->reset_hits();
	ret->set_data_ready(false);
	return ret;
}

#ifdef USE_LRU
/* 
 * the end of the vector points to the pages
 * that are most recently accessed.
 */
thread_safe_page *hash_cell::get_empty_page() {
	int pos;
	if (pos_vec.size() < CELL_SIZE) {
		pos = pos_vec.size();
	}
	else {
		/* evict the first page */
		pos = pos_vec[0];
		pos_vec.erase(pos_vec.begin());
	}
	thread_safe_page *ret = buf.get_page(pos);
	while (ret->get_ref()) {}
	pos_vec.push_back(pos);
	ret->set_data_ready(false);
	return ret;
}
#endif

#ifdef USE_FIFO
/* this function has to be called with lock held */
thread_safe_page *hash_cell::get_empty_page() {
	thread_safe_page *ret = buf.get_empty_page();
	// TODO I assume this situation is rare
	while (ret->get_ref()) {
		ret = buf.get_empty_page();
		printf("try another empty page.\n");
	}
	ret->set_data_ready(false);
	return ret;
}
#endif

void clock_shadow_cell::add(shadow_page pg) {
	if (!queue.is_full()) {
		queue.push_back(pg);
		return;
	}
	/*
	 * We need to evict a page from the set.
	 * Find the first page whose reference bit isn't set.
	 */
	bool inserted = false;
	do {
		for (int i = 0; i < queue.size(); i++) {
			last_idx = (last_idx + 1) % queue.size();
			shadow_page old = queue.get(last_idx);
			/* 
			 * The page has been referenced recently,
			 * we should spare it.
			 */
			if (old.referenced()) {
				queue.get(last_idx).set_referenced(false);
				continue;
			}
			queue.set(pg, last_idx);
			inserted = true;
			break;
		}
		/* 
		 * If we can't insert the page in the for loop above,
		 * we need to go through the for loop again.
		 * But for the second time, we will definitely
		 * insert the page.
		 */
	} while (!inserted);
}

shadow_page clock_shadow_cell::search(off_t off) {
	for (int i = 0; i < queue.size(); i++) {
		shadow_page pg = queue.get(i);
		if (pg.get_offset() == off) {
			queue.get(i).set_referenced(true);
			return pg;
		}
	}
	return shadow_page();
}

void clock_shadow_cell::scale_down_hits() {
	for (int i = 0; i < queue.size(); i++) {
		queue.get(i).set_hits(queue.get(i).get_hits() / 2);
	}
}

shadow_page LRU_shadow_cell::search(off_t off) {
	for (int i = 0; i < queue.size(); i++) {
		shadow_page pg = queue.get(i);
		if (pg.get_offset() == off) {
			queue.remove(i);
			queue.push_back(pg);
			return pg;
		}
	}
	return shadow_page();
}

void LRU_shadow_cell::scale_down_hits() {
	for (int i = 0; i < queue.size(); i++) {
		queue.get(i).set_hits(queue.get(i).get_hits() / 2);
	}
}

bool associative_cache::expand(hash_cell *cell) {
	hash_cell *cells = NULL;
	unsigned int i;

	if (flags.test_and_set_flags(TABLE_EXPANDING)) {
		/*
		 * if the flag has been set before,
		 * it means another thread is expanding the table,
		 */
		return false;
	}

	/* starting from this point, only one thred can be here. */
	for (i = 0; i < cells_table.size(); i++) {
		cells = cells_table[i];
		if (cells == NULL)
			break;
		if (cell >= cells && cell < cells + init_ncells)
			break;
	}
	assert(cells);
	int global_idx = i * init_ncells + (cell - cells);

	cell = get_cell(split);
	long size = pow(2, level) * init_ncells;
	while (split < global_idx || cell->is_overflow()) {
		unsigned int cells_idx = (split + size) / init_ncells;
		/* 
		 * I'm sure only this thread can change the table,
		 * so it doesn't need to hold a lock when accessing the size.
		 */
		unsigned int orig_size = ncells.get();
		if (cells_idx >= orig_size) {
			bool out_of_memory = false;
			/* create cells and put them in a temporary table. */
			std::vector<hash_cell *> table;
			for (unsigned int i = orig_size; i <= cells_idx; i++) {
				hash_cell *cells = new hash_cell[init_ncells];
				printf("create %d cells: %p\n", init_ncells, cells);
				try {
					for (int j = 0; j < init_ncells; j++) {
						cells[j] = hash_cell(this, i * init_ncells + j);
					}
					table.push_back(cells);
				} catch (oom_exception e) {
					out_of_memory = true;
					delete [] cells;
					break;
				}
			}

			/*
			 * here we need to hold the lock because other threads
			 * might be accessing the table. by using the write lock,
			 * we notify others the table has been changed.
			 */
			table_lock.write_lock();
			for (unsigned int i = 0; i < table.size(); i++) {
				cells_table[orig_size + i] = table[i];
			}
			ncells.inc(table.size());
			table_lock.write_unlock();
			if (out_of_memory)
				return false;
		}

		hash_cell *expanded_cell = get_cell(split + size);
		cell->rehash(expanded_cell);
		table_lock.write_lock();
		split++;
		if (split == size) {
			level++;
			printf("increase level to %d\n", level);
			split = 0;
			table_lock.write_unlock();
			break;
		}
		table_lock.write_unlock();
		cell = get_cell(split);
	}
	flags.clear_flags(TABLE_EXPANDING);
	return true;
}

page *associative_cache::search(off_t offset, off_t &old_off) {
	/*
	 * search might change the structure of the cell,
	 * and cause the cell table to expand.
	 * Thus, the page might not be placed in the cell
	 * we found before. Therefore, we need to research
	 * for the cell.
	 */
	do {
		try {
			return get_cell_offset(offset)->search(offset, old_off);
		} catch (expand_exception e) {
		}
	} while (true);
}

associative_cache::associative_cache(memory_manager *manager) {
	printf("associative cache is used\n");
	level = 0;
	split = 0;
	this->manager = manager;
	manager->register_cache(this);
	int npages = init_cache_size / PAGE_SIZE;
	assert(init_cache_size >= CELL_SIZE * PAGE_SIZE);
	init_ncells = npages / CELL_SIZE;
	hash_cell *cells = new hash_cell[init_ncells];
	printf("%d cells: %p\n", init_ncells, cells);
	int max_npages = manager->get_max_size() / PAGE_SIZE;
	try {
		for (int i = 0; i < init_ncells; i++)
			cells[i] = hash_cell(this, i);
	} catch (oom_exception e) {
		fprintf(stderr,
				"out of memory: max npages: %d, init npages: %d\n",
				max_npages, npages);
		exit(1);
	}

	cells_table.push_back(cells);
	ncells.inc(1);

	int max_ncells = max_npages / CELL_SIZE;
	for (int i = 1; i < max_ncells / init_ncells; i++)
		cells_table.push_back(NULL);
}
