#pragma once

// A skiplist with one writer and any number of concurrent lock-free readers.
//
// Thread safety:
//  - Insert() calls must be serialized externally (the DB's write path has a
//    single committing writer at a time).
//  - Readers need no locks. They only require that the list (and its arena)
//    outlives them, which the owning memtable's reference count guarantees.
//
// Why this is safe without locks: nodes are never deleted or modified after
// they are linked in, except for their forward pointers, and those only ever
// change from "skip over" to "point at a newly inserted node". A new node is
// fully initialized before the release-store that links it into level 0, and
// readers follow pointers with acquire-loads, so a reader either sees the new
// node complete or does not see it at all. Higher levels are only shortcuts:
// a reader that misses a node there still finds it on level 0.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <new>

#include "util/arena.h"
#include "util/random.h"

namespace kvlog {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  // `compare` orders keys; `arena` supplies node memory and must outlive the
  // list.
  SkipList(Comparator compare, Arena* arena);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Requires: no key comparing equal to `key` is in the list, and no other
  // Insert() runs concurrently.
  void Insert(const Key& key);

  bool Contains(const Key& key) const;

  // Iterates over the list. Concurrent inserts may or may not be observed.
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}

    bool Valid() const { return node_ != nullptr; }

    // Requires: Valid().
    const Key& key() const {
      assert(Valid());
      return node_->key;
    }

    // Requires: Valid().
    void Next() {
      assert(Valid());
      node_ = node_->Next(0);
    }

    // Positions at the first entry with key >= target.
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

    void SeekToFirst() { node_ = list_->head_->Next(0); }

   private:
    const SkipList* list_;
    Node* node_;
  };

 private:
  static constexpr int kMaxHeight = 12;
  // Each level holds about 1/kBranching of the nodes of the level below.
  static constexpr uint32_t kBranching = 4;

  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }
  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool KeyIsAfterNode(const Key& key, const Node* node) const {
    return node != nullptr && compare_(node->key, key) < 0;
  }

  // Returns the first node with key >= `key`, or nullptr. If `prev` is not
  // null, fills prev[level] with the last node before it on every level.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  const Comparator compare_;
  Arena* const arena_;
  Node* const head_;
  // Height of the tallest node. Read without synchronization by readers; a
  // stale value only means they start their search one level lower.
  std::atomic<int> max_height_;
  // Only touched by the (single) writer.
  Random rnd_;
};

// A node is allocated with room for `height` forward pointers: the struct
// declares the first, and the others follow it in the same allocation.
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  Node* Next(int level) {
    assert(level >= 0);
    // Acquire: whoever linked the returned node had fully initialized it.
    return next_[level].load(std::memory_order_acquire);
  }
  void SetNext(int level, Node* node) {
    assert(level >= 0);
    // Release: publishes the node's contents to readers that load this link.
    next_[level].store(node, std::memory_order_release);
  }
  // For links of a node that no reader can reach yet.
  Node* NoBarrierNext(int level) { return next_[level].load(std::memory_order_relaxed); }
  void NoBarrierSetNext(int level, Node* node) {
    next_[level].store(node, std::memory_order_relaxed);
  }

 private:
  friend class SkipList;
  // Trailing array; the allocation extends it to the node's height.
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key,
                                                                             int height) {
  const size_t size = sizeof(Node) + sizeof(std::atomic<Node*>) * static_cast<size_t>(height - 1);
  char* const memory = arena_->AllocateAligned(size);
  Node* node = new (memory) Node(key);
  for (int level = 1; level < height; ++level) {
    new (&node->next_[level]) std::atomic<Node*>(nullptr);
  }
  return node;
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
    ++height;
  }
  return height;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      x = next;  // Keep moving right on this level.
    } else {
      if (prev != nullptr) {
        prev[level] = x;
      }
      if (level == 0) {
        return next;
      }
      --level;  // Drop down a level.
    }
  }
}

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator compare, Arena* arena)
    : compare_(compare),
      arena_(arena),
      head_(NewNode(Key(), kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int level = 0; level < kMaxHeight; ++level) {
    head_->NoBarrierSetNext(level, nullptr);
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight];
  [[maybe_unused]] Node* existing = FindGreaterOrEqual(key, prev);
  assert(existing == nullptr || compare_(key, existing->key) != 0);

  const int height = RandomHeight();
  if (height > GetMaxHeight()) {
    for (int level = GetMaxHeight(); level < height; ++level) {
      prev[level] = head_;
    }
    // A reader that sees the new height before the node is linked finds
    // nullptr at the new levels of head_ and simply drops down, so no
    // ordering with the inserts below is needed.
    max_height_.store(height, std::memory_order_relaxed);
  }

  Node* x = NewNode(key, height);
  for (int level = 0; level < height; ++level) {
    // x is not reachable yet, so its own links need no barrier; the
    // release-store in SetNext publishes x (and these links) to readers.
    x->NoBarrierSetNext(level, prev[level]->NoBarrierNext(level));
    prev[level]->SetNext(level, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  return x != nullptr && compare_(key, x->key) == 0;
}

}  // namespace kvlog
