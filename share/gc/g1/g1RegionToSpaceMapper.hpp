/*
 * Copyright (c) 2014, 2019, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1REGIONTOSPACEMAPPER_HPP
#define SHARE_GC_G1_G1REGIONTOSPACEMAPPER_HPP

#include "gc/g1/g1PageBasedVirtualSpace.hpp"
#include "memory/allocation.hpp"
#include "utilities/debug.hpp"

class WorkGang;

class G1MappingChangedListener {
 public:
  // Fired after commit of the memory, i.e. the memory this listener is registered
  // for can be accessed.
  // Zero_filled indicates that the memory can be considered as filled with zero bytes
  // when called.
  virtual void on_commit(uint start_idx, size_t num_regions, bool zero_filled) = 0;
};

// Maps region based commit/uncommit requests to the underlying page sized virtual
// space.
class G1RegionToSpaceMapper : public CHeapObj<mtGC> {
 private:
  G1MappingChangedListener* _listener;
 protected:
  // Backing storage.
  G1PageBasedVirtualSpace _storage;

  size_t _region_granularity;
  // Mapping management
  CHeapBitMap _region_commit_map;

  MemoryType _memory_type;

  G1RegionToSpaceMapper(ReservedSpace rs, size_t used_size, size_t page_size, size_t region_granularity, size_t commit_factor, MemoryType type);

  void fire_on_commit(uint start_idx, size_t num_regions, bool zero_filled);
 public:
  MemRegion reserved() { return _storage.reserved(); }

  size_t reserved_size() { return _storage.reserved_size(); }
  size_t committed_size() { return _storage.committed_size(); }

  void set_mapping_changed_listener(G1MappingChangedListener* listener) { _listener = listener; }

  virtual ~G1RegionToSpaceMapper() {}

  void commit_and_set_special();
  virtual void commit_regions(uint start_idx, size_t num_regions = 1, WorkGang* pretouch_workers = NULL) = 0;
  virtual void uncommit_regions(uint start_idx, size_t num_regions = 1) = 0;

  // Creates an appropriate G1RegionToSpaceMapper for the given parameters.
  // The actual space to be used within the given reservation is given by actual_size.
  // This is because some OSes need to round up the reservation size to guarantee
  // alignment of page_size.
  // The byte_translation_factor defines how many bytes in a region correspond to
  // a single byte in the data structure this mapper is for.
  // Eg. in the card table, this value corresponds to the size a single card
  // table entry corresponds to in the heap.
  static G1RegionToSpaceMapper* create_mapper(ReservedSpace rs,
                                              size_t actual_size,
                                              size_t page_size,
                                              size_t region_granularity,
                                              size_t byte_translation_factor,
                                              MemoryType type);

  static G1RegionToSpaceMapper* create_heap_mapper(ReservedSpace rs,
                                                   size_t actual_size,
                                                   size_t page_size,
                                                   size_t region_granularity,
                                                   size_t byte_translation_factor,
                                                   MemoryType type);
};

// G1RegionToSpaceMapper implementation where
// part of space is mapped to dram and part to nv-dimm
class G1RegionToHeteroSpaceMapper : public G1RegionToSpaceMapper {
private:
  ReservedSpace _rs;
  G1RegionToSpaceMapper* _dram_mapper;
  G1RegionToSpaceMapper* _nvram_mapper;
  uint _num_committed_dram;
  uint _num_committed_nvram;
  uint _start_index_of_dram;
  size_t _page_size;
  size_t _commit_factor;
  MemoryType _type;

public:
  G1RegionToHeteroSpaceMapper(ReservedSpace rs, size_t used_size, size_t page_size, size_t region_granularity, size_t commit_factor, MemoryType type);
  bool initialize();
  uint num_committed_dram() const;
  uint num_committed_nvram() const;
  uint start_index_of_dram() { return _start_index_of_dram; }

  virtual void commit_regions(uint start_idx, size_t num_regions = 1, WorkGang* pretouch_workers = NULL);
  virtual void uncommit_regions(uint start_idx, size_t num_regions = 1);
};
#endif // SHARE_GC_G1_G1REGIONTOSPACEMAPPER_HPP
