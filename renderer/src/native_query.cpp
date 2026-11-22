/**
 * Sleeping Dogs port (SDNX): native renderer, occlusion queries.
 *
 * Direct3D on the Xbox 360 issues EVENT_WRITE_ZPD at the query begin and end, with RB_SAMPLE_COUNT_ADDR pointing
 * at a sample-count block; at the end it has written 0xFFFFFEED into ZPass (or ZFail) and waits for the GPU to
 * replace it. The base command processor answers with a fixed count, so everything tested by a query is visible
 * (light coronas through walls). Here the draws between begin and end are counted with Vulkan occlusion queries;
 * the guest is answered at once with the last count known for that address, and the count of this query is read
 * back when its frame has completed - no stall, a lag of 1-3 frames that the fading coronas hide.
 */
#include <rex/graphics/sdnx/native_command_processor.h>


#include <algorithm>
#include <cstring>

#include <rex/graphics/register_file.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/memory/ring_buffer.h>

namespace rex::graphics::sdnx {

namespace {
constexpr uint32_t kFirstFrameSamples = 1000;  // before a real count exists: visible, as the base class
}

// Pools and readback buffers are the backend's (kMaxQueries per slot); here the guest queries are mapped onto them.
bool NativeCommandProcessor::InitializeQueries() {
  static_assert(kMaxQueries == GpuBackend::kMaxQueries);
  return true;
}

void NativeCommandProcessor::ShutdownQueries() {}

void NativeCommandProcessor::BeginHostQuery() {
  uint32_t slot = uint32_t(frame_ % kFramesInFlight);
  if (!guest_query_active_ || host_query_open_ || !rendering_ || query_used_[slot] >= kMaxQueries) return;
  uint32_t index = query_used_[slot];
  if (!backend_->BeginQuery(slot, index)) return;  // no queries: the guest gets the fixed count
  ++query_used_[slot];
  guest_query_indices_.push_back(index);
  host_query_open_ = true;
}

void NativeCommandProcessor::EndHostQuery() {
  if (!host_query_open_) return;
  backend_->EndQuery(uint32_t(frame_ % kFramesInFlight), guest_query_indices_.back());
  host_query_open_ = false;
}

void NativeCommandProcessor::ResetQueries(uint32_t slot) {
  // The slot's frame has completed (its fence was waited for): its counts are in the readback buffer.
  const uint64_t* results = backend_->QueryResults(slot);
  for (const PendingQuery& query : pending_queries_[slot]) {
    uint64_t samples = 0;
    for (uint32_t index : query.indices) samples += results ? results[index] : 0;
    query_results_[query.address] = uint32_t(std::min<uint64_t>(samples, 0xFFFFFFFFu));
  }
  pending_queries_[slot].clear();
  backend_->ResetQueries(slot, kMaxQueries);
  query_used_[slot] = 0;
}

void NativeCommandProcessor::CopyQueryResults(uint32_t slot) {
  // Called outside rendering, before the frame is submitted. A guest query still open goes on in the next frame
  // with that frame's pool; what was counted here is dropped (rare: queries are short).
  if (guest_query_active_) guest_query_indices_.clear();
  if (!query_used_[slot]) return;
  backend_->ResolveQueries(slot, query_used_[slot]);
  transfer_pending_ = true;  // see BeginPass: the next rendering pass needs a full barrier
}

bool NativeCommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader, uint32_t packet,
                                                               uint32_t count) {
  (void)packet;
  (void)count;
  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEEDu);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
  uint32_t address = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* counts = memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(address);
  if (!counts) return true;
  bool is_end = counts->ZPass_A == kQueryFinished || counts->ZPass_B == kQueryFinished ||
                counts->ZFail_A == kQueryFinished || counts->ZFail_B == kQueryFinished;
  std::memset(counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
  if (!is_end) {
    // Begin (the begin block stays zero: visibility = end - begin).
    EnsureFrame();
    if (guest_query_active_) EndHostQuery();
    guest_query_active_ = true;
    guest_query_address_ = address;
    guest_query_indices_.clear();
    BeginHostQuery();  // when a rendering pass is open; otherwise when the next one begins
    return true;
  }
  // End: the guest gets the last known count of this address now.
  auto known = query_results_.find(address);
  uint32_t samples = known != query_results_.end() ? known->second : kFirstFrameSamples;
  ++qdiag_queries_;  // §16.161
  if (samples) ++qdiag_visible_;
  if (known == query_results_.end()) ++qdiag_first_;
  counts->ZPass_A = samples;
  counts->Total_A = samples;
  if (guest_query_active_) {
    EnsureFrame();
    EndHostQuery();
    if (!guest_query_indices_.empty()) {
      pending_queries_[uint32_t(frame_ % kFramesInFlight)].push_back({address, std::move(guest_query_indices_)});
    } else if (known == query_results_.end() && backend_->QueryResults(uint32_t(frame_ % kFramesInFlight))) {
      query_results_[address] = 0;  // nothing was drawn while it was open
    }
    guest_query_indices_.clear();
    guest_query_active_ = false;
  }
  return true;
}

}  // namespace rex::graphics::sdnx
