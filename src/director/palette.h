#pragma once
#include <string>
#include <vector>

namespace pd {

enum class RecolourMode { GradientMap = 0, Duotone = 1 };

struct Colour {
	float r = 0.0f;
	float g = 0.0f;
	float b = 0.0f;
};

// One named palette: a recolour mode plus 2..8 colours (dark -> bright).
struct PaletteSpec {
	std::string name;
	RecolourMode mode = RecolourMode::GradientMap;
	std::vector<Colour> colours;
};

} // namespace pd
