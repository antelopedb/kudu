// Copyright (c) 2013, Cloudera, inc.

#include <algorithm>
#include <glog/logging.h>

#include "cfile/cfile.h"
#include "cfile/string_plain_block.h"
#include "common/columnblock.h"
#include "gutil/stringprintf.h"
#include "util/coding.h"
#include "util/coding-inl.h"
#include "util/group_varint-inl.h"
#include "util/hexdump.h"
#include "util/memory/arena.h"

namespace kudu {
namespace cfile {

StringPlainBlockBuilder::StringPlainBlockBuilder(const WriterOptions *options) :
  end_of_data_offset_(0),
  size_estimate_(0),
  options_(options)
{
  Reset();
}

void StringPlainBlockBuilder::Reset(){
  offsets_.clear();
  buffer_.clear();
  buffer_.resize(kHeaderSize);
  buffer_.reserve(options_->block_size);

  size_estimate_ = kHeaderSize;
  end_of_data_offset_ = kHeaderSize;
  finished_ = false;
}

Slice StringPlainBlockBuilder::Finish(uint32_t ordinal_pos) {
  finished_ = true;

  size_t offsets_pos = buffer_.size();

  // Set up the header
  InlineEncodeFixed32(&buffer_[0], ordinal_pos);
  InlineEncodeFixed32(&buffer_[4], offsets_.size());
  InlineEncodeFixed32(&buffer_[8], offsets_pos);

  // append the offsets
  coding::AppendGroupVarInt32Sequence(&buffer_, 0, &offsets_[0], offsets_.size());

  return Slice(buffer_);
}

int StringPlainBlockBuilder::Add(const uint8_t *vals,
                                 size_t count,
                                 size_t stride) {
  DCHECK(!finished_);
  DCHECK_GT(count, 0);
  if (count > 1) {
    DCHECK_GE(stride, sizeof(Slice));
  }

  size_t i;
  for (i = 0; i < count; i++) {

    // Every fourth entry needs a gvint selector byte
    // TODO: does it cost a lot to account these things specifically?
    // maybe cheaper to just over-estimate - allocation is cheaper than math?
    if (offsets_.size() % 4 == 0) {
      size_estimate_++;
    }

    const Slice *src = reinterpret_cast<const Slice *>(vals);
    size_t offset = buffer_.size();
    offsets_.push_back(offset);
    size_estimate_ += coding::CalcRequiredBytes32(offset);

    buffer_.append(src->data(), src->size());
    size_estimate_ += src->size();

    vals += stride;
  }

  end_of_data_offset_ = buffer_.size();

  return i;
}

uint64_t StringPlainBlockBuilder::EstimateEncodedSize() const {
  return size_estimate_;
}

uint64_t StringPlainBlockBuilder::Count() const {
  return offsets_.size();
}

Status StringPlainBlockBuilder::GetFirstKey(void *key_void) const {
  CHECK(finished_);

  Slice *slice = reinterpret_cast<Slice *>(key_void);

  if (offsets_.empty()) {
    return Status::NotFound("no keys in data block");
  }

  if (PREDICT_FALSE(offsets_.size() == 1)) {
    *slice = Slice(&buffer_[kHeaderSize],
                   end_of_data_offset_ - kHeaderSize);
  } else {
    *slice = Slice(&buffer_[kHeaderSize],
                   offsets_[1] - offsets_[0]);
  }
  return Status::OK();
}

////////////////////////////////////////////////////////////
// Decoding
////////////////////////////////////////////////////////////

StringPlainBlockDecoder::StringPlainBlockDecoder(const Slice &slice) :
  data_(slice),
  parsed_(false),
  num_elems_(0),
  ordinal_pos_base_(0),
  cur_idx_(0)
{}

Status StringPlainBlockDecoder::ParseHeader() {
  CHECK(!parsed_);

  if (data_.size() < StringPlainBlockBuilder::kHeaderSize) {
    return Status::Corruption("not enough bytes for header in StringPlainBlockDecoder");
  }

  // Decode header.
  ordinal_pos_base_  = DecodeFixed32(&data_[0]);
  num_elems_         = DecodeFixed32(&data_[4]);
  size_t offsets_pos = DecodeFixed32(&data_[8]);

  // Sanity check.
  if (offsets_pos > data_.size()) {
    return Status::Corruption(
      StringPrintf("offsets_pos %ld > block size %ld in plain string block",
                   offsets_pos, data_.size()));
  }

  // Decode the string offsets themselves
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&data_[offsets_pos]);
  const uint8_t *limit = reinterpret_cast<const uint8_t *>(data_.data() + data_.size());

  offsets_.clear();
  offsets_.reserve(num_elems_);

  size_t rem = num_elems_;
  while (rem >= 4) {
    uint32_t ints[4];
    p = coding::DecodeGroupVarInt32_SSE(p, &ints[0], &ints[1], &ints[2], &ints[3]);
    if (p > limit) {
      LOG(WARNING) << "bad block: " << HexDump(data_);
      return Status::Corruption(
        StringPrintf("unable to decode offsets in block"));
    }

    offsets_.push_back(ints[0]);
    offsets_.push_back(ints[1]);
    offsets_.push_back(ints[2]);
    offsets_.push_back(ints[3]);
    rem -= 4;
  }

  if (rem > 0) {
    uint32_t ints[4];
    p = coding::DecodeGroupVarInt32_SSE(p, &ints[0], &ints[1], &ints[2], &ints[3]);
    if (p > limit) {
      LOG(WARNING) << "bad block: " << HexDump(data_);
      return Status::Corruption(
        StringPrintf("unable to decode offsets in block"));
    }

    for (int i = 0; i < rem; i++) {
      offsets_.push_back(ints[i]);
    }
  }

  // Add one extra entry pointing after the last item to make the indexing easier.
  offsets_.push_back(offsets_pos);

  parsed_ = true;

  return Status::OK();
}

void StringPlainBlockDecoder::SeekToPositionInBlock(uint pos) {
  DCHECK_LT(pos, num_elems_);
  cur_idx_ = pos;
}

Status StringPlainBlockDecoder::SeekAtOrAfterValue(const void *value_void, bool *exact) {
  DCHECK(value_void != NULL);

  const Slice &target = *reinterpret_cast<const Slice *>(value_void);

  // Binary search in restart array to find the first restart point
  // with a key >= target
  int32_t left = 0;
  int32_t right = num_elems_;
  while (left != right) {
    uint32_t mid = (left + right) / 2;
    Slice mid_key(string_at_index(mid));
    int c = mid_key.compare(target);
    if (c < 0) {
      left = mid + 1;
    } else if (c > 0) {
      right = mid;
    } else {
      cur_idx_ = mid;
      *exact = true;
      return Status::OK();
    }
  }
  *exact = false;
  cur_idx_ = left;
  if (cur_idx_ == num_elems_) {
    return Status::NotFound("after last key in block");
  }

  return Status::OK();
}

Status StringPlainBlockDecoder::CopyNextValues(size_t *n, ColumnBlock *dst) {
  DCHECK(parsed_);
  CHECK_EQ(dst->type_info().type(), STRING);
  DCHECK_LE(*n, dst->size());

  Arena *out_arena = dst->arena();
  if (PREDICT_FALSE(*n == 0 || cur_idx_ >= num_elems_)) {
    *n = 0;
    return Status::OK();
  }

  size_t max_fetch = std::min(*n, static_cast<size_t>(num_elems_ - cur_idx_));

  uint8_t *out = reinterpret_cast<uint8_t *>(dst->data());
  for (size_t i = 0; i < max_fetch; i++) {
    Slice elem(string_at_index(cur_idx_));

    // TODO: in a lot of cases, we might be able to get away with the decoder
    // owning it and not truly copying. But, we should extend the CopyNextValues
    // API so that the caller can specify if they truly _need_ copies or not.
    CHECK( out_arena->RelocateSlice(elem, reinterpret_cast<Slice *>(out)) );
    out += dst->stride();
    cur_idx_++;
  }

  return Status::OK();
}

Slice StringPlainBlockDecoder::string_at_index(size_t idx) const {
  const uint32_t offset = offsets_[idx];
  uint32_t len = offsets_[idx + 1] - offset;
  return Slice(&data_[offset], len);
}

} // namespace cfile
} // namespace kudu
