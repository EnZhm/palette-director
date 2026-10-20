#include "doctest.h"

#include <cstdio>
#include <cstring>

#include "palette/palette.h"

/*
 * Palette set lifecycle (src/palette/palette.c). These tests pin the design-doc
 * rules the Palette Lock filter relies on: unique names, live/edit references
 * that point at a palette (not a position), and the 1..8 / 2..8 bounds.
 */

static struct pl_set make_three(int *live)
{
	struct pl_set s;
	pl_set_default(&s); // "Palette 1", gradient, 4 colours
	int i = -1;
	pl_add_palette(&s, "Palette 2", &i); // -> "Palette 2"
	pl_add_palette(&s, "Palette 3", &i); // -> "Palette 3"
	*live = 1;                           // live = "Palette 2"
	return s;
}

TEST_CASE("duplicating a palette before the live palette keeps live on its palette")
{
	int live = -1;
	struct pl_set s = make_three(&live); // [P1, P2, P3], live = P2 (idx 1)
	int edit = 0;                        // duplicate P1
	int out = -1;

	REQUIRE(pl_duplicate_palette(&s, 0, &live, &edit, &out));

	// [P1, P1 copy, P2, P3] -- P2 slid from 1 to 2, live must follow it
	CHECK(s.n_palettes == 4);
	CHECK(out == 1);
	CHECK(live == 2);
	CHECK(strcmp(s.palettes[live].name, "Palette 2") == 0);
}

TEST_CASE("duplicating a palette after the live palette leaves live untouched")
{
	int live = -1;
	struct pl_set s = make_three(&live); // live = P2 (idx 1)
	int edit = 2;                        // duplicate P3 (after live)
	int out = -1;

	REQUIRE(pl_duplicate_palette(&s, 2, &live, &edit, &out));

	CHECK(live == 1);
	CHECK(strcmp(s.palettes[live].name, "Palette 2") == 0);
}

TEST_CASE("duplicating the live palette keeps live on the original")
{
	int live = -1;
	struct pl_set s = make_three(&live); // live = P2 (idx 1)
	int edit = 1;                        // duplicate live palette
	int out = -1;

	REQUIRE(pl_duplicate_palette(&s, 1, &live, &edit, &out));

	// [P1, P2, P2 copy, P3] -- live stays at idx 1 (still P2)
	CHECK(live == 1);
	CHECK(strcmp(s.palettes[live].name, "Palette 2") == 0);
	CHECK(out == 2);
}

TEST_CASE("duplicating adjusts a trailing edit reference")
{
	int live = -1;
	struct pl_set s = make_three(&live); // live = P2 (idx 1)
	int edit = 2;                        // P3
	int out = -1;

	REQUIRE(pl_duplicate_palette(&s, 0, &live, &edit, &out));

	// [P1, P1 copy, P2, P3] -- P3 slid 2 -> 3
	CHECK(edit == 3);
	CHECK(live == 2);
}

TEST_CASE("rename rejects duplicate names")
{
	int live = -1;
	struct pl_set s = make_three(&live);

	CHECK_FALSE(pl_rename_palette(&s, 0, "Palette 2")); // collides with idx 1
	CHECK(strcmp(s.palettes[0].name, "Palette 1") == 0);
}

TEST_CASE("rename rejects empty and null names")
{
	int live = -1;
	struct pl_set s = make_three(&live);

	CHECK_FALSE(pl_rename_palette(&s, 0, ""));
	CHECK_FALSE(pl_rename_palette(&s, 0, nullptr));
	CHECK(strcmp(s.palettes[0].name, "Palette 1") == 0);
}

TEST_CASE("rename accepts a unique name and self-rename")
{
	int live = -1;
	struct pl_set s = make_three(&live);

	CHECK(pl_rename_palette(&s, 0, "Foo"));
	CHECK(strcmp(s.palettes[0].name, "Foo") == 0);

	CHECK(pl_rename_palette(&s, 0, "Foo")); // renaming to its own name is not a collision
	CHECK(strcmp(s.palettes[0].name, "Foo") == 0);
}

TEST_CASE("delete of the live palette hands the marker to the next palette")
{
	int live = -1;
	struct pl_set s = make_three(&live); // live = P2 (idx 1)
	int edit = 1;

	REQUIRE(pl_delete_palette(&s, 1, &live, &edit));

	// [P1, P3] -- the palette that slid into idx 1 is now live
	CHECK(s.n_palettes == 2);
	CHECK(live == 1);
	CHECK(strcmp(s.palettes[live].name, "Palette 3") == 0);
}

TEST_CASE("delete before the live palette shifts live down to follow it")
{
	int live = -1;
	struct pl_set s = make_three(&live); // live = P2 (idx 1)
	int edit = 0;

	REQUIRE(pl_delete_palette(&s, 0, &live, &edit));

	// [P2, P3] -- P2 slid 1 -> 0
	CHECK(live == 0);
	CHECK(strcmp(s.palettes[live].name, "Palette 2") == 0);
}

TEST_CASE("move keeps the live reference on its palette")
{
	int live = -1;
	struct pl_set s = make_three(&live); // live = P2 (idx 1)
	int edit = 0;

	pl_move_palette(&s, 0, 1, &live, &edit); // swap P1 <-> P2

	// [P2, P1, P3] -- P2 moved 1 -> 0
	CHECK(live == 0);
	CHECK(strcmp(s.palettes[live].name, "Palette 2") == 0);
	CHECK(strcmp(s.palettes[0].name, "Palette 2") == 0);
}

TEST_CASE("add dedupes names and enforces the 8-palette cap")
{
	int live = -1;
	struct pl_set s = make_three(&live);

	int out = -1;
	REQUIRE(pl_add_palette(&s, "Palette 1", &out)); // collides -> "Palette 1 (2)"
	CHECK(strcmp(s.palettes[out].name, "Palette 1 (2)") == 0);

	// fill to 8, then the 9th add is rejected
	while (s.n_palettes < PL_MAX_PALETTES) {
		char nm[PL_NAME_MAX];
		std::snprintf(nm, sizeof(nm), "Extra %d", s.n_palettes);
		REQUIRE(pl_add_palette(&s, nm, &out));
	}
	CHECK(s.n_palettes == PL_MAX_PALETTES);
	CHECK_FALSE(pl_add_palette(&s, "Overflow", &out));
}

TEST_CASE("delete refuses to remove the last palette")
{
	int live = -1;
	struct pl_set s = make_three(&live);
	int edit = 0;

	REQUIRE(pl_delete_palette(&s, 0, &live, &edit));
	REQUIRE(pl_delete_palette(&s, 0, &live, &edit));
	CHECK(s.n_palettes == 1);
	CHECK_FALSE(pl_delete_palette(&s, 0, &live, &edit));
	CHECK(s.n_palettes == 1);
}

TEST_CASE("colour count is bounded 2..8")
{
	int live = -1;
	struct pl_set s = make_three(&live);
	struct pl_palette *p = &s.palettes[0];
	CHECK(p->n_colours == 4);

	while (p->n_colours < PL_MAX_COLOURS)
		REQUIRE(pl_add_colour(p));
	CHECK(p->n_colours == PL_MAX_COLOURS);
	CHECK_FALSE(pl_add_colour(p)); // capped at 8

	while (p->n_colours > 2)
		REQUIRE(pl_remove_colour(p, p->n_colours - 1));
	CHECK(p->n_colours == 2);
	CHECK_FALSE(pl_remove_colour(p, 0)); // floor at 2
}

/*
 * OBS colour properties store their value in 0xAABBGGRR byte order
 * (shared/properties-view/properties-view.cpp color_from_int: red = val & 0xff).
 * The filter must decode/encode that exact order so red is red and blue is blue.
 */
TEST_CASE("pl_colour_from_obs decodes the OBS 0xAABBGGRR byte order")
{
	struct pl_colour c = pl_colour_from_obs(0xFF0000FFu); // R=FF G=00 B=00
	CHECK(c.r == doctest::Approx(1.0f));
	CHECK(c.g == doctest::Approx(0.0f));
	CHECK(c.b == doctest::Approx(0.0f));

	struct pl_colour g = pl_colour_from_obs(0xFF00FF00u); // G=FF
	CHECK(g.r == doctest::Approx(0.0f));
	CHECK(g.g == doctest::Approx(1.0f));
	CHECK(g.b == doctest::Approx(0.0f));

	struct pl_colour b = pl_colour_from_obs(0xFFFF0000u); // B=FF
	CHECK(b.r == doctest::Approx(0.0f));
	CHECK(b.g == doctest::Approx(0.0f));
	CHECK(b.b == doctest::Approx(1.0f));
}

TEST_CASE("pl_colour_to_obs encodes the OBS 0xAABBGGRR byte order")
{
	struct pl_colour red = {1.0f, 0.0f, 0.0f};
	CHECK(pl_colour_to_obs(red) == 0xFF0000FFu);

	struct pl_colour green = {0.0f, 1.0f, 0.0f};
	CHECK(pl_colour_to_obs(green) == 0xFF00FF00u);

	struct pl_colour blue = {0.0f, 0.0f, 1.0f};
	CHECK(pl_colour_to_obs(blue) == 0xFFFF0000u);
}

TEST_CASE("#FF00A0 round-trips exactly through the obs int and hex helpers")
{
	const char *hex = "#FF00A0";
	struct pl_colour c = pl_colour_from_hex(hex);

	unsigned obs = pl_colour_to_obs(c);
	CHECK(obs == 0xFFA000FFu); // alpha FF, blue A0, green 00, red FF

	struct pl_colour back = pl_colour_from_obs(obs);
	char out[PL_HEX_LEN];
	pl_colour_to_hex(back, out);
	CHECK(strcmp(out, hex) == 0);
}
