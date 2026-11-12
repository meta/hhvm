/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include "hphp/util/concurrent-slist.h"

namespace HPHP::Treadmill {

//////////////////////////////////////////////////////////////////////

struct WorkItem;
void enqueueInternal(std::unique_ptr<WorkItem>);

//////////////////////////////////////////////////////////////////////

struct WorkItem : ConcurrentSListNode {
  WorkItem() = default;
  WorkItem(const WorkItem&) = delete;
  WorkItem& operator=(const WorkItem&) = delete;
  virtual ~WorkItem() {}

  virtual void run() noexcept = 0;

private:
  friend void finishRequest();
  friend void enqueueInternal(std::unique_ptr<WorkItem>);

  // Inherently racy. We get a lower bound on the generation;
  // presumably clients are aware of this, and are creating the
  // trigger for an object that was reachable strictly in the past.
  Clock::time_point m_timestamp;
};

template<class F>
struct WorkItemImpl final : WorkItem {
  explicit WorkItemImpl(F f) : f(std::move(f)) {}

  void run() noexcept override { f(); }

private:
  F f;
};

template<class F>
void enqueue(F&& f) {
  static_assert(!std::is_lvalue_reference_v<F>,
                "Treadmill::enqueue takes ownership of its callback; "
                "pass an rvalue (std::move it)");
  enqueueInternal(
    std::make_unique<WorkItemImpl<std::decay_t<F>>>(std::forward<F>(f)));
}

//////////////////////////////////////////////////////////////////////

}

