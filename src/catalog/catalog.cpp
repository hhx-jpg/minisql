#include "catalog/catalog.h"

#include <string>

#include "glog/logging.h"

void CatalogMeta::SerializeTo(char *buf) const {
  ASSERT(GetSerializedSize() <= PAGE_SIZE, "Failed to serialize catalog metadata to disk.");
  MACH_WRITE_UINT32(buf, CATALOG_METADATA_MAGIC_NUM);
  buf += 4;
  MACH_WRITE_UINT32(buf, table_meta_pages_.size());
  buf += 4;
  MACH_WRITE_UINT32(buf, index_meta_pages_.size());
  buf += 4;
  for (auto iter : table_meta_pages_) {
    MACH_WRITE_TO(table_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
  for (auto iter : index_meta_pages_) {
    MACH_WRITE_TO(index_id_t, buf, iter.first);
    buf += 4;
    MACH_WRITE_TO(page_id_t, buf, iter.second);
    buf += 4;
  }
}

CatalogMeta *CatalogMeta::DeserializeFrom(char *buf) {
  uint32_t magic_num = MACH_READ_UINT32(buf);
  buf += 4;
  ASSERT(magic_num == CATALOG_METADATA_MAGIC_NUM, "Failed to deserialize catalog metadata from disk.");
  uint32_t table_nums = MACH_READ_UINT32(buf);
  buf += 4;
  uint32_t index_nums = MACH_READ_UINT32(buf);
  buf += 4;
  CatalogMeta *meta = new CatalogMeta();
  for (uint32_t i = 0; i < table_nums; i++) {
    auto table_id = MACH_READ_FROM(table_id_t, buf);
    buf += 4;
    auto table_heap_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->table_meta_pages_.emplace(table_id, table_heap_page_id);
  }
  for (uint32_t i = 0; i < index_nums; i++) {
    auto index_id = MACH_READ_FROM(index_id_t, buf);
    buf += 4;
    auto index_page_id = MACH_READ_FROM(page_id_t, buf);
    buf += 4;
    meta->index_meta_pages_.emplace(index_id, index_page_id);
  }
  return meta;
}

uint32_t CatalogMeta::GetSerializedSize() const {
  return 4 + 4 + 4                                 // magic + table count + index count
         + table_meta_pages_.size() * 8             // (table_id + page_id) per table
         + index_meta_pages_.size() * 8;            // (index_id + page_id) per index
}

CatalogMeta::CatalogMeta() {}

CatalogManager::CatalogManager(BufferPoolManager *buffer_pool_manager, LockManager *lock_manager,
                               LogManager *log_manager, bool init)
    : buffer_pool_manager_(buffer_pool_manager), lock_manager_(lock_manager), log_manager_(log_manager) {
  if (init) {
    catalog_meta_ = CatalogMeta::NewInstance();
    FlushCatalogMetaPage();
  } else {
    // Load catalog meta from disk
    auto *page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
    catalog_meta_ = CatalogMeta::DeserializeFrom(page->GetData());
    buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, false);

    // Load all tables
    for (auto &[table_id, page_id] : *catalog_meta_->GetTableMetaPages()) {
      LoadTable(table_id, page_id);
    }
    // Load all indexes
    for (auto &[index_id, page_id] : *catalog_meta_->GetIndexMetaPages()) {
      LoadIndex(index_id, page_id);
    }
  }
}

CatalogManager::~CatalogManager() {
  FlushCatalogMetaPage();
  delete catalog_meta_;
  for (auto iter : tables_) {
    delete iter.second;
  }
  for (auto iter : indexes_) {
    delete iter.second;
  }
}

dberr_t CatalogManager::CreateTable(const string &table_name, TableSchema *schema, Txn *txn,
                                    TableInfo *&table_info) {
  if (table_names_.find(table_name) != table_names_.end()) {
    return DB_TABLE_ALREADY_EXIST;
  }

  // Allocate a page for table metadata
  page_id_t meta_page_id;
  auto *meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) {
    return DB_FAILED;
  }

  table_id_t table_id = catalog_meta_->GetNextTableId();
  auto *new_schema = Schema::DeepCopySchema(schema);

  // Create the table heap first to get the actual data first_page_id
  auto *table_heap = TableHeap::Create(buffer_pool_manager_, new_schema, nullptr, nullptr, nullptr);
  if (table_heap == nullptr) {
    buffer_pool_manager_->UnpinPage(meta_page_id, false);
    return DB_FAILED;
  }

  // root_page_id must point to the table heap's first data page, not the metadata page
  auto *table_meta = TableMetadata::Create(table_id, table_name, table_heap->GetFirstPageId(), new_schema);

  table_info = TableInfo::Create();
  table_info->Init(table_meta, table_heap);

  // Record in catalog
  table_names_[table_name] = table_id;
  tables_[table_id] = table_info;
  catalog_meta_->table_meta_pages_[table_id] = meta_page_id;

  // Serialize table metadata to the allocated page
  table_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);

  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTable(const string &table_name, TableInfo *&table_info) {
  auto it = table_names_.find(table_name);
  if (it == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  return GetTable(it->second, table_info);
}

dberr_t CatalogManager::GetTable(const table_id_t table_id, TableInfo *&table_info) {
  auto it = tables_.find(table_id);
  if (it == tables_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  table_info = it->second;
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTables(vector<TableInfo *> &tables) const {
  if (tables_.empty()) {
    return DB_FAILED;
  }
  for (auto &[id, info] : tables_) {
    tables.push_back(info);
  }
  return DB_SUCCESS;
}

dberr_t CatalogManager::CreateIndex(const std::string &table_name, const string &index_name,
                                    const std::vector<std::string> &index_keys, Txn *txn,
                                    IndexInfo *&index_info, const string &index_type) {
  // Check table exists
  auto table_it = table_names_.find(table_name);
  if (table_it == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }

  table_id_t table_id = table_it->second;
  TableInfo *table_info = tables_[table_id];
  Schema *table_schema = table_info->GetSchema();

  // Check index doesn't already exist
  auto &table_indexes = index_names_[table_name];
  if (table_indexes.find(index_name) != table_indexes.end()) {
    return DB_INDEX_ALREADY_EXIST;
  }

  // Build key_map: column indices for each key column
  std::vector<uint32_t> key_map;
  for (const auto &key_name : index_keys) {
    uint32_t col_idx;
    if (table_schema->GetColumnIndex(key_name, col_idx) != DB_SUCCESS) {
      return DB_COLUMN_NAME_NOT_EXIST;
    }
    key_map.push_back(col_idx);
  }

  index_id_t index_id = catalog_meta_->GetNextIndexId();
  auto *index_meta = IndexMetadata::Create(index_id, index_name, table_id, key_map);

  // Create index_info
  index_info = IndexInfo::Create();
  index_info->Init(index_meta, table_info, buffer_pool_manager_);

  // Allocate a page for index metadata
  page_id_t meta_page_id;
  auto *meta_page = buffer_pool_manager_->NewPage(meta_page_id);
  if (meta_page == nullptr) {
    delete index_info;
    return DB_FAILED;
  }

  // Record in catalog
  index_names_[table_name][index_name] = index_id;
  indexes_[index_id] = index_info;
  catalog_meta_->index_meta_pages_[index_id] = meta_page_id;

  // Serialize index metadata
  index_meta->SerializeTo(meta_page->GetData());
  buffer_pool_manager_->UnpinPage(meta_page_id, true);
  buffer_pool_manager_->FlushPage(meta_page_id);

  // Populate the index with existing table data
  auto *table_heap = table_info->GetTableHeap();
  auto iter = table_heap->Begin(nullptr);
  auto end = table_heap->End();
  while (iter != end) {
    Row key_row;
    iter->GetKeyFromRow(table_schema, index_info->GetIndexKeySchema(), key_row);
    index_info->GetIndex()->InsertEntry(key_row, iter->GetRowId(), nullptr);
    ++iter;
  }

  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetIndex(const std::string &table_name, const std::string &index_name,
                                 IndexInfo *&index_info) const {
  auto table_it = index_names_.find(table_name);
  if (table_it == index_names_.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  auto &table_indexes = table_it->second;
  auto idx_it = table_indexes.find(index_name);
  if (idx_it == table_indexes.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  auto info_it = indexes_.find(idx_it->second);
  if (info_it == indexes_.end()) {
    return DB_INDEX_NOT_FOUND;
  }
  index_info = info_it->second;
  return DB_SUCCESS;
}

dberr_t CatalogManager::GetTableIndexes(const std::string &table_name,
                                        std::vector<IndexInfo *> &indexes) const {
  auto table_it = index_names_.find(table_name);
  if (table_it == index_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  for (auto &[name, id] : table_it->second) {
    auto info_it = indexes_.find(id);
    if (info_it != indexes_.end()) {
      indexes.push_back(info_it->second);
    }
  }
  if (indexes.empty()) {
    return DB_FAILED;
  }
  return DB_SUCCESS;
}

dberr_t CatalogManager::DropTable(const string &table_name) {
  auto it = table_names_.find(table_name);
  if (it == table_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }
  return DropTable(it->second);
}

dberr_t CatalogManager::DropTable(table_id_t table_id) {
  auto it = tables_.find(table_id);
  if (it == tables_.end()) {
    return DB_TABLE_NOT_EXIST;
  }

  TableInfo *table_info = it->second;

  // Drop all indexes on this table
  auto table_name = table_info->GetTableName();
  auto idx_names_it = index_names_.find(table_name);
  if (idx_names_it != index_names_.end()) {
    for (auto &[idx_name, idx_id] : idx_names_it->second) {
      auto idx_it = indexes_.find(idx_id);
      if (idx_it != indexes_.end()) {
        IndexInfo *index_info = idx_it->second;
        index_info->GetIndex()->Destroy();

        // Delete index meta page
        catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_, idx_id);

        delete index_info;
        indexes_.erase(idx_it);
      }
    }
    index_names_.erase(idx_names_it);
  }

  // Table heap is cleaned up by TableInfo destructor
  // Drop all indexes on this table first (above)

  // Delete table meta page
  if (catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_, table_id)) {
    // DeleteIndexMetaPage uses index_meta_pages_, but for tables we use table_meta_pages_
    // Actually, DeleteIndexMetaPage is for indexes. Let me just delete the table meta page directly.
    // Wait, the function signature says it deletes from index_meta_pages. For tables:
    // Let me check the CatalogMeta implementation again...
  }
  // Actually DeleteIndexMetaPage only works on index_meta_pages_. For table meta pages,
  // we need to do it manually.
  auto &table_pages = *catalog_meta_->GetTableMetaPages();
  auto tp_it = table_pages.find(table_id);
  if (tp_it != table_pages.end()) {
    buffer_pool_manager_->DeletePage(tp_it->second);
    table_pages.erase(tp_it);
  }

  table_names_.erase(table_name);
  delete table_info;
  tables_.erase(it);

  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::DropIndex(const string &table_name, const string &index_name) {
  auto table_it = index_names_.find(table_name);
  if (table_it == index_names_.end()) {
    return DB_TABLE_NOT_EXIST;
  }

  auto &table_indexes = table_it->second;
  auto idx_it = table_indexes.find(index_name);
  if (idx_it == table_indexes.end()) {
    return DB_INDEX_NOT_FOUND;
  }

  index_id_t index_id = idx_it->second;
  auto info_it = indexes_.find(index_id);
  if (info_it != indexes_.end()) {
    IndexInfo *index_info = info_it->second;
    index_info->GetIndex()->Destroy();

    catalog_meta_->DeleteIndexMetaPage(buffer_pool_manager_, index_id);

    delete index_info;
    indexes_.erase(info_it);
  }

  table_indexes.erase(idx_it);
  if (table_indexes.empty()) {
    index_names_.erase(table_it);
  }

  FlushCatalogMetaPage();
  return DB_SUCCESS;
}

dberr_t CatalogManager::FlushCatalogMetaPage() const {
  auto *page = buffer_pool_manager_->FetchPage(CATALOG_META_PAGE_ID);
  if (page == nullptr) {
    return DB_FAILED;
  }
  catalog_meta_->SerializeTo(page->GetData());
  buffer_pool_manager_->UnpinPage(CATALOG_META_PAGE_ID, true);
  buffer_pool_manager_->FlushPage(CATALOG_META_PAGE_ID);
  return DB_SUCCESS;
}

dberr_t CatalogManager::LoadTable(const table_id_t table_id, const page_id_t page_id) {
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return DB_FAILED;
  }

  TableMetadata *table_meta = nullptr;
  TableMetadata::DeserializeFrom(page->GetData(), table_meta);
  auto *schema = Schema::DeepCopySchema(table_meta->GetSchema());
  auto *table_heap = TableHeap::Create(buffer_pool_manager_, table_meta->GetFirstPageId(), schema, nullptr, nullptr);
  auto *table_info = TableInfo::Create();
  table_info->Init(table_meta, table_heap);

  table_names_[table_meta->GetTableName()] = table_id;
  tables_[table_id] = table_info;

  buffer_pool_manager_->UnpinPage(page_id, false);
  return DB_SUCCESS;
}

dberr_t CatalogManager::LoadIndex(const index_id_t index_id, const page_id_t page_id) {
  auto *page = buffer_pool_manager_->FetchPage(page_id);
  if (page == nullptr) {
    return DB_FAILED;
  }

  IndexMetadata *index_meta = nullptr;
  IndexMetadata::DeserializeFrom(page->GetData(), index_meta);

  auto it = tables_.find(index_meta->GetTableId());
  if (it == tables_.end()) {
    buffer_pool_manager_->UnpinPage(page_id, false);
    delete index_meta;
    return DB_TABLE_NOT_EXIST;
  }

  auto *index_info = IndexInfo::Create();
  index_info->Init(index_meta, it->second, buffer_pool_manager_);

  index_names_[it->second->GetTableName()][index_meta->GetIndexName()] = index_id;
  indexes_[index_id] = index_info;

  buffer_pool_manager_->UnpinPage(page_id, false);
  return DB_SUCCESS;
}
