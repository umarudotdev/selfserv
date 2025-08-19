#pragma once

#include <unistd.h>

// RAII wrapper for a file descriptor
// Copying duplicates the underlying descriptor with dup(); assignment closes
// previous.
class FD {
 public:
  FD() : m_fd(-1) {}
  explicit FD(int fd) : m_fd(fd) {}
  FD(const FD &other) : m_fd(-1) {
    if (other.m_fd >= 0) m_fd = ::dup(other.m_fd);
  }
  FD &operator=(const FD &other) {
    if (this != &other) {
      CloseIfValid();
      m_fd = other.m_fd >= 0 ? ::dup(other.m_fd) : -1;
    }
    return *this;
  }
  ~FD() { CloseIfValid(); }

  int Get() const { return m_fd; }
  bool Valid() const { return m_fd >= 0; }
  void Reset(int fd) {
    if (m_fd == fd) return;
    CloseIfValid();
    m_fd = fd;
  }
  int Release() {
    int tmp = m_fd;
    m_fd = -1;
    return tmp;
  }

 private:
  void CloseIfValid() {
    if (m_fd >= 0) {
      ::close(m_fd);
      m_fd = -1;
    }
  }

  int m_fd;
};
