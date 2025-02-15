#include <cassert>
#include <iostream>

#include "selfserv.h"

int main(void) {
  assert(SELFSERV_VERSION_MAJOR == 0);

  std::cout << "Hello, " SELFSERV_NAME "!\n";

  return 0;
}
