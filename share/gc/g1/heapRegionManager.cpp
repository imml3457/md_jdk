/*
 * Copyright (c) 2001, 2020, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/g1/g1Arguments.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentRefine.hpp"
#include "gc/g1/g1NUMAStats.hpp"
#include "gc/g1/heapRegion.hpp"
#include "gc/g1/heapRegionManager.inline.hpp"
#include "gc/g1/heapRegionSet.inline.hpp"
#include "gc/g1/heterogeneousHeapRegionManager.hpp"
#include "jfr/jfrEvents.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.hpp"
#include "runtime/atomic.hpp"
#include "runtime/orderAccess.hpp"
#include "utilities/bitMap.inline.hpp"
#include "utilities/g1SendMsg.hpp"


class MasterFreeRegionListChecker : public HeapRegionSetChecker {
public:
  void check_mt_safety() {
    // Master Free List MT safety protocol:
    // (a) If we're at a safepoint, operations on the master free list
    // should be invoked by either the VM thread (which will serialize
    // them) or by the GC workers while holding the
    // FreeList_lock.
    // (b) If we're not at a safepoint, operations on the master free
    // list should be invoked while holding the Heap_lock.

    if (SafepointSynchronize::is_at_safepoint()) {
      guarantee(Thread::current()->is_VM_thread() ||
                FreeList_lock->owned_by_self(), "master free list MT safety protocol at a safepoint");
    } else {
      guarantee(Heap_lock->owned_by_self(), "master free list MT safety protocol outside a safepoint");
    }
  }
  bool is_correct_type(HeapRegion* hr) { return hr->is_free(); }
  const char* get_description() { return "Free Regions"; }
};

HeapRegionRange::HeapRegionRange(uint start, uint end) : _start(start), _end(end) {
  assert(start <= end, "Invariant");
}

HeapRegionManager::HeapRegionManager() :
  _bot_mapper(NULL),
  _cardtable_mapper(NULL),
  _card_counts_mapper(NULL),
  _available_map(mtGC),
  _num_committed(0),
  _allocated_heapregions_length(0),
  _regions(), _heap_mapper(NULL),
  _prev_bitmap_mapper(NULL),
  _next_bitmap_mapper(NULL),
  _free_list("Free list", new MasterFreeRegionListChecker())
{ }

HeapRegionManager* HeapRegionManager::create_manager(G1CollectedHeap* heap) {
  if (G1Arguments::is_heterogeneous_heap()) {
    return new HeterogeneousHeapRegionManager((uint)(G1Arguments::heap_max_size_bytes() / HeapRegion::GrainBytes) /*heap size as num of regions*/);
  }
  return new HeapRegionManager();
}

void HeapRegionManager::initialize(G1RegionToSpaceMapper* heap_storage,
                               G1RegionToSpaceMapper* prev_bitmap,
                               G1RegionToSpaceMapper* next_bitmap,
                               G1RegionToSpaceMapper* bot,
                               G1RegionToSpaceMapper* cardtable,
                               G1RegionToSpaceMapper* card_counts) {
  _allocated_heapregions_length = 0;

  _heap_mapper = heap_storage;

  _prev_bitmap_mapper = prev_bitmap;
  _next_bitmap_mapper = next_bitmap;

  _bot_mapper = bot;
  _cardtable_mapper = cardtable;

  _card_counts_mapper = card_counts;

  _regions.initialize(heap_storage->reserved(), HeapRegion::GrainBytes);

  _available_map.initialize(_regions.length());
}

bool HeapRegionManager::is_available(uint region) const {
  return _available_map.at(region);
}

HeapRegion* HeapRegionManager::allocate_free_region(HeapRegionType type, uint requested_node_index) {
  HeapRegion* hr = NULL;
  bool from_head = !type.is_young();
  G1NUMA* numa = G1NUMA::numa();

  if (requested_node_index != G1NUMA::AnyNodeIndex && numa->is_enabled()) {
    // Try to allocate with requested node index.
    hr = _free_list.remove_region_with_node_index(from_head, requested_node_index);
  }

  if (hr == NULL) {
    // If there's a single active node or we did not get a region from our requested node,
    // try without requested node index.
    hr = _free_list.remove_region(from_head);
  }

  if (hr != NULL) {
    assert(hr->next() == NULL, "Single region should not have next");
    assert(is_available(hr->hrm_index()), "Must be committed");

    if (numa->is_enabled() && hr->node_index() < numa->num_active_nodes()) {
      numa->update_statistics(G1NUMAStats::NewRegionAlloc, requested_node_index, hr->node_index());
    }
    
//      msg.sendmsg(p2i(hr->bottom()), hr->capacity(), 0, 0);
  }
  return hr;
}

HeapRegion* HeapRegionManager::allocate_humongous_from_free_list(uint num_regions) {
  uint candidate = find_contiguous_in_free_list(num_regions);
  if (candidate == G1_NO_HRM_INDEX) {
    return NULL;
  }
  return allocate_free_regions_starting_at(candidate, num_regions);
}

HeapRegion* HeapRegionManager::allocate_humongous_allow_expand(uint num_regions) {
  uint candidate = find_contiguous_allow_expand(num_regions);
  if (candidate == G1_NO_HRM_INDEX) {
    return NULL;
  }
  expand_exact(candidate, num_regions, G1CollectedHeap::heap()->workers());
  return allocate_free_regions_starting_at(candidate, num_regions);
}

HeapRegion* HeapRegionManager::allocate_humongous(uint num_regions) {
  // Special case a single region to avoid expensive search.
  if (num_regions == 1) {
    return allocate_free_region(HeapRegionType::Humongous, G1NUMA::AnyNodeIndex);
  }
  return allocate_humongous_from_free_list(num_regions);
}

HeapRegion* HeapRegionManager::expand_and_allocate_humongous(uint num_regions) {
  return allocate_humongous_allow_expand(num_regions);
}

#ifdef ASSERT
bool HeapRegionManager::is_free(HeapRegion* hr) const {
  return _free_list.contains(hr);
}
#endif

HeapRegion* HeapRegionManager::new_heap_region(uint hrm_index) {
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  HeapWord* bottom = g1h->bottom_addr_for_region(hrm_index);
  MemRegion mr(bottom, bottom + HeapRegion::GrainWords);
  assert(reserved().contains(mr), "invariant");
  return g1h->new_heap_region(hrm_index, mr);
}

void HeapRegionManager::commit_regions(uint index, size_t num_regions, WorkGang* pretouch_gang) {
  guarantee(num_regions > 0, "Must commit more than zero regions");
  guarantee(_num_committed + num_regions <= max_length(), "Cannot commit more than the maximum amount of regions");

  _num_committed += (uint)num_regions;

  _heap_mapper->commit_regions(index, num_regions, pretouch_gang);

  // Also commit auxiliary data
  _prev_bitmap_mapper->commit_regions(index, num_regions, pretouch_gang);
  _next_bitmap_mapper->commit_regions(index, num_regions, pretouch_gang);

  _bot_mapper->commit_regions(index, num_regions, pretouch_gang);
  _cardtable_mapper->commit_regions(index, num_regions, pretouch_gang);

  _card_counts_mapper->commit_regions(index, num_regions, pretouch_gang);
}

void HeapRegionManager::uncommit_regions(uint start, size_t num_regions) {
  guarantee(num_regions >= 1, "Need to specify at least one region to uncommit, tried to uncommit zero regions at %u", start);
  guarantee(_num_committed >= num_regions, "pre-condition");

  // Reset node index to distinguish with committed regions.
  for (uint i = start; i < start + num_regions; i++) {
    at(i)->set_node_index(G1NUMA::UnknownNodeIndex);
  }

  // Print before uncommitting.
  if (G1CollectedHeap::heap()->hr_printer()->is_active()) {
    for (uint i = start; i < start + num_regions; i++) {
      HeapRegion* hr = at(i);
      G1CollectedHeap::heap()->hr_printer()->uncommit(hr);
    }
  }

  _num_committed -= (uint)num_regions;

  _available_map.par_clear_range(start, start + num_regions, BitMap::unknown_range);
  _heap_mapper->uncommit_regions(start, num_regions);

  // Also uncommit auxiliary data
  _prev_bitmap_mapper->uncommit_regions(start, num_regions);
  _next_bitmap_mapper->uncommit_regions(start, num_regions);

  _bot_mapper->uncommit_regions(start, num_regions);
  _cardtable_mapper->uncommit_regions(start, num_regions);

  _card_counts_mapper->uncommit_regions(start, num_regions);
}

void HeapRegionManager::make_regions_available(uint start, uint num_regions, WorkGang* pretouch_gang) {
  guarantee(num_regions > 0, "No point in calling this for zero regions");
  commit_regions(start, num_regions, pretouch_gang);
  for (uint i = start; i < start + num_regions; i++) {
    if (_regions.get_by_index(i) == NULL) {
      HeapRegion* new_hr = new_heap_region(i);
      OrderAccess::storestore();
      _regions.set_by_index(i, new_hr);
      _allocated_heapregions_length = MAX2(_allocated_heapregions_length, i + 1);
    }
  }

  _available_map.par_set_range(start, start + num_regions, BitMap::unknown_range);

  for (uint i = start; i < start + num_regions; i++) {
    assert(is_available(i), "Just made region %u available but is apparently not.", i);
    HeapRegion* hr = at(i);
    if (G1CollectedHeap::heap()->hr_printer()->is_active()) {
      G1CollectedHeap::heap()->hr_printer()->commit(hr);
    }

    hr->initialize();
    hr->set_node_index(G1NUMA::numa()->index_for_region(hr));
    insert_into_free_list(at(i));
  }
}

MemoryUsage HeapRegionManager::get_auxiliary_data_memory_usage() const {
  size_t used_sz =
    _prev_bitmap_mapper->committed_size() +
    _next_bitmap_mapper->committed_size() +
    _bot_mapper->committed_size() +
    _cardtable_mapper->committed_size() +
    _card_counts_mapper->committed_size();

  size_t committed_sz =
    _prev_bitmap_mapper->reserved_size() +
    _next_bitmap_mapper->reserved_size() +
    _bot_mapper->reserved_size() +
    _cardtable_mapper->reserved_size() +
    _card_counts_mapper->reserved_size();

  return MemoryUsage(0, used_sz, committed_sz, committed_sz);
}

uint HeapRegionManager::expand_by(uint num_regions, WorkGang* pretouch_workers) {
  return expand_at(0, num_regions, pretouch_workers);
}

uint HeapRegionManager::expand_at(uint start, uint num_regions, WorkGang* pretouch_workers) {
  if (num_regions == 0) {
    return 0;
  }

  uint offset = start;
  uint expanded = 0;

  do {
    HeapRegionRange regions = find_unavailable_from_idx(offset);
    if (regions.length() == 0) {
      // No more unavailable regions.
      break;
    }

    uint to_expand = MIN2(num_regions - expanded, regions.length());
    make_regions_available(regions.start(), to_expand, pretouch_workers);
    expanded += to_expand;
    offset = regions.end();
  } while (expanded < num_regions);

  verify_optional();
  return expanded;
}

void HeapRegionManager::expand_exact(uint start, uint num_regions, WorkGang* pretouch_workers) {
  assert(num_regions != 0, "Need to request at least one region");
  uint end = start + num_regions;

  for (uint i = start; i < end; i++) {
    if (!is_available(i)) {
      make_regions_available(i, 1, pretouch_workers);
    }
  }

  verify_optional();
}

uint HeapRegionManager::expand_on_preferred_node(uint preferred_index) {
  uint expand_candidate = UINT_MAX;
  for (uint i = 0; i < max_length(); i++) {
    if (is_available(i)) {
      // Already in use continue
      continue;
    }
    // Always save the candidate so we can expand later on.
    expand_candidate = i;
    if (is_on_preferred_index(expand_candidate, preferred_index)) {
      // We have found a candidate on the preffered node, break.
      break;
    }
  }

  if (expand_candidate == UINT_MAX) {
     // No regions left, expand failed.
    return 0;
  }

  expand_exact(expand_candidate, 1, NULL);
  return 1;
}

bool HeapRegionManager::is_on_preferred_index(uint region_index, uint preferred_node_index) {
  uint region_node_index = G1NUMA::numa()->preferred_node_index_for_index(region_index);
  return region_node_index == preferred_node_index;
}

#ifdef ASSERT
void HeapRegionManager::assert_contiguous_range(uint start, uint num_regions) {
  // General sanity check, regions found should either be available and empty
  // or not available so that we can make them available and use them.
  for (uint i = start; i < (start + num_regions); i++) {
    HeapRegion* hr = _regions.get_by_index(i);
    assert(!is_available(i) || hr->is_free(),
           "Found region sequence starting at " UINT32_FORMAT ", length " UINT32_FORMAT
           " that is not free at " UINT32_FORMAT ". Hr is " PTR_FORMAT ", type is %s",
           start, num_regions, i, p2i(hr), hr->get_type_str());
  }
}
#endif

uint HeapRegionManager::find_contiguous_in_range(uint start, uint end, uint num_regions) {
  assert(start <= end, "precondition");
  assert(num_regions >= 1, "precondition");
  uint candidate = start;       // First region in candidate sequence.
  uint unchecked = candidate;   // First unchecked region in candidate.
  // While the candidate sequence fits in the range...
  while (num_regions <= (end - candidate)) {
    // Walk backward over the regions for the current candidate.
    for (uint i = candidate + num_regions - 1; true; --i) {
      if (is_available(i) && !at(i)->is_free()) {
        // Region i can't be used, so restart with i+1 as the start
        // of a new candidate sequence, and with the region after the
        // old candidate sequence being the first unchecked region.
        unchecked = candidate + num_regions;
        candidate = i + 1;
        break;
      } else if (i == unchecked) {
        // All regions of candidate sequence have passed check.
        assert_contiguous_range(candidate, num_regions);
        return candidate;
      }
    }
  }
  return G1_NO_HRM_INDEX;
}

uint HeapRegionManager::find_contiguous_in_free_list(uint num_regions) {
  BitMap::idx_t range_start = 0;
  BitMap::idx_t range_end = range_start;
  uint candidate = G1_NO_HRM_INDEX;

  do {
    range_start = _available_map.get_next_one_offset(range_end);
    range_end = _available_map.get_next_zero_offset(range_start);
    candidate = find_contiguous_in_range((uint) range_start, (uint) range_end, num_regions);
  } while (candidate == G1_NO_HRM_INDEX && range_end < max_length());

  return candidate;
}

uint HeapRegionManager::find_contiguous_allow_expand(uint num_regions) {
  // Find any candidate.
  return find_contiguous_in_range(0, max_length(), num_regions);
}

HeapRegion* HeapRegionManager::next_region_in_heap(const HeapRegion* r) const {
  guarantee(r != NULL, "Start region must be a valid region");
  guarantee(is_available(r->hrm_index()), "Trying to iterate starting from region %u which is not in the heap", r->hrm_index());
  for (uint i = r->hrm_index() + 1; i < _allocated_heapregions_length; i++) {
    HeapRegion* hr = _regions.get_by_index(i);
    if (is_available(i)) {
      return hr;
    }
  }
  return NULL;
}

void HeapRegionManager::iterate(HeapRegionClosure* blk) const {
  uint len = max_length();

  for (uint i = 0; i < len; i++) {
    if (!is_available(i)) {
      continue;
    }
    guarantee(at(i) != NULL, "Tried to access region %u that has a NULL HeapRegion*", i);
    bool res = blk->do_heap_region(at(i));
    if (res) {
      blk->set_incomplete();
      return;
    }
  }
}

HeapRegionRange HeapRegionManager::find_unavailable_from_idx(uint index) const {
  guarantee(index <= max_length(), "checking");

  // Find first unavailable region from offset.
  BitMap::idx_t start = _available_map.get_next_zero_offset(index);
  if (start == _available_map.size()) {
    // No unavailable regions found.
    return HeapRegionRange(max_length(), max_length());
  }

  // The end of the range is the next available region.
  BitMap::idx_t end = _available_map.get_next_one_offset(start);

  assert(!_available_map.at(start), "Found region (" SIZE_FORMAT ") is not unavailable", start);
  assert(!_available_map.at(end - 1), "Last region (" SIZE_FORMAT ") in range is not unavailable", end - 1);
  assert(end == _available_map.size() || _available_map.at(end), "Region (" SIZE_FORMAT ") is not available", end);

  return HeapRegionRange((uint) start, (uint) end);
}

uint HeapRegionManager::find_highest_free(bool* expanded) {
  // Loop downwards from the highest region index, looking for an
  // entry which is either free or not yet committed.  If not yet
  // committed, expand_at that index.
  uint curr = max_length() - 1;
  while (true) {
    HeapRegion *hr = _regions.get_by_index(curr);
    if (hr == NULL || !is_available(curr)) {
      uint res = expand_at(curr, 1, NULL);
      if (res == 1) {
        *expanded = true;
        return curr;
      }
    } else {
      if (hr->is_free()) {
        *expanded = false;
        return curr;
      }
    }
    if (curr == 0) {
      return G1_NO_HRM_INDEX;
    }
    curr--;
  }
}

bool HeapRegionManager::allocate_containing_regions(MemRegion range, size_t* commit_count, WorkGang* pretouch_workers) {
  size_t commits = 0;
  uint start_index = (uint)_regions.get_index_by_address(range.start());
  uint last_index = (uint)_regions.get_index_by_address(range.last());

  // Ensure that each G1 region in the range is free, returning false if not.
  // Commit those that are not yet available, and keep count.
  for (uint curr_index = start_index; curr_index <= last_index; curr_index++) {
    if (!is_available(curr_index)) {
      commits++;
      expand_at(curr_index, 1, pretouch_workers);
    }
    HeapRegion* curr_region  = _regions.get_by_index(curr_index);
    if (!curr_region->is_free()) {
      return false;
    }
  }

  allocate_free_regions_starting_at(start_index, (last_index - start_index) + 1);
  *commit_count = commits;
  return true;
}

void HeapRegionManager::par_iterate(HeapRegionClosure* blk, HeapRegionClaimer* hrclaimer, const uint start_index) const {
  // Every worker will actually look at all regions, skipping over regions that
  // are currently not committed.
  // This also (potentially) iterates over regions newly allocated during GC. This
  // is no problem except for some extra work.
  const uint n_regions = hrclaimer->n_regions();
  for (uint count = 0; count < n_regions; count++) {
    const uint index = (start_index + count) % n_regions;
    assert(index < n_regions, "sanity");
    // Skip over unavailable regions
    if (!is_available(index)) {
      continue;
    }
    HeapRegion* r = _regions.get_by_index(index);
    // We'll ignore regions already claimed.
    // However, if the iteration is specified as concurrent, the values for
    // is_starts_humongous and is_continues_humongous can not be trusted,
    // and we should just blindly iterate over regions regardless of their
    // humongous status.
    if (hrclaimer->is_region_claimed(index)) {
      continue;
    }
    // OK, try to claim it
    if (!hrclaimer->claim_region(index)) {
      continue;
    }
    bool res = blk->do_heap_region(r);
    if (res) {
      return;
    }
  }
}

uint HeapRegionManager::shrink_by(uint num_regions_to_remove) {
  assert(length() > 0, "the region sequence should not be empty");
  assert(length() <= _allocated_heapregions_length, "invariant");
  assert(_allocated_heapregions_length > 0, "we should have at least one region committed");
  assert(num_regions_to_remove < length(), "We should never remove all regions");

  if (num_regions_to_remove == 0) {
    return 0;
  }

  uint removed = 0;
  uint cur = _allocated_heapregions_length - 1;
  uint idx_last_found = 0;
  uint num_last_found = 0;

  while ((removed < num_regions_to_remove) &&
      (num_last_found = find_empty_from_idx_reverse(cur, &idx_last_found)) > 0) {
    uint to_remove = MIN2(num_regions_to_remove - removed, num_last_found);

    shrink_at(idx_last_found + num_last_found - to_remove, to_remove);

    cur = idx_last_found;
    removed += to_remove;
  }

  verify_optional();

  return removed;
}

void HeapRegionManager::shrink_at(uint index, size_t num_regions) {
#ifdef ASSERT
  for (uint i = index; i < (index + num_regions); i++) {
    assert(is_available(i), "Expected available region at index %u", i);
    assert(at(i)->is_empty(), "Expected empty region at index %u", i);
    assert(at(i)->is_free(), "Expected free region at index %u", i);
  }
#endif
  uncommit_regions(index, num_regions);
}

uint HeapRegionManager::find_empty_from_idx_reverse(uint start_idx, uint* res_idx) const {
  guarantee(start_idx < _allocated_heapregions_length, "checking");
  guarantee(res_idx != NULL, "checking");

  uint num_regions_found = 0;

  jlong cur = start_idx;
  while (cur != -1 && !(is_available(cur) && at(cur)->is_empty())) {
    cur--;
  }
  if (cur == -1) {
    return num_regions_found;
  }
  jlong old_cur = cur;
  // cur indexes the first empty region
  while (cur != -1 && is_available(cur) && at(cur)->is_empty()) {
    cur--;
  }
  *res_idx = cur + 1;
  num_regions_found = old_cur - cur;

#ifdef ASSERT
  for (uint i = *res_idx; i < (*res_idx + num_regions_found); i++) {
    assert(at(i)->is_empty(), "just checking");
  }
#endif
  return num_regions_found;
}

void HeapRegionManager::verify() {
  guarantee(length() <= _allocated_heapregions_length,
            "invariant: _length: %u _allocated_length: %u",
            length(), _allocated_heapregions_length);
  guarantee(_allocated_heapregions_length <= max_length(),
            "invariant: _allocated_length: %u _max_length: %u",
            _allocated_heapregions_length, max_length());

  bool prev_committed = true;
  uint num_committed = 0;
  HeapWord* prev_end = heap_bottom();
  for (uint i = 0; i < _allocated_heapregions_length; i++) {
    if (!is_available(i)) {
      prev_committed = false;
      continue;
    }
    num_committed++;
    HeapRegion* hr = _regions.get_by_index(i);
    guarantee(hr != NULL, "invariant: i: %u", i);
    guarantee(!prev_committed || hr->bottom() == prev_end,
              "invariant i: %u " HR_FORMAT " prev_end: " PTR_FORMAT,
              i, HR_FORMAT_PARAMS(hr), p2i(prev_end));
    guarantee(hr->hrm_index() == i,
              "invariant: i: %u hrm_index(): %u", i, hr->hrm_index());
    // Asserts will fire if i is >= _length
    HeapWord* addr = hr->bottom();
    guarantee(addr_to_region(addr) == hr, "sanity");
    // We cannot check whether the region is part of a particular set: at the time
    // this method may be called, we have only completed allocation of the regions,
    // but not put into a region set.
    prev_committed = true;
    prev_end = hr->end();
  }
  for (uint i = _allocated_heapregions_length; i < max_length(); i++) {
    guarantee(_regions.get_by_index(i) == NULL, "invariant i: %u", i);
  }

  guarantee(num_committed == _num_committed, "Found %u committed regions, but should be %u", num_committed, _num_committed);
  _free_list.verify();
}

#ifndef PRODUCT
void HeapRegionManager::verify_optional() {
  verify();
}
#endif // PRODUCT

HeapRegionClaimer::HeapRegionClaimer(uint n_workers) :
    _n_workers(n_workers), _n_regions(G1CollectedHeap::heap()->_hrm->_allocated_heapregions_length), _claims(NULL) {
  assert(n_workers > 0, "Need at least one worker.");
  uint* new_claims = NEW_C_HEAP_ARRAY(uint, _n_regions, mtGC);
  memset(new_claims, Unclaimed, sizeof(*_claims) * _n_regions);
  _claims = new_claims;
}

HeapRegionClaimer::~HeapRegionClaimer() {
  FREE_C_HEAP_ARRAY(uint, _claims);
}

uint HeapRegionClaimer::offset_for_worker(uint worker_id) const {
  assert(worker_id < _n_workers, "Invalid worker_id.");
  return _n_regions * worker_id / _n_workers;
}

bool HeapRegionClaimer::is_region_claimed(uint region_index) const {
  assert(region_index < _n_regions, "Invalid index.");
  return _claims[region_index] == Claimed;
}

bool HeapRegionClaimer::claim_region(uint region_index) {
  assert(region_index < _n_regions, "Invalid index.");
  uint old_val = Atomic::cmpxchg(&_claims[region_index], Unclaimed, Claimed);
  return old_val == Unclaimed;
}

class G1RebuildFreeListTask : public AbstractGangTask {
  HeapRegionManager* _hrm;
  FreeRegionList*    _worker_freelists;
  uint               _worker_chunk_size;
  uint               _num_workers;

public:
  G1RebuildFreeListTask(HeapRegionManager* hrm, uint num_workers) :
      AbstractGangTask("G1 Rebuild Free List Task"),
      _hrm(hrm),
      _worker_freelists(NEW_C_HEAP_ARRAY(FreeRegionList, num_workers, mtGC)),
      _worker_chunk_size((_hrm->max_length() + num_workers - 1) / num_workers),
      _num_workers(num_workers) {
    for (uint worker = 0; worker < _num_workers; worker++) {
      ::new (&_worker_freelists[worker]) FreeRegionList("Appendable Worker Free List");
    }
  }

  ~G1RebuildFreeListTask() {
    for (uint worker = 0; worker < _num_workers; worker++) {
      _worker_freelists[worker].~FreeRegionList();
    }
    FREE_C_HEAP_ARRAY(FreeRegionList, _worker_freelists);
  }

  FreeRegionList* worker_freelist(uint worker) {
    return &_worker_freelists[worker];
  }

  // Each worker creates a free list for a chunk of the heap. The chunks won't
  // be overlapping so we don't need to do any claiming.
  void work(uint worker_id) {
    Ticks start_time = Ticks::now();
    EventGCPhaseParallel event;

    uint start = worker_id * _worker_chunk_size;
    uint end = MIN2(start + _worker_chunk_size, _hrm->max_length());

    // If start is outside the heap, this worker has nothing to do.
    if (start > end) {
      return;
    }

    FreeRegionList *free_list = worker_freelist(worker_id);
    for (uint i = start; i < end; i++) {
      HeapRegion *region = _hrm->at_or_null(i);
      if (region != NULL && region->is_free()) {
        // Need to clear old links to allow to be added to new freelist.
        region->unlink_from_list();
        free_list->add_to_tail(region);
      }
    }

    event.commit(GCId::current(), worker_id, G1GCPhaseTimes::phase_name(G1GCPhaseTimes::RebuildFreeList));
    G1CollectedHeap::heap()->phase_times()->record_time_secs(G1GCPhaseTimes::RebuildFreeList, worker_id, (Ticks::now() - start_time).seconds());
  }
};

void HeapRegionManager::rebuild_free_list(WorkGang* workers) {
  // Abandon current free list to allow a rebuild.
  _free_list.abandon();

  uint const num_workers = clamp(max_length(), 1u, workers->active_workers());
  G1RebuildFreeListTask task(this, num_workers);

  log_debug(gc, ergo)("Running %s using %u workers for rebuilding free list of regions",
                      task.name(), num_workers);
  workers->run_task(&task, num_workers);

  // Link the partial free lists together.
  Ticks serial_time = Ticks::now();
  for (uint worker = 0; worker < num_workers; worker++) {
    _free_list.append_ordered(task.worker_freelist(worker));
  }
  G1CollectedHeap::heap()->phase_times()->record_serial_rebuild_freelist_time_ms((Ticks::now() - serial_time).seconds() * 1000.0);
}
