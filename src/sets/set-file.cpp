/*
Palette Director
Copyright (C) 2026 EnZhm

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "set-file.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/* ------------------------------------------------------------------------- */
/* minimal JSON tree (order-preserving objects)                               */

struct Value {
	enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
	bool b = false;
	double num = 0.0;
	bool is_int = false;
	std::string raw; // number text as written (writer side)
	std::string str;
	std::vector<Value> arr;
	std::vector<std::pair<std::string, Value>> obj;

	const Value *get(const char *k) const
	{
		if (type != Type::Object)
			return nullptr;
		for (const auto &kv : obj)
			if (kv.first == k)
				return &kv.second;
		return nullptr;
	}
};

class Parser {
public:
	explicit Parser(const char *s) : p_(s) {}
	bool parse(Value &out, std::string &err)
	{
		if (!value(out, 0)) {
			err = err_.empty() ? "invalid JSON" : err_;
			return false;
		}
		ws();
		if (*p_) {
			err = "trailing characters after JSON";
			return false;
		}
		return true;
	}

private:
	const char *p_;
	std::string err_;

	void ws()
	{
		while (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')
			p_++;
	}
	bool fail(const char *m)
	{
		if (err_.empty())
			err_ = m;
		return false;
	}
	bool lit(const char *w)
	{
		const size_t n = strlen(w);
		if (strncmp(p_, w, n) != 0)
			return false;
		p_ += n;
		return true;
	}
	static void utf8(std::string &o, unsigned cp)
	{
		if (cp < 0x80) {
			o += static_cast<char>(cp);
		} else if (cp < 0x800) {
			o += static_cast<char>(0xC0 | (cp >> 6));
			o += static_cast<char>(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			o += static_cast<char>(0xE0 | (cp >> 12));
			o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			o += static_cast<char>(0x80 | (cp & 0x3F));
		} else {
			o += static_cast<char>(0xF0 | (cp >> 18));
			o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
			o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
			o += static_cast<char>(0x80 | (cp & 0x3F));
		}
	}
	bool hex4(unsigned &v)
	{
		v = 0;
		for (int i = 0; i < 4; i++) {
			const char c = *p_++;
			v <<= 4;
			if (c >= '0' && c <= '9')
				v |= static_cast<unsigned>(c - '0');
			else if (c >= 'a' && c <= 'f')
				v |= static_cast<unsigned>(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				v |= static_cast<unsigned>(c - 'A' + 10);
			else
				return fail("bad \\u escape");
		}
		return true;
	}
	bool string(std::string &o)
	{
		if (*p_ != '"')
			return fail("expected string");
		p_++;
		while (*p_ && *p_ != '"') {
			if (static_cast<unsigned char>(*p_) < 0x20)
				return fail("control character in string");
			if (*p_ != '\\') {
				o += *p_++;
				continue;
			}
			p_++;
			switch (*p_++) {
			case '"':
				o += '"';
				break;
			case '\\':
				o += '\\';
				break;
			case '/':
				o += '/';
				break;
			case 'b':
				o += '\b';
				break;
			case 'f':
				o += '\f';
				break;
			case 'n':
				o += '\n';
				break;
			case 'r':
				o += '\r';
				break;
			case 't':
				o += '\t';
				break;
			case 'u': {
				unsigned cp;
				if (!hex4(cp))
					return false;
				if (cp >= 0xD800 && cp < 0xDC00 && p_[0] == '\\' && p_[1] == 'u') {
					p_ += 2;
					unsigned lo;
					if (!hex4(lo))
						return false;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				utf8(o, cp);
				break;
			}
			default:
				return fail("bad escape");
			}
		}
		if (*p_ != '"')
			return fail("unterminated string");
		p_++;
		return true;
	}
	bool number(Value &v)
	{
		const char *s = p_;
		if (*p_ == '-')
			p_++;
		bool frac = false;
		while ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '+' ||
		       *p_ == '-') {
			if (*p_ == '.' || *p_ == 'e' || *p_ == 'E')
				frac = true;
			p_++;
		}
		double d = 0.0;
		const auto r = std::from_chars(s, p_, d);
		if (r.ec != std::errc() || r.ptr != p_)
			return fail("bad number");
		v.type = Value::Type::Number;
		v.num = d;
		v.is_int = !frac;
		return true;
	}
	bool value(Value &v, int depth)
	{
		if (depth > 32)
			return fail("nesting too deep");
		ws();
		if (*p_ == '{') {
			p_++;
			v.type = Value::Type::Object;
			ws();
			if (*p_ == '}') {
				p_++;
				return true;
			}
			for (;;) {
				ws();
				std::string k;
				if (!string(k))
					return false;
				ws();
				if (*p_++ != ':')
					return fail("expected ':'");
				Value item;
				if (!value(item, depth + 1))
					return false;
				v.obj.emplace_back(std::move(k), std::move(item));
				ws();
				if (*p_ == ',') {
					p_++;
					continue;
				}
				if (*p_ == '}') {
					p_++;
					return true;
				}
				return fail("expected ',' or '}'");
			}
		}
		if (*p_ == '[') {
			p_++;
			v.type = Value::Type::Array;
			ws();
			if (*p_ == ']') {
				p_++;
				return true;
			}
			for (;;) {
				Value item;
				if (!value(item, depth + 1))
					return false;
				v.arr.push_back(std::move(item));
				ws();
				if (*p_ == ',') {
					p_++;
					continue;
				}
				if (*p_ == ']') {
					p_++;
					return true;
				}
				return fail("expected ',' or ']'");
			}
		}
		if (*p_ == '"') {
			v.type = Value::Type::String;
			return string(v.str);
		}
		if (lit("true")) {
			v.type = Value::Type::Bool;
			v.b = true;
			return true;
		}
		if (lit("false")) {
			v.type = Value::Type::Bool;
			v.b = false;
			return true;
		}
		if (lit("null")) {
			v.type = Value::Type::Null;
			return true;
		}
		if (*p_ == '-' || (*p_ >= '0' && *p_ <= '9'))
			return number(v);
		return fail("unexpected character");
	}
};

/* ---- writer: 2-space indent, "key": value, one item per line, ASCII (\u escapes) ---- */
void dump_string(std::string &o, const std::string &s)
{
	o += '"';
	for (size_t i = 0; i < s.size();) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		unsigned cp = c;
		size_t n = 1;
		if (c >= 0xF0 && i + 3 < s.size()) {
			cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3Fu) << 12) | ((s[i + 2] & 0x3Fu) << 6) |
			     (s[i + 3] & 0x3Fu);
			n = 4;
		} else if (c >= 0xE0 && i + 2 < s.size()) {
			cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
			n = 3;
		} else if (c >= 0xC0 && i + 1 < s.size()) {
			cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3Fu);
			n = 2;
		}
		i += n;
		char buf[32];
		switch (cp) {
		case '"':
			o += "\\\"";
			continue;
		case '\\':
			o += "\\\\";
			continue;
		case '\n':
			o += "\\n";
			continue;
		case '\r':
			o += "\\r";
			continue;
		case '\t':
			o += "\\t";
			continue;
		case '\b':
			o += "\\b";
			continue;
		case '\f':
			o += "\\f";
			continue;
		default:
			break;
		}
		if (cp < 0x20 || (cp >= 0x7F && cp < 0x10000)) {
			snprintf(buf, sizeof(buf), "\\u%04x", cp);
			o += buf;
		} else if (cp >= 0x10000) {
			cp -= 0x10000;
			snprintf(buf, sizeof(buf), "\\u%04x\\u%04x", 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF));
			o += buf;
		} else {
			o += static_cast<char>(cp);
		}
	}
	o += '"';
}

void dump(std::string &o, const Value &v, int ind)
{
	const std::string pad(static_cast<size_t>(ind + 2), ' ');
	const std::string end(static_cast<size_t>(ind), ' ');
	switch (v.type) {
	case Value::Type::Null:
		o += "null";
		break;
	case Value::Type::Bool:
		o += v.b ? "true" : "false";
		break;
	case Value::Type::Number:
		o += v.raw;
		break;
	case Value::Type::String:
		dump_string(o, v.str);
		break;
	case Value::Type::Array:
		if (v.arr.empty()) {
			o += "[]";
			break;
		}
		o += "[\n";
		for (size_t i = 0; i < v.arr.size(); i++) {
			o += pad;
			dump(o, v.arr[i], ind + 2);
			o += i + 1 < v.arr.size() ? ",\n" : "\n";
		}
		o += end + "]";
		break;
	case Value::Type::Object:
		if (v.obj.empty()) {
			o += "{}";
			break;
		}
		o += "{\n";
		for (size_t i = 0; i < v.obj.size(); i++) {
			o += pad;
			dump_string(o, v.obj[i].first);
			o += ": ";
			dump(o, v.obj[i].second, ind + 2);
			o += i + 1 < v.obj.size() ? ",\n" : "\n";
		}
		o += end + "}";
		break;
	}
}

/* ---- builders ---- */
Value V_obj()
{
	Value v;
	v.type = Value::Type::Object;
	return v;
}
Value V_arr()
{
	Value v;
	v.type = Value::Type::Array;
	return v;
}
Value V_str(const std::string &s)
{
	Value v;
	v.type = Value::Type::String;
	v.str = s;
	return v;
}
Value V_bool(bool b)
{
	Value v;
	v.type = Value::Type::Bool;
	v.b = b;
	return v;
}
Value V_int(long long n)
{
	Value v;
	v.type = Value::Type::Number;
	v.raw = std::to_string(n);
	return v;
}
// shortest text that round-trips the float; "1.0" style when integral (real fields)
Value V_real(float f, bool int_if_integral = false)
{
	Value v;
	v.type = Value::Type::Number;
	if (!std::isfinite(f))
		f = 0.0f;
	if (int_if_integral && std::fabs(f) < 1e9f && f == std::floor(f)) {
		v.raw = std::to_string(static_cast<long long>(f));
		return v;
	}
	char buf[64];
	const auto r = std::to_chars(buf, buf + sizeof(buf), f);
	std::string s(buf, r.ptr);
	if (s.find_first_of(".eE") == std::string::npos)
		s += ".0";
	v.raw = s;
	return v;
}
void put(Value &o, const char *k, Value v)
{
	o.obj.emplace_back(k, std::move(v));
}

/* ---- typed readers ---- */
struct Reader {
	std::string err;
	bool ok() const { return err.empty(); }
	void bad(const std::string &m)
	{
		if (err.empty())
			err = m;
	}
	float real(const Value *o, const char *k, float def, float lo, float hi)
	{
		const Value *v = o ? o->get(k) : nullptr;
		if (!v)
			return def;
		if (v->type != Value::Type::Number) {
			bad(std::string("'") + k + "' must be a number");
			return def;
		}
		if (v->num < lo || v->num > hi) {
			bad(std::string("'") + k + "' out of range");
			return def;
		}
		return static_cast<float>(v->num);
	}
	int integer(const Value *o, const char *k, int def, int lo, int hi)
	{
		const Value *v = o ? o->get(k) : nullptr;
		if (!v)
			return def;
		if (v->type != Value::Type::Number || v->num != std::floor(v->num)) {
			bad(std::string("'") + k + "' must be an integer");
			return def;
		}
		if (v->num < lo || v->num > hi) {
			bad(std::string("'") + k + "' out of range");
			return def;
		}
		return static_cast<int>(v->num);
	}
	bool boolean(const Value *o, const char *k, bool def)
	{
		const Value *v = o ? o->get(k) : nullptr;
		if (!v)
			return def;
		if (v->type != Value::Type::Bool) {
			bad(std::string("'") + k + "' must be true/false");
			return def;
		}
		return v->b;
	}
	std::string string(const Value *o, const char *k, const std::string &def)
	{
		const Value *v = o ? o->get(k) : nullptr;
		if (!v || v->type == Value::Type::Null)
			return def;
		if (v->type != Value::Type::String) {
			bad(std::string("'") + k + "' must be a string");
			return def;
		}
		return v->str;
	}
	const Value *object(const Value *o, const char *k)
	{
		const Value *v = o ? o->get(k) : nullptr;
		if (v && v->type != Value::Type::Object) {
			bad(std::string("'") + k + "' must be an object");
			return nullptr;
		}
		return v;
	}
};

void copy_name(char *dst, size_t n, const std::string &s)
{
	snprintf(dst, n, "%s", s.c_str());
}

bool parse_hex(const std::string &h, struct pl_colour &c)
{
	if (h.size() != 7 || h[0] != '#')
		return false;
	for (size_t i = 1; i < 7; i++)
		if (!isxdigit(static_cast<unsigned char>(h[i])))
			return false;
	c = pl_colour_from_hex(h.c_str());
	return true;
}

} // namespace

extern "C" bool pd_set_parse(const char *json, struct pl_set *out, char *err, size_t err_len)
{
	auto set_err = [&](const std::string &m) {
		if (err && err_len)
			snprintf(err, err_len, "%s", m.c_str());
		return false;
	};
	if (!json)
		return set_err("no data");
	/* tolerate a UTF-8 BOM */
	if (static_cast<unsigned char>(json[0]) == 0xEF && static_cast<unsigned char>(json[1]) == 0xBB &&
	    static_cast<unsigned char>(json[2]) == 0xBF)
		json += 3;

	Value root;
	std::string perr;
	if (!Parser(json).parse(root, perr))
		return set_err(perr);
	if (root.type != Value::Type::Object)
		return set_err("set file must be a JSON object");

	Reader r;
	const int schema = r.integer(&root, "schema", 0, 0, 1000);
	if (!r.ok() || schema != 2)
		return set_err("unsupported schema (expected \"schema\": 2)");

	struct pl_set s;
	pl_set_init(&s);
	const std::string name = r.string(&root, "name", "");
	if (name.empty())
		return set_err("set has no name");
	copy_name(s.name, sizeof(s.name), name);
	copy_name(s.synesthesia_playlist, sizeof(s.synesthesia_playlist), r.string(&root, "synesthesia_playlist", ""));

	const Value *pals = root.get("palettes");
	if (!pals || pals->type != Value::Type::Array || pals->arr.empty() || pals->arr.size() > PL_MAX_PALETTES)
		return set_err("'palettes' must be an array of 1-16 palettes");
	s.n_palettes = static_cast<int>(pals->arr.size());
	for (int i = 0; i < s.n_palettes; i++) {
		const Value &pv = pals->arr[static_cast<size_t>(i)];
		if (pv.type != Value::Type::Object)
			return set_err("palette entries must be objects");
		struct pl_palette &p = s.palettes[i];
		const std::string pn = r.string(&pv, "name", "");
		if (pn.empty())
			return set_err("palette without a name");
		copy_name(p.name, sizeof(p.name), pn);
		for (int j = 0; j < i; j++)
			if (strcmp(s.palettes[j].name, p.name) == 0)
				return set_err("duplicate palette name '" + pn + "'");
		const std::string mode = r.string(&pv, "mode", "gradient_map");
		if (mode == "gradient_map")
			p.mode = PL_MODE_GRADIENT;
		else if (mode == "duotone")
			p.mode = PL_MODE_DUOTONE;
		else
			return set_err("palette '" + pn + "': unknown mode '" + mode + "'");
		const Value *cols = pv.get("colours");
		if (!cols || cols->type != Value::Type::Array || cols->arr.size() < 2 ||
		    cols->arr.size() > PL_MAX_COLOURS)
			return set_err("palette '" + pn + "': 'colours' must hold 2-8 \"#RRGGBB\" strings");
		p.n_colours = static_cast<int>(cols->arr.size());
		for (int j = 0; j < p.n_colours; j++) {
			const Value &cv = cols->arr[static_cast<size_t>(j)];
			if (cv.type != Value::Type::String || !parse_hex(cv.str, p.colours[j]))
				return set_err("palette '" + pn + "': bad colour (use \"#RRGGBB\")");
		}
	}

	const Value *col = r.object(&root, "colour");
	s.strength = r.real(col, "strength", s.strength, 0.0f, 1.0f);
	s.contrast = r.real(col, "contrast", s.contrast, 0.5f, 2.0f);
	s.preserve = r.real(col, "keep_brightness", s.preserve, 0.0f, 1.0f);
	/* v1 amendments: optional, so older schema-2 files still load with the defaults */
	s.input_black = r.real(col, "input_black", s.input_black, 0.0f, 0.5f);
	s.input_white = r.real(col, "input_white", s.input_white, 0.5f, 1.0f);
	const std::string bsrc = r.string(col, "brightness_source", "perceived");
	if (bsrc == "perceived")
		s.brightness_source = PL_BSRC_PERCEIVED;
	else if (bsrc == "max")
		s.brightness_source = PL_BSRC_MAX;
	else if (bsrc == "average")
		s.brightness_source = PL_BSRC_AVERAGE;
	else
		r.bad("unknown brightness_source '" + bsrc + "' (perceived, max or average)");

	const Value *pc = r.object(&root, "palette_changes");
	const std::string trig = r.string(pc, "trigger", "drop");
	if (trig == "drop")
		s.trigger = PL_TRIGGER_DROP;
	else if (trig == "phrases")
		s.trigger = PL_TRIGGER_PHRASES;
	else if (trig == "random")
		s.trigger = PL_TRIGGER_RANDOM;
	else if (trig == "manual")
		s.trigger = PL_TRIGGER_MANUAL;
	else
		r.bad("unknown trigger '" + trig + "'");
	s.every_phrases = r.integer(pc, "every_phrases", s.every_phrases, 1, 16);
	s.change_chance = r.real(pc, "chance", s.change_chance, 0.0f, 1.0f);
	const std::string order = r.string(pc, "order", "sequence");
	if (order == "sequence")
		s.order = PL_ORDER_SEQUENCE;
	else if (order == "shuffle")
		s.order = PL_ORDER_SHUFFLE;
	else
		r.bad("unknown order '" + order + "'");
	s.crossfade_beats = r.real(pc, "crossfade_beats", s.crossfade_beats, 0.0f, 16.0f);
	const std::string bd = r.string(pc, "breakdown_palette", "");
	copy_name(s.breakdown, sizeof(s.breakdown), bd);
	if (!bd.empty() && pl_breakdown_index(&s) < 0)
		r.bad("breakdown palette '" + bd + "' is not in the set");

	const Value *dir = r.object(&root, "director");
	s.dir_enabled = r.boolean(dir, "enabled", s.dir_enabled);
	s.phrase_beats = r.integer(dir, "phrase_beats", s.phrase_beats, 16, 32);
	if (s.phrase_beats != 16 && s.phrase_beats != 32)
		r.bad("'phrase_beats' must be 16 or 32");
	s.lock_fade_beats = r.real(dir, "lock_fade_beats", s.lock_fade_beats, 0.25f, 8.0f);
	s.fallback_bpm = r.real(dir, "fallback_bpm", s.fallback_bpm, 60.0f, 200.0f);
	const Value *ro = r.object(dir, "random_off");
	s.random_off = r.boolean(ro, "enabled", s.random_off);
	s.random_chance = r.real(ro, "chance", s.random_chance, 0.0f, 1.0f);
	s.bars_min = r.integer(ro, "bars_min", s.bars_min, 1, 8);
	s.bars_max = r.integer(ro, "bars_max", s.bars_max, 1, 16);
	s.cooldown_phrases = r.integer(ro, "cooldown_phrases", s.cooldown_phrases, 0, 8);
	s.off_on_breakdown = r.boolean(dir, "off_on_breakdown", s.off_on_breakdown);
	const Value *od = r.object(dir, "off_on_drop");
	s.off_on_drop = r.boolean(od, "enabled", s.off_on_drop);
	s.drop_bars = r.integer(od, "bars", s.drop_bars, 1, 8);
	s.breathing = r.real(dir, "breathing", s.breathing, 0.0f, 1.0f);
	s.rotation = r.boolean(dir, "rotation", s.rotation);
	s.seed = static_cast<unsigned>(r.integer(dir, "seed", static_cast<int>(s.seed), 0, 99999));
	/* v1.0.1: optional, so v1.0.0 files load with today's behaviour */
	s.input_gain_db = r.real(dir, "input_gain_db", s.input_gain_db, -12.0f, 24.0f);
	s.kick_sensitivity = r.real(dir, "kick_sensitivity", s.kick_sensitivity, 0.0f, 1.0f);

	if (!r.ok())
		return set_err(r.err);
	*out = s;
	return true;
}

extern "C" char *pd_set_write(const struct pl_set *s)
{
	static const char *triggers[4] = {"drop", "phrases", "random", "manual"};

	Value root = V_obj();
	put(root, "schema", V_int(2));
	put(root, "name", V_str(s->name));
	put(root, "synesthesia_playlist", V_str(s->synesthesia_playlist));
	Value pals = V_arr();
	for (int i = 0; i < s->n_palettes && i < PL_MAX_PALETTES; i++) {
		const struct pl_palette &p = s->palettes[i];
		Value pv = V_obj();
		put(pv, "name", V_str(p.name));
		put(pv, "mode", V_str(p.mode == PL_MODE_DUOTONE ? "duotone" : "gradient_map"));
		Value cols = V_arr();
		for (int j = 0; j < p.n_colours && j < PL_MAX_COLOURS; j++) {
			char hex[PL_HEX_LEN];
			pl_colour_to_hex(p.colours[j], hex);
			cols.arr.push_back(V_str(hex));
		}
		put(pv, "colours", std::move(cols));
		pals.arr.push_back(std::move(pv));
	}
	put(root, "palettes", std::move(pals));

	Value col = V_obj();
	put(col, "strength", V_real(s->strength));
	put(col, "contrast", V_real(s->contrast));
	put(col, "keep_brightness", V_real(s->preserve));
	put(col, "input_black", V_real(s->input_black));
	put(col, "input_white", V_real(s->input_white));
	static const char *bsrcs[3] = {"perceived", "max", "average"};
	put(col, "brightness_source",
	    V_str(bsrcs[(s->brightness_source >= 0 && s->brightness_source < 3) ? s->brightness_source : 0]));
	put(root, "colour", std::move(col));

	Value pc = V_obj();
	put(pc, "trigger", V_str(triggers[(s->trigger >= 0 && s->trigger < 4) ? s->trigger : 0]));
	put(pc, "every_phrases", V_int(s->every_phrases));
	put(pc, "chance", V_real(s->change_chance));
	put(pc, "order", V_str(s->order == PL_ORDER_SHUFFLE ? "shuffle" : "sequence"));
	put(pc, "crossfade_beats", V_real(s->crossfade_beats, true));
	put(pc, "breakdown_palette", V_str(pl_breakdown_index(s) >= 0 ? s->breakdown : ""));
	put(root, "palette_changes", std::move(pc));

	Value dir = V_obj();
	put(dir, "enabled", V_bool(s->dir_enabled));
	put(dir, "phrase_beats", V_int(s->phrase_beats == 16 ? 16 : 32));
	put(dir, "lock_fade_beats", V_real(s->lock_fade_beats, true));
	put(dir, "fallback_bpm", V_real(s->fallback_bpm, true));
	Value ro = V_obj();
	put(ro, "enabled", V_bool(s->random_off));
	put(ro, "chance", V_real(s->random_chance));
	put(ro, "bars_min", V_int(s->bars_min));
	put(ro, "bars_max", V_int(s->bars_max));
	put(ro, "cooldown_phrases", V_int(s->cooldown_phrases));
	put(dir, "random_off", std::move(ro));
	put(dir, "off_on_breakdown", V_bool(s->off_on_breakdown));
	Value od = V_obj();
	put(od, "enabled", V_bool(s->off_on_drop));
	put(od, "bars", V_int(s->drop_bars));
	put(dir, "off_on_drop", std::move(od));
	put(dir, "breathing", V_real(s->breathing));
	put(dir, "rotation", V_bool(s->rotation));
	put(dir, "seed", V_int(static_cast<long long>(s->seed)));
	put(dir, "input_gain_db", V_real(s->input_gain_db, true));
	put(dir, "kick_sensitivity", V_real(s->kick_sensitivity));
	put(root, "director", std::move(dir));

	std::string o;
	dump(o, root, 0);
	o += "\n";
	char *out = static_cast<char *>(malloc(o.size() + 1));
	if (out)
		memcpy(out, o.c_str(), o.size() + 1);
	return out;
}

extern "C" void pd_set_free(char *p)
{
	free(p);
}

extern "C" void pd_set_slug(const char *name, char *out, size_t out_len)
{
	/* mock: name.replace(/[^\w\- ]/g, '').replace(/\s+/g, '-') */
	std::string slug;
	bool space = false;
	for (const char *c = name ? name : ""; *c; c++) {
		const unsigned char u = static_cast<unsigned char>(*c);
		if (u == ' ') {
			space = true;
			continue;
		}
		if (!(isalnum(u) || u == '_' || u == '-'))
			continue;
		if (space)
			slug += '-';
		space = false;
		slug += static_cast<char>(u);
	}
	if (space)
		slug += '-';
	if (slug.empty())
		slug = "set";
	snprintf(out, out_len, "%s", slug.c_str());
}
