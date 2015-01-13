/***************************************************************************
 *  include/stxxl/bits/containers/parallel_priority_queue.h
 *
 *  Part of the STXXL. See http://stxxl.sourceforge.net
 *
 *  Copyright (C) 2014 Thomas Keh <thomas.keh@student.kit.edu>
 *
 *  Distributed under the Boost Software License, Version 1.0.
 *  (See accompanying file LICENSE_1_0.txt or copy at
 *  http://www.boost.org/LICENSE_1_0.txt)
 **************************************************************************/

#ifndef STXXL_CONTAINERS_PARALLEL_PRIORITY_QUEUE_HEADER
#define STXXL_CONTAINERS_PARALLEL_PRIORITY_QUEUE_HEADER

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <list>
#include <utility>
#include <vector>

#if STXXL_PARALLEL
    #include <omp.h>
    #include <parallel/algorithm>
    #include <parallel/numeric>
#endif

#include <stxxl/bits/common/winner_tree.h>
#include <stxxl/bits/common/custom_stats.h>
#include <stxxl/bits/common/mutex.h>
#include <stxxl/bits/common/timer.h>
#include <stxxl/bits/common/is_heap.h>
#include <stxxl/bits/config.h>
#include <stxxl/bits/io/request_operations.h>
#include <stxxl/bits/mng/block_alloc.h>
#include <stxxl/bits/mng/buf_ostream.h>
#include <stxxl/bits/mng/prefetch_pool.h>
#include <stxxl/bits/mng/block_manager.h>
#include <stxxl/bits/mng/read_write_pool.h>
#include <stxxl/bits/mng/typed_block.h>
#include <stxxl/bits/namespace.h>
#include <stxxl/bits/noncopyable.h>
#include <stxxl/bits/parallel.h>
#include <stxxl/bits/verbose.h>
#include <stxxl/types>

STXXL_BEGIN_NAMESPACE

#define STXXL_VERBOSE1_PPQ(msg) STXXL_MSG("ppq[" << static_cast<const void*>(this) << "]::" << msg)
#define STXXL_VERBOSE2_PPQ(msg) STXXL_VERBOSE1("ppq[" << static_cast<const void*>(this) << "]::" << msg)

namespace ppq_local {

//! A random-access iterator class for block oriented data.
//! The iterator is intended to be provided by the internal_array and
//! external_array classes and to be used by the multiway_merge algorithm.
//!
//! \tparam ValueType the value type
template <class ValueType>
class ppq_iterator
{
public:
    typedef ValueType value_type;
    typedef value_type& reference;
    typedef value_type* pointer;
    typedef ptrdiff_t difference_type;
    typedef std::random_access_iterator_tag iterator_category;
    typedef std::vector<std::pair<pointer, pointer> > block_pointers_type;

protected:
    typedef ppq_iterator self_type;

    //! pointer to a vector of begin/end pointer pairs
    //! They allow access to the data blocks.
    const block_pointers_type* m_block_pointers;

    //! pointer to the current element
    pointer m_current;

    //! index of the current element
    size_t m_index;

    //! index of the current element's block
    size_t m_block_index;

    //! size of each data block
    size_t m_block_size;

public:
    //! default constructor
    //! should not be used directly
    ppq_iterator()
        : m_block_pointers(NULL)
    { }

    //! constructor
    //!
    //! \param block_pointers   A reference to the properly initialized vector of begin and end pointers.
    //!                         One pair for each block. The pointers should be valid for all blocks that
    //!                         are expected to be accessed with this iterator.
    //! \param block_size       The size of a single block. If there is only one block (e.g. if the iterator
    //!                         belongs to an internal_array), use the total size here.
    //! \param index            The index of the current element (global - index 0 belongs to the first element
    //!                         in the first block, no matter if the values are still valid)
    ppq_iterator(const block_pointers_type* block_pointers, size_t block_size, size_t index)
        : m_block_pointers(block_pointers),
          m_index(index),
          m_block_size(block_size)
    {
        update();
    }

    //! copy constructor
    ppq_iterator(const self_type& o)
        : m_block_pointers(o.m_block_pointers),
          m_current(o.m_current),
          m_index(o.m_index),
          m_block_index(o.m_block_index),
          m_block_size(o.m_block_size)
    { }

    //! copy assignment
    self_type& operator = (const self_type& o)
    {
        m_block_pointers = o.m_block_pointers;
        m_current = o.m_current;
        m_index = o.m_index;
        m_block_index = o.m_block_index;
        m_block_size = o.m_block_size;
        return *this;
    }

    //! returns the value's index in the external array
    size_t get_index() const
    {
        return m_index;
    }

    reference operator * () const
    {
        return *m_current;
    }
    pointer operator -> () const
    {
        return m_current;
    }
    reference operator [] (difference_type relative_index) const
    {
        const difference_type index = m_index + relative_index;

        const size_t block_index = index / m_block_size;
        const size_t local_index = index % m_block_size;

        assert(block_index < m_block_pointers->size());
        assert((*m_block_pointers)[block_index].first + local_index
               < (*m_block_pointers)[block_index].second);

        return *((*m_block_pointers)[block_index].first + local_index);
    }

    //! pre-increment operator
    self_type& operator ++ ()
    {
        ++m_index;
        ++m_current;

        if (m_current == (*m_block_pointers)[m_block_index].second) {
            if (m_block_index + 1 < m_block_pointers->size()) {
                m_current = (*m_block_pointers)[++m_block_index].first;
            }
            else {
                // global end
                assert(m_block_index + 1 == m_block_pointers->size());
                m_current = (*m_block_pointers)[m_block_index++].second;
            }
        }

        return *this;
    }
    //! post-increment operator
    self_type operator ++ (int)
    {
        self_type former(*this);
        operator ++ ();
        return former;
    }
    //! pre-increment operator
    self_type& operator -- ()
    {
        assert(m_index > 0);
        --m_index;

        if (m_block_index >= m_block_pointers->size()
            || m_current == (*m_block_pointers)[m_block_index].first) {
            // begin of current block or global end
            assert(m_block_index > 0);
            assert(m_block_index <= m_block_pointers->size());
            m_current = (*m_block_pointers)[--m_block_index].second - 1;
        }
        else {
            --m_current;
        }

        return *this;
    }
    //! post-increment operator
    self_type operator -- (int)
    {
        self_type former(*this);
        operator -- ();
        return former;
    }
    self_type operator + (difference_type addend) const
    {
        return self_type(m_block_pointers, m_block_size, m_index + addend);
    }
    self_type& operator += (difference_type addend)
    {
        m_index += addend;
        update();
        return *this;
    }
    self_type operator - (difference_type subtrahend) const
    {
        return self_type(m_block_pointers, m_block_size, m_index - subtrahend);
    }
    difference_type operator - (const self_type& o) const
    {
        return (m_index - o.m_index);
    }
    self_type& operator -= (difference_type subtrahend)
    {
        m_index -= subtrahend;
        update();
        return *this;
    }
    bool operator == (const self_type& o) const
    {
        return m_index == o.m_index;
    }
    bool operator != (const self_type& o) const
    {
        return m_index != o.m_index;
    }
    bool operator < (const self_type& o) const
    {
        return m_index < o.m_index;
    }
    bool operator <= (const self_type& o) const
    {
        return m_index <= o.m_index;
    }
    bool operator > (const self_type& o) const
    {
        return m_index > o.m_index;
    }
    bool operator >= (const self_type& o) const
    {
        return m_index >= o.m_index;
    }

private:
    //! updates m_block_index and m_current based on m_index
    inline void update()
    {
        m_block_index = m_index / m_block_size;
        const size_t local_index = m_index % m_block_size;

        if (m_block_index < m_block_pointers->size()) {
            m_current = (*m_block_pointers)[m_block_index].first + local_index;
            assert(m_current < (*m_block_pointers)[m_block_index].second);
        }
        else {
            // global end
            assert(m_block_index == m_block_pointers->size());
            assert(local_index == 0);
            m_current = (*m_block_pointers)[m_block_index - 1].second;
        }
    }
};

/*!
 * Internal arrays store a sorted sequence of values in RAM, which will be
 * merged together into the deletion buffer when it needs to be
 * refilled. Internal arrays are constructed from the insertions heaps when
 * they overflow.
 */
template <class ValueType>
class internal_array : private noncopyable
{
public:
    typedef ValueType value_type;
    typedef ppq_iterator<value_type> iterator;

protected:
    typedef typename iterator::block_pointers_type block_pointers_type;

    //! Contains the items of the sorted sequence.
    std::vector<value_type> m_values;

    //! Index of the current head
    size_t m_min_index;

    //! Begin and end pointers of the array
    //! This is used by the iterator
    block_pointers_type m_block_pointers;

public:
    //! Default constructor. Don't use this directy. Needed for regrowing in
    //! surrounding vector.
    internal_array() { }

    //! Constructor which takes a value vector. The value vector is empty
    //! afterwards.
    internal_array(std::vector<ValueType>& values)
        : m_values(), m_min_index(0), m_block_pointers(1)
    {
        std::swap(m_values, values);
        m_block_pointers[0] = std::make_pair(&(*m_values.begin()), &(*m_values.end()));
    }

    //! Swap internal_array with another one.
    void swap(internal_array& o)
    {
        std::swap(m_values, o.m_values);
        std::swap(m_min_index, o.m_min_index);
        std::swap(m_block_pointers, o.m_block_pointers);
    }

    //! Random access operator
    inline ValueType& operator [] (size_t i)
    {
        return m_values[i];
    }

    //! Use inc_min if a value has been extracted.
    inline void inc_min()
    {
        m_min_index++;
    }

    //! Use inc_min(diff) if multiple values have been extracted.
    inline void inc_min(size_t diff)
    {
        m_min_index += diff;
    }

    //! The currently smallest element in the array.
    inline const ValueType & get_min() const
    {
        return m_values[m_min_index];
    }

    //! The index of the currently smallest element in the array.
    inline size_t get_min_index() const
    {
        return m_min_index;
    }

    //! The index of the largest element in the array.
    inline size_t get_max_index() const
    {
        return (m_values.size() - 1);
    }

    //! Returns if the array has run empty.
    inline bool empty() const
    {
        return (m_min_index >= m_values.size());
    }

    //! Returns the current size of the array.
    inline size_t size() const
    {
        return (m_values.size() - m_min_index);
    }

    //! Returns the initial size of the array.
    inline size_t capacity() const
    {
        return m_values.size();
    }

    //! Return the amount of internal memory used by the array
    inline size_t int_memory() const
    {
        return m_values.capacity() * sizeof(ValueType);
    }

    //! Begin iterator
    inline iterator begin()
    {
        // not const, unfortunately.
        return iterator(&m_block_pointers, capacity(), m_min_index);
    }

    //! End iterator
    inline iterator end()
    {
        // not const, unfortunately.
        return iterator(&m_block_pointers, capacity(), capacity());
    }
};

/*!
 * External array stores a sorted sequence of values on the hard disk and
 * allows access to the first block (containing the smallest values).  The
 * class uses buffering and prefetching in order to improve the performance.
 *
 * \tparam ValueType Type of the contained objects (POD with no references to
 * internal memory).
 *
 * \tparam BlockSize External block size. Default =
 * STXXL_DEFAULT_BLOCK_SIZE(ValueType).
 *
 * \tparam AllocStrategy Allocation strategy for the external memory. Default =
 * STXXL_DEFAULT_ALLOC_STRATEGY.
 */
template <
    class ValueType,
    unsigned_type BlockSize = STXXL_DEFAULT_BLOCK_SIZE(ValueType),
    class AllocStrategy = STXXL_DEFAULT_ALLOC_STRATEGY
    >
class external_array : private noncopyable
{
public:
    typedef ValueType value_type;
    typedef ppq_iterator<value_type> iterator;

protected:
    typedef external_array<ValueType, BlockSize, AllocStrategy> self_type;
    typedef typed_block<BlockSize, ValueType> block_type;
    typedef read_write_pool<block_type> pool_type;
    typedef std::vector<BID<BlockSize> > bid_vector;
    typedef std::vector<block_type*> block_vector;
    typedef std::vector<request_ptr> request_vector;
    typedef std::vector<ValueType> maxima_vector;
    typedef typename iterator::block_pointers_type block_pointers_type;

public:
    //! The number of elements fitting into one block
    static const size_t block_size = (size_t)BlockSize / sizeof(ValueType);

protected:
    //! The total capacity of the external array. Cannot be changed after construction.
    size_t m_capacity;

    //! Number of blocks
    size_t m_num_blocks;

    //! Number of blocks to prefetch from EM
    size_t m_num_prefetch_blocks;

    //! Size of the write buffer in number of blocks
    size_t m_num_write_buffer_blocks;

    //! Prefetch and write buffer pool
    pool_type* m_pool;

    //! The IDs of each block in external memory
    bid_vector m_bids;

    //! The block with the currently smallest elements
    block_vector m_blocks;

    //! Begin and end pointers for each block
    block_pointers_type m_block_pointers;

    //! The read request pointers are used to wait until the block has been completely fetched.
    request_vector m_requests;

    //! stores the maximum value of each block
    maxima_vector m_maxima;

    //! Is array in write phase? True = write phase, false = read phase.
    bool m_write_phase;

    //! The total number of elements minus the number of extracted values
    size_t m_size;

    //! The read position in the array.
    size_t m_index;

    //! The index behind the last element that is located in RAM (or is at least requested to be so)
    size_t m_end_index;

    //! The index of the next block to be prefetched.
    size_t m_hint_index;

public:
    /*!
     * Constructs an external array
     *
     * \param size The total number of elements. Cannot be changed after
     * construction.
     *
     * \param num_prefetch_blocks Number of blocks to prefetch from hard disk
     *
     * \param num_write_buffer_blocks Size of the write buffer in number of
     * blocks
     */
    external_array(size_t size, size_t num_prefetch_blocks,
                   size_t num_write_buffer_blocks)
        :   // constants
          m_capacity(size),
          m_num_blocks((size_t)div_ceil(m_capacity, block_size)),
          m_num_prefetch_blocks(std::min(num_prefetch_blocks, m_num_blocks)),
          m_num_write_buffer_blocks(std::min(num_write_buffer_blocks, m_num_blocks)),
          m_pool(new pool_type(0, m_num_write_buffer_blocks)),

          // vectors
          m_bids(m_num_blocks),
          m_blocks(m_num_blocks, NULL),
          m_block_pointers(m_num_blocks),
          m_requests(m_num_blocks, NULL),
          m_maxima(m_num_blocks),

          // state
          m_write_phase(true),

          // indices
          m_size(0),
          m_index(0),
          m_end_index(1),                      // buffer size must be at least 1
          m_hint_index(1)
    {
        assert(m_capacity > 0);
        assert(m_num_write_buffer_blocks > 0); // needed for steal()
        // allocate blocks in EM.
        block_manager* bm = block_manager::get_instance();
        // first BID is not used, because first block is never written to EM.
        bm->new_blocks(AllocStrategy(), m_bids.begin() + 1, m_bids.end());
        m_blocks[0] = new block_type;
        update_block_pointers(0);
    }

    //! Default constructor. Don't use this directy. Needed for regrowing in
    //! surrounding vector.
    external_array()
        :   // constants
          m_capacity(0),
          m_num_blocks(0),
          m_num_prefetch_blocks(0),
          m_num_write_buffer_blocks(0),
          m_pool(NULL),

          // vectors
          m_bids(0),
          m_blocks(0),
          m_block_pointers(0),
          m_requests(0),
          m_maxima(0),

          // state
          m_write_phase(false),

          // indices
          m_size(0),
          m_index(0),
          m_end_index(0),
          m_hint_index(0)
    { }

    //! Swap internal_array with another one.
    void swap(external_array& o)
    {
        // constants
        std::swap(m_capacity, o.m_capacity);
        std::swap(m_num_blocks, o.m_num_blocks);
        std::swap(m_num_prefetch_blocks, o.m_num_prefetch_blocks);
        std::swap(m_num_write_buffer_blocks, o.m_num_write_buffer_blocks);
        std::swap(m_pool, o.m_pool);

        // vectors
        std::swap(m_bids, o.m_bids);
        std::swap(m_requests, o.m_requests);
        std::swap(m_blocks, o.m_blocks);
        std::swap(m_block_pointers, o.m_block_pointers);
        std::swap(m_maxima, o.m_maxima);

        // state
        std::swap(m_write_phase, o.m_write_phase);

        // indices
        std::swap(m_size, o.m_size);
        std::swap(m_index, o.m_index);
        std::swap(m_end_index, o.m_end_index);
        std::swap(m_hint_index, o.m_hint_index);
    }

    //! Destructor
    ~external_array()
    {
        if (m_size > 0) {
            // not all data has been read!

            assert(m_end_index > 0); // true because m_size>0
            const size_t block_index = m_index / block_size;
            const size_t end_block_index = (m_end_index - 1) / block_size;

            block_manager* bm = block_manager::get_instance();
            typename bid_vector::iterator i_begin = m_bids.begin() + std::max(block_index, (size_t)1);
            bm->delete_blocks(i_begin, m_bids.end());

            for (size_t i = block_index; i < end_block_index; ++i) {
                delete m_blocks[i];
            }
        }
        delete m_pool;
    }

    //! Returns the current size
    size_t size() const
    {
        return m_size;
    }

    //! Returns if the array is empty
    bool empty() const
    {
        return (m_size == 0);
    }

    //! Returns if the array is empty
    size_t buffer_size() const
    {
        return (m_end_index - m_index);
    }

    //! Returns a random-access iterator to the begin of the data
    //! in internal memory.
    iterator begin()
    {
        assert(m_index == m_capacity || block_valid(m_index / block_size));
        // not const, unfortunately.
        return iterator(&m_block_pointers, block_size, m_index);
    }

    //! Returns a random-access iterator 1 behind the end of the data
    //! in internal memory.
    iterator end()
    {
        assert(m_end_index == m_index || block_valid((m_end_index - 1) / block_size));
        // not const, unfortunately.
        return iterator(&m_block_pointers, block_size, m_end_index);
    }

    //! Returns the smallest element in the array
    ValueType & get_min()
    {
        return *begin();
    }

    //! Returns the largest element in internal memory (or at least
    //! requested to be in internal memory)
    ValueType & get_current_max()
    {
        assert(m_end_index > 0);
        const size_t last_block_index = (m_end_index - 1) / block_size;
        return m_maxima[last_block_index];
    }

    //! Returns if there is data in EM, that's not randomly accessible.
    bool has_em_data() const
    {
        assert(m_end_index > 0);
        const size_t last_block_index = (m_end_index - 1) / block_size;
        return (last_block_index + 1 < m_num_blocks);
    }

    //! Returns if the data requested to be in internal memory is
    //! completely fetched. True if wait() has been called before.
    bool valid() const
    {
        bool result = true;
        const size_t block_index = m_index / block_size;
        const size_t end_block_index = (m_end_index - 1) / block_size;
        for (size_t i = block_index; i <= end_block_index; ++i) {
            result = result && block_valid(i);
        }
        return result;
    }

    //! Random access operator for data in internal memory
    //! You should call wait() once after fetching data from EM.
    ValueType& operator [] (size_t i) const
    {
        assert(i < m_capacity);
        const size_t block_index = i / block_size;
        const size_t local_index = i % block_size;
        assert(i < m_capacity);
        assert(block_valid(block_index));
        return m_blocks[block_index]->elem[local_index];
    }

    //! Pushes the value at the end of the array
    void push_back(const ValueType& value)
    {
        assert(m_write_phase);
        assert(m_size < m_capacity);

        const size_t block_index = m_size / block_size;
        const size_t local_index = m_size % block_size;

        m_blocks[block_index]->elem[local_index] = value;
        ++m_size;

        // block finished? -> write out.
        if (UNLIKELY(m_size % block_size == 0 || m_size == m_capacity)) {
            m_maxima[block_index] = m_blocks[block_index]->elem[block_size - 1];
            if (block_index > 0) {
                // first block is never written to EM!
                assert(m_bids[block_index].valid());
                m_pool->write(m_blocks[block_index], m_bids[block_index]);
            }
            if (block_index + 1 < m_num_blocks) {
                // Steal block for further filling.
                m_blocks[block_index + 1] = m_pool->steal();
                update_block_pointers(block_index + 1);
            }
        }

        // compatibility to the block write interface
        m_index = m_size;
        m_end_index = m_index + 1;
    }

    //! Request a write buffer of at least the given size.
    //! It can be accessed using begin() and end().
    void request_write_buffer(size_t size)
    {
        assert(m_write_phase);
        assert(size > 0);
        assert(m_size + size <= m_capacity);
        assert(m_end_index > m_index);

        const size_t block_index = m_index / block_size;
        const size_t last_block_index = (m_end_index - 1) / block_size;

        m_end_index = m_size + size;

        const size_t last_block_index_after = (m_end_index - 1) / block_size;

        const size_t num_buffer_blocks = std::max(last_block_index_after, (size_t)1)
                                         - std::max(block_index, (size_t)1) + 1;

        if (num_buffer_blocks > m_pool->size_write()) {
            // needed for m_pool->steal()
            m_pool->resize_write(num_buffer_blocks);
        }

        for (size_t i = last_block_index + 1; i <= last_block_index_after; ++i) {
            m_blocks[i] = m_pool->steal();
            update_block_pointers(i);
        }
    }

    //! Writes the data from the write buffer to EM, if necessary.
    void flush_write_buffer()
    {
        const size_t size = m_end_index - m_index;
        const size_t block_index = m_index / block_size;
        size_t last_block_index = (m_end_index - 1) / block_size;

        if (m_size + size == m_capacity) {
            // we have to flush the last block, too.
            ++last_block_index;
        }

        for (size_t i = std::max(block_index, (size_t)1); i < last_block_index; ++i) {
            if (i < m_num_blocks - 1) {
                m_maxima[i] = m_blocks[i]->elem[block_size - 1];
            }
            else {
                // last block
                m_maxima[i] = m_blocks[i]->elem[last_block_size() - 1];
            }
            assert(m_bids[i].valid());
            m_pool->write(m_blocks[i], m_bids[i]);
        }

        if (m_size + size < m_capacity
            && m_end_index % block_size == 0
            && last_block_index + 1 < m_num_blocks) {
            // block was full -> request new one
            m_blocks[last_block_index + 1] = m_pool->steal();
            update_block_pointers(last_block_index + 1);
        }

        m_size += size;
        m_index = m_size;
        m_end_index = m_index + 1; // buffer size must be at least 1
    }

    //! Finish write phase. Afterwards the values can be extracted from bottom
    //! up (ascending order).
    void finish_write_phase()
    {
        assert(m_capacity == m_size);
        m_pool->resize_write(0);
        m_pool->resize_prefetch(m_num_prefetch_blocks);
        m_write_phase = false;

        // set m_maxima[0]
        if (m_capacity <= block_size) {
            m_maxima[0] = m_blocks[0]->elem[m_capacity - 1];
        }
        else {
            m_maxima[0] = m_blocks[0]->elem[block_size - 1];
        }

        const size_t local_block_size = block_size; // std::min cannot access static block_size
        m_end_index = std::min(local_block_size, m_capacity);
        m_index = 0;

        for (size_t i = 0; i < m_num_prefetch_blocks; ++i) {
            hint();
        }

        assert(m_blocks[0]);
    }

    //! Waits for requested data to be completely fetched into
    //! internal memory. Call this if you want to read data after
    //! remove() or request_further_block() has been called.
    void wait()
    {
        assert(!m_write_phase);
        assert(m_end_index > 0);
        const size_t block_index = m_index / block_size;
        const size_t end_block_index = (m_end_index - 1) / block_size;

        // ignore first block (never in EM)
        for (size_t i = std::max((size_t)1, block_index); i <= end_block_index; ++i) {
            assert(m_requests[i]);
            m_requests[i]->wait();
            assert(m_requests[i]->poll());
            assert(m_blocks[i]);
        }
    }

    //! Removes the first n elements from the array.  Loads the
    //! next block if the current one has run empty. Make shure there
    //! are at least n elements in RAM.
    void remove(size_t n)
    {
        assert(m_index + n <= m_capacity);
        assert(m_index + n <= m_end_index);
        assert(m_size >= n);
        assert(n > 0);

        const size_t block_index = m_index / block_size;

        const size_t index_after = m_index + n;
        const size_t block_index_after = index_after / block_size;
        const size_t local_index_after = index_after % block_size;

        assert(block_index_after <= m_num_blocks);

        block_manager* bm = block_manager::get_instance();
        typename bid_vector::iterator i_begin = m_bids.begin() + std::max(block_index, (size_t)1);
        typename bid_vector::iterator i_end = m_bids.begin() + std::max(block_index_after, (size_t)1);
        if (i_end > i_begin) {
            bm->delete_blocks(i_begin, i_end);
        }

        for (size_t i = block_index; i < block_index_after; ++i) {
            assert(block_valid(i));
            delete m_blocks[i];
        }

        m_index = index_after;
        m_size -= n;

        if (block_index_after >= m_num_blocks) {
            assert(block_index_after == m_num_blocks);
            assert(m_size == 0);
        }
        else if (local_index_after == 0) {
            assert(block_index_after == (m_end_index - 1) / block_size + 1);
            request_further_block();
        }
        else {
            assert(block_valid(block_index_after));
        }
    }

    //! Requests one more block to be fetched into RAM
    void request_further_block()
    {
        assert(has_em_data());
        assert(m_end_index > 0);

        const size_t block_index = (m_end_index - 1) / block_size + 1;

        assert(block_index < m_num_blocks);
        assert(block_index > 0); // first block is already in RAM
        assert(m_bids[block_index].valid());

        m_blocks[block_index] = new block_type;
        m_requests[block_index] = m_pool->read(m_blocks[block_index], m_bids[block_index]);
        update_block_pointers(block_index);

        hint();
        m_end_index = std::min(m_capacity, (block_index + 1) * block_size);
    }

protected:
    //! Returns if the block with the given index is completely fetched.
    bool block_valid(size_t block_index) const
    {
        if (!m_write_phase) {
            return ((block_index == 0) || (m_requests[block_index] && m_requests[block_index]->poll()));
        }
        else {
            return (bool)m_blocks[block_index];
        }
    }

    //! Gives the prefetcher a hint for the next block to prefetch.
    void hint()
    {
        if (m_hint_index < m_num_blocks) {
            m_pool->hint(m_bids[m_hint_index]);
        }
        ++m_hint_index;
    }

    //! Updates the m_block_pointers vector.
    //! Should be called after any steal() or read() operation.
    //! This is necessary for the iterators to work properly.
    inline void update_block_pointers(size_t block_index)
    {
        m_block_pointers[block_index].first = m_blocks[block_index]->begin();
        m_block_pointers[block_index].second = m_blocks[block_index]->end();
        assert(m_block_pointers[block_index].first != NULL);
        assert(m_block_pointers[block_index].second != NULL);
    }

    inline size_t last_block_size()
    {
        size_t mod = m_capacity % block_size;
        return (mod > 0) ? mod : block_size;
    }
};

} // namespace ppq_local

STXXL_END_NAMESPACE

namespace std {

template <class ValueType>
void swap(stxxl::ppq_local::internal_array<ValueType>& a,
          stxxl::ppq_local::internal_array<ValueType>& b)
{
    a.swap(b);
}

template <
    class ValueType,
    stxxl::unsigned_type BlockSize,
    class AllocStrategy
    >
void swap(stxxl::ppq_local::external_array<ValueType, BlockSize, AllocStrategy>& a,
          stxxl::ppq_local::external_array<ValueType, BlockSize, AllocStrategy>& b)
{
    a.swap(b);
}
} // end namespace std

// swap_vector MUST be included after the swap spezialization but before parallel_priority_queue class!
#include <stxxl/bits/common/swap_vector.h>

STXXL_BEGIN_NAMESPACE

namespace ppq_local {

/*!
 * The minima_tree contains minima from all sources inside the PPQ. It contains
 * four substructures: winner trees for insertion heaps, internal and external
 * arrays, each containing the minima from all currently allocated
 * structures. These three sources, plus the deletion buffer are combined using
 * a "head" inner tree containing only up to four item.
 */
template <class ParentType>
class minima_tree
{
public:
    typedef ParentType parent_type;
    typedef minima_tree<ParentType> self_type;

    typedef typename parent_type::inv_compare_type compare_type;
    typedef typename parent_type::value_type value_type;
    typedef typename parent_type::proc_vector_type proc_vector_type;
    typedef typename parent_type::internal_arrays_type ias_type;
    typedef typename parent_type::external_arrays_type eas_type;

    static const unsigned initial_ia_size = 2;
    static const unsigned initial_ea_size = 2;

protected:
    //! WinnerTree-Comparator for the head winner tree. It accesses all
    //! relevant data structures from the priority queue.
    struct head_comp
    {
        self_type& m_parent;
        proc_vector_type& m_proc;
        ias_type& m_ias;
        eas_type& m_eas;
        const compare_type& m_compare;

        head_comp(self_type& parent, proc_vector_type& proc,
                  ias_type& ias, eas_type& eas,
                  const compare_type& compare)
            : m_parent(parent),
              m_proc(proc),
              m_ias(ias),
              m_eas(eas),
              m_compare(compare)
        { }

        const value_type & get_value(int input) const
        {
            switch (input) {
            case HEAP:
                return m_proc[m_parent.m_heaps.top()].insertion_heap[0];
            case EB:
                return m_parent.m_parent.m_extract_buffer[
                    m_parent.m_parent.m_extract_buffer_index
                ];
            case IA:
                return m_ias[m_parent.m_ia.top()].get_min();
            case EA:
                return m_eas[m_parent.m_ea.top()].get_min();
            default:
                abort();
            }
        }

        bool operator () (const int a, const int b) const
        {
            return m_compare(get_value(a), get_value(b));
        }
    };

    //! Comparator for the insertion heaps winner tree.
    struct heaps_comp
    {
        proc_vector_type& m_proc;
        const compare_type& m_compare;

        heaps_comp(proc_vector_type& proc, const compare_type& compare)
            : m_proc(proc), m_compare(compare)
        { }

        const value_type & get_value(int index) const
        {
            return m_proc[index].insertion_heap[0];
        }

        bool operator () (const int a, const int b) const
        {
            return m_compare(get_value(a), get_value(b));
        }
    };

    //! Comparator for the internal arrays winner tree.
    struct ia_comp
    {
        ias_type& m_ias;
        const compare_type& m_compare;

        ia_comp(ias_type& ias, const compare_type& compare)
            : m_ias(ias), m_compare(compare)
        { }

        bool operator () (const int a, const int b) const
        {
            return m_compare(m_ias[a].get_min(), m_ias[b].get_min());
        }
    };

    //! Comparator for the external arrays winner tree.
    struct ea_comp
    {
        eas_type& m_eas;
        const compare_type& m_compare;

        ea_comp(eas_type& eas, const compare_type& compare)
            : m_eas(eas), m_compare(compare)
        { }

        bool operator () (const int a, const int b) const
        {
            return m_compare(m_eas[a].get_min(),
                             m_eas[b].get_min());
        }
    };

protected:
    //! The priority queue
    parent_type& m_parent;

    //! value_type comparator
    const compare_type& m_compare;

    //! Comperator instances
    head_comp m_head_comp;
    heaps_comp m_heaps_comp;
    ia_comp m_ia_comp;
    ea_comp m_ea_comp;

    //! The winner trees
    winner_tree<head_comp> m_head;
    winner_tree<heaps_comp> m_heaps;
    winner_tree<ia_comp> m_ia;
    winner_tree<ea_comp> m_ea;

public:
    //! Entries in the head winner tree.
    enum Types {
        HEAP = 0,
        EB = 1,
        IA = 2,
        EA = 3,
        ERROR = 4
    };

    //! Construct the tree of minima sources.
    minima_tree(parent_type& parent)
        : m_parent(parent),
          m_compare(parent.m_inv_compare),
          // construct comparators
          m_head_comp(*this, parent.m_proc,
                      parent.m_internal_arrays, parent.m_external_arrays,
                      m_compare),
          m_heaps_comp(parent.m_proc, m_compare),
          m_ia_comp(parent.m_internal_arrays, m_compare),
          m_ea_comp(parent.m_external_arrays, m_compare),
          // construct header winner tree
          m_head(4, m_head_comp),
          m_heaps(m_parent.m_num_insertion_heaps, m_heaps_comp),
          m_ia(initial_ia_size, m_ia_comp),
          m_ea(initial_ea_size, m_ea_comp)
    { }

    //! Return smallest items of head winner tree.
    std::pair<unsigned, unsigned> top()
    {
        unsigned type = m_head.top();
        switch (type)
        {
        case HEAP:
            return std::make_pair(HEAP, m_heaps.top());
        case EB:
            return std::make_pair(EB, 0);
        case IA:
            return std::make_pair(IA, m_ia.top());
        case EA:
            return std::make_pair(EA, m_ea.top());
        default:
            return std::make_pair(ERROR, 0);
        }
    }

    //! Update minima tree after an item from the heap index was removed.
    void update_heap(unsigned index)
    {
        m_heaps.notify_change(index);
        m_head.notify_change(HEAP);
    }

    //! Update minima tree after an item of the extract buffer was removed.
    void update_extract_buffer()
    {
        m_head.notify_change(EB);
    }

    //! Update minima tree after an item from an internal array was removed.
    void update_internal_array(unsigned index)
    {
        m_ia.notify_change(index);
        m_head.notify_change(IA);
    }

    //! Update minima tree after an item from an external array was removed.
    void update_external_array(unsigned index)
    {
        m_ea.notify_change(index);
        m_head.notify_change(EA);
    }

    //! Add a newly created internal array to the minima tree.
    void add_internal_array(unsigned index)
    {
        m_ia.activate_player(index);
        m_head.notify_change(IA);
    }

    //! Add a newly created external array to the minima tree.
    void add_external_array(unsigned index)
    {
        m_ea.activate_player(index);
        m_head.notify_change(EA);
    }

    //! Remove an insertion heap from the minima tree.
    void deactivate_heap(unsigned index)
    {
        m_heaps.deactivate_player(index);
        if (!m_heaps.empty())
            m_head.notify_change(HEAP);
        else
            m_head.deactivate_player(HEAP);
    }

    //! Remove the extract buffer from the minima tree.
    void deactivate_extract_buffer()
    {
        m_head.deactivate_player(EB);
    }

    //! Remove an internal array from the minima tree.
    void deactivate_internal_array(unsigned index)
    {
        m_ia.deactivate_player(index);
        if (!m_ia.empty())
            m_head.notify_change(IA);
        else
            m_head.deactivate_player(IA);
    }

    //! Remove an external array from the minima tree.
    void deactivate_external_array(unsigned index)
    {
        m_ea.deactivate_player(index);
        if (!m_ea.empty())
            m_head.notify_change(EA);
        else
            m_head.deactivate_player(EA);
    }

    //! Remove all insertion heaps from the minima tree.
    void clear_heaps()
    {
        m_heaps.clear();
        m_head.deactivate_player(HEAP);
    }

    //! Remove all internal arrays from the minima tree.
    void clear_internal_arrays()
    {
        m_ia.resize_and_clear(initial_ia_size);
        m_head.deactivate_player(IA);
    }

    //! Remove all external arrays from the minima tree.
    void clear_external_arrays()
    {
        m_ea.resize_and_clear(initial_ea_size);
        m_head.deactivate_player(EA);
    }

    //! Returns a readable representation of the winner tree as string.
    std::string to_string() const
    {
        std::ostringstream ss;
        ss << "Head:" << std::endl << m_head.to_string() << std::endl;
        ss << "Heaps:" << std::endl << m_heaps.to_string() << std::endl;
        ss << "IA:" << std::endl << m_ia.to_string() << std::endl;
        ss << "EA:" << std::endl << m_ea.to_string() << std::endl;
        return ss.str();
    }

    //! Prints statistical data.
    void print_stats() const
    {
        STXXL_MSG("Head winner tree stats:");
        m_head.print_stats();
        STXXL_MSG("Heaps winner tree stats:");
        m_heaps.print_stats();
        STXXL_MSG("IA winner tree stats:");
        m_ia.print_stats();
        STXXL_MSG("EA winner tree stats:");
        m_ea.print_stats();
    }
};

} // namespace ppq_local

/*!
 * Parallelized External Memory Priority Queue.
 *
 * \tparam ValueType Type of the contained objects (POD with no references to
 * internal memory).
 *
 * \tparam CompareType The comparator type used to determine whether one
 * element is smaller than another element.
 *
 * \tparam DefaultMemSize Maximum memory consumption by the queue. Can be
 * overwritten by the constructor. Default = 1 GiB.
 *
 * \tparam MaxItems Maximum number of elements the queue contains at one
 * time. Default = 0 = unlimited. This is no hard limit and only used for
 * optimization. Can be overwritten by the constructor.
 *
 * \tparam BlockSize External block size. Default =
 * STXXL_DEFAULT_BLOCK_SIZE(ValueType).
 *
 * \tparam AllocStrategy Allocation strategy for the external memory. Default =
 * STXXL_DEFAULT_ALLOC_STRATEGY.
 */
template <
    class ValueType,
    class CompareType = std::less<ValueType>,
    class AllocStrategy = STXXL_DEFAULT_ALLOC_STRATEGY,
    uint64 BlockSize = STXXL_DEFAULT_BLOCK_SIZE(ValueType),
    uint64 DefaultMemSize = 1* 1024L* 1024L* 1024L,
    uint64 MaxItems = 0
    >
class parallel_priority_queue : private noncopyable
{
    //! \name Types
    //! \{

public:
    typedef ValueType value_type;
    typedef CompareType compare_type;
    typedef AllocStrategy alloc_strategy;
    static const uint64 block_size = BlockSize;
    typedef uint64 size_type;

protected:
    typedef typed_block<block_size, ValueType> block_type;
    typedef std::vector<BID<block_size> > bid_vector;
    typedef bid_vector bids_container_type;
    typedef ppq_local::internal_array<ValueType> internal_array_type;
    typedef ppq_local::external_array<ValueType, block_size, AllocStrategy> external_array_type;
    typedef typename std::vector<ValueType>::iterator value_iterator;
    typedef typename internal_array_type::iterator iterator;

    //! type of insertion heap itself
    typedef std::vector<ValueType> heap_type;

    //! type of internal arrays vector
    typedef typename stxxl::swap_vector<internal_array_type> internal_arrays_type;
    //! type of external arrays vector
    typedef typename stxxl::swap_vector<external_array_type> external_arrays_type;
    //! type of minima tree combining the structures
    typedef ppq_local::minima_tree<
            parallel_priority_queue<value_type, compare_type, alloc_strategy,
                                    block_size, DefaultMemSize, MaxItems> > minima_type;
    //! allow minima tree access to internal data structures
    friend class ppq_local::minima_tree<
        parallel_priority_queue<ValueType, compare_type, alloc_strategy,
                                block_size, DefaultMemSize, MaxItems> >;

    //! Inverse comparison functor
    struct inv_compare_type
    {
        const compare_type& compare;

        inv_compare_type(const compare_type& c)
            : compare(c)
        { }

        bool operator () (const value_type& x, const value_type& y) const
        {
            return compare(y, x);
        }
    };

    //! <-Comparator for ValueType
    compare_type m_compare;

    //! >-Comparator for ValueType
    inv_compare_type m_inv_compare;

    //! Defines if statistics are gathered: dummy_custom_stats_counter or
    //! custom_stats_counter
    typedef dummy_custom_stats_counter<uint64> stats_counter;

    //! Defines if statistics are gathered: fake_timer or timer
    typedef fake_timer stats_timer;

    //! \}

    //! \name Compile-Time Parameters
    //! \{

    //! Merge sorted heaps when flushing into an internal array.
    //! Pro: Reduces the risk of a large winner tree
    //! Con: Flush insertion heaps becomes slower.
    static const bool c_merge_sorted_heaps = true;

    //! Also merge internal arrays into the extract buffer
    static const bool c_merge_ias_into_eb = true;

    //! Default number of prefetch blocks per external array.
    static const unsigned c_num_prefetch_buffer_blocks = 1;

    //! Default number of write buffer block for a new external array being
    //! filled.
    static const unsigned c_num_write_buffer_blocks = 14;

    //! Defines for how much external arrays memory should be reserved in the
    //! constructor.
    static const unsigned c_num_reserved_external_arrays = 10;

    //! Size of a single insertion heap in Byte, if not defined otherwise in
    //! the constructor. Default: 1 MiB
    static const size_type c_default_single_heap_ram = 1L * 1024L * 1024L;

    //! Default limit of the extract buffer ram consumption as share of total
    //! ram
    // C++11: constexpr static double c_default_extract_buffer_ram_part = 0.05;
    // C++98 does not allow static const double initialization here.
    // It's located in global scope instead.
    static const double c_default_extract_buffer_ram_part;

    /*!
     * Limit the size of the extract buffer to an absolute value.
     *
     * The actual size can be set using the extract_buffer_ram parameter of the
     * constructor. If this parameter is not set, the value is calculated by
     * (total_ram*c_default_extract_buffer_ram_part)
     *
     * If c_limit_extract_buffer==false, the memory consumption of the extract
     * buffer is only limited by the number of external and internal
     * arrays. This is considered in memory management using the
     * ram_per_external_array and ram_per_internal_array values. Attention:
     * Each internal array reserves space for the extract buffer in the size of
     * all heaps together.
     */
    static const bool c_limit_extract_buffer = false;

    //! For bulks of size up to c_single_insert_limit sequential single insert
    //! is faster than bulk_push.
    static const unsigned c_single_insert_limit = 100;

    //! \}

    //! \name Parameters and Sizes for Memory Allocation Policy

    //! Number of prefetch blocks per external array
    const unsigned m_num_prefetchers;

    //! Number of write buffer blocks for a new external array being filled
    const unsigned m_num_write_buffers;

    //! Number of insertion heaps. Usually equal to the number of CPUs.
    const unsigned m_num_insertion_heaps;

    //! Capacity of one inserion heap
    const unsigned m_insertion_heap_capacity;

    //! Return size of insertion heap reservation in bytes
    size_type insertion_heap_int_memory() const
    {
        return m_insertion_heap_capacity * sizeof(value_type);
    }

    //! Total amount of internal memory
    const size_type m_mem_total;

    //! Maximum size of extract buffer in number of elements
    //! Only relevant if c_limit_extract_buffer==true
    size_type m_extract_buffer_limit;

    //! Size of all insertion heaps together in bytes
    const size_type m_mem_for_heaps;

    //! Free memory in bytes
    size_type m_mem_left;

    //! Amount of internal memory an external array needs during it's lifetime
    //! in bytes
    size_type m_mem_per_external_array;

    //! \}

    //! If the bulk currently being inserted is very large, this boolean is set
    //! and bulk_push just accumulate the elements for eventual sorting.
    bool m_is_very_large_bulk;

    //! Index of the currently smallest element in the extract buffer
    size_type m_extract_buffer_index;

    //! \name Number of elements currently in the data structures
    //! \{

    //! Number of elements int the insertion heaps
    size_type m_heaps_size;

    //! Number of elements in the extract buffer
    size_type m_extract_buffer_size;

    //! Number of elements in the internal arrays
    size_type m_internal_size;

    //! Number of elements in the external arrays
    size_type m_external_size;

    //! \}

    //! \name Data Holding Structures
    //! \{

    //! A struct containing the local insertion heap and other information
    //! _local_ to a processor.
    struct ProcessorData
    {
        //! The heaps where new elements are usually inserted into
        heap_type insertion_heap;

        //! The number of items inserted into the insheap during bulk parallel
        //! access.
        size_type heap_add_size;

        //! alignment should avoid cache thrashing between processors
    } __attribute__ ((aligned(64)));

    typedef std::vector<ProcessorData> proc_vector_type;

    //! Array of processor local data structures, including the insertion heaps.
    proc_vector_type m_proc;

    //! The extract buffer where external (and internal) arrays are merged into
    //! for extracting
    std::vector<ValueType> m_extract_buffer;

    //! The sorted arrays in internal memory
    internal_arrays_type m_internal_arrays;

    //! The sorted arrays in external memory
    external_arrays_type m_external_arrays;

    //! The aggregated pushes. They cannot be extracted yet.
    std::vector<ValueType> m_aggregated_pushes;

    //! The winner tree containing the smallest values of all sources
    //! where the globally smallest element could come from.
    minima_type m_minima;

    //! \}

    /*
     * Helper function to remove empty internal/external arrays.
     */

    //! Unary operator which returns true if the external array has run empty.
    struct empty_external_array_eraser {
        bool operator () (external_array_type& a) const
        { return a.empty(); }
    };

    //! Unary operator which returns true if the internal array has run empty.
    struct empty_internal_array_eraser {
        bool operator () (internal_array_type& a) const
        { return a.empty(); }
    };

    //! Clean up empty internal arrays, free their memory and capacity
    void cleanup_internal_arrays()
    {
        typename internal_arrays_type::iterator swapend =
            stxxl::swap_remove_if(m_internal_arrays.begin(),
                                  m_internal_arrays.end(),
                                  empty_internal_array_eraser());

        //size_type size_removed = 0;

        for (typename internal_arrays_type::iterator i = swapend;
             i != m_internal_arrays.end(); ++i)
        {
            m_mem_left += i->int_memory();
        }

        m_internal_arrays.erase(swapend, m_internal_arrays.end());
    }

    /*!
     * SiftUp a new element from the last position in the heap, reestablishing
     * the heap invariant. This is identical to std::push_heap, except that it
     * returns the last element modified by siftUp. Thus we can identify if the
     * minimum may have changed.
     */
    template <typename RandomAccessIterator, typename HeapCompareType>
    static inline unsigned_type
    push_heap(RandomAccessIterator first, RandomAccessIterator last,
              HeapCompareType comp)
    {
        typedef typename std::iterator_traits<RandomAccessIterator>::value_type
            value_type;

        value_type value = _GLIBCXX_MOVE(*(last - 1));

        unsigned_type index = (last - first) - 1;
        unsigned_type parent = (index - 1) / 2;

        while (index > 0 && comp(*(first + parent), value))
        {
            *(first + index) = _GLIBCXX_MOVE(*(first + parent));
            index = parent;
            parent = (index - 1) / 2;
        }
        *(first + index) = _GLIBCXX_MOVE(value);

        return index;
    }

public:
    //! \name Initialization
    //! \{

    /*!
     * Constructor.
     *
     * \param compare Comparator for priority queue, which is a Max-PQ.
     *
     * \param total_ram Maximum RAM usage. 0 = Default = Use the template
     * value Ram.
     *
     * \param num_prefetch_buffer_blocks Number of prefetch blocks per external
     * array. Default = c_num_prefetch_buffer_blocks
     *
     * \param num_write_buffer_blocks Number of write buffer blocks for a new
     * external array being filled. 0 = Default = c_num_write_buffer_blocks
     *
     * \param num_insertion_heaps Number of insertion heaps. 0 = Determine by
     * omp_get_max_threads(). Default = Determine by omp_get_max_threads().
     *
     * \param single_heap_ram Memory usage for a single insertion heap. 0 =
     * Default = c_single_heap_ram.
     *
     * \param extract_buffer_ram Memory usage for the extract buffer. Only
     * relevant if c_limit_extract_buffer==true. 0 = Default = total_ram *
     * c_default_extract_buffer_ram_part.
     */
    parallel_priority_queue(
        const compare_type& compare = compare_type(),
        size_type total_ram = DefaultMemSize,
        unsigned_type num_prefetch_buffer_blocks = c_num_prefetch_buffer_blocks,
        unsigned_type num_write_buffer_blocks = c_num_write_buffer_blocks,
        unsigned_type num_insertion_heaps = 0,
        size_type single_heap_ram = c_default_single_heap_ram,
        size_type extract_buffer_ram = 0)
        : m_compare(compare),
          m_inv_compare(m_compare),
          m_num_prefetchers(num_prefetch_buffer_blocks),
          m_num_write_buffers(num_write_buffer_blocks),
#if STXXL_PARALLEL
          m_num_insertion_heaps(num_insertion_heaps > 0 ? num_insertion_heaps : omp_get_max_threads()),
#else
          m_num_insertion_heaps(num_insertion_heaps > 0 ? num_insertion_heaps : 1),
#endif
          m_insertion_heap_capacity(single_heap_ram / sizeof(ValueType)),
          m_mem_total(total_ram),
          m_mem_for_heaps(m_num_insertion_heaps * single_heap_ram),
          m_is_very_large_bulk(false),
          m_extract_buffer_index(0),
          m_heaps_size(0),
          m_extract_buffer_size(0),
          m_internal_size(0),
          m_external_size(0),
          m_proc(m_num_insertion_heaps),
          m_minima(*this)
    {
        srand(static_cast<unsigned>(time(NULL)));

        if (!omp_get_nested()) {
            omp_set_nested(1);
            if (!omp_get_nested()) {
                STXXL_ERRMSG("Could not enable OpenMP's nested parallelism, "
                             "however, the PPQ requires this OpenMP feature.");
                abort();
            }
        }

        if (c_limit_extract_buffer) {
            m_extract_buffer_limit = (extract_buffer_ram > 0)
                                     ? extract_buffer_ram / sizeof(ValueType)
                                     : static_cast<size_type>(((double)(m_mem_total) * c_default_extract_buffer_ram_part / sizeof(ValueType)));
        }

        init_memmanagement();

        for (size_t i = 0; i < m_num_insertion_heaps; ++i)
        {
            m_proc[i].insertion_heap.reserve(m_insertion_heap_capacity);
            assert(m_proc[i].insertion_heap.capacity() * sizeof(value_type)
                   == insertion_heap_int_memory());
        }

        m_mem_left -= m_num_insertion_heaps * insertion_heap_int_memory();

        m_external_arrays.reserve(c_num_reserved_external_arrays);

        if (c_merge_sorted_heaps) {
            m_internal_arrays.reserve(m_mem_total / m_mem_for_heaps);
        }
        else {
            m_internal_arrays.reserve(m_mem_total * m_num_insertion_heaps / m_mem_for_heaps);
        }

        check_invariants();
    }

    //! Destructor.
    ~parallel_priority_queue()
    { }

protected:
    //! Initializes member variables concerning the memory management.
    void init_memmanagement()
    {
        // total_ram - ram for the heaps - ram for the heap merger - ram for the external array write buffer -
        m_mem_left = m_mem_total - 2 * m_mem_for_heaps - m_num_write_buffers * block_size;

        // prefetch blocks + first block of array
        m_mem_per_external_array = (m_num_prefetchers + 1) * block_size;

        if (c_limit_extract_buffer) {
            // ram for the extract buffer
            //TODO m_mem_left -= m_extract_buffer_limit * sizeof(ValueType);
        }
        else {
            // each: part of the (maximum) ram for the extract buffer
            m_mem_per_external_array += block_size;
        }

        if (c_merge_sorted_heaps) {
            // part of the ram for the merge buffer
            //TODO m_mem_left -= m_mem_for_heaps;
        }

        if (m_mem_left < 2 * m_mem_per_external_array + m_mem_for_heaps) {
            STXXL_ERRMSG("Insufficent memory: " << m_mem_for_heaps << " < " <<
                         2 * m_mem_per_external_array + m_mem_for_heaps);
            exit(EXIT_FAILURE);
        }
        else if (m_mem_left < 4 * m_mem_per_external_array + 2 * m_mem_for_heaps) {
            STXXL_ERRMSG("Warning: Low memory. Performance could suffer.");
        }
    }

    //! Assert many invariants of the data structures.
    void check_invariants()
    {
        size_type mem_used = 0;

        mem_used += 2 * m_mem_for_heaps + m_num_write_buffers * block_size;

        // test the processor local data structures

        size_type heaps_size = 0;

        for (unsigned p = 0; p < m_num_insertion_heaps; ++p)
        {
            // check that each insertion heap is a heap

            // TODO: remove soon, because this is very expensive
            STXXL_CHECK(1 || stxxl::is_heap(m_proc[p].insertion_heap.begin(),
                                            m_proc[p].insertion_heap.end(),
                                            m_compare));

            STXXL_CHECK(m_proc[p].insertion_heap.capacity() <= m_insertion_heap_capacity);

            heaps_size += m_proc[p].insertion_heap.size();
            mem_used += m_proc[p].insertion_heap.capacity() * sizeof(value_type);
        }

        STXXL_CHECK(m_heaps_size == heaps_size);

        // count number of items and memory size of internal arrays

        size_type ia_size = 0;
        size_type ia_memory = 0;

        for (typename internal_arrays_type::iterator ia = m_internal_arrays.begin();
             ia != m_internal_arrays.end(); ++ia)
        {
            ia_size += ia->size();
            ia_memory += ia->int_memory();
        }

        STXXL_CHECK(m_internal_size == ia_size);
        mem_used += ia_memory;

        // count number of items in external arrays

        size_type ea_size = 0;
        size_type ea_memory = 0;

        for (typename external_arrays_type::iterator ea = m_external_arrays.begin();
             ea != m_external_arrays.end(); ++ea)
        {
            ea_size += ea->size();
            ea_memory += m_mem_per_external_array;
        }

        STXXL_CHECK(m_external_size == ea_size);
        mem_used += ea_memory;

        // calculate mem_used so that == mem_total - mem_left

        STXXL_CHECK_EQUAL(memory_consumption(), mem_used);
    }

    //! \}

    //! \name Properties
    //! \{

public:
    //! The number of elements in the queue.
    inline size_type size() const
    {
        return m_heaps_size + m_internal_size + m_external_size + m_extract_buffer_size;
    }

    //! Returns if the queue is empty.
    inline bool empty() const
    {
        return (size() == 0);
    }

    //! The memory consumption in Bytes.
    inline size_type memory_consumption() const
    {
        assert(m_mem_total >= m_mem_left);
        return (m_mem_total - m_mem_left);
    }

protected:
    //! Returns if the extract buffer is empty.
    inline bool extract_buffer_empty() const
    {
        return (m_extract_buffer_size == 0);
    }

    //! \}

public:
    //! \name Bulk Operations
    //! \{

    /*!
     * Start a sequence of push operations.
     * \param bulk_size Exact number of elements to push before the next pop.
     */
    void bulk_push_begin(size_type bulk_size)
    {
        size_type heap_capacity = m_num_insertion_heaps * m_insertion_heap_capacity;

        // if bulk_size is large: use simple aggregation instead of keeping the
        // heap property and sort everything afterwards.
        if (bulk_size > heap_capacity && 0) {
            m_is_very_large_bulk = true;
        }
        else {
            m_is_very_large_bulk = false;

            if (bulk_size + m_heaps_size > heap_capacity) {
                if (m_heaps_size > 0) {
                    //flush_insertion_heaps();
                }
            }
        }

        // zero bulk insertion counters
        for (unsigned p = 0; p < m_num_insertion_heaps; ++p)
            m_proc[p].heap_add_size = 0;
    }

    /*!
     * Push an element inside a sequence of pushes.
     * Run bulk_push_begin() before using this method.
     *
     * \param element The element to push.
     * \param p The id of the insertion heap to use (usually the thread id).
     */
    void bulk_push(const ValueType& element, const int p)
    {
        heap_type& insheap = m_proc[p].insertion_heap;

        if (!m_is_very_large_bulk && 0)
        {
            // if small bulk: if heap is full -> sort locally and put into
            // internal array list. insert items and keep heap invariant.
            if (insheap.size() >= m_insertion_heap_capacity) {
#pragma omp atomic
                m_heaps_size += m_proc[p].heap_add_size;

                m_proc[p].heap_add_size = 0;
                flush_insertion_heap(p);
            }

            assert(insheap.size() < insheap.capacity());

            // put item onto heap and siftUp
            insheap.push_back(element);
            std::push_heap(insheap.begin(), insheap.end(), m_compare);
        }
        else if (!m_is_very_large_bulk && 1)
        {
            // if small bulk: if heap is full -> sort locally and put into
            // internal array list. insert items but DO NOT keep heap
            // invariant.
            if (insheap.size() >= m_insertion_heap_capacity) {
#pragma omp atomic
                m_heaps_size += m_proc[p].heap_add_size;

                m_proc[p].heap_add_size = 0;
                flush_insertion_heap(p);
            }

            assert(insheap.size() < insheap.capacity());

            // put item onto heap and DO NOT siftUp
            insheap.push_back(element);
        }
        else // m_is_very_large_bulk
        {
            if (insheap.size() >= 2 * 1024 * 1024) {
#pragma omp atomic
                m_heaps_size += m_proc[p].heap_add_size;

                m_proc[p].heap_add_size = 0;
                flush_insertion_heap(p);
            }

            assert(insheap.size() < insheap.capacity());

            // put onto insertion heap but do not keep heap property
            insheap.push_back(element);
        }

        m_proc[p].heap_add_size++;
    }

    /*!
     * Push an element inside a bulk sequence of pushes. Run bulk_push_begin()
     * before using this method. This function uses the insertion heap id =
     * omp_get_thread_num().
     *
     * \param element The element to push.
     */
    void bulk_push(const ValueType& element)
    {
#if STXXL_PARALLEL
        return bulk_push(element, omp_get_thread_num());
#else
        int id = rand() % m_num_insertion_heaps;
        return bulk_push(element, id);
#endif
    }

    /*!
     * Ends a sequence of push operations. Run bulk_push_begin() and some
     * bulk_push() before this.
     */
    void bulk_push_end()
    {
        if (!m_is_very_large_bulk && 0)
        {
            for (unsigned p = 0; p < m_num_insertion_heaps; ++p)
            {
                m_heaps_size += m_proc[p].heap_add_size;

                if (!m_proc[p].insertion_heap.empty())
                    m_minima.update_heap(p);
            }
        }
        else if (!m_is_very_large_bulk && 1)
        {
#pragma omp parallel for
            for (unsigned p = 0; p < m_num_insertion_heaps; ++p)
            {
                // reestablish heap property: siftUp only those items pushed
                for (unsigned_type index = m_proc[p].heap_add_size; index != 0; ) {
                    std::push_heap(m_proc[p].insertion_heap.begin(),
                                   m_proc[p].insertion_heap.end() - (--index),
                                   m_compare);
                }

#pragma omp atomic
                m_heaps_size += m_proc[p].heap_add_size;
            }

            for (unsigned p = 0; p < m_num_insertion_heaps; ++p)
            {
                if (!m_proc[p].insertion_heap.empty())
                    m_minima.update_heap(p);
            }
        }
        else // m_is_very_large_bulk
        {
#pragma omp parallel for
            for (unsigned p = 0; p < m_num_insertion_heaps; ++p)
            {
                if (m_proc[p].insertion_heap.size() >= m_insertion_heap_capacity) {
                    // flush out overfull insertion heap arrays
#pragma omp atomic
                    m_heaps_size += m_proc[p].heap_add_size;

                    m_proc[p].heap_add_size = 0;
                    flush_insertion_heap(p);
                }
                else {
                    // reestablish heap property: siftUp only those items pushed
                    for (unsigned_type index = m_proc[p].heap_add_size; index != 0; ) {
                        std::push_heap(m_proc[p].insertion_heap.begin(),
                                       m_proc[p].insertion_heap.end() - (--index),
                                       m_compare);
                    }

#pragma omp atomic
                    m_heaps_size += m_proc[p].heap_add_size;
                    m_proc[p].heap_add_size = 0;
                }
            }

            for (unsigned p = 0; p < m_num_insertion_heaps; ++p)
            {
                if (!m_proc[p].insertion_heap.empty())
                    m_minima.update_heap(p);
            }
        }

        check_invariants();
    }

    /*!
     * Insert a vector of elements at one time.
     * \param elements Vector containing the elements to push.
     * Attention: elements vector may be owned by the PQ afterwards.
     */
    void bulk_push_vector(std::vector<ValueType>& elements)
    {
        size_type heap_capacity = m_num_insertion_heaps * m_insertion_heap_capacity;
        if (elements.size() > heap_capacity / 2) {
            flush_array(elements);
            return;
        }

        bulk_push_begin(elements.size());
#if STXXL_PARALLEL
        #pragma omp parallel
        {
            const unsigned thread_num = omp_get_thread_num();
            #pragma omp parallel for
            for (size_type i = 0; i < elements.size(); ++i) {
                bulk_push(elements[i], thread_num);
            }
        }
#else
        const unsigned thread_num = rand() % m_num_insertion_heaps;
        for (size_type i = 0; i < elements.size(); ++i) {
            bulk_push(elements[i], thread_num);
        }
#endif
        bulk_push_end();
    }

    //! \}

    //! \name Aggregation Operations
    //! \{

    /*!
     * Aggregate pushes. Use flush_aggregated_pushes() to finally push
     * them. extract_min is allowed is allowed in between the aggregation of
     * pushes if you can assure, that the extracted value is smaller than all
     * of the aggregated values.
     * \param element The element to push.
     */
    void aggregate_push(const ValueType& element)
    {
        m_aggregated_pushes.push_back(element);
    }

    /*!
     * Insert the aggregated values into the queue using push(), bulk insert,
     * or sorting, depending on the number of aggregated values.
     */
    void flush_aggregated_pushes()
    {
        size_type size = m_aggregated_pushes.size();
        size_type ram_internal = 2 * size * sizeof(ValueType); // ram for the sorted array + part of the ram for the merge buffer
        size_type heap_capacity = m_num_insertion_heaps * m_insertion_heap_capacity;

        if (ram_internal > m_mem_for_heaps / 2) {
            flush_array(m_aggregated_pushes);
        }
        else if ((m_aggregated_pushes.size() > c_single_insert_limit) && (m_aggregated_pushes.size() < heap_capacity)) {
            bulk_push_vector(m_aggregated_pushes);
        }
        else {
            for (value_iterator i = m_aggregated_pushes.begin(); i != m_aggregated_pushes.end(); ++i) {
                push(*i);
            }
        }

        m_aggregated_pushes.clear();
    }

    //! \}

    //! \name std::priority_queue compliant operations
    //! \{

    /*!
     * Insert new element
     * \param element the element to insert.
     * \param id number of insertion heap to insert item into
     */
    void push(const ValueType& element, unsigned id)
    {
        heap_type& insheap = m_proc[id].insertion_heap;

        if (insheap.size() >= m_insertion_heap_capacity) {
            flush_insertion_heaps();
        }

        // push item to end of heap and siftUp
        insheap.push_back(element);
        unsigned_type index = push_heap(insheap.begin(), insheap.end(),
                                        m_compare);
        ++m_heaps_size;

        if (insheap.size() == 1 || index == 0)
            m_minima.update_heap(id);
    }

    /*!
     * Insert new element into a randomly selected insertion heap.
     * \param element the element to insert.
     */
    void push(const ValueType& element)
    {
        unsigned id = rand() % m_num_insertion_heaps;
        return push(element, id);
    }

    //! Access the minimum element.
    ValueType top()
    {
        if (extract_buffer_empty()) {
            refill_extract_buffer();
        }

        std::pair<unsigned, unsigned> type_and_index = m_minima.top();
        unsigned type = type_and_index.first;
        unsigned index = type_and_index.second;

        assert(type < 4);

        switch (type) {
        case minima_type::HEAP:
            //STXXL_VERBOSE1_PPQ("heap "<<index<<": "<<m_proc[index].insertion_heap[0]);
            return m_proc[index].insertion_heap[0];
        case minima_type::EB:
            //STXXL_VERBOSE1_PPQ("eb "<<m_extract_buffer_index<<": "<<m_extract_buffer[m_extract_buffer_index]);
            return m_extract_buffer[m_extract_buffer_index];
        case minima_type::IA:
            //STXXL_VERBOSE1_PPQ("ia "<<index<<": "<<m_internal_arrays[index].get_min());
            return m_internal_arrays[index].get_min();
        case minima_type::EA:
            //STXXL_VERBOSE1_PPQ("ea "<<index<<": "<<m_external_arrays[index].get_min());
            // wait() already done by comparator....
            return m_external_arrays[index].get_min();
        default:
            STXXL_ERRMSG("Unknown extract type: " << type);
            abort();
        }
    }

    //! Access and remove the minimum element.
    ValueType pop()
    {
        m_stats.num_extracts++;

        if (extract_buffer_empty()) {
            refill_extract_buffer();
        }

        m_stats.extract_min_time.start();

        std::pair<unsigned, unsigned> type_and_index = m_minima.top();
        unsigned type = type_and_index.first;
        unsigned index = type_and_index.second;
        ValueType min;

        assert(type < 4);

        switch (type) {
        case minima_type::HEAP:
        {
            heap_type& insheap = m_proc[index].insertion_heap;

            min = insheap[0];

            m_stats.pop_heap_time.start();
            std::pop_heap(insheap.begin(), insheap.end(), m_compare);
            insheap.pop_back();
            m_stats.pop_heap_time.stop();

            m_heaps_size--;

            if (!insheap.empty())
                m_minima.update_heap(index);
            else
                m_minima.deactivate_heap(index);

            break;
        }
        case minima_type::EB:
        {
            min = m_extract_buffer[m_extract_buffer_index];
            ++m_extract_buffer_index;
            assert(m_extract_buffer_size > 0);
            --m_extract_buffer_size;

            if (!extract_buffer_empty())
                m_minima.update_extract_buffer();
            else
                m_minima.deactivate_extract_buffer();

            break;
        }
        case minima_type::IA:
        {
            min = m_internal_arrays[index].get_min();
            m_internal_arrays[index].inc_min();
            m_internal_size--;

            if (!(m_internal_arrays[index].empty()))
                m_minima.update_internal_array(index);
            else
                // internal array has run empty
                m_minima.deactivate_internal_array(index);

            break;
        }
        case minima_type::EA:
        {
            // wait() already done by comparator...
            min = m_external_arrays[index].get_min();
            assert(m_external_size > 0);
            --m_external_size;
            m_external_arrays[index].remove(1);
            m_external_arrays[index].wait();

            if (!m_external_arrays[index].empty())
                m_minima.update_external_array(index);
            else
                // external array has run empty
                m_minima.deactivate_external_array(index);

            break;
        }
        default:
            STXXL_ERRMSG("Unknown extract type: " << type);
            abort();
        }

        m_stats.extract_min_time.stop();
        check_invariants();

        return min;
    }

    //! \}

    /*!
     * Merges all external arrays into one external array.  Public for
     * benchmark purposes.
     */
    void merge_external_arrays()
    {
        m_stats.num_external_array_merges++;
        m_stats.external_array_merge_time.start();

        m_minima.clear_external_arrays();

        // clean up external arrays that have been deleted in extract_min!
        m_external_arrays.erase(stxxl::swap_remove_if(m_external_arrays.begin(), m_external_arrays.end(), empty_external_array_eraser()), m_external_arrays.end());

        size_type total_size = m_external_size;
        assert(total_size > 0);

        external_array_type a(total_size, m_num_prefetchers, m_num_write_buffers);
        m_external_arrays.swap_back(a);

        m_mem_left += (m_external_arrays.size() - 1) * m_mem_per_external_array;

        while (m_external_arrays.size() > 0) {
            size_type eas = m_external_arrays.size();

            m_external_arrays[0].wait();
            ValueType min_max_value = m_external_arrays[0].get_current_max();

            for (size_type i = 1; i < eas; ++i) {
                m_external_arrays[i].wait();
                ValueType max_value = m_external_arrays[i].get_current_max();
                if (m_inv_compare(max_value, min_max_value)) {
                    min_max_value = max_value;
                }
            }

            std::vector<size_type> sizes(eas);
            std::vector<std::pair<iterator, iterator> > sequences(eas);
            size_type output_size = 0;

#if STXXL_PARALLEL
            #pragma omp parallel for if(eas > m_num_insertion_heaps)
#endif
            for (size_type i = 0; i < eas; ++i) {
                assert(!m_external_arrays[i].empty());

                m_external_arrays[i].wait();
                iterator begin = m_external_arrays[i].begin();
                iterator end = m_external_arrays[i].end();

                assert(begin != end);

                iterator ub = std::upper_bound(begin, end, min_max_value, m_inv_compare);
                sizes[i] = ub - begin;
#pragma omp atomic
                output_size += ub - begin;
                sequences[i] = std::make_pair(begin, ub);
            }

            a.request_write_buffer(output_size);

#if STXXL_PARALLEL
            parallel::sw_multiway_merge(sequences.begin(), sequences.end(),
                                        a.begin(), m_inv_compare, output_size);
#else
            // TODO
#endif

            a.flush_write_buffer();

            for (size_type i = 0; i < eas; ++i) {
                m_external_arrays[i].remove(sizes[i]);
            }

            for (size_type i = 0; i < eas; ++i) {
                m_external_arrays[i].wait();
            }

            m_external_arrays.erase(stxxl::swap_remove_if(m_external_arrays.begin(), m_external_arrays.end(), empty_external_array_eraser()), m_external_arrays.end());
        }

        a.finish_write_phase();
        m_external_arrays.swap_back(a);

        if (!extract_buffer_empty()) {
            m_stats.num_new_external_arrays++;
            m_stats.max_num_new_external_arrays.set_max(m_stats.num_new_external_arrays);
            a.wait();
            m_minima.add_external_array(static_cast<unsigned>(m_external_arrays.size()) - 1);
        }

        m_stats.external_array_merge_time.stop();

        check_invariants();
    }

    //! Print statistics.
    void print_stats() const
    {
        STXXL_VARDUMP(c_merge_sorted_heaps);
        STXXL_VARDUMP(c_merge_ias_into_eb);
        STXXL_VARDUMP(c_limit_extract_buffer);
        STXXL_VARDUMP(c_single_insert_limit);

        if (c_limit_extract_buffer) {
            STXXL_VARDUMP(m_extract_buffer_limit);
            STXXL_MEMDUMP(m_extract_buffer_limit * sizeof(ValueType));
        }

#if STXXL_PARALLEL
        STXXL_VARDUMP(omp_get_max_threads());
#endif

        STXXL_MEMDUMP(m_mem_for_heaps);
        STXXL_MEMDUMP(m_mem_left);
        STXXL_MEMDUMP(m_mem_per_external_array);

        //if (num_extract_buffer_refills > 0) {
        //    STXXL_VARDUMP(total_extract_buffer_size / num_extract_buffer_refills);
        //    STXXL_MEMDUMP(total_extract_buffer_size / num_extract_buffer_refills * sizeof(ValueType));
        //}

        STXXL_MSG(m_stats);
        m_minima.print_stats();
    }

protected:
    //! Refills the extract buffer from the external arrays.
    inline void refill_extract_buffer()
    {
        STXXL_VERBOSE1_PPQ("refilling extract buffer");

        check_invariants();

        assert(extract_buffer_empty());
        assert(m_extract_buffer_size == 0);
        m_extract_buffer_index = 0;

        m_minima.clear_external_arrays();
        m_external_arrays.erase(stxxl::swap_remove_if(m_external_arrays.begin(), m_external_arrays.end(), empty_external_array_eraser()), m_external_arrays.end());
        size_type eas = m_external_arrays.size();

        size_type ias;

        if (c_merge_ias_into_eb) {
            m_minima.clear_internal_arrays();
            cleanup_internal_arrays();
            ias = m_internal_arrays.size();
        }
        else {
            ias = 0;
        }

        if (eas == 0 && ias == 0) {
            m_extract_buffer.resize(0);
            m_minima.deactivate_extract_buffer();
            return;
        }

        m_stats.num_extract_buffer_refills++;
        m_stats.refill_extract_buffer_time.start();
        m_stats.refill_time_before_merge.start();
        m_stats.refill_minmax_time.start();

        /*
         * determine maximum of each first block
         */

        ValueType min_max_value;

        // check only relevant if c_merge_ias_into_eb==true
        if (eas > 0) {
            m_stats.refill_wait_time.start();
            m_external_arrays[0].wait();
            m_stats.refill_wait_time.stop();
            assert(m_external_arrays[0].size() > 0);
            min_max_value = m_external_arrays[0].get_current_max();
        }
        else {
            assert(ias > 0);
        }

        for (size_type i = 1; i < eas; ++i) {
            m_stats.refill_wait_time.start();
            m_external_arrays[i].wait();
            m_stats.refill_wait_time.stop();

            ValueType max_value = m_external_arrays[i].get_current_max();
            if (m_inv_compare(max_value, min_max_value)) {
                min_max_value = max_value;
            }
        }

        STXXL_VARDUMP(min_max_value);

        m_stats.refill_minmax_time.stop();

        // the number of elements in each external array that are smaller than
        // min_max_value or equal plus the number of elements in the internal
        // arrays
        std::vector<size_type> sizes(eas + ias);
        std::vector<std::pair<iterator, iterator> > sequences(eas + ias);

        /*
         * calculate size and create sequences to merge
         */

#if STXXL_PARALLEL
        //#pragma omp parallel for if(eas + ias > m_num_insertion_heaps)
#endif
        for (size_type i = 0; i < eas + ias; ++i) {
            // check only relevant if c_merge_ias_into_eb==true
            if (i < eas) {
                assert(!m_external_arrays[i].empty());

                assert(m_external_arrays[i].valid());
                iterator begin = m_external_arrays[i].begin();
                iterator end = m_external_arrays[i].end();

                assert(begin != end);

                // remove if parallel
                //stats.refill_upper_bound_time.start();
                iterator ub = std::upper_bound(begin, end, min_max_value, m_inv_compare);
                //stats.refill_upper_bound_time.stop();

                sizes[i] = std::distance(begin, ub);
                sequences[i] = std::make_pair(begin, ub);

                STXXL_VERBOSE1_PPQ("adding external seq, size=" << sizes[i] <<
                                   " begin=" << *begin <<
                                   " ub=" << (ub == begin ? *ub : *(ub - 1)) <<
                                   " end=" << *(end - 1) <<
                                   " currentmax=" << m_external_arrays[i].get_current_max());
            }
            else {
                // else part only relevant if c_merge_ias_into_eb==true

                size_type j = i - eas;
                assert(!(m_internal_arrays[j].empty()));

                iterator begin = m_internal_arrays[j].begin();
                iterator end = m_internal_arrays[j].end();
                assert(begin != end);

                if (eas > 0) {
                    //remove if parallel
                    //stats.refill_upper_bound_time.start();
                    iterator ub = std::upper_bound(begin, end, min_max_value, m_inv_compare);
                    //stats.refill_upper_bound_time.stop();

                    sizes[i] = std::distance(begin, ub);
                    sequences[i] = std::make_pair(begin, ub);

                    STXXL_VERBOSE1_PPQ("adding internal seq, size=" << sizes[i] <<
                                       " begin=" << *begin <<
                                       " ub=" << (ub == begin ? *ub : *(ub - 1)) <<
                                       " end=" << *(end - 1));

                    if (ub != end) {
                        STXXL_VARDUMP(*ub);
                    }
                }
                else {
                    // there is no min_max_value
                    sizes[i] = std::distance(begin, end);
                    sequences[i] = std::make_pair(begin, end);
                }

                if (!c_limit_extract_buffer) { // otherwise see below...
                    // remove elements
                    m_internal_arrays[j].inc_min(sizes[i]);
                    m_internal_size -= sizes[i];
                }
            }
        }

        size_type output_size = std::accumulate(sizes.begin(), sizes.end(), 0);

        if (c_limit_extract_buffer) {
            if (output_size > m_extract_buffer_limit) {
                output_size = m_extract_buffer_limit;
            }
        }

        m_stats.max_extract_buffer_size.set_max(output_size);
        m_stats.total_extract_buffer_size += output_size;

        assert(output_size > 0);
        m_extract_buffer.resize(output_size);
        m_extract_buffer_size = output_size;

        m_stats.refill_time_before_merge.stop();
        m_stats.refill_merge_time.start();

        STXXL_VARDUMP(output_size);

#if STXXL_PARALLEL
        parallel::sw_multiway_merge(sequences.begin(), sequences.end(),
                                    m_extract_buffer.begin(), m_inv_compare, output_size);
#else
        // TODO
#endif

        m_stats.refill_merge_time.stop();
        m_stats.refill_time_after_merge.start();

        size_t deleted_size = 0;

        // remove elements
        if (c_limit_extract_buffer) {
            for (size_type i = 0; i < eas + ias; ++i) {
                // dist represents the number of elements that haven't been merged
                size_type dist = std::distance(sequences[i].first, sequences[i].second);

                assert(dist <= sizes[i]);
                if (dist == sizes[i]) continue;

                deleted_size += sizes[i] - dist;

                if (i < eas) {
                    m_external_arrays[i].remove(sizes[i] - dist);
                    assert(m_external_size >= sizes[i] - dist);
                    m_external_size -= sizes[i] - dist;
                }
                else {
                    size_type j = i - eas;
                    m_internal_arrays[j].inc_min(sizes[i] - dist);
                    assert(m_internal_size >= sizes[i] - dist);
                    m_internal_size -= sizes[i] - dist;
                }
            }
        }
        else {
            for (size_type i = 0; i < eas + ias; ++i)
            {
                if (sizes[i] == 0) continue;

                deleted_size += sizes[i];

                if (i < eas) {
                    m_external_arrays[i].remove(sizes[i]);
                    assert(m_external_size >= sizes[i]);
                    m_external_size -= sizes[i];
                }
                // item from internal arrays have already been removed
            }
        }

        assert(deleted_size == output_size);

        //stats.refill_wait_time.start();
        for (size_type i = 0; i < eas; ++i) {
            m_external_arrays[i].wait();
        }
        //stats.refill_wait_time.stop();

        // remove empty arrays - important for the next round
        m_external_arrays.erase(stxxl::swap_remove_if(m_external_arrays.begin(), m_external_arrays.end(), empty_external_array_eraser()), m_external_arrays.end());
        size_type num_deleted_arrays = eas - m_external_arrays.size();
        if (num_deleted_arrays > 0) {
            m_mem_left += num_deleted_arrays * m_mem_per_external_array;
        }

        m_stats.num_new_external_arrays = 0;

        if (c_merge_ias_into_eb)
            cleanup_internal_arrays();

        m_minima.update_extract_buffer();

        m_stats.refill_time_after_merge.stop();
        m_stats.refill_extract_buffer_time.stop();

        check_invariants();
    }

    //! Flushes the insertions heap id into an internal array.
    inline void flush_insertion_heap(unsigned_type id)
    {
        assert(m_proc[id].insertion_heap.size() != 0);

        heap_type& insheap = m_proc[id].insertion_heap;
        size_t size = insheap.size();

        STXXL_VERBOSE2_PPQ(
            "Flushing insertion heap array id=" << id <<
            " size=" << insheap.size() <<
            " capacity=" << insheap.capacity() <<
            " int_memory=" << insheap.capacity() * sizeof(value_type) <<
            " mem_left=" << m_mem_left);

        m_stats.num_insertion_heap_flushes++;
        stats_timer flush_time(true); // separate timer due to parallel sorting

        // sort locally, independent of others
        std::sort(insheap.begin(), insheap.end(), m_inv_compare);

#pragma omp critical
        {
            // test that enough RAM is available for merged internal array:
            // otherwise flush the existing internal arrays out to disk.
            if (m_mem_left < insertion_heap_int_memory()) {
                if (m_internal_size > 0) {
                    flush_internal_arrays();
                    // still not enough?
                    if (m_mem_left < insertion_heap_int_memory())
                        merge_external_arrays();
                }
                else {
                    merge_external_arrays();
                }
            }

            internal_array_type temp_array(insheap);
            assert(temp_array.int_memory() == size * sizeof(value_type));
            m_internal_arrays.swap_back(temp_array);
            // insheap is empty now, insheap vector was swapped into temp_array.

            if (c_merge_ias_into_eb) {
                if (!extract_buffer_empty()) {
                    m_stats.num_new_internal_arrays++;
                    m_stats.max_num_new_internal_arrays.set_max(m_stats.num_new_internal_arrays);
                    m_minima.add_internal_array(
                        static_cast<unsigned>(m_internal_arrays.size()) - 1
                        );
                }
            }
            else {
                m_minima.add_internal_array(
                    static_cast<unsigned>(m_internal_arrays.size()) - 1
                    );
            }

            // reserve new insertion heap
            insheap.reserve(m_insertion_heap_capacity);
            assert(insheap.capacity() * sizeof(value_type)
                   == insertion_heap_int_memory());

#pragma omp atomic
            m_mem_left -= insertion_heap_int_memory();

            // update item counts
#pragma omp atomic
            m_heaps_size -= size;
            m_internal_size += size;

            // invalidate player in minima tree
            m_minima.deactivate_heap(id);
        }

        m_stats.max_num_internal_arrays.set_max(m_internal_arrays.size());
        m_stats.insertion_heap_flush_time += flush_time;
    }

    //! Flushes the insertions heaps into an internal array.
    inline void flush_insertion_heaps()
    {
        size_type ram_needed;

        if (c_merge_sorted_heaps) {
            ram_needed = m_mem_for_heaps;
        }
        else {
            ram_needed = insertion_heap_int_memory();
        }

        // test that enough RAM is available for merged internal array:
        // otherwise flush the existing internal arrays out to disk.
        if (m_mem_left < ram_needed) {
            if (m_internal_size > 0) {
                flush_internal_arrays();
                // still not enough?
                if (m_mem_left < ram_needed)
                    merge_external_arrays();
            }
            else {
                merge_external_arrays();
            }
        }

        m_stats.num_insertion_heap_flushes++;
        m_stats.insertion_heap_flush_time.start();

        size_type size = m_heaps_size;
        size_type int_memory = 0;
        assert(size > 0);
        std::vector<std::pair<value_iterator, value_iterator> > sequences(m_num_insertion_heaps);

#if STXXL_PARALLEL
        #pragma omp parallel for
#endif
        for (unsigned i = 0; i < m_num_insertion_heaps; ++i)
        {
            heap_type& insheap = m_proc[i].insertion_heap;

            std::sort(insheap.begin(), insheap.end(), m_inv_compare);

            if (c_merge_sorted_heaps)
                sequences[i] = std::make_pair(insheap.begin(), insheap.end());

            int_memory += insheap.capacity();
        }

        if (c_merge_sorted_heaps && 0)
        {
            m_stats.merge_sorted_heaps_time.start();
            std::vector<ValueType> merged_array(size);

#if STXXL_PARALLEL
            parallel::sw_multiway_merge(sequences.begin(), sequences.end(),
                                        merged_array.begin(), m_inv_compare, size);
#else
            // TODO
#endif

            m_stats.merge_sorted_heaps_time.stop();

            internal_array_type temp_array(merged_array);
            assert(temp_array.int_memory() == size * sizeof(value_type));
            m_internal_arrays.swap_back(temp_array);
            // merged_array is empty now.

            if (c_merge_ias_into_eb) {
                if (!extract_buffer_empty()) {
                    m_stats.num_new_internal_arrays++;
                    m_stats.max_num_new_internal_arrays.set_max(m_stats.num_new_internal_arrays);
                    m_minima.add_internal_array(static_cast<unsigned>(m_internal_arrays.size()) - 1);
                }
            }
            else {
                m_minima.add_internal_array(static_cast<unsigned>(m_internal_arrays.size()) - 1);
            }

            for (unsigned i = 0; i < m_num_insertion_heaps; ++i)
            {
                m_proc[i].insertion_heap.clear();
                m_proc[i].insertion_heap.reserve(m_insertion_heap_capacity);
            }
            m_minima.clear_heaps();

            m_mem_left -= size * sizeof(value_type);
        }
        else
        {
            for (unsigned i = 0; i < m_num_insertion_heaps; ++i)
            {
                heap_type& insheap = m_proc[i].insertion_heap;
                size_type insheap_capacity = insheap.capacity() * sizeof(value_type);

                if (insheap.size() > 0)
                {
                    internal_array_type temp_array(insheap);
                    assert(temp_array.int_memory() == insheap_capacity);
                    m_internal_arrays.swap_back(temp_array);
                    // insheap is empty now, insheap vector was swapped into temp_array.

                    if (c_merge_ias_into_eb) {
                        if (!extract_buffer_empty()) {
                            m_stats.num_new_internal_arrays++;
                            m_stats.max_num_new_internal_arrays.set_max(m_stats.num_new_internal_arrays);
                            m_minima.add_internal_array(static_cast<unsigned>(m_internal_arrays.size()) - 1);
                        }
                    }
                    else {
                        m_minima.add_internal_array(static_cast<unsigned>(m_internal_arrays.size()) - 1);
                    }

                    insheap.reserve(m_insertion_heap_capacity);
                }
            }

            m_minima.clear_heaps();

            m_mem_left -= m_num_insertion_heaps * insertion_heap_int_memory();
        }

        m_internal_size += size;
        m_heaps_size = 0;

        m_stats.max_num_internal_arrays.set_max(m_internal_arrays.size());
        m_stats.insertion_heap_flush_time.stop();

        check_invariants();
    }

    //! Flushes the internal arrays into an external array.
    inline void flush_internal_arrays()
    {
        STXXL_VERBOSE1_PPQ("Flushing internal arrays into external memory");

        m_stats.num_internal_array_flushes++;
        m_stats.internal_array_flush_time.start();

        m_minima.clear_internal_arrays();

        // clean up internal arrays that have been deleted in extract_min!
        cleanup_internal_arrays();

        size_type num_arrays = m_internal_arrays.size();
        size_type size = m_internal_size;
        size_type int_memory = 0;
        std::vector<std::pair<iterator, iterator> > sequences(num_arrays);

        for (unsigned i = 0; i < num_arrays; ++i)
        {
            sequences[i] = std::make_pair(m_internal_arrays[i].begin(),
                                          m_internal_arrays[i].end());

            int_memory += m_internal_arrays[i].int_memory();
        }

        external_array_type temp_array(size, m_num_prefetchers, m_num_write_buffers);
        m_external_arrays.swap_back(temp_array);
        external_array_type& a = m_external_arrays[m_external_arrays.size() - 1];

        // TODO: write in chunks in order to safe RAM?

        m_stats.max_merge_buffer_size.set_max(size);

        a.request_write_buffer(size);

#if STXXL_PARALLEL
        parallel::sw_multiway_merge(sequences.begin(), sequences.end(),
                                    a.begin(), m_inv_compare, size);
#else
        // TODO
#endif

        a.flush_write_buffer();
        a.finish_write_phase();

        STXXL_VERBOSE1_PPQ("Merge done");

        m_internal_size = 0;
        m_external_size += size;

        m_internal_arrays.clear();
        m_stats.num_new_internal_arrays = 0;

        if (!extract_buffer_empty()) {
            m_stats.num_new_external_arrays++;
            m_stats.max_num_new_external_arrays.set_max(m_stats.num_new_external_arrays);
            a.wait();
            m_minima.add_external_array(static_cast<unsigned>(m_external_arrays.size()) - 1);
        }

        m_mem_left += int_memory;
        m_mem_left -= m_mem_per_external_array;

        m_stats.max_num_external_arrays.set_max(m_external_arrays.size());
        m_stats.internal_array_flush_time.stop();

        STXXL_VERBOSE1_PPQ("Write done");
    }

    //! Flushes the insertion heaps into an external array.
    void flush_directly_to_hd()
    {
        if (m_mem_left < m_mem_per_external_array) {
            merge_external_arrays();
        }

        m_stats.num_direct_flushes++;
        m_stats.direct_flush_time.start();

        size_type size = m_heaps_size;
        std::vector<std::pair<value_iterator, value_iterator> > sequences(m_num_insertion_heaps);

#if STXXL_PARALLEL
        #pragma omp parallel for
#endif
        for (unsigned i = 0; i < m_num_insertion_heaps; ++i)
        {
            heap_type& insheap = m_proc[i].insertion_heap;
            // TODO std::sort_heap? We would have to reverse the order...
            std::sort(insheap.begin(), insheap.end(), m_inv_compare);
            sequences[i] = std::make_pair(insheap.begin(), insheap.end());
        }

        external_array_type temp_array(size, m_num_prefetchers, m_num_write_buffers);
        m_external_arrays.swap_back(temp_array);
        external_array_type& a = m_external_arrays[m_external_arrays.size() - 1];

        // TODO: write in chunks in order to safe RAM?
        a.request_write_buffer(size);

#if STXXL_PARALLEL
        parallel::sw_multiway_merge(sequences.begin(), sequences.end(),
                                    a.begin(), m_inv_compare, size);
#else
        // TODO
#endif

        a.flush_write_buffer();
        a.finish_write_phase();

        m_external_size += size;
        m_heaps_size = 0;

        // inefficient...
        //#if STXXL_PARALLEL
        //#pragma omp parallel for
        //#endif
        for (unsigned i = 0; i < m_num_insertion_heaps; ++i) {
            m_proc[i].insertion_heap.clear();
            m_proc[i].insertion_heap.reserve(m_insertion_heap_capacity);
        }
        m_minima.clear_heaps();

        if (!extract_buffer_empty()) {
            m_stats.num_new_external_arrays++;
            m_stats.max_num_new_external_arrays.set_max(m_stats.num_new_external_arrays);
            a.wait();
            m_minima.add_external_array(static_cast<unsigned>(m_external_arrays.size()) - 1);
        }

        //TODO m_mem_left -= m_mem_per_external_array;
        STXXL_CHECK(0);

        m_stats.max_num_external_arrays.set_max(m_external_arrays.size());
        m_stats.direct_flush_time.stop();

        check_invariants();
    }

    //! Sorts the values from values and writes them into an external array.
    //! \param values the vector to sort and store
    void flush_array_to_hd(std::vector<ValueType>& values)
    {
#if STXXL_PARALLEL
        __gnu_parallel::sort(values.begin(), values.end(), m_inv_compare);
#else
        std::sort(values.begin(), values.end(), m_inv_compare);
#endif

        external_array_type temp_array(values.size(), m_num_prefetchers, m_num_write_buffers);
        m_external_arrays.swap_back(temp_array);
        external_array_type& a = m_external_arrays[m_external_arrays.size() - 1];

        for (value_iterator i = values.begin(); i != values.end(); ++i) {
            a.push_back(*i);
        }

        a.finish_write_phase();

        m_external_size += values.size();

        if (!extract_buffer_empty()) {
            m_stats.num_new_external_arrays++;
            m_stats.max_num_new_external_arrays.set_max(m_stats.num_new_external_arrays);
            a.wait();
            m_minima.add_external_array(static_cast<unsigned>(m_external_arrays.size()) - 1);
        }

        //TODO m_mem_left -= m_mem_per_external_array;
        STXXL_CHECK(0);

        m_stats.max_num_external_arrays.set_max(m_external_arrays.size());

        check_invariants();
    }

    /*!
     * Sorts the values from values and writes them into an internal array.
     * Don't use the value vector afterwards!
     *
     * \param values the vector to sort and store
     */
    void flush_array_internal(std::vector<ValueType>& values)
    {
        m_internal_size += values.size();

#if STXXL_PARALLEL
        __gnu_parallel::sort(values.begin(), values.end(), m_inv_compare);
#else
        std::sort(values.begin(), values.end(), m_inv_compare);
#endif

        internal_array_type temp_array(values);
        m_internal_arrays.swap_back(temp_array);
        // values is now empty.

        if (c_merge_ias_into_eb) {
            if (!extract_buffer_empty()) {
                m_stats.num_new_internal_arrays++;
                m_stats.max_num_new_internal_arrays.set_max(m_stats.num_new_internal_arrays);
                m_minima.add_internal_array(static_cast<unsigned>(m_internal_arrays.size()) - 1);
            }
        }
        else {
            m_minima.add_internal_array(static_cast<unsigned>(m_internal_arrays.size()) - 1);
        }

        // TODO: use real value size: ram_left -= 2*values->size()*sizeof(ValueType);
        //TODO m_mem_left -= m_mem_per_internal_array;
        STXXL_CHECK(0);

        m_stats.max_num_internal_arrays.set_max(m_internal_arrays.size());

        // Vector is now owned by PPQ...
    }

    /*!
     * Lets the priority queue decide if flush_array_to_hd() or
     * flush_array_internal() should be called.  Don't use the value vector
     * afterwards!
     *
     * \param values the vector to sort and store
     */
    void flush_array(std::vector<ValueType>& values)
    {
        size_type size = values.size();
        size_type ram_internal = 2 * size * sizeof(ValueType); // ram for the sorted array + part of the ram for the merge buffer

        size_type ram_for_all_internal_arrays = m_mem_total - 2 * m_mem_for_heaps - m_num_write_buffers * block_size - m_external_arrays.size() * m_mem_per_external_array;

        if (ram_internal > ram_for_all_internal_arrays) {
            flush_array_to_hd(values);
            return;
        }

        if (m_mem_left < ram_internal) {
            flush_internal_arrays();
        }

        if (m_mem_left < ram_internal) {
            if (m_mem_left < m_mem_per_external_array) {
                merge_external_arrays();
            }

            flush_array_to_hd(values);
            return;
        }

        flush_array_internal(values);
    }

    //! Struct of all statistical counters and timers.  Turn on/off statistics
    //! using the stats_counter and stats_timer typedefs.
    struct stats_type
    {
        //! Largest number of elements in the extract buffer at the same time
        stats_counter max_extract_buffer_size;

        //! Sum of the sizes of each extract buffer refill. Used for average
        //! size.
        stats_counter total_extract_buffer_size;

        //! Largest number of elements in the merge buffer when running
        //! flush_internal_arrays()
        stats_counter max_merge_buffer_size;

        //! Total number of extracts
        stats_counter num_extracts;

        //! Number of refill_extract_buffer() calls
        stats_counter num_extract_buffer_refills;

        //! Number of flush_insertion_heaps() calls
        stats_counter num_insertion_heap_flushes;

        //! Number of flush_directly_to_hd() calls
        stats_counter num_direct_flushes;

        //! Number of flush_internal_arrays() calls
        stats_counter num_internal_array_flushes;

        //! Number of merge_external_arrays() calls
        stats_counter num_external_array_merges;

        //! Largest number of internal arrays at the same time
        stats_counter max_num_internal_arrays;

        //! Largest number of external arrays at the same time
        stats_counter max_num_external_arrays;

        //! Temporary number of new external arrays at the same time (which
        //! were created while the extract buffer hadn't been empty)
        stats_counter num_new_external_arrays;

        //! Largest number of new external arrays at the same time (which were
        //! created while the extract buffer hadn't been empty)
        stats_counter max_num_new_external_arrays;

        //if (c_merge_ias_into_eb) {

        //! Temporary number of new internal arrays at the same time (which
        //! were created while the extract buffer hadn't been empty)
        stats_counter num_new_internal_arrays;

        //! Largest number of new internal arrays at the same time (which were
        //! created while the extract buffer hadn't been empty)
        stats_counter max_num_new_internal_arrays;

        //}

        //! Total time for flush_insertion_heaps()
        stats_timer insertion_heap_flush_time;

        //! Total time for flush_directly_to_hd()
        stats_timer direct_flush_time;

        //! Total time for flush_internal_arrays()
        stats_timer internal_array_flush_time;

        //! Total time for merge_external_arrays()
        stats_timer external_array_merge_time;

        //! Total time for extract_min()
        stats_timer extract_min_time;

        //! Total time for refill_extract_buffer()
        stats_timer refill_extract_buffer_time;

        //! Total time for the merging in refill_extract_buffer()
        //! Part of refill_extract_buffer_time.
        stats_timer refill_merge_time;

        //! Total time for all things before merging in refill_extract_buffer()
        //! Part of refill_extract_buffer_time.
        stats_timer refill_time_before_merge;

        //! Total time for all things after merging in refill_extract_buffer()
        //! Part of refill_extract_buffer_time.
        stats_timer refill_time_after_merge;

        //! Total time of wait() calls in first part of
        //! refill_extract_buffer(). Part of refill_time_before_merge and
        //! refill_extract_buffer_time.
        stats_timer refill_wait_time;

        //! Total time for pop_heap() in extract_min().
        //! Part of extract_min_time.
        stats_timer pop_heap_time;

        //! Total time for merging the sorted heaps.
        //! Part of flush_insertion_heaps.
        stats_timer merge_sorted_heaps_time;

        //! Total time for std::upper_bound calls in refill_extract_buffer()
        //! Part of refill_extract_buffer_time and refill_time_before_merge.
        // stats_timer refill_upper_bound_time;

        //! Total time for std::accumulate calls in refill_extract_buffer()
        //! Part of refill_extract_buffer_time and refill_time_before_merge.
        stats_timer refill_accumulate_time;

        //! Total time for determining the smallest max value in refill_extract_buffer()
        //! Part of refill_extract_buffer_time and refill_time_before_merge.
        stats_timer refill_minmax_time;

        friend std::ostream& operator << (std::ostream& os, const stats_type& o)
        {
            return os << "max_extract_buffer_size=" << o.max_extract_buffer_size.as_memory_amount(sizeof(ValueType)) << std::endl
                      << "total_extract_buffer_size=" << o.total_extract_buffer_size.as_memory_amount(sizeof(ValueType)) << std::endl
                      << "max_merge_buffer_size=" << o.max_merge_buffer_size.as_memory_amount(sizeof(ValueType)) << std::endl
                      << "num_extracts=" << o.num_extracts << std::endl
                      << "num_extract_buffer_refills=" << o.num_extract_buffer_refills << std::endl
                      << "num_insertion_heap_flushes=" << o.num_insertion_heap_flushes << std::endl
                      << "num_direct_flushes=" << o.num_direct_flushes << std::endl
                      << "num_internal_array_flushes=" << o.num_internal_array_flushes << std::endl
                      << "num_external_array_merges=" << o.num_external_array_merges << std::endl
                      << "max_num_internal_arrays=" << o.max_num_internal_arrays << std::endl
                      << "max_num_external_arrays=" << o.max_num_external_arrays << std::endl
                      << "num_new_external_arrays=" << o.num_new_external_arrays << std::endl
                      << "max_num_new_external_arrays=" << o.max_num_new_external_arrays << std::endl
                   //if (c_merge_ias_into_eb) {
                      << "num_new_internal_arrays=" << o.num_new_internal_arrays << std::endl
                      << "max_num_new_internal_arrays=" << o.max_num_new_internal_arrays << std::endl
                   //}
                      << "insertion_heap_flush_time=" << o.insertion_heap_flush_time << std::endl
                      << "direct_flush_time=" << o.direct_flush_time << std::endl
                      << "internal_array_flush_time=" << o.internal_array_flush_time << std::endl
                      << "external_array_merge_time=" << o.external_array_merge_time << std::endl
                      << "extract_min_time=" << o.extract_min_time << std::endl
                      << "refill_extract_buffer_time=" << o.refill_extract_buffer_time << std::endl
                      << "refill_merge_time=" << o.refill_merge_time << std::endl
                      << "refill_time_before_merge=" << o.refill_time_before_merge << std::endl
                      << "refill_time_after_merge=" << o.refill_time_after_merge << std::endl
                      << "refill_wait_time=" << o.refill_wait_time << std::endl
                      << "pop_heap_time=" << o.pop_heap_time << std::endl
                      << "merge_sorted_heaps_time=" << o.merge_sorted_heaps_time << std::endl
                   // << "refill_upper_bound_time=" << o.refill_upper_bound_time << std::endl
                      << "refill_accumulate_time=" << o.refill_accumulate_time << std::endl
                      << "refill_minmax_time=" << o.refill_minmax_time << std::endl;
        }
    };

    stats_type m_stats;
};

// For C++98 compatibility:
template <
    class ValueType,
    class CompareType,
    class AllocStrategy,
    uint64 BlockSize,
    uint64 DefaultMemSize,
    uint64 MaxItems
    >
const double parallel_priority_queue<ValueType, CompareType, AllocStrategy, BlockSize,
                                     DefaultMemSize, MaxItems>::c_default_extract_buffer_ram_part = 0.05;

STXXL_END_NAMESPACE

#endif // !STXXL_CONTAINERS_PARALLEL_PRIORITY_QUEUE_HEADER
