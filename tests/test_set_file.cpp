#include "doctest.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "sets/set-file.h"

/*
 * Set files, schema 2 (spec 5.1). The bundled preset must load, and saving it again must
 * reproduce the source byte for byte (design acceptance 11: "Save round-trips to an
 * identical file"; card P6: load -> save -> JSON-equal).
 */

static std::string slurp(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	std::ostringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static const std::string kBundled = std::string(PD_SOURCE_ROOT) + "/data/sets/techno-apocalypse.json";

TEST_CASE("bundled Techno Apocalypse loads with every palette, mode and setting")
{
	const std::string src = slurp(kBundled);
	REQUIRE(!src.empty());
	struct pl_set s;
	char err[256] = {0};
	REQUIRE_MESSAGE(pd_set_parse(src.c_str(), &s, err, sizeof(err)), err);
	CHECK(std::string(s.name) == "Techno Apocalypse");
	CHECK(std::string(s.synesthesia_playlist) == "Techno Apocalypse");
	REQUIRE(s.n_palettes == 7);
	CHECK(std::string(s.palettes[0].name) == "Neon Grid");
	CHECK(s.palettes[0].n_colours == 5);
	CHECK(std::string(s.palettes[6].name) == "Spectrum Fallout");
	CHECK(s.palettes[6].n_colours == 8);
	for (int i = 0; i < 7; i++)
		CHECK(s.palettes[i].mode == PL_MODE_GRADIENT);
	char hex[PL_HEX_LEN];
	pl_colour_to_hex(s.palettes[0].colours[2], hex);
	CHECK(std::string(hex) == "#FF00A0");
	CHECK(s.strength == doctest::Approx(1.0f));
	CHECK(s.contrast == doctest::Approx(1.2f));
	CHECK(s.preserve == doctest::Approx(0.15f));
	CHECK(s.trigger == PL_TRIGGER_PHRASES);
	CHECK(s.every_phrases == 8);
	CHECK(s.order == PL_ORDER_SHUFFLE);
	CHECK(s.crossfade_beats == doctest::Approx(1.0f));
	CHECK(std::string(s.breakdown) == "Spectrum Fallout");
	CHECK(pl_breakdown_index(&s) == 6); // resolved BY NAME
	CHECK(s.fallback_bpm == doctest::Approx(132.0f));
	CHECK(s.random_off == true);
	CHECK(s.random_chance == doctest::Approx(0.12f));
	CHECK(s.bars_min == 1);
	CHECK(s.bars_max == 2);
	CHECK(s.cooldown_phrases == 3);
	CHECK(s.off_on_breakdown == false);
	CHECK(s.off_on_drop == true);
	CHECK(s.drop_bars == 1);
	CHECK(s.breathing == doctest::Approx(0.25f));
	CHECK(s.rotation == false);
	CHECK(s.seed == 2077u);
}

TEST_CASE("load -> save round-trips the bundled preset byte for byte")
{
	const std::string src = slurp(kBundled);
	struct pl_set s;
	char err[256] = {0};
	REQUIRE(pd_set_parse(src.c_str(), &s, err, sizeof(err)));
	char *out = pd_set_write(&s);
	REQUIRE(out != nullptr);
	CHECK(std::string(out) == src);
	// and the saved text loads back to the same set
	struct pl_set s2;
	REQUIRE(pd_set_parse(out, &s2, err, sizeof(err)));
	char *out2 = pd_set_write(&s2);
	CHECK(std::string(out2) == std::string(out));
	pd_set_free(out);
	pd_set_free(out2);
}

TEST_CASE("the spec 5.1 example loads; breakdown is a name: follows reorder + rename, cleared on delete")
{
	const char *spec = R"({
  "schema": 2,
  "name": "Warehouse 2026-11-07",
  "synesthesia_playlist": "Techno Nov",
  "palettes": [
    { "name": "Night Neon",    "mode": "gradient_map", "colours": ["#0B0B12", "#3A0CA3", "#F72585", "#4CC9F0"] },
    { "name": "Infrared",      "mode": "gradient_map", "colours": ["#07070A", "#5A0000", "#FF2A00", "#FFD166"] },
    { "name": "Ice",           "mode": "gradient_map", "colours": ["#05060A", "#0D3B66", "#7FDBFF", "#F0FBFF"] },
    { "name": "Breakdown Duo", "mode": "duotone",      "colours": ["#0A0A12", "#C9C9FF"] }
  ],
  "colour": { "strength": 1.0, "contrast": 1.1, "keep_brightness": 0.0 },
  "palette_changes": {
    "trigger": "drop", "every_phrases": 4, "chance": 0.25, "order": "sequence",
    "crossfade_beats": 2, "breakdown_palette": "Breakdown Duo"
  },
  "director": {
    "enabled": true, "phrase_beats": 32, "lock_fade_beats": 1, "fallback_bpm": 128,
    "random_off": { "enabled": true, "chance": 0.15, "bars_min": 1, "bars_max": 4, "cooldown_phrases": 2 },
    "off_on_breakdown": false,
    "off_on_drop": { "enabled": false, "bars": 2 },
    "breathing": 0.0, "rotation": false, "seed": 7
  }
})";
	struct pl_set s;
	char err[256] = {0};
	REQUIRE_MESSAGE(pd_set_parse(spec, &s, err, sizeof(err)), err);
	CHECK(s.n_palettes == 4);
	CHECK(s.palettes[3].mode == PL_MODE_DUOTONE);
	CHECK(pl_breakdown_index(&s) == 3);
	int live = 0, edit = 3;
	pl_move_palette(&s, 3, -1, &live, &edit); // move the breakdown palette up
	CHECK(pl_breakdown_index(&s) == 2);
	CHECK(pl_rename_palette(&s, 2, "BD"));
	CHECK(std::string(s.breakdown) == "BD"); // follows the rename
	CHECK(pl_breakdown_index(&s) == 2);
	CHECK(pl_delete_palette(&s, 2, &live, &edit));
	CHECK(std::string(s.breakdown).empty()); // deleting it sets breakdown to None
	char *out = pd_set_write(&s);
	struct pl_set s2;
	REQUIRE(pd_set_parse(out, &s2, err, sizeof(err)));
	CHECK(s2.n_palettes == 3);
	CHECK(std::string(s2.breakdown).empty());
	pd_set_free(out);
}

TEST_CASE("invalid set files are rejected with a reason")
{
	struct pl_set s;
	char err[256] = {0};
	CHECK_FALSE(pd_set_parse("{\"schema\": 1, \"name\": \"x\"}", &s, err, sizeof(err)));
	CHECK(std::strstr(err, "schema") != nullptr);
	CHECK_FALSE(pd_set_parse("{\"schema\": 2, \"name\": \"x\", \"palettes\": []}", &s, err, sizeof(err)));
	CHECK_FALSE(pd_set_parse("{\"schema\": 2, \"name\": \"x\", \"palettes\": [{\"name\": \"a\", \"colours\": "
				 "[\"#000000\"]}]}",
				 &s, err, sizeof(err)));
	CHECK_FALSE(pd_set_parse("{\"schema\": 2, \"name\": \"x\", \"palettes\": [{\"name\": \"a\", \"colours\": "
				 "[\"#000000\", \"#FFF\"]}]}",
				 &s, err, sizeof(err)));
	CHECK_FALSE(pd_set_parse("{\"schema\": 2, \"name\": \"x\", \"palettes\": [{\"name\": \"a\", \"colours\": "
				 "[\"#000000\", \"#FFFFFF\"]}], \"palette_changes\": {\"breakdown_palette\": \"zz\"}}",
				 &s, err, sizeof(err)));
	CHECK_FALSE(pd_set_parse("not json", &s, err, sizeof(err)));
}

TEST_CASE("set file names follow the mock's slug rule")
{
	char out[128];
	pd_set_slug("Techno Apocalypse", out, sizeof(out));
	CHECK(std::string(out) == "Techno-Apocalypse");
	pd_set_slug("Warehouse 2026-11-07", out, sizeof(out));
	CHECK(std::string(out) == "Warehouse-2026-11-07");
	pd_set_slug("A/B: test  set!", out, sizeof(out));
	CHECK(std::string(out) == "AB-test-set");
	pd_set_slug("///", out, sizeof(out));
	CHECK(std::string(out) == "set");
}
