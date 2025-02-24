/*
 * Copyright (c) 1998, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_RUNTIME_OBJECTMONITOR_INLINE_HPP
#define SHARE_RUNTIME_OBJECTMONITOR_INLINE_HPP

#include "runtime/objectMonitor.hpp"

#include "logging/log.hpp"
#include "oops/access.inline.hpp"
#include "oops/markWord.hpp"
#include "runtime/atomic.hpp"
#include "runtime/globals.hpp"
#include "runtime/javaThread.inline.hpp"
#include "runtime/lockStack.inline.hpp"
#include "runtime/synchronizer.hpp"
#include "runtime/threadIdentifier.hpp"
#include "utilities/checkedCast.hpp"
#include "utilities/globalDefinitions.hpp"

inline void* ObjectMonitor::owner_for(JavaThread* thread) const {
  int64_t tid = thread->lock_id();
  assert(tid >= 3 && tid < ThreadIdentifier::current(), "must be reasonable");
  return (void*)tid;
}

inline bool ObjectMonitor::is_entered(JavaThread* current) const {
  if (is_owner_anonymous()) {
    if (LockingMode == LM_LIGHTWEIGHT) {
      return current->lock_stack().contains(object());
    } else {
      return current->is_lock_owned((address)stack_locker());
    }
  } else {
    return is_owner(current);
  }
  return false;
}

inline uintptr_t ObjectMonitor::metadata() const {
  return Atomic::load(&_metadata);
}

inline void ObjectMonitor::set_metadata(uintptr_t value) {
  Atomic::store(&_metadata, value);
}

inline volatile uintptr_t* ObjectMonitor::metadata_addr() {
  STATIC_ASSERT(std::is_standard_layout<ObjectMonitor>::value);
  STATIC_ASSERT(offsetof(ObjectMonitor, _metadata) == 0);
  return &_metadata;
}

inline markWord ObjectMonitor::header() const {
  assert(!UseObjectMonitorTable, "Lightweight locking with OM table does not use header");
  return markWord(metadata());
}

inline void ObjectMonitor::set_header(markWord hdr) {
  assert(!UseObjectMonitorTable, "Lightweight locking with OM table does not use header");
  set_metadata(hdr.value());
}

inline intptr_t ObjectMonitor::hash() const {
  assert(UseObjectMonitorTable, "Only used by lightweight locking with OM table");
  return metadata();
}

inline void ObjectMonitor::set_hash(intptr_t hash) {
  assert(UseObjectMonitorTable, "Only used by lightweight locking with OM table");
  set_metadata(hash);
}

inline int ObjectMonitor::waiters() const {
  return _waiters;
}

inline bool ObjectMonitor::has_owner() const {
  void* owner = owner_raw();
  return owner != nullptr && owner != DEFLATER_MARKER;
}

// Returns null if DEFLATER_MARKER is observed.
inline void* ObjectMonitor::owner() const {
  void* owner = owner_raw();
  return owner != DEFLATER_MARKER ? owner : nullptr;
}

inline void* ObjectMonitor::owner_raw() const {
  return Atomic::load(&_owner);
}

inline BasicLock* ObjectMonitor::stack_locker() const {
  return Atomic::load(&_stack_locker);
}

inline void ObjectMonitor::set_stack_locker(BasicLock* locker) {
  Atomic::store(&_stack_locker, locker);
}

inline bool ObjectMonitor::is_stack_locker(JavaThread* current) {
  return is_owner_anonymous() && current->is_lock_owned((address)stack_locker());
}

// Returns true if owner field == DEFLATER_MARKER and false otherwise.
// This accessor is called when we really need to know if the owner
// field == DEFLATER_MARKER and any non-null value won't do the trick.
inline bool ObjectMonitor::owner_is_DEFLATER_MARKER() const {
  return owner_raw() == DEFLATER_MARKER;
}

// Returns true if 'this' is being async deflated and false otherwise.
inline bool ObjectMonitor::is_being_async_deflated() {
  return contentions() < 0;
}

// Return number of threads contending for this monitor.
inline int ObjectMonitor::contentions() const {
  return Atomic::load(&_contentions);
}

// Add value to the contentions field.
inline void ObjectMonitor::add_to_contentions(int value) {
  Atomic::add(&_contentions, value);
}

inline void ObjectMonitor::set_recursions(size_t recursions) {
  assert(_recursions == 0, "must be");
  assert(has_owner(), "must be owned");
  _recursions = checked_cast<intx>(recursions);
}

// Clear _owner field; current value must match old_value.
inline void ObjectMonitor::release_clear_owner(JavaThread* old_owner) {
  void* old_value = owner_for(old_owner);
#ifdef ASSERT
  void* prev = Atomic::load(&_owner);
  assert(prev == old_value, "unexpected prev owner=" INTPTR_FORMAT
         ", expected=" INTPTR_FORMAT, p2i(prev), p2i(old_value));
#endif
  Atomic::release_store(&_owner, (void*)nullptr);
  log_trace(monitorinflation, owner)("release_clear_owner(): mid="
                                     INTPTR_FORMAT ", old_value=" INTPTR_FORMAT,
                                     p2i(this), p2i(old_value));
}

// Simply set _owner field to new_value; current value must match old_value.
// (Simple means no memory sync needed.)
inline void ObjectMonitor::set_owner_from_raw(void* old_value, void* new_value) {
#ifdef ASSERT
  void* prev = Atomic::load(&_owner);
  assert((int64_t)prev < ThreadIdentifier::current(), "must be reasonable");
  assert(prev == old_value, "unexpected prev owner=" INTPTR_FORMAT
         ", expected=" INTPTR_FORMAT, p2i(prev), p2i(old_value));
#endif
  Atomic::store(&_owner, new_value);
  log_trace(monitorinflation, owner)("set_owner_from(): mid="
                                     INTPTR_FORMAT ", old_value=" INTPTR_FORMAT
                                     ", new_value=" INTPTR_FORMAT, p2i(this),
                                     p2i(old_value), p2i(new_value));
}

inline void ObjectMonitor::set_owner_from(void* old_value, JavaThread* current) {
  set_owner_from_raw(old_value, owner_for(current));
}

// Simply set _owner field to self; current value must match basic_lock_p.
inline void ObjectMonitor::set_owner_from_BasicLock(JavaThread* current) {
  BasicLock* basic_lock_p = stack_locker();

  set_stack_locker(nullptr); // first
  assert(is_owner_anonymous(), "should be anon for now");

  // Non-null owner field to non-null owner field is safe without
  // cmpxchg() as long as all readers can tolerate either flavor.
  Atomic::store(&_owner, owner_for(current));
  log_trace(monitorinflation, owner)("set_owner_from_BasicLock(): mid="
                                     INTPTR_FORMAT ", basic_lock_p="
                                     INTPTR_FORMAT ", new_value=" INTPTR_FORMAT,
                                     p2i(this), p2i(basic_lock_p), p2i(current));
}

// Try to set _owner field to new_value if the current value matches
// old_value. Otherwise, does not change the _owner field. Returns
// the prior value of the _owner field.
inline void* ObjectMonitor::try_set_owner_from_raw(void* old_value, void* new_value) {
  assert((int64_t)new_value < ThreadIdentifier::current(), "must be reasonable");
  void* prev = Atomic::cmpxchg(&_owner, old_value, new_value);
  if (prev == old_value) {
    log_trace(monitorinflation, owner)("try_set_owner_from(): mid="
                                       INTPTR_FORMAT ", prev=" INTPTR_FORMAT
                                       ", new=" INTPTR_FORMAT, p2i(this),
                                       p2i(prev), p2i(new_value));
  }
  return prev;
}

inline void* ObjectMonitor::try_set_owner_from(void* old_value, JavaThread* current) {
  return try_set_owner_from_raw(old_value, owner_for(current));
}

// The _next_om field can be concurrently read and modified so we
// use Atomic operations to disable compiler optimizations that
// might try to elide loading and/or storing this field.

// Simply get _next_om field.
inline ObjectMonitor* ObjectMonitor::next_om() const {
  return Atomic::load(&_next_om);
}

// Simply set _next_om field to new_value.
inline void ObjectMonitor::set_next_om(ObjectMonitor* new_value) {
  Atomic::store(&_next_om, new_value);
}

inline ObjectMonitorContentionMark::ObjectMonitorContentionMark(ObjectMonitor* monitor)
  : _monitor(monitor) {
  _monitor->add_to_contentions(1);
}

inline ObjectMonitorContentionMark::~ObjectMonitorContentionMark() {
  _monitor->add_to_contentions(-1);
}

inline oop ObjectMonitor::object_peek() const {
  if (_object.is_null()) {
    return nullptr;
  }
  return _object.peek();
}

inline bool ObjectMonitor::object_is_dead() const {
  return object_peek() == nullptr;
}

inline bool ObjectMonitor::object_refers_to(oop obj) const {
  if (_object.is_null()) {
    return false;
  }
  return _object.peek() == obj;
}

#endif // SHARE_RUNTIME_OBJECTMONITOR_INLINE_HPP
