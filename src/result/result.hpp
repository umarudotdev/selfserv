#pragma once

#include <cstring>
#include <new>
#include <stdexcept>

namespace selfserv {

/**
 * Custom exception thrown when attempting to unwrap the wrong variant of
 * Result.
 */
class BadResultAccess : public std::runtime_error {
 public:
  explicit BadResultAccess(const char* msg) : std::runtime_error(msg) {}
};

/**
 * Result<T, E> - A C++98 implementation of the Either monad.
 *
 * DESIGN CHOICE: Discriminated union with placement new
 *
 * Similar to Option<T>, we use placement new for the same performance and
 * memory management benefits. However, Result is more complex because it
 * must store either T OR E, not both.
 *
 * APPROACH:
 * 1. Union storage sized for max(sizeof(T), sizeof(E))
 * 2. Boolean discriminator to track which type is active
 * 3. Placement new/explicit destructor for active type management
 *
 * ALIGNMENT CONSIDERATIONS:
 * The union must be aligned for both T and E. We use a double aligner
 * as a reasonable approximation for C++98 (no alignof/alignas).
 *
 * MEMORY LAYOUT:
 * | Union Storage (max(T,E) + alignment) | bool isOk | padding |
 *
 * This ensures type safety while maintaining C++98 compatibility.
 */
template <typename T, typename E>
class Result {
 private:
  // Discriminated union for either T or E
  union Storage {
    char dummy;  // Ensures union is at least 1 byte
    struct {
      char tData[sizeof(T)];
      double tAligner;
    } tStorage;
    struct {
      char eData[sizeof(E)];
      double eAligner;
    } eStorage;
  } m_storage;

  bool m_isOk;  // true = contains T, false = contains E

  // Helper methods for type-safe access
  T* GetTPtr() { return reinterpret_cast<T*>(&m_storage.tStorage.tData[0]); }
  const T* GetTPtr() const {
    return reinterpret_cast<const T*>(&m_storage.tStorage.tData[0]);
  }

  E* GetEPtr() { return reinterpret_cast<E*>(&m_storage.eStorage.eData[0]); }
  const E* GetEPtr() const {
    return reinterpret_cast<const E*>(&m_storage.eStorage.eData[0]);
  }

 public:
  /**
   * Constructor for Ok state - contains a success value of type T.
   */
  explicit Result(const T& value) : m_isOk(true) { new (GetTPtr()) T(value); }

  /**
   * Copy constructor - implements Rule of Three.
   * Copies whichever variant is active in the source Result.
   */
  Result(const Result& other) : m_isOk(other.m_isOk) {
    if (m_isOk) {
      new (GetTPtr()) T(*other.GetTPtr());
    } else {
      new (GetEPtr()) E(*other.GetEPtr());
    }
  }

  /**
   * Assignment operator - implements Rule of Three.
   * Handles destruction of current state and construction of new state.
   */
  Result& operator=(const Result& other) {
    if (this != &other) {  // Self-assignment guard
      // Destroy current active object
      if (m_isOk) {
        GetTPtr()->~T();
      } else {
        GetEPtr()->~E();
      }

      // Copy construct new active object
      m_isOk = other.m_isOk;
      if (m_isOk) {
        new (GetTPtr()) T(*other.GetTPtr());
      } else {
        new (GetEPtr()) E(*other.GetEPtr());
      }
    }
    return *this;
  }

  /**
   * Destructor - implements Rule of Three.
   * Destroys whichever variant is currently active.
   */
  ~Result() {
    if (m_isOk) {
      GetTPtr()->~T();
    } else {
      GetEPtr()->~E();
    }
  }

  /**
   * Static factory method for creating Ok result.
   */
  static Result Ok(const T& value) { return Result(value); }

  /**
   * Static factory method for creating Err result.
   * We need a separate private constructor for this.
   */
  static Result Err(const E& error) {
    Result result;
    result.m_isOk = false;
    new (result.GetEPtr()) E(error);
    return result;
  }

  /**
   * Check if Result contains a success value.
   */
  bool IsOk() const { return m_isOk; }

  /**
   * Check if Result contains an error value.
   */
  bool IsErr() const { return !m_isOk; }

  /**
   * Extract the success value.
   * @throws BadResultAccess if Result contains an error
   */
  T& Unwrap() {
    if (!m_isOk) {
      throw BadResultAccess("Called unwrap() on Err Result");
    }
    return *GetTPtr();
  }

  /**
   * Extract the success value (const version).
   * @throws BadResultAccess if Result contains an error
   */
  const T& Unwrap() const {
    if (!m_isOk) {
      throw BadResultAccess("Called unwrap() on Err Result");
    }
    return *GetTPtr();
  }

  /**
   * Extract the error value.
   * @throws BadResultAccess if Result contains a success value
   */
  E& UnwrapErr() {
    if (m_isOk) {
      throw BadResultAccess("Called unwrapErr() on Ok Result");
    }
    return *GetEPtr();
  }

  /**
   * Extract the error value (const version).
   * @throws BadResultAccess if Result contains a success value
   */
  const E& UnwrapErr() const {
    if (m_isOk) {
      throw BadResultAccess("Called unwrapErr() on Ok Result");
    }
    return *GetEPtr();
  }

  /**
   * Get pointer to success value, or NULL if error.
   */
  T* Get() { return m_isOk ? GetTPtr() : NULL; }

  /**
   * Get pointer to success value, or NULL if error (const version).
   */
  const T* Get() const { return m_isOk ? GetTPtr() : NULL; }

  /**
   * Get pointer to error value, or NULL if success.
   */
  E* GetErr() { return m_isOk ? NULL : GetEPtr(); }

  /**
   * Get pointer to error value, or NULL if success (const version).
   */
  const E* GetErr() const { return m_isOk ? NULL : GetEPtr(); }

  /**
   * Extract success value or return default if error.
   */
  T UnwrapOr(const T& defaultValue) const {
    return m_isOk ? *GetTPtr() : defaultValue;
  }

 private:
  /**
   * Private default constructor for internal use by Err factory.
   */
  Result() : m_isOk(true) {
    // Don't construct anything yet - factory methods will handle it
  }
};

}  // namespace selfserv
