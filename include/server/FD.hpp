#pragma once
#include <unistd.h>

// RAII wrapper for a file descriptor
// Copying duplicates the underlying descriptor with dup(); assignment closes previous.
class FD {
 public:
  FD() : fd_(-1) {}
  explicit FD(int fd) : fd_(fd) {}
  FD(const FD &other) : fd_(-1) { if (other.fd_ >= 0) fd_ = ::dup(other.fd_); }
  FD &operator=(const FD &other) {
    if (this != &other) {
      closeIfValid();
      fd_ = other.fd_ >= 0 ? ::dup(other.fd_) : -1;
    }
    return *this;
  }
  ~FD() { closeIfValid(); }
  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void reset(int fd) {
    if (fd_ == fd) return;
    closeIfValid();
    fd_ = fd;
  }
  int release() {
    int tmp = fd_;
    fd_ = -1;
    return tmp;
  }
 private:
  void closeIfValid() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }
  int fd_;
};
