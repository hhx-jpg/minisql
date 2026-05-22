#include "record/schema.h"


uint32_t Schema::SerializeTo(char *buf) const {
  uint32_t offset = 0;
  MACH_WRITE_UINT32(buf + offset, SCHEMA_MAGIC_NUM);
  offset += sizeof(uint32_t);
  uint32_t column_count = static_cast<uint32_t>(columns_.size());
  MACH_WRITE_UINT32(buf + offset, column_count);
  offset += sizeof(uint32_t);
  for (uint32_t i = 0; i < column_count; i++) {
    offset += columns_[i]->SerializeTo(buf + offset);
  }
  return offset;
}

uint32_t Schema::GetSerializedSize() const {
  uint32_t size = 2 * sizeof(uint32_t);
  for (const auto *col : columns_) {
    size += col->GetSerializedSize();
  }
  return size;
}

uint32_t Schema::DeserializeFrom(char *buf, Schema *&schema) {
  uint32_t offset = 0;
  uint32_t magic_num = MACH_READ_UINT32(buf + offset);
  ASSERT(magic_num == SCHEMA_MAGIC_NUM, "Schema magic number mismatch.");
  offset += sizeof(uint32_t);
  uint32_t column_count = MACH_READ_UINT32(buf + offset);
  offset += sizeof(uint32_t);
  std::vector<Column *> columns;
  for (uint32_t i = 0; i < column_count; i++) {
    Column *col = nullptr;
    offset += Column::DeserializeFrom(buf + offset, col);
    columns.push_back(col);
  }
  schema = new Schema(columns, true);
  return offset;
}