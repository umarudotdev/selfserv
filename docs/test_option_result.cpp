#include <cassert>
#include <iostream>
#include <string>

#include "option/option.hpp"
#include "result/result.hpp"

using selfserv::BadOptionAccess;
using selfserv::BadResultAccess;
using selfserv::Option;
using selfserv::Result;

/**
 * Simple test framework for C++98 - just assertions with descriptions
 */
void test_option_basic() {
  std::cout << "Testing Option basic functionality...\n";

  // Test empty option
  Option<int> empty;
  assert(empty.IsNone());
  assert(!empty.IsSome());
  assert(empty.Get() == NULL);

  // Test option with value
  Option<int> some = Option<int>::Some(42);
  assert(some.IsSome());
  assert(!some.IsNone());
  assert(some.Get() != NULL);
  assert(*some.Get() == 42);
  assert(some.Unwrap() == 42);
  assert(some.UnwrapOr(0) == 42);

  // Test empty option defaults
  assert(empty.UnwrapOr(99) == 99);

  std::cout << "  ✓ Basic Option functionality passed\n";
}

void test_option_copy_semantics() {
  std::cout << "Testing Option copy semantics...\n";

  Option<std::string> original =
      Option<std::string>::Some(std::string("hello"));
  Option<std::string> copied = original;  // Copy constructor
  Option<std::string> assigned;
  assigned = copied;  // Assignment operator

  assert(original.Unwrap() == "hello");
  assert(copied.Unwrap() == "hello");
  assert(assigned.Unwrap() == "hello");

  // Modify original, ensure copies are independent
  original = Option<std::string>::Some(std::string("world"));
  assert(original.Unwrap() == "world");
  assert(copied.Unwrap() == "hello");    // Should be unchanged
  assert(assigned.Unwrap() == "hello");  // Should be unchanged

  std::cout << "  ✓ Option copy semantics passed\n";
}

void test_option_exceptions() {
  std::cout << "Testing Option exception handling...\n";

  Option<int> empty;

  try {
    empty.Unwrap();
    assert(false);  // Should not reach here
  } catch (const BadOptionAccess& e) {
    // Expected
    assert(std::string(e.what()).find("empty Option") != std::string::npos);
  }

  std::cout << "  ✓ Option exception handling passed\n";
}

void test_result_basic() {
  std::cout << "Testing Result basic functionality...\n";

  // Test Ok result
  Result<int, std::string> ok = Result<int, std::string>::Ok(42);
  assert(ok.IsOk());
  assert(!ok.IsErr());
  assert(ok.Get() != NULL);
  assert(*ok.Get() == 42);
  assert(ok.Unwrap() == 42);
  assert(ok.UnwrapOr(0) == 42);
  assert(ok.GetErr() == NULL);

  // Test Err result
  Result<int, std::string> err =
      Result<int, std::string>::Err(std::string("error"));
  assert(!err.IsOk());
  assert(err.IsErr());
  assert(err.Get() == NULL);
  assert(err.GetErr() != NULL);
  assert(*err.GetErr() == "error");
  assert(err.UnwrapErr() == "error");
  assert(err.UnwrapOr(99) == 99);

  std::cout << "  ✓ Basic Result functionality passed\n";
}

void test_result_copy_semantics() {
  std::cout << "Testing Result copy semantics...\n";

  Result<std::string, int> original =
      Result<std::string, int>::Ok(std::string("success"));
  Result<std::string, int> copied = original;  // Copy constructor
  Result<std::string, int> assigned = Result<std::string, int>::Err(999);
  assigned = copied;  // Assignment operator

  assert(original.Unwrap() == "success");
  assert(copied.Unwrap() == "success");
  assert(assigned.Unwrap() == "success");

  // Test error copying
  Result<int, std::string> errOriginal =
      Result<int, std::string>::Err(std::string("fail"));
  Result<int, std::string> errCopied = errOriginal;

  assert(errOriginal.UnwrapErr() == "fail");
  assert(errCopied.UnwrapErr() == "fail");

  std::cout << "  ✓ Result copy semantics passed\n";
}

void test_result_exceptions() {
  std::cout << "Testing Result exception handling...\n";

  Result<int, std::string> ok = Result<int, std::string>::Ok(42);
  Result<int, std::string> err =
      Result<int, std::string>::Err(std::string("error"));

  // Test unwrap on error
  try {
    err.Unwrap();
    assert(false);  // Should not reach here
  } catch (const BadResultAccess& e) {
    assert(std::string(e.what()).find("Err Result") != std::string::npos);
  }

  // Test unwrapErr on ok
  try {
    ok.UnwrapErr();
    assert(false);  // Should not reach here
  } catch (const BadResultAccess& e) {
    assert(std::string(e.what()).find("Ok Result") != std::string::npos);
  }

  std::cout << "  ✓ Result exception handling passed\n";
}

void test_complex_types() {
  std::cout << "Testing with complex types...\n";

  // Test with std::string (non-trivial type)
  Option<std::string> strOpt =
      Option<std::string>::Some(std::string("complex"));
  assert(strOpt.Unwrap() == "complex");

  // Test Result with both types being non-trivial
  Result<std::string, std::string> strResult =
      Result<std::string, std::string>::Ok(std::string("ok"));
  assert(strResult.Unwrap() == "ok");

  Result<std::string, std::string> strError =
      Result<std::string, std::string>::Err(std::string("error"));
  assert(strError.UnwrapErr() == "error");

  std::cout << "  ✓ Complex types test passed\n";
}

int main() {
  std::cout << "=== Running Option<T> and Result<T,E> Tests ===\n\n";

  test_option_basic();
  test_option_copy_semantics();
  test_option_exceptions();
  test_result_basic();
  test_result_copy_semantics();
  test_result_exceptions();
  test_complex_types();

  std::cout << "\n=== All Tests Passed! ===\n";
  return 0;
}
