#include "doctest.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

/*
 * Constant-buffer layout regression guard for data/effects/palette-lock.effect.
 *
 * On D3D11, OBS sizes and positions shader constants CPU-side with
 * gs_shader::BuildConstantBuffer() (libobs-d3d11/d3d11-shader.cpp, OBS 32.2.2),
 * then uploads that tightly packed buffer to the GPU.  The GPU reads the same
 * buffer through the HLSL cbuffer packing rules.  These two layouts only agree
 * when every uniform is a scalar or a float4 (16-byte) type.  A float3 array is
 * the pathological case: OBS packs it as 12 * arrayCount bytes, while HLSL gives
 * every array element its own 16-byte register, so every following uniform lands
 * at a different offset and the GPU reads garbage (the original all-black bug).
 *
 * This test parses the shipped .effect, runs BOTH calculators over its uniforms,
 * and asserts they agree.  If anyone reintroduces a float3 array (or any other
 * type the two layouts disagree on) this test fails.
 */

// Repo root injected by tests/CMakeLists.txt; fall back to the build dir.
#ifndef PD_SOURCE_ROOT
#define PD_SOURCE_ROOT ".."
#endif

enum class VarType {
	Unknown,
	Texture,
	Scalar, /* float / int / bool */
	Vec2,   /* float2 / int2 */
	Vec3,   /* float3 / int3 */
	Vec4,   /* float4 / int4 */
	Matrix4x4,
};

struct Uniform {
	std::string name;
	VarType type;
	int array_count; /* 0 == not an array */
};

static const char *type_name(VarType t)
{
	switch (t) {
	case VarType::Scalar:
		return "scalar";
	case VarType::Vec2:
		return "vec2";
	case VarType::Vec3:
		return "vec3";
	case VarType::Vec4:
		return "vec4";
	case VarType::Matrix4x4:
		return "mat4x4";
	case VarType::Texture:
		return "texture";
	default:
		return "unknown";
	}
}

/* Parse the `uniform <type> <name>[<count>];` declarations from the .effect. */
static std::vector<Uniform> parse_uniforms(const std::string &path)
{
	std::ifstream in(path);
	REQUIRE_MESSAGE(in.is_open(), "cannot open effect file: ", path);

	std::vector<Uniform> out;
	std::string line;
	while (std::getline(in, line)) {
		/* strip a trailing // comment, if any */
		const size_t slash = line.find("//");
		if (slash != std::string::npos)
			line = line.substr(0, slash);

		std::istringstream ss(line);
		std::string kw, type, name;
		if (!(ss >> kw >> type >> name))
			continue;
		if (kw != "uniform")
			continue;

		int count = 0;
		const size_t lb = name.find('[');
		if (lb != std::string::npos) {
			const size_t rb = name.find(']', lb);
			if (rb != std::string::npos)
				count = std::stoi(name.substr(lb + 1, rb - lb - 1));
			name = name.substr(0, lb);
		}
		/* strip the trailing ';' */
		const size_t semi = name.find(';');
		if (semi != std::string::npos)
			name = name.substr(0, semi);

		Uniform u;
		u.name = name;
		u.array_count = count;
		if (type == "texture2d" || type == "texture2D" || type == "texture3d")
			u.type = VarType::Texture;
		else if (type == "float4x4" || type == "matrix")
			u.type = VarType::Matrix4x4;
		else if (type == "float3" || type == "int3")
			u.type = VarType::Vec3;
		else if (type == "float4" || type == "int4")
			u.type = VarType::Vec4;
		else if (type == "float2" || type == "int2")
			u.type = VarType::Vec2;
		else if (type == "float" || type == "int" || type == "bool")
			u.type = VarType::Scalar;
		else
			u.type = VarType::Unknown;
		out.push_back(u);
	}
	return out;
}

struct Slot {
	uint32_t offset;
	uint32_t size;
};

/*
 * OBS's CPU-side layout.  Copied from obs-studio 32.2.2
 * libobs-d3d11/d3d11-shader.cpp, gs_shader::BuildConstantBuffer():
 *
 *   - each param contributes sizeof by type (scalar 4, vec2 8, vec3 12,
 *     vec4 16, matrix4x4 64), scaled by arrayCount when present;
 *   - if the running size is not 16-byte aligned and the next param would
 *     straddle the register boundary, the running size is aligned up to that
 *     boundary first ("checks to see if this constant needs to start at a new
 *     register");
 *   - textures/strings/unknown params are skipped (they live outside the
 *     constant buffer).
 */
static std::vector<Slot> obs_build_constant_buffer(const std::vector<Uniform> &vars)
{
	std::vector<Slot> out;
	uint32_t constant_size = 0;
	for (const Uniform &v : vars) {
		uint32_t size = 0;
		switch (v.type) {
		case VarType::Scalar:
			size = 4;
			break;
		case VarType::Vec2:
			size = 8;
			break;
		case VarType::Vec3:
			size = 12;
			break;
		case VarType::Vec4:
			size = 16;
			break;
		case VarType::Matrix4x4:
			size = 64;
			break;
		case VarType::Texture:
		case VarType::Unknown:
			size = 0;
			break;
		}
		if (v.array_count)
			size *= (uint32_t)v.array_count;

		/* checks to see if this constant needs to start at a new register */
		if (size && (constant_size & 15) != 0) {
			uint32_t align_max = (constant_size + 15) & ~15u;
			if (size + constant_size > align_max)
				constant_size = align_max;
		}

		out.push_back({constant_size, size});
		constant_size += size;
	}
	return out;
}

/*
 * HLSL cbuffer packing.  Microsoft "Packing Rules for Constant Variables":
 *   - scalar: 4 bytes, 4-aligned; vec2: 8 bytes, 8-aligned; vec3: 12 bytes
 *     but 16-aligned; vec4: 16 bytes, 16-aligned; matrix4x4: 64 bytes,
 *     16-aligned;
 *   - "every element in an array is stored in a four-component vector", i.e.
 *     an array element has a 16-byte stride regardless of its component count.
 */
static std::vector<Slot> hlsl_cbuffer_packing(const std::vector<Uniform> &vars)
{
	std::vector<Slot> out;
	uint32_t offset = 0;
	for (const Uniform &v : vars) {
		uint32_t align = 0, elem = 0;
		switch (v.type) {
		case VarType::Scalar:
			align = 4;
			elem = 4;
			break;
		case VarType::Vec2:
			align = 8;
			elem = 8;
			break;
		case VarType::Vec3:
			align = 16;
			elem = 12;
			break;
		case VarType::Vec4:
			align = 16;
			elem = 16;
			break;
		case VarType::Matrix4x4:
			align = 16;
			elem = 64;
			break;
		case VarType::Texture:
		case VarType::Unknown:
			align = 0;
			elem = 0;
			break;
		}
		if (align == 0) {
			out.push_back({offset, 0});
			continue;
		}

		uint32_t stride = elem;
		if (v.array_count)
			stride = (elem + 15) & ~15u; /* every element its own 16-byte slot */

		offset = (offset + align - 1) & ~(align - 1);
		uint32_t size = v.array_count ? stride * (uint32_t)v.array_count : stride;
		out.push_back({offset, size});
		offset += size;
	}
	return out;
}

TEST_CASE("palette-lock.effect cbuffer layout matches OBS BuildConstantBuffer")
{
	const std::string effect = std::string(PD_SOURCE_ROOT) + "/data/effects/palette-lock.effect";
	const std::vector<Uniform> uniforms = parse_uniforms(effect);
	REQUIRE_FALSE(uniforms.empty());

	/* OBS builds a separate constant buffer per shader.  ViewProj is used only
	 * by the vertex shader; the rest (image is a texture, not a constant) form
	 * the pixel-shader cbuffer that the filter uploads via gs_effect_set_val. */
	std::vector<Uniform> vertex;
	std::vector<Uniform> pixel;
	for (const Uniform &u : uniforms) {
		if (u.type == VarType::Texture || u.type == VarType::Unknown)
			continue;
		if (u.name == "ViewProj")
			vertex.push_back(u);
		else
			pixel.push_back(u);
	}

	REQUIRE(vertex.size() == 1);
	CHECK(vertex[0].type == VarType::Matrix4x4);

	const std::vector<Slot> obs = obs_build_constant_buffer(pixel);
	const std::vector<Slot> hlsl = hlsl_cbuffer_packing(pixel);
	REQUIRE(obs.size() == hlsl.size());
	REQUIRE(obs.size() == pixel.size());

	std::printf("\npalette-lock.effect pixel-shader cbuffer (OBS BuildConstantBuffer vs HLSL packing)\n");
	std::printf("  %-14s %10s %10s %8s %8s\n", "uniform", "OBS", "HLSL", "bytes", "type");
	for (size_t i = 0; i < pixel.size(); i++) {
		std::string n = pixel[i].name;
		if (pixel[i].array_count)
			n += "[" + std::to_string(pixel[i].array_count) + "]";
		std::printf("  %-14s %10u %10u %8u %8s\n", n.c_str(), obs[i].offset, hlsl[i].offset, obs[i].size,
			    type_name(pixel[i].type));

		INFO("uniform: " << n);
		CHECK(obs[i].offset == hlsl[i].offset);
		CHECK(obs[i].size == hlsl[i].size);
	}
}
