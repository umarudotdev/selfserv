#pragma once

#include <cstring>
#include <new>
#include <stdexcept>

namespace selfserv {

/**
 * Custom exception thrown when attempting to unwrap an empty Option.
 */
class BadOptionAccess : public std::runtime_error {
 public:
  explicit BadOptionAccess(const char* msg) : std::runtime_error(msg) {}
};

/**
 * Option<T> - A C++98 implementation of the Maybe monad.
 *
 * DESIGN CHOICE: Placement new with aligned storage + boolean flag
 *
 * We chose placement new over heap allocation (T*) for several reasons:
 *
 * 1. PERFORMANCE: Eliminates heap allocation overhead - no malloc/free calls.
 *    The value is stored directly within the Option object's memory layout.
 *
 * 2. MEMORY FRAGMENTATION: Avoids heap fragmentation that would occur with
 *    frequent allocation/deallocation of small T objects.
 *
 * 3. CACHE LOCALITY: Better cache performance since the value is stored
 *    contiguously with the Option metadata.
 *
 * 4. EXCEPTION SAFETY: In C++98, heap allocation can throw std::bad_alloc.
 *    Our approach only throws during T's constructor, which is more
 * predictable.
 *
 * TRADE-OFFS:
 * - COMPLEXITY: Requires manual destructor calls and placement new expertise.
 * - SIZE: Option<T> is always sizeof(T) + alignment + bool, even when empty.
 * - ALIGNMENT: Must ensure proper alignment for any type T.
 *
 * For a mission-critical 2005 application, these trade-offs favor reliability
 * and performance over memory compactness.
 */
template <typename T>
class Option {
 private:
  // Aligned storage for T - ensures proper alignment for any type
  union Storage {
    char dummy;  // Ensures Storage is at least 1 byte
    // Use char array with proper alignment. In C++98, we approximate
    // alignment requirements. For most types, double alignment suffices.
    // For more precise alignment, we'd need compiler-specific attributes.
    struct {
      char data[sizeof(T)];
      double aligner;  // Forces alignment suitable for most types
    } aligned;
  } m_storage;

  bool m_hasValue;

  // Helper to get typed pointer to storage
  T* GetPtr() { return reinterpret_cast<T*>(&m_storage.aligned.data[0]); }
  const T* GetPtr() const {
    return reinterpret_cast<const T*>(&m_storage.aligned.data[0]);
  }

 public:
  /**
   * Default constructor - creates an empty Option (None state).
   */
  Option() : m_hasValue(false) {
    // No need to initialize storage for empty Option
  }

  /**
   * Value constructor - creates an Option containing the given value (Some
   * state). Uses copy constructor of T.
   */
  explicit Option(const T& value) : m_hasValue(true) {
    new (GetPtr()) T(value);  // Placement new with copy constructor
  }

  /**
   * Copy constructor - implements Rule of Three.
   * Performs deep copy of the contained value if present.
   */
  Option(const Option& other) : m_hasValue(other.m_hasValue) {
    if (m_hasValue) {
      new (GetPtr()) T(*other.GetPtr());  // Copy construct T
    }
  }

  /**
   * Assignment operator - implements Rule of Three.
   * Handles self-assignment and properly destroys/constructs values.
   */
  Option& operator=(const Option& other) {
    if (this != &other) {  // Self-assignment guard
      if (m_hasValue) {
        GetPtr()->~T();  // Destroy current value
        m_hasValue = false;
      }

      if (other.m_hasValue) {
        new (GetPtr()) T(*other.GetPtr());  // Copy construct new value
        m_hasValue = true;
      }
    }
    return *this;
  }

  /**
   * Destructor - implements Rule of Three.
   * Properly destroys contained value if present.
   */
  ~Option() {
    if (m_hasValue) {
      GetPtr()->~T();  // Explicit destructor call
    }
  }

  /**
   * Static factory method for creating empty Option.
   */
  static Option None() { return Option(); }

  /**
   * Static factory method for creating Option with value.
   */
  static Option Some(const T& value) { return Option(value); }

  /**
   * Check if Option contains a value.
   */
  bool IsSome() const { return m_hasValue; }

  /**
   * Check if Option is empty.
   */
  bool IsNone() const { return !m_hasValue; }

  /**
   * Extract the contained value.
   * @throws BadOptionAccess if Option is empty
   */
  T& Unwrap() {
    if (!m_hasValue) {
      throw BadOptionAccess("Called unwrap() on empty Option");
    }
    return *GetPtr();
  }

  /**
   * Extract the contained value (const version).
   * @throws BadOptionAccess if Option is empty
   */
  const T& Unwrap() const {
    if (!m_hasValue) {
      throw BadOptionAccess("Called unwrap() on empty Option");
    }
    return *GetPtr();
  }

  /**
   * Get pointer to contained value, or NULL if empty.
   * Safer alternative to unwrap() when you want to check for presence.
   */
  T* Get() { return m_hasValue ? GetPtr() : NULL; }

  /**
   * Get pointer to contained value, or NULL if empty (const version).
   */
  const T* Get() const { return m_hasValue ? GetPtr() : NULL; }

  /**
   * Extract value or return default if empty.
   */
  T UnwrapOr(const T& defaultValue) const {
    return m_hasValue ? *GetPtr() : defaultValue;
  }
};

}  // namespace selfserv
