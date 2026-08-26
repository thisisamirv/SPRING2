// Declares the progress tracking functionality for terminal output, including
// formatted bars, logging interfaces, and ETA calculations.

#ifndef SPRING_PROGRESS_H
#define SPRING_PROGRESS_H

#include <mutex>
#include <string>

namespace spring {

enum class log_level : uint8_t { quiet = 0, info = 1, debug = 2 };

class Logger {
public:
  static void set_level(log_level level);
  static bool is_info_enabled();
  static bool is_debug_enabled();

  static void log_info(const std::string &msg);
  static void log_debug(const std::string &msg);
  static void log_warning(const std::string &msg);
  static void log_error(const std::string &msg);
};

class ProgressBar {
public:
  ProgressBar(bool enabled);
  ~ProgressBar();

  void set_stage(const std::string &label, float start_pct, float end_pct);
  void update(float stage_progress); // stage_progress is 0.0 to 1.0
  void finalize();

  static ProgressBar *GlobalInstance();
  static void SetGlobalInstance(ProgressBar *instance);

private:
  void render(float global_progress);

  bool enabled_;
  std::string current_label_;
  float stage_start_ = 0.0f;
  float stage_end_ = 1.0f;
  float last_rendered_pct_ = -1.0f;
  bool finalized_ = false;
  std::mutex mutex_;
};

} // namespace spring

#define SPRING_LOG_INFO(msg)                                                   \
  do {                                                                         \
    if (spring::Logger::is_info_enabled())                                     \
      spring::Logger::log_info((msg));                                         \
  } while (false)

#define SPRING_LOG_DEBUG(msg)                                                  \
  do {                                                                         \
    if (spring::Logger::is_debug_enabled())                                    \
      spring::Logger::log_debug((msg));                                        \
  } while (false)

#endif // SPRING_PROGRESS_H
