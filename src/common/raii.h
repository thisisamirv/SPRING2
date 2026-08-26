// Small RAII helper for OpenMP locks.
#ifndef SPRING_RAII_H_
#define SPRING_RAII_H_

#include <omp.h>

namespace spring {

class OmpLock {
public:
  OmpLock() { omp_init_lock(&lock_); }
  ~OmpLock() { omp_destroy_lock(&lock_); }
  OmpLock(const OmpLock &) = delete;
  OmpLock &operator=(const OmpLock &) = delete;
  OmpLock(OmpLock &&) = delete;
  OmpLock &operator=(OmpLock &&) = delete;
  omp_lock_t *get() noexcept { return &lock_; }
  const omp_lock_t *get() const noexcept { return &lock_; }

private:
  omp_lock_t lock_;
};

} // namespace spring

#endif // SPRING_RAII_H_
