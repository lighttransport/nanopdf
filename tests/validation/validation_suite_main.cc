// Single entry point for the aggregated validation test binary.
//
// Every validation test .cc auto-registers its TEST_CASEs into the nanotest
// global registry (their own main() is compiled out via NANOPDF_TEST_SUITE_NO_MAIN).
// This file provides the one main() that runs them all.
#include "nanotest.hh"

int main() { return nanotest::run_all_tests(); }
