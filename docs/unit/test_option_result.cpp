#include <stdint.h>
#include <string>
#include <iostream>

#include "util/OptionResult.hpp"

#ifdef __has_include
# if __has_include(<criterion/criterion.h>)
#  define HAVE_CRITERION 1
# endif
#endif

#ifdef HAVE_CRITERION
#include <criterion/criterion.h>
#endif

using util::Option;
using util::Result;

static void option_basic_impl() {
  Option<int> none = Option<int>::None();
  if (!none.IsNone()) std::cerr << "FAIL none IsNone" << std::endl;
  Option<int> some = Option<int>::Some(3);
  if (some.Value() != 3) std::cerr << "FAIL some value" << std::endl;
  some.Replace(9);
  if (some.ValueOr(0) != 9) std::cerr << "FAIL replace" << std::endl;
  Option<int> copy(some);
  if (copy.Value() != 9) std::cerr << "FAIL copy" << std::endl;
}

static void result_basic_impl() {
  Result<std::string,int> ok = Result<std::string,int>::MakeOk("hi");
  if (!ok.IsOk() || ok.Ok() != "hi") std::cerr << "FAIL ok" << std::endl;
  Result<std::string,int> err = Result<std::string,int>::MakeErr(5);
  if (!err.IsErr() || err.Err() != 5) std::cerr << "FAIL err" << std::endl;
  if (err.OkOr("x") != std::string("x")) std::cerr << "FAIL okor" << std::endl;
  ok = err; // state change
  if (!ok.IsErr()) std::cerr << "FAIL state change" << std::endl;
}

#ifdef HAVE_CRITERION
Test(OptionResult, option_basic) { option_basic_impl(); }
Test(OptionResult, result_basic) { result_basic_impl(); }
#else
int main() { option_basic_impl(); result_basic_impl(); return 0; }
#endif
