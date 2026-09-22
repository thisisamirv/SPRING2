// Streaming archive reader utilities and DecompressionSink implementations
// used to pull records out of SPRING archives for library-style consumers.
#include "archive_stream_reader.h"
#include "archive_record_reconstruction.h"
#include "common/bundle_manifest.h"
#include "fs_utils.h"
#include "integrity_utils.h"
#include "params.h"
#include "progress.h"
#include "workflow_internal.h"
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>

namespace spring {

namespace {

class SpringReaderShutdown final : public std::exception {
public:
  const char *what() const noexcept override {
    return "SpringReader shutdown requested";
  }
};

/**
 * @brief Sink implementation that buffers records into a thread-safe queue.
 */
class BufferDecompressionSink : public DecompressionSink {
public:
  using StepPair = std::pair<std::vector<ReadRecord>, std::vector<ReadRecord>>;

  BufferDecompressionSink(std::queue<StepPair> &queue, std::mutex &mutex,
                          std::condition_variable &cv, bool &shutdown_requested,
                          bool paired_end, size_t max_queue_size)
      : queue_(queue), mutex_(mutex), cv_(cv),
        shutdown_requested_(shutdown_requested), paired_end_(paired_end),
        max_queue_size_(max_queue_size) {}

  void consume_step(std::string *id_buffer, std::string *read_buffer,
                    const std::string *quality_buffer, uint32_t count,
                    int stream_index) override {

    std::unique_lock<std::mutex> lock(mutex_.get());
    const auto wait_start = std::chrono::steady_clock::now();
    const bool was_full = queue_.get().size() >= max_queue_size_;
    // Throttle decompression if the queue is full
    cv_.get().wait(lock, [this] {
      return queue_.get().size() < max_queue_size_ || shutdown_requested_.get();
    });
    if (shutdown_requested_.get()) {
      throw SpringReaderShutdown();
    }
    if (was_full) {
      wait_events_++;
      wait_ns_total_ += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - wait_start)
              .count());
    }

    for (uint32_t i = 0; i < count; ++i) {
      update_record_crc(sequence_crc_[stream_index], read_buffer[i]);
      if (quality_buffer) {
        update_record_crc(quality_crc_[stream_index], quality_buffer[i]);
      }
      update_record_crc(id_crc_[stream_index], id_buffer[i]);
    }

    if (stream_index == 0) {
      current_step_.first.clear();
      current_step_.first.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        ReadRecord rec;
        rec.id = id_buffer[i];
        rec.sequence = read_buffer[i];
        if (quality_buffer)
          rec.quality = quality_buffer[i];
        current_step_.first.push_back(std::move(rec));
      }
      if (!paired_end_) {
        queue_.get().push(std::move(current_step_));
        max_queue_depth_ = std::max(max_queue_depth_,
                                    static_cast<uint64_t>(queue_.get().size()));
        steps_pushed_++;
        records_pushed_ += count;
        current_step_ = {}; // Reset
        lock.unlock();
        cv_.get().notify_all();
      }
    } else {
      // stream_index == 1
      current_step_.second.clear();
      current_step_.second.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        ReadRecord rec;
        rec.id = id_buffer[i];
        rec.sequence = read_buffer[i];
        if (quality_buffer)
          rec.quality = quality_buffer[i];
        current_step_.second.push_back(std::move(rec));
      }
      queue_.get().push(std::move(current_step_));
      max_queue_depth_ = std::max(max_queue_depth_,
                                  static_cast<uint64_t>(queue_.get().size()));
      steps_pushed_++;
      records_pushed_ += count;
      current_step_ = {}; // Reset
      lock.unlock();
      cv_.get().notify_all();
    }

    if ((steps_pushed_ > 0) && (steps_pushed_ % 128 == 0)) {
      SPRING_LOG_DEBUG("block_id=spring-reader:sink, SpringReader sink "
                       "progress: pushed_steps=" +
                       std::to_string(steps_pushed_) +
                       ", pushed_records=" + std::to_string(records_pushed_) +
                       ", backpressure_events=" + std::to_string(wait_events_) +
                       ", max_queue_depth=" + std::to_string(max_queue_depth_));
    }
  }

  void log_summary() const {
    const uint64_t wait_ms = wait_ns_total_ / 1000000ULL;
    SPRING_LOG_DEBUG("block_id=spring-reader:sink, SpringReader sink summary: "
                     "pushed_steps=" +
                     std::to_string(steps_pushed_) +
                     ", pushed_records=" + std::to_string(records_pushed_) +
                     ", backpressure_events=" + std::to_string(wait_events_) +
                     ", backpressure_wait_ms=" + std::to_string(wait_ms) +
                     ", max_queue_depth=" + std::to_string(max_queue_depth_));
  }

  void copy_digests(uint32_t seq_crc[2], uint32_t qual_crc[2],
                    uint32_t id_crc[2]) const {
    for (int i = 0; i < 2; ++i) {
      seq_crc[i] = sequence_crc_[i];
      qual_crc[i] = quality_crc_[i];
      id_crc[i] = id_crc_[i];
    }
  }

private:
  std::reference_wrapper<std::queue<StepPair>> queue_;
  std::reference_wrapper<std::mutex> mutex_;
  std::reference_wrapper<std::condition_variable> cv_;
  std::reference_wrapper<bool> shutdown_requested_;
  bool paired_end_;
  size_t max_queue_size_;
  StepPair current_step_;
  uint64_t steps_pushed_ = 0;
  uint64_t records_pushed_ = 0;
  uint64_t wait_events_ = 0;
  uint64_t wait_ns_total_ = 0;
  uint64_t max_queue_depth_ = 0;
};

} // namespace

class SpringReader::Impl {
public:
  // Disallow copying and moving for safety of background thread and resources.
  Impl(const Impl &) = delete;
  Impl &operator=(const Impl &) = delete;
  Impl(Impl &&) = delete;
  Impl &operator=(Impl &&) = delete;

  Impl(std::string archive_path, int num_thr)
      : archive_path_(std::move(archive_path)), user_num_thr_(num_thr) {
    artifact_.scratch_dir.clear();
    const auto manifest_contents =
        read_files_from_tar_memory(archive_path_, {kBundleManifestName});
    if (manifest_contents.contains(kBundleManifestName)) {
      const bundle_manifest manifest = read_bundle_manifest_from_string(
          manifest_contents.at(kBundleManifestName));
      const auto grouped_archives = read_files_from_tar_memory(
          archive_path_, {manifest.read_archive_name});
      auto read_archive_it = grouped_archives.find(manifest.read_archive_name);
      if (read_archive_it == grouped_archives.end()) {
        throw std::runtime_error(
            "Failed to read grouped archive metadata: read archive not found.");
      }

      artifact_.files = read_all_files_from_tar_bytes(read_archive_it->second);
    } else {
      artifact_.files = read_all_files_from_tar_memory(archive_path_);
    }

    if (!artifact_.contains("cp.bin")) {
      SPRING_LOG_DEBUG("block_id=spring-reader:metadata, SpringReader metadata "
                       "open failure: path=cp.bin, expected_bytes=1, "
                       "actual_bytes=0, index=0");
      throw std::runtime_error("Failed to read archive metadata.");
    }
    try {
      read_archive_compression_params(artifact_, params_);
    } catch (const std::exception &) {
      throw std::runtime_error("Failed to parse archive metadata.");
    }
    const archive_decompression_plan decompression_plan =
        build_archive_decompression_plan(params_);
    ensure_archive_decompression_plan_supported(decompression_plan);

    decode_num_thr_ =
        (user_num_thr_ > 0) ? user_num_thr_ : params_.encoding.num_thr;

    SPRING_LOG_DEBUG(
        "block_id=spring-reader:init, SpringReader init: archive=" +
        archive_path_ + ", paired_end=" +
        std::string(params_.encoding.paired_end ? "true" : "false") +
        ", long_mode=" +
        std::string(params_.encoding.long_flag ? "true" : "false") +
        ", archive_format_version=" +
        std::to_string(decompression_plan.archive_format_version) +
        ", compressor_version=" + decompression_plan.compressor_version +
        ", decompression_route=" +
        archive_decompression_route_name(decompression_plan) +
        ", encoding_threads=" + std::to_string(params_.encoding.num_thr) +
        ", decoding_threads=" + std::to_string(decode_num_thr_));

    // Start worker thread
    worker_thread_ = std::thread([this, decompression_plan]() {
      try {
        const auto worker_start = std::chrono::steady_clock::now();
        BufferDecompressionSink sink(queue_, mutex_, cv_, shutdown_requested_,
                                     params_.encoding.paired_end, 2);
        execute_archive_decompression_plan(artifact_, sink, params_,
                                           decompression_plan);
        sink.log_summary();
        sink.copy_digests(sequence_crc_, quality_crc_, id_crc_);
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - worker_start)
                .count();
        SPRING_LOG_DEBUG("block_id=spring-reader:worker, SpringReader worker "
                         "finished: elapsed_ms=" +
                         std::to_string(elapsed_ms));

        {
          std::scoped_lock<std::mutex> lock(mutex_);
          digests_ready_ = true;
          worker_done_ = true;
        }
        cv_.notify_all();
      } catch (const SpringReaderShutdown &) {
        std::scoped_lock<std::mutex> lock(mutex_);
        worker_done_ = true;
        cv_.notify_all();
      } catch (...) {
        std::scoped_lock<std::mutex> lock(mutex_);
        SPRING_LOG_DEBUG(
            "block_id=spring-reader:worker, SpringReader worker error branch: "
            "archive=" +
            archive_path_ +
            ", queued_batches=" + std::to_string(queue_.size()) +
            ", popped_batches=" + std::to_string(queue_batches_popped_));
        worker_exception_ = std::current_exception();
        worker_done_ = true;
        cv_.notify_all();
      }
    });
  }

  ~Impl() {
    {
      std::scoped_lock<std::mutex> lock(mutex_);
      shutdown_requested_ = true;
    }
    cv_.notify_all();
    if (worker_thread_.joinable())
      worker_thread_.join();
  }

  bool next(ReadRecord &mate1, ReadRecord *mate2) {
    for (;;) {
      if (!current_step_cache_.first.empty() &&
          cache_pos_ < current_step_cache_.first.size()) {
        mate1 = std::move(current_step_cache_.first[cache_pos_]);
        if (mate2)
          *mate2 = std::move(current_step_cache_.second[cache_pos_]);
        cache_pos_++;

        const bool empty_mate1 = mate1.id.empty() && mate1.sequence.empty();
        const bool empty_mate2 =
            !mate2 || (mate2->id.empty() && mate2->sequence.empty());
        if (empty_mate1 && empty_mate2) {
          continue;
        }

        return true;
      }

      // Try to get next batch
      std::unique_lock<std::mutex> lock(mutex_);
      const bool queue_was_empty = queue_.empty() && !worker_done_;
      const auto wait_start = std::chrono::steady_clock::now();
      cv_.wait(lock, [this] { return !queue_.empty() || worker_done_; });
      if (queue_was_empty) {
        consumer_wait_events_++;
        consumer_wait_ns_total_ += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - wait_start)
                .count());
      }

      if (worker_exception_) {
        SPRING_LOG_DEBUG(
            "block_id=spring-reader:consumer, SpringReader consumer rethrow: "
            "archive=" +
            archive_path_ +
            ", popped_batches=" + std::to_string(queue_batches_popped_) +
            ", popped_records=" + std::to_string(queue_records_popped_) +
            ", queued_batches=" + std::to_string(queue_.size()));
        std::rethrow_exception(worker_exception_);
      }

      if (queue_.empty() && worker_done_)
        return false;

      current_step_cache_ = std::move(queue_.front());
      queue_.pop();
      queue_batches_popped_++;
      queue_records_popped_ += current_step_cache_.first.size();
      lock.unlock();
      cv_.notify_all(); // Notify worker that space is available

      if ((queue_batches_popped_ > 0) && (queue_batches_popped_ % 128 == 0)) {
        SPRING_LOG_DEBUG(
            "block_id=spring-reader:consumer, SpringReader consumer progress: "
            "popped_batches=" +
            std::to_string(queue_batches_popped_) +
            ", popped_records=" + std::to_string(queue_records_popped_) +
            ", wait_events=" + std::to_string(consumer_wait_events_) +
            ", wait_ms=" +
            std::to_string(consumer_wait_ns_total_ / 1000000ULL));
      }

      cache_pos_ = 0;
      if (current_step_cache_.first.empty())
        return false;
    }
  }

  [[nodiscard]] const compression_params &params() const { return params_; }

  void get_digests(uint32_t seq_crc[2], uint32_t qual_crc[2],
                   uint32_t id_crc[2]) {
    std::scoped_lock<std::mutex> lock(mutex_);
    const bool cache_drained = current_step_cache_.first.empty() ||
                               cache_pos_ >= current_step_cache_.first.size();
    if (worker_exception_) {
      std::rethrow_exception(worker_exception_);
    }
    if (!worker_done_ || !queue_.empty() || !cache_drained || !digests_ready_) {
      throw std::runtime_error(
          "SpringReader digests are only available after the archive has "
          "been fully consumed.");
    }
    for (int i = 0; i < 2; ++i) {
      seq_crc[i] = sequence_crc_[i];
      qual_crc[i] = quality_crc_[i];
      id_crc[i] = id_crc_[i];
    }
  }

private:
  std::string archive_path_;
  decompression_archive_artifact artifact_;
  compression_params params_;
  int user_num_thr_;
  int decode_num_thr_ = 1;

  std::thread worker_thread_;
  std::queue<BufferDecompressionSink::StepPair> queue_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool shutdown_requested_ = false;
  bool worker_done_ = false;
  bool digests_ready_ = false;
  std::exception_ptr worker_exception_ = nullptr;
  uint32_t sequence_crc_[2] = {0, 0};
  uint32_t quality_crc_[2] = {0, 0};
  uint32_t id_crc_[2] = {0, 0};
  uint64_t consumer_wait_events_ = 0;
  uint64_t consumer_wait_ns_total_ = 0;
  uint64_t queue_batches_popped_ = 0;
  uint64_t queue_records_popped_ = 0;

  BufferDecompressionSink::StepPair current_step_cache_;
  size_t cache_pos_ = 0;
};

SpringReader::SpringReader(const std::string &archive_path, int num_thr)
    : impl_(std::make_unique<Impl>(archive_path, num_thr)) {}

SpringReader::~SpringReader() = default;

const compression_params &SpringReader::params() const {
  return impl_->params();
}

bool SpringReader::next(ReadRecord &record) {
  if (impl_->params().encoding.paired_end) {
    SPRING_LOG_DEBUG("block_id=spring-reader:api, SpringReader API mode "
                     "mismatch: next(record) called for paired-end archive");
    throw std::runtime_error("Archive is paired-end, use next(mate1, mate2)");
  }
  return impl_->next(record, nullptr);
}

bool SpringReader::next(ReadRecord &mate1, ReadRecord &mate2) {
  if (!impl_->params().encoding.paired_end) {
    SPRING_LOG_DEBUG(
        "block_id=spring-reader:api, SpringReader API mode mismatch: "
        "next(mate1,mate2) called for single-end archive");
    throw std::runtime_error("Archive is single-end, use next(record)");
  }
  return impl_->next(mate1, &mate2);
}

void SpringReader::get_digests(uint32_t seq_crc[2], uint32_t qual_crc[2],
                               uint32_t id_crc[2]) {
  impl_->get_digests(seq_crc, qual_crc, id_crc);
}

} // namespace spring
