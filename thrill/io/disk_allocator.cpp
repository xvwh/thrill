/*******************************************************************************
 * thrill/io/disk_allocator.cpp
 *
 * Copied and modified from STXXL https://github.com/stxxl/stxxl, which is
 * distributed under the Boost Software License, Version 1.0.
 *
 * Part of Project Thrill - http://project-thrill.org
 *
 * Copyright (C) 2002 Roman Dementiev <dementiev@mpi-sb.mpg.de>
 * Copyright (C) 2008, 2010 Andreas Beckmann <beckmann@cs.uni-frankfurt.de>
 * Copyright (C) 2015-2016 Timo Bingmann <tb@panthema.net>
 *
 * All rights reserved. Published under the BSD-2 license in the LICENSE file.
 ******************************************************************************/

#include <thrill/io/disk_allocator.hpp>
#include <thrill/io/error_handling.hpp>

#include <algorithm>
#include <cassert>
#include <map>
#include <ostream>
#include <utility>
#include <vector>

namespace thrill {
namespace io {

void DiskAllocator::dump() const {
    int64_t total = 0;
    SortSeq::const_iterator cur = free_space_.begin();
    LOG1 << "Free regions dump:";
    for ( ; cur != free_space_.end(); ++cur)
    {
        LOG1 << "Free chunk: begin: " << cur->first << " size: " << cur->second;
        total += cur->second;
    }
    LOG1 << "Total bytes: " << total;
}

template <typename BidIterator>
void DiskAllocator::new_blocks(BidIterator begin, BidIterator end) {
    uint64_t requested_size = 0;
    static constexpr bool debug = false;

    size_t block_size = 0;

    for (BidIterator cur = begin; cur != end; ++cur)
    {
        LOG << "Asking for a block with size: " << cur->size;
        requested_size += cur->size;
        block_size = std::max<size_t>(block_size, cur->size);
    }

    std::unique_lock<std::mutex> lock(mutex_);

    LOG << "disk_allocator::new_blocks<>"
        << ", free:" << free_bytes_ << " total:" << disk_bytes_
        << ", blocks: " << (end - begin)
        << " begin: " << static_cast<void*>(&(*begin))
        << " end: " << static_cast<void*>(&(*end))
        << ", requested_size=" << requested_size;

    if (free_bytes_ < (int64_t)requested_size)
    {
        if (!autogrow_) {
            THRILL_THROW(BadExternalAlloc,
                         "Out of external memory error: " << requested_size <<
                         " requested, " << free_bytes_ << " bytes free. "
                         "Maybe enable autogrow flags?");
        }

        LOG << "External memory block allocation error: " << requested_size <<
            " bytes requested, " << free_bytes_ <<
            " bytes free. Trying to extend the external memory space...";

        grow_file(requested_size);
    }

    // dump();

    SortSeq::iterator space;
    space = std::find_if(free_space_.begin(), free_space_.end(),
                         bind2nd(FirstFit(), requested_size));

    if (space == free_space_.end() && requested_size == block_size)
    {
        assert(end - begin == 1);

        if (!autogrow_) {
            LOG1 << "Warning: Severe external memory space fragmentation!";
            dump();

            LOG1 << "External memory block allocation error: " << requested_size
                 << " bytes requested, " << free_bytes_
                 << " bytes free. Trying to extend the external memory space...";
        }

        grow_file(block_size);

        space = std::find_if(free_space_.begin(), free_space_.end(),
                             bind2nd(FirstFit(), requested_size));
    }

    if (space != free_space_.end())
    {
        int64_t region_pos = (*space).first;
        int64_t region_size = (*space).second;
        free_space_.erase(space);
        if (region_size > (int64_t)requested_size)
            free_space_[region_pos + requested_size] = region_size - requested_size;

        for (int64_t pos = region_pos; begin != end; ++begin)
        {
            begin->offset = pos;
            pos += begin->size;
        }
        free_bytes_ -= requested_size;
        // dump();

        return;
    }

    // no contiguous region found
    LOG1 << "Warning, when allocating an external memory space, no contiguous region found";
    LOG1 << "It might harm the performance";

    assert(requested_size > block_size);
    assert(end - begin > 1);

    lock.unlock();

    BidIterator middle = begin + ((end - begin) / 2);
    new_blocks(begin, middle);
    new_blocks(middle, end);
}

// template function instantiations
template void DiskAllocator::new_blocks(BID<0>* begin, BID<0>* end);
template void DiskAllocator::new_blocks(
    std::vector<BID<0> >::iterator begin, std::vector<BID<0> >::iterator end);

template void DiskAllocator::new_blocks(
    std::vector<BID<131072> >::iterator begin,
    std::vector<BID<131072> >::iterator end);
template void DiskAllocator::new_blocks(
    std::vector<BID<524288> >::iterator begin,
    std::vector<BID<524288> >::iterator end);

template <size_t BlockSize>
void DiskAllocator::delete_block(const BID<BlockSize>& bid) {
    std::unique_lock<std::mutex> lock(mutex_);

    LOG << "disk_allocator::delete_block<" << BlockSize
        << ">(pos=" << bid.offset << ", size=" << bid.size
        << "), free:" << free_bytes_ << " total:" << disk_bytes_;

    add_free_region(bid.offset, bid.size);
}

// template function instantiations
template void DiskAllocator::delete_block(const BID<0>& bid);
template void DiskAllocator::delete_block(const BID<131072>& bid);
template void DiskAllocator::delete_block(const BID<524288>& bid);

void DiskAllocator::deallocation_error(
    int64_t block_pos, int64_t block_size,
    const SortSeq::iterator& pred, const SortSeq::iterator& succ) const {
    LOG1 << "Error deallocating block at " << block_pos << " size " << block_size;
    LOG1 << ((pred == succ) ? "pred==succ" : "pred!=succ");
    if (pred == free_space_.end()) {
        LOG1 << "pred==free_space.end()";
    }
    else {
        if (pred == free_space_.begin())
            LOG1 << "pred==free_space.begin()";
        LOG1 << "pred: begin=" << pred->first << " size=" << pred->second;
    }
    if (succ == free_space_.end()) {
        LOG1 << "succ==free_space.end()";
    }
    else {
        if (succ == free_space_.begin())
            LOG1 << "succ==free_space.begin()";
        LOG1 << "succ: begin=" << succ->first << " size=" << succ->second;
    }
    dump();
}

void DiskAllocator::add_free_region(int64_t block_pos, int64_t block_size) {
    // assert(block_size);
    // dump();
    LOG << "Deallocating a block with size: " << block_size << " position: " << block_pos;
    int64_t region_pos = block_pos;
    int64_t region_size = block_size;
    if (!free_space_.empty())
    {
        SortSeq::iterator succ = free_space_.upper_bound(region_pos);
        SortSeq::iterator pred = succ;
        if (pred != free_space_.begin())
            pred--;
        if (pred != free_space_.end())
        {
            if (pred->first <= region_pos && pred->first + pred->second > region_pos)
            {
                THRILL_THROW2(BadExternalAlloc,
                              "disk_allocator::check_corruption", "Error: double deallocation of external memory, trying to deallocate region " << region_pos << " + " << region_size << "  in empty space [" << pred->first << " + " << pred->second << "]");
            }
        }
        if (succ != free_space_.end())
        {
            if (region_pos <= succ->first && region_pos + region_size > succ->first)
            {
                THRILL_THROW2(BadExternalAlloc, "disk_allocator::check_corruption", "Error: double deallocation of external memory, trying to deallocate region " << region_pos << " + " << region_size << "  which overlaps empty space [" << succ->first << " + " << succ->second << "]");
            }
        }
        if (succ == free_space_.end())
        {
            if (pred == free_space_.end())
            {
                deallocation_error(block_pos, block_size, pred, succ);
                assert(pred != free_space_.end());
            }
            if ((*pred).first + (*pred).second == region_pos)
            {
                // coalesce with predecessor
                region_size += (*pred).second;
                region_pos = (*pred).first;
                free_space_.erase(pred);
            }
        }
        else
        {
            if (free_space_.size() > 1)
            {
#if 0
                if (pred == succ)
                {
                    deallocation_error(block_pos, block_size, pred, succ);
                    assert(pred != succ);
                }
#endif
                bool succ_is_not_the_first = (succ != free_space_.begin());
                if ((*succ).first == region_pos + region_size)
                {
                    // coalesce with successor
                    region_size += (*succ).second;
                    free_space_.erase(succ);
                    //-tb: set succ to pred afterwards due to iterator invalidation
                    succ = pred;
                }
                if (succ_is_not_the_first)
                {
                    if (pred == free_space_.end())
                    {
                        deallocation_error(block_pos, block_size, pred, succ);
                        assert(pred != free_space_.end());
                    }
                    if ((*pred).first + (*pred).second == region_pos)
                    {
                        // coalesce with predecessor
                        region_size += (*pred).second;
                        region_pos = (*pred).first;
                        free_space_.erase(pred);
                    }
                }
            }
            else
            {
                if ((*succ).first == region_pos + region_size)
                {
                    // coalesce with successor
                    region_size += (*succ).second;
                    free_space_.erase(succ);
                }
            }
        }
    }

    free_space_[region_pos] = region_size;
    free_bytes_ += block_size;

    // dump();
}

} // namespace io
} // namespace thrill

/******************************************************************************/
