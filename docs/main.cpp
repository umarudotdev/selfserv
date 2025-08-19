#include <cassert>
#include <cstddef>
#include <iostream>

#include "option/option.hpp"
#include "result/result.hpp"
#include "selfserv.h"

using selfserv::Option;
using selfserv::Result;

/**
 * Demonstrates Result<T, E> with a division function that can fail.
 * Returns either the quotient or an error message.
 */
Result<double, const char*> safe_divide(double a, double b) {
  if (b == 0.0) {
    return Result<double, const char*>::Err("Division by zero");
  }
  return Result<double, const char*>::Ok(a / b);
}

/**
 * Demonstrates Option<T> with a search function that may not find a value.
 * Returns Some(index) if found, None if not found.
 */
Option<int> find_first(const int* array, int size, int value) {
  if (array == NULL) {
    return Option<int>::None();
  }

  for (int i = 0; i < size; ++i) {
    if (array[i] == value) {
      return Option<int>::Some(i);
    }
  }

  return Option<int>::None();
}

/**
 * Demonstrates error propagation with nested Result operations.
 */
Result<double, const char*> calculate_average(double a, double b, double c) {
  Result<double, const char*> sum_ab =
      safe_divide(a + b, 1.0);  // Always succeeds
  if (sum_ab.IsErr()) return sum_ab;

  Result<double, const char*> total = safe_divide(sum_ab.Unwrap() + c, 3.0);
  return total;
}

int main(int /* argc */, char const* /* argv */[]) {
  assert(SELFSERV_VERSION_MAJOR == 0);

  std::cout << "=== C++98 Option<T> and Result<T,E> Demonstration ===\n\n";

  // === Option<T> Demonstration ===
  std::cout << "--- Option<T> Examples ---\n";

  // Test array for search demonstrations
  int numbers[] = {10, 20, 30, 40, 50};
  int size = sizeof(numbers) / sizeof(numbers[0]);

  // Successful search
  Option<int> found = find_first(numbers, size, 30);
  if (found.IsSome()) {
    std::cout << "Found value 30 at index: " << found.Unwrap() << "\n";
  } else {
    std::cout << "Value 30 not found\n";
  }

  // Failed search
  Option<int> notFound = find_first(numbers, size, 99);
  if (notFound.IsNone()) {
    std::cout << "Value 99 not found (as expected)\n";
  }

  // Using unwrapOr for default values
  int index = notFound.UnwrapOr(-1);
  std::cout << "Index of 99 (with default -1): " << index << "\n";

  // Safe pointer access
  const int* indexPtr = found.Get();
  if (indexPtr) {
    std::cout << "Safe access to found index: " << *indexPtr << "\n";
  }

  std::cout << "\n";

  // === Result<T, E> Demonstration ===
  std::cout << "--- Result<T,E> Examples ---\n";

  // Successful division
  Result<double, const char*> success = safe_divide(10.0, 2.0);
  if (success.IsOk()) {
    std::cout << "10.0 / 2.0 = " << success.Unwrap() << "\n";
  }

  // Failed division
  Result<double, const char*> failure = safe_divide(5.0, 0.0);
  if (failure.IsErr()) {
    std::cout << "Division error: " << failure.UnwrapErr() << "\n";
  }

  // Using unwrapOr for error handling
  double safeResult = failure.UnwrapOr(0.0);
  std::cout << "Safe division result (with default 0.0): " << safeResult
            << "\n";

  // Chained operations with error propagation
  Result<double, const char*> avg = calculate_average(6.0, 9.0, 12.0);
  if (avg.IsOk()) {
    std::cout << "Average of 6, 9, 12: " << avg.Unwrap() << "\n";
  } else {
    std::cout << "Average calculation failed: " << avg.UnwrapErr() << "\n";
  }

  std::cout << "\n";

  // === Copy semantics demonstration ===
  std::cout << "--- Copy Semantics (Rule of Three) ---\n";

  Option<int> original = Option<int>::Some(42);
  Option<int> copied = original;  // Copy constructor
  Option<int> assigned;
  assigned = copied;  // Assignment operator

  std::cout << "Original: " << original.Unwrap() << "\n";
  std::cout << "Copied: " << copied.Unwrap() << "\n";
  std::cout << "Assigned: " << assigned.Unwrap() << "\n";

  // Verify they are independent copies
  original = Option<int>::Some(100);
  std::cout << "After modifying original to 100:\n";
  std::cout << "Original: " << original.Unwrap() << "\n";
  std::cout << "Copied (unchanged): " << copied.Unwrap() << "\n";

  std::cout << "\n--- Demonstration Complete ---\n";

  return 0;
}

/*
 * === MODERN C++ COMPARISON AND RETROSPECTIVE ===
 *
 * Implementing Option<T> and Result<T,E> in C++98 required significant manual
 * work that would be dramatically simplified in modern C++ (C++11+):
 *
 * 1. **MOVE SEMANTICS (C++11):**
 *    - Our C++98 implementation only supports copy construction/assignment
 *    - Move semantics would eliminate unnecessary copies when transferring
 *      ownership of expensive-to-copy types
 *    - std::move and && references would provide zero-cost transfers
 *    - Example: Result<std::vector<Data>, Error> would be much more efficient
 *
 * 2. **std::union IMPROVEMENTS (C++11):**
 *    - C++11 allows unions with non-trivial constructors/destructors
 *    - Would eliminate our manual placement new/explicit destructor calls
 *    - std::variant (C++17) would handle the discriminated union automatically
 *    - Memory alignment would be handled automatically
 *
 * 3. **nullptr (C++11):**
 *    - Our C++98 code uses NULL (which is typically 0)
 *    - nullptr provides type safety and better overload resolution
 *    - Eliminates ambiguity between null pointers and integer zeros
 *
 * 4. **VARIADIC TEMPLATES (C++11):**
 *    - Would allow perfect forwarding for construction
 *    - Could implement emplace() methods for in-place construction
 *    - Would enable more sophisticated factory methods
 *    - Example: Option<T>::emplace(args...) for direct construction
 *
 * 5. **ADDITIONAL MODERN FEATURES:**
 *    - alignof/alignas (C++11): Proper alignment without double tricks
 *    - constexpr (C++11): Compile-time evaluation of simple operations
 *    - auto (C++11): Type deduction would simplify template usage
 *    - Lambda expressions (C++11): Functional-style operations like map/filter
 *    - std::optional (C++17): Standard library implementation
 *    - std::expected (C++23): Standard library Result equivalent
 *
 * 6. **PERFORMANCE IMPROVEMENTS:**
 *    - RVO/NRVO optimizations are guaranteed in modern C++
 *    - Move semantics eliminate many temporary copies
 *    - constexpr enables compile-time computation
 *    - Better compiler optimizations with stronger type information
 *
 * 7. **SAFETY IMPROVEMENTS:**
 *    - Modern C++ would catch alignment and type safety issues at compile time
 *    - RAII is more automatic with smart pointers and modern containers
 *    - Exception safety is easier with RAII and strong exception guarantees
 *
 * Despite these advantages, our C++98 implementation demonstrates that robust,
 * functional programming concepts can be implemented even in older language
 * standards with careful attention to resource management and type safety.
 *
 * The fundamental concepts (type safety, error handling, optional values)
 * remain the same - only the implementation techniques have evolved to be
 * more expressive and less error-prone.
 */
