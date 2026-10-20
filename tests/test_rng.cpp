#include "doctest.h"
#include "director/rng.h"

TEST_CASE("mulberry32 matches the mock v2 reference (seed 7)")
{
	pd::Mulberry32 r(7);
	// Reference values derived from the mock v2 mulberry32 (design D9).
	CHECK(r.next() == doctest::Approx(0.011705).epsilon(1e-6));
	CHECK(r.next() == doctest::Approx(0.061958).epsilon(1e-6));
	CHECK(r.next() == doctest::Approx(0.976908).epsilon(1e-6));
	CHECK(r.next() == doctest::Approx(0.699029).epsilon(1e-6));
	CHECK(r.next() == doctest::Approx(0.521445).epsilon(1e-6));
}

TEST_CASE("mulberry32 is deterministic per seed")
{
	pd::Mulberry32 a(42), b(42);
	for (int i = 0; i < 100; ++i)
		CHECK(a.next() == b.next());
}
