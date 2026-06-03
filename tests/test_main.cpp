#include "test_framework.h"

// Cases self-register via TEST_CASE across the other TUs; just run them all.
int main()
{
	return ::tf::run();
}
