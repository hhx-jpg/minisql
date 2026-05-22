#include "index/b_plus_tree.h"

#include <string>

#include "glog/logging.h"
#include "index/basic_comparator.h"
#include "index/generic_key.h"
#include "page/index_roots_page.h"

BPlusTree::BPlusTree(index_id_t index_id, BufferPoolManager *buffer_pool_manager, const KeyManager &KM,
                     int leaf_max_size, int internal_max_size)
    : index_id_(index_id),
      buffer_pool_manager_(buffer_pool_manager),
      processor_(KM),
      leaf_max_size_(leaf_max_size),
      internal_max_size_(internal_max_size) {
  auto *header_page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
  if (header_page != nullptr) {
    auto *roots = reinterpret_cast<IndexRootsPage *>(header_page->GetData());
    page_id_t root_id;
    if (roots->GetRootId(index_id_, &root_id)) {
      root_page_id_ = root_id;
    }
    buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, false);
  }
}

bool BPlusTree::IsEmpty() const {
  return root_page_id_ == INVALID_PAGE_ID;
}

/*****************************************************************************
 * FIND LEAF PAGE
 *****************************************************************************/
Page *BPlusTree::FindLeafPage(const GenericKey *key, page_id_t page_id, bool leftMost) {
  if (IsEmpty()) return nullptr;

  page_id_t current_id = (page_id == INVALID_PAGE_ID) ? root_page_id_ : page_id;
  while (true) {
    auto *page = buffer_pool_manager_->FetchPage(current_id);
    auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());
    if (node->IsLeafPage()) {
      return page;
    }
    // Internal page: follow child pointer
    auto *internal = reinterpret_cast<InternalPage *>(node);
    page_id_t child_id;
    if (leftMost) {
      child_id = internal->ValueAt(0);
    } else {
      child_id = internal->Lookup(key, processor_);
    }
    buffer_pool_manager_->UnpinPage(current_id, false);
    current_id = child_id;
  }
}

/*****************************************************************************
 * SEARCH
 *****************************************************************************/
bool BPlusTree::GetValue(const GenericKey *key, std::vector<RowId> &result, Txn *transaction) {
  if (IsEmpty()) return false;

  auto *page = FindLeafPage(key);
  if (page == nullptr) return false;

  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  RowId value;
  bool found = leaf->Lookup(key, value, processor_);
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);

  if (found) {
    result.push_back(value);
  }
  return found;
}

/*****************************************************************************
 * INSERTION
 *****************************************************************************/
bool BPlusTree::Insert(GenericKey *key, const RowId &value, Txn *transaction) {
  if (IsEmpty()) {
    StartNewTree(key, value);
    return true;
  }
  return InsertIntoLeaf(key, value, transaction);
}

void BPlusTree::StartNewTree(GenericKey *key, const RowId &value) {
  page_id_t new_page_id;
  auto *page = buffer_pool_manager_->NewPage(new_page_id);
  if (page == nullptr) {
    throw std::bad_alloc();
  }

  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  leaf->Init(new_page_id, INVALID_PAGE_ID, processor_.GetKeySize(), leaf_max_size_);
  leaf->Insert(key, value, processor_);

  root_page_id_ = new_page_id;
  UpdateRootPageId(1);
  buffer_pool_manager_->UnpinPage(new_page_id, true);
}

bool BPlusTree::InsertIntoLeaf(GenericKey *key, const RowId &value, Txn *transaction) {
  auto *page = FindLeafPage(key);
  if (page == nullptr) return false;

  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int new_size = leaf->Insert(key, value, processor_);

  if (new_size == -1) {
    // Duplicate key
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return false;
  }

  if (new_size > leaf->GetMaxSize()) {
    // Split
    auto *new_leaf = Split(leaf, transaction);
    InsertIntoParent(leaf, new_leaf->KeyAt(0), new_leaf, transaction);
    buffer_pool_manager_->UnpinPage(new_leaf->GetPageId(), true);
  }

  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);
  return true;
}

BPlusTreeLeafPage *BPlusTree::Split(LeafPage *node, Txn *transaction) {
  page_id_t new_page_id;
  auto *new_page = buffer_pool_manager_->NewPage(new_page_id);
  if (new_page == nullptr) {
    throw std::bad_alloc();
  }

  auto *new_leaf = reinterpret_cast<LeafPage *>(new_page->GetData());
  new_leaf->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), leaf_max_size_);

  node->MoveHalfTo(new_leaf);
  // Link the leaf chain
  new_leaf->SetNextPageId(node->GetNextPageId());
  node->SetNextPageId(new_page_id);

  return new_leaf;
}

BPlusTreeInternalPage *BPlusTree::Split(InternalPage *node, Txn *transaction) {
  page_id_t new_page_id;
  auto *new_page = buffer_pool_manager_->NewPage(new_page_id);
  if (new_page == nullptr) {
    throw std::bad_alloc();
  }

  auto *new_internal = reinterpret_cast<InternalPage *>(new_page->GetData());
  new_internal->Init(new_page_id, node->GetParentPageId(), processor_.GetKeySize(), internal_max_size_);

  node->MoveHalfTo(new_internal, buffer_pool_manager_);

  return new_internal;
}

void BPlusTree::InsertIntoParent(BPlusTreePage *old_node, GenericKey *key, BPlusTreePage *new_node,
                                 Txn *transaction) {
  if (old_node->IsRootPage()) {
    // Create a new root
    page_id_t new_root_id;
    auto *new_root_page = buffer_pool_manager_->NewPage(new_root_id);
    if (new_root_page == nullptr) {
      throw std::bad_alloc();
    }

    auto *new_root = reinterpret_cast<InternalPage *>(new_root_page->GetData());
    new_root->Init(new_root_id, INVALID_PAGE_ID, processor_.GetKeySize(), internal_max_size_);
    new_root->PopulateNewRoot(old_node->GetPageId(), key, new_node->GetPageId());

    // Update parent pointers of children
    old_node->SetParentPageId(new_root_id);
    new_node->SetParentPageId(new_root_id);

    root_page_id_ = new_root_id;
    UpdateRootPageId(0);
    buffer_pool_manager_->UnpinPage(new_root_id, true);
    return;
  }

  // Non-root: go up to parent
  page_id_t parent_id = old_node->GetParentPageId();
  auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

  parent->InsertNodeAfter(old_node->GetPageId(), key, new_node->GetPageId());

  if (parent->GetSize() > parent->GetMaxSize()) {
    auto *new_parent = Split(parent, transaction);
    InsertIntoParent(parent, new_parent->KeyAt(0), new_parent, transaction);
    buffer_pool_manager_->UnpinPage(new_parent->GetPageId(), true);
  }

  buffer_pool_manager_->UnpinPage(parent_id, true);
}

/*****************************************************************************
 * REMOVE
 *****************************************************************************/
void BPlusTree::Remove(const GenericKey *key, Txn *transaction) {
  if (IsEmpty()) return;

  auto *page = FindLeafPage(key);
  if (page == nullptr) return;

  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int old_size = leaf->GetSize();
  leaf->RemoveAndDeleteRecord(key, processor_);

  if (leaf->GetSize() == old_size) {
    // Key not found
    buffer_pool_manager_->UnpinPage(leaf->GetPageId(), false);
    return;
  }

  bool deleted = CoalesceOrRedistribute(leaf, transaction);
  buffer_pool_manager_->UnpinPage(leaf->GetPageId(), true);

  if (deleted) {
    buffer_pool_manager_->DeletePage(leaf->GetPageId());
  }
}

template <typename N>
bool BPlusTree::CoalesceOrRedistribute(N *&node, Txn *transaction) {
  if (node->IsRootPage()) {
    return AdjustRoot(node);
  }

  if (node->GetSize() >= node->GetMinSize()) {
    return false;
  }

  // Find the parent and the index of this node in the parent
  page_id_t parent_id = node->GetParentPageId();
  auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
  int idx = parent->ValueIndex(node->GetPageId());

  // Find a sibling (predecessor preferred)
  int sibling_idx;
  if (idx > 0) {
    sibling_idx = idx - 1;
  } else {
    sibling_idx = idx + 1;
  }

  page_id_t sibling_page_id = parent->ValueAt(sibling_idx);
  auto *sibling_page = buffer_pool_manager_->FetchPage(sibling_page_id);
  auto *sibling_node = reinterpret_cast<N *>(sibling_page->GetData());

  // Check if we can coalesce: total size <= max_size
  int total_size = node->GetSize() + sibling_node->GetSize();
  if constexpr (std::is_same_v<N, LeafPage>) {
    if (total_size <= node->GetMaxSize()) {
      if (sibling_idx < idx) {
        // Left sibling (receiver): move this node INTO sibling.
        // donor = node (at idx), donor_index = idx
        bool parent_deleted = Coalesce(sibling_node, node, parent, idx, transaction);
        buffer_pool_manager_->UnpinPage(parent_id, true);
        if (parent_deleted) {
          buffer_pool_manager_->DeletePage(parent_id);
        }
        buffer_pool_manager_->UnpinPage(sibling_page_id, true);
        return true;  // caller deletes node
      } else {
        // Right sibling: move sibling INTO this node.
        // donor = sibling_node (at sibling_idx), donor_index = sibling_idx
        bool parent_deleted = Coalesce(node, sibling_node, parent, sibling_idx, transaction);
        buffer_pool_manager_->UnpinPage(parent_id, true);
        if (parent_deleted) {
          buffer_pool_manager_->DeletePage(parent_id);
        }
        buffer_pool_manager_->UnpinPage(sibling_page_id, true);
        buffer_pool_manager_->DeletePage(sibling_page_id);
        return false;
      }
    }
  } else {
    if (total_size < node->GetMaxSize()) {
      if (sibling_idx < idx) {
        // Left sibling (receiver): move this node INTO sibling.
        // donor = node (at idx), donor_index = idx
        bool parent_deleted = Coalesce(sibling_node, node, parent, idx, transaction);
        buffer_pool_manager_->UnpinPage(parent_id, true);
        if (parent_deleted) {
          buffer_pool_manager_->DeletePage(parent_id);
        }
        buffer_pool_manager_->UnpinPage(sibling_page_id, true);
        return true;  // caller deletes node
      } else {
        // Right sibling: move sibling INTO this node.
        // donor = sibling_node (at sibling_idx), donor_index = sibling_idx
        bool parent_deleted = Coalesce(node, sibling_node, parent, sibling_idx, transaction);
        buffer_pool_manager_->UnpinPage(parent_id, true);
        if (parent_deleted) {
          buffer_pool_manager_->DeletePage(parent_id);
        }
        buffer_pool_manager_->UnpinPage(sibling_page_id, true);
        buffer_pool_manager_->DeletePage(sibling_page_id);
        return false;
      }
    }
  }

  // Redistribute: index==0 means neighbor is LEFT of node, >0 means RIGHT.
  if (sibling_idx < idx) {
    Redistribute(sibling_node, node, 0);
  } else {
    Redistribute(sibling_node, node, 1);
  }

  buffer_pool_manager_->UnpinPage(parent_id, true);
  buffer_pool_manager_->UnpinPage(sibling_page_id, true);
  return false;
}

// Explicit template instantiation
template bool BPlusTree::CoalesceOrRedistribute(LeafPage *&node, Txn *transaction);
template bool BPlusTree::CoalesceOrRedistribute(InternalPage *&node, Txn *transaction);

bool BPlusTree::Coalesce(LeafPage *&neighbor_node, LeafPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  if (index == 0) {
    // node is at index 1 in parent, neighbor is at index 0
    // Move node into neighbor
    node->MoveAllTo(neighbor_node);
    parent->Remove(1);
  } else {
    // node is at index, neighbor is at index-1
    // Move node into neighbor
    node->MoveAllTo(neighbor_node);
    parent->Remove(index);
  }
  return CoalesceOrRedistribute(parent, transaction);
}

bool BPlusTree::Coalesce(InternalPage *&neighbor_node, InternalPage *&node, InternalPage *&parent, int index,
                         Txn *transaction) {
  if (index == 0) {
    // node is at index 1 in parent, neighbor is at index 0
    // middle_key is parent->KeyAt(1)
    GenericKey *middle_key = parent->KeyAt(1);
    node->MoveAllTo(neighbor_node, middle_key, buffer_pool_manager_);
    parent->Remove(1);
  } else {
    // node is at index, neighbor is at index-1
    // middle_key is parent->KeyAt(index)
    GenericKey *middle_key = parent->KeyAt(index);
    node->MoveAllTo(neighbor_node, middle_key, buffer_pool_manager_);
    parent->Remove(index);
  }
  return CoalesceOrRedistribute(parent, transaction);
}

void BPlusTree::Redistribute(LeafPage *neighbor_node, LeafPage *node, int index) {
  if (index == 0) {
    // neighbor is at index 0 (to the left), node is at index 1
    // Move neighbor's last entry to front of node
    neighbor_node->MoveLastToFrontOf(node);
    // Update parent's key for node
    page_id_t parent_id = node->GetParentPageId();
    auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
    auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
    int node_index = parent->ValueIndex(node->GetPageId());
    parent->SetKeyAt(node_index, node->KeyAt(0));
    buffer_pool_manager_->UnpinPage(parent_id, true);
  } else {
    // neighbor is at index 1 (to the right), node is at index 0
    // Move neighbor's first entry to end of node
    neighbor_node->MoveFirstToEndOf(node);
    // Update parent's key for neighbor
    page_id_t parent_id = node->GetParentPageId();
    auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
    auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());
    int neighbor_index = parent->ValueIndex(neighbor_node->GetPageId());
    parent->SetKeyAt(neighbor_index, neighbor_node->KeyAt(0));
    buffer_pool_manager_->UnpinPage(parent_id, true);
  }
}

void BPlusTree::Redistribute(InternalPage *neighbor_node, InternalPage *node, int index) {
  page_id_t parent_id = node->GetParentPageId();
  auto *parent_page = buffer_pool_manager_->FetchPage(parent_id);
  auto *parent = reinterpret_cast<InternalPage *>(parent_page->GetData());

  if (index == 0) {
    // neighbor is at index 0, node is at index 1
    // middle key is parent->KeyAt(1)
    GenericKey *middle_key = parent->KeyAt(1);
    neighbor_node->MoveLastToFrontOf(node, middle_key, buffer_pool_manager_);
    parent->SetKeyAt(1, node->KeyAt(0));
    // After MoveLastToFrontOf, node's KeyAt(0) is the new separator
    // Actually, MoveLastToFrontOf sets KeyAt(1) to middle_key
  } else {
    // neighbor is at index 1, node is at index 0
    // middle key is parent->KeyAt(1)
    GenericKey *middle_key = parent->KeyAt(1);
    neighbor_node->MoveFirstToEndOf(node, middle_key, buffer_pool_manager_);
    parent->SetKeyAt(1, neighbor_node->KeyAt(0));
  }

  buffer_pool_manager_->UnpinPage(parent_id, true);
}

bool BPlusTree::AdjustRoot(BPlusTreePage *old_root_node) {
  if (old_root_node->IsLeafPage()) {
    // Leaf root: if empty, tree is empty
    if (old_root_node->GetSize() == 0) {
      root_page_id_ = INVALID_PAGE_ID;
      UpdateRootPageId(0);
      return true;  // delete this page
    }
    return false;
  }

  // Internal root: if only one child, promote it
  auto *root = reinterpret_cast<InternalPage *>(old_root_node);
  if (root->GetSize() == 1) {
    page_id_t child_id = root->RemoveAndReturnOnlyChild();
    root_page_id_ = child_id;
    UpdateRootPageId(0);

    // Update the new root's parent to INVALID_PAGE_ID
    auto *child_page = buffer_pool_manager_->FetchPage(child_id);
    auto *child_node = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
    child_node->SetParentPageId(INVALID_PAGE_ID);
    buffer_pool_manager_->UnpinPage(child_id, true);

    return true;  // delete the old root
  }
  return false;
}

void BPlusTree::UpdateRootPageId(int insert_record) {
  auto *header_page = buffer_pool_manager_->FetchPage(INDEX_ROOTS_PAGE_ID);
  auto *roots = reinterpret_cast<IndexRootsPage *>(header_page->GetData());

  if (insert_record) {
    roots->Insert(index_id_, root_page_id_);
  } else {
    if (root_page_id_ == INVALID_PAGE_ID) {
      roots->Delete(index_id_);
    } else {
      roots->Update(index_id_, root_page_id_);
    }
  }
  buffer_pool_manager_->UnpinPage(INDEX_ROOTS_PAGE_ID, true);
}

/*****************************************************************************
 * INDEX ITERATOR
 *****************************************************************************/
IndexIterator BPlusTree::Begin() {
  if (IsEmpty()) return IndexIterator();
  auto *page = FindLeafPage(nullptr, INVALID_PAGE_ID, true);
  if (page == nullptr) return IndexIterator();
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  // The page is already pinned by FindLeafPage
  // IndexIterator constructor will FetchPage it again — so unpin first
  page_id_t page_id = leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(page_id, false);
  return IndexIterator(page_id, buffer_pool_manager_, 0);
}

IndexIterator BPlusTree::Begin(const GenericKey *key) {
  if (IsEmpty()) return IndexIterator();
  auto *page = FindLeafPage(key);
  if (page == nullptr) return IndexIterator();
  auto *leaf = reinterpret_cast<LeafPage *>(page->GetData());
  int idx = leaf->KeyIndex(key, processor_);
  page_id_t page_id = leaf->GetPageId();
  buffer_pool_manager_->UnpinPage(page_id, false);
  return IndexIterator(page_id, buffer_pool_manager_, idx);
}

IndexIterator BPlusTree::End() {
  return IndexIterator();
}

/*****************************************************************************
 * CHECK & DEBUG
 *****************************************************************************/
bool BPlusTree::Check() {
  if (!buffer_pool_manager_->CheckAllUnpinned()) {
    return false;
  }
  if (IsEmpty()) {
    return true;
  }

  // Verify root
  auto *root_page = buffer_pool_manager_->FetchPage(root_page_id_);
  auto *root_node = reinterpret_cast<BPlusTreePage *>(root_page->GetData());
  if (!root_node->IsRootPage()) {
    buffer_pool_manager_->UnpinPage(root_page_id_, false);
    return false;
  }

  // BFS traversal, tracking leaf depth
  std::queue<std::pair<page_id_t, int>> q;
  q.push({root_page_id_, 0});
  buffer_pool_manager_->UnpinPage(root_page_id_, false);

  int leaf_depth = -1;

  while (!q.empty()) {
    auto [page_id, depth] = q.front();
    q.pop();

    auto *page = buffer_pool_manager_->FetchPage(page_id);
    auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());

    if (node->IsLeafPage()) {
      auto *leaf = reinterpret_cast<LeafPage *>(node);

      if (leaf_depth == -1) {
        leaf_depth = depth;
      } else if (leaf_depth != depth) {
        buffer_pool_manager_->UnpinPage(page_id, false);
        return false;
      }

      // Verify leaf key ordering
      for (int i = 0; i < leaf->GetSize() - 1; i++) {
        if (processor_.CompareKeys(leaf->KeyAt(i), leaf->KeyAt(i + 1)) >= 0) {
          buffer_pool_manager_->UnpinPage(page_id, false);
          return false;
        }
      }
    } else {
      auto *internal = reinterpret_cast<InternalPage *>(node);

      // Verify key ordering (skip dummy at index 0)
      for (int i = 1; i < internal->GetSize() - 1; i++) {
        if (processor_.CompareKeys(internal->KeyAt(i), internal->KeyAt(i + 1)) >= 0) {
          buffer_pool_manager_->UnpinPage(page_id, false);
          return false;
        }
      }

      // Enqueue children and verify parent link
      for (int i = 0; i < internal->GetSize(); i++) {
        page_id_t child_id = internal->ValueAt(i);
        auto *child_page = buffer_pool_manager_->FetchPage(child_id);
        if (child_page == nullptr) {
          buffer_pool_manager_->UnpinPage(page_id, false);
          return false;
        }
        auto *child_node = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
        if (child_node->GetParentPageId() != page_id) {
          buffer_pool_manager_->UnpinPage(child_id, false);
          buffer_pool_manager_->UnpinPage(page_id, false);
          return false;
        }
        if (child_node->GetPageId() != child_id) {
          buffer_pool_manager_->UnpinPage(child_id, false);
          buffer_pool_manager_->UnpinPage(page_id, false);
          return false;
        }
        buffer_pool_manager_->UnpinPage(child_id, false);
        q.push({child_id, depth + 1});
      }
    }

    buffer_pool_manager_->UnpinPage(page_id, false);
  }

  return buffer_pool_manager_->CheckAllUnpinned();
}

void BPlusTree::ToGraph(BPlusTreePage *page, BufferPoolManager *bpm, std::ofstream &out, Schema *schema) const {
  page_id_t page_id = page->GetPageId();
  out << "  node" << page_id << " [label=\"";

  if (page->IsLeafPage()) {
    auto *leaf = reinterpret_cast<LeafPage *>(page);
    out << "Leaf " << page_id << " (size=" << leaf->GetSize() << ")\\n";
    for (int i = 0; i < leaf->GetSize(); i++) {
      Row row;
      processor_.DeserializeToKey(leaf->KeyAt(i), row, schema);
      out << row.GetField(0)->toString();
      if (i < leaf->GetSize() - 1) out << ", ";
    }
  } else {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    out << "Int " << page_id << " (size=" << internal->GetSize() << ")\\n";
    for (int i = 0; i < internal->GetSize(); i++) {
      out << "ptr→" << internal->ValueAt(i);
      if (i < internal->GetSize() - 1) {
        Row row;
        processor_.DeserializeToKey(internal->KeyAt(i + 1), row, schema);
        out << " | " << row.GetField(0)->toString() << " | ";
      }
    }
  }

  out << "\"];" << std::endl;

  if (!page->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(page);
    for (int i = 0; i < internal->GetSize(); i++) {
      page_id_t child_id = internal->ValueAt(i);
      out << "  node" << page_id << " -> node" << child_id << ";" << std::endl;
      auto *child_page = bpm->FetchPage(child_id);
      auto *child_node = reinterpret_cast<BPlusTreePage *>(child_page->GetData());
      ToGraph(child_node, bpm, out, schema);
      bpm->UnpinPage(child_id, false);
    }
  }
}

void BPlusTree::ToString(BPlusTreePage *page, BufferPoolManager *bpm) const {
  // Stub: not required by tests
}
void BPlusTree::Destroy(page_id_t current_page_id) {
  if (IsEmpty()) return;

  if (current_page_id == INVALID_PAGE_ID) {
    current_page_id = root_page_id_;
    root_page_id_ = INVALID_PAGE_ID;
    UpdateRootPageId(0);
  }

  auto *page = buffer_pool_manager_->FetchPage(current_page_id);
  auto *node = reinterpret_cast<BPlusTreePage *>(page->GetData());

  if (!node->IsLeafPage()) {
    auto *internal = reinterpret_cast<InternalPage *>(node);
    for (int i = 0; i < internal->GetSize(); i++) {
      Destroy(internal->ValueAt(i));
    }
  }

  buffer_pool_manager_->UnpinPage(current_page_id, false);
  buffer_pool_manager_->DeletePage(current_page_id);
}
