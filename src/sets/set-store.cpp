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

#include "set-store.h"
#include "set-file.h"

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace {

std::string base_dir()
{
	/* PD_SETS_DIR overrides the location (tests / portable setups) */
	const char *env = getenv("PD_SETS_DIR");
	if (env && *env)
		return env;
#ifdef _WIN32
	PWSTR wpath = nullptr;
	std::string out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &wpath)) && wpath) {
		char *u8 = nullptr;
		os_wcs_to_utf8_ptr(wpath, 0, &u8);
		if (u8) {
			out = std::string(u8) + "\\PaletteDirector\\sets";
			bfree(u8);
		}
	}
	if (wpath)
		CoTaskMemFree(wpath);
	return out;
#else
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg)
		return std::string(xdg) + "/PaletteDirector/sets";
	const char *home = getenv("HOME");
	return home ? std::string(home) + "/.config/PaletteDirector/sets" : std::string();
#endif
}

std::string read_file(const char *path)
{
	FILE *f = os_fopen(path, "rb");
	if (!f)
		return std::string();
	std::string s;
	char buf[4096];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		s.append(buf, n);
	fclose(f);
	return s;
}

bool write_file(const char *path, const std::string &data)
{
	/* write to a temp file then rename, so a crash can't leave a half-written set */
	const std::string tmp = std::string(path) + ".tmp";
	FILE *f = os_fopen(tmp.c_str(), "wb");
	if (!f)
		return false;
	const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	fclose(f);
	if (!ok) {
		os_unlink(tmp.c_str());
		return false;
	}
	if (os_file_exists(path))
		os_unlink(path);
	return os_rename(tmp.c_str(), path) == 0;
}

} // namespace

extern "C" bool pd_sets_dir(char *out, size_t out_len)
{
	const std::string d = base_dir();
	if (d.empty())
		return false;
	if (os_mkdirs(d.c_str()) == MKDIR_ERROR)
		return false;
	snprintf(out, out_len, "%s", d.c_str());
	return true;
}

extern "C" bool pd_sets_path_for(const char *name, char *out, size_t out_len)
{
	char dir[512];
	if (!pd_sets_dir(dir, sizeof(dir)))
		return false;
	char slug[PL_NAME_MAX + 8];
	pd_set_slug(name, slug, sizeof(slug));
#ifdef _WIN32
	snprintf(out, out_len, "%s\\%s.json", dir, slug);
#else
	snprintf(out, out_len, "%s/%s.json", dir, slug);
#endif
	return true;
}

extern "C" int pd_sets_list(struct pd_set_entry *out, int max)
{
	char dir[512];
	if (!pd_sets_dir(dir, sizeof(dir)))
		return 0;
	std::vector<pd_set_entry> v;
	os_dir_t *d = os_opendir(dir);
	if (!d)
		return 0;
	struct os_dirent *e;
	while ((e = os_readdir(d)) != nullptr) {
		if (e->directory)
			continue;
		const size_t n = strlen(e->d_name);
		if (n < 6 || strcmp(e->d_name + n - 5, ".json") != 0)
			continue;
		pd_set_entry ent;
		/* std::string: gcc -Wformat-truncation rejects dir[512] + d_name[256] into path[512] */
#ifdef _WIN32
		const std::string full = std::string(dir) + "\\" + e->d_name;
#else
		const std::string full = std::string(dir) + "/" + e->d_name;
#endif
		if (full.size() >= sizeof(ent.path))
			continue;
		memcpy(ent.path, full.c_str(), full.size() + 1);
		struct pl_set s;
		char err[128];
		if (!pd_sets_load_file(ent.path, &s, err, sizeof(err)))
			continue;
		memcpy(ent.name, s.name, sizeof(ent.name));
		ent.name[sizeof(ent.name) - 1] = '\0';
		v.push_back(ent);
	}
	os_closedir(d);
	std::sort(v.begin(), v.end(),
		  [](const pd_set_entry &a, const pd_set_entry &b) { return strcmp(a.name, b.name) < 0; });
	int count = 0;
	for (const auto &ent : v) {
		if (count >= max)
			break;
		out[count++] = ent;
	}
	return count;
}

extern "C" bool pd_sets_load_file(const char *path, struct pl_set *out, char *err, size_t err_len)
{
	const std::string data = read_file(path);
	if (data.empty()) {
		if (err && err_len)
			snprintf(err, err_len, "can't read %s", path);
		return false;
	}
	return pd_set_parse(data.c_str(), out, err, err_len);
}

extern "C" bool pd_sets_load_name(const char *name, struct pl_set *out, char *err, size_t err_len)
{
	char path[512];
	if (!pd_sets_path_for(name, path, sizeof(path)))
		return false;
	return pd_sets_load_file(path, out, err, err_len);
}

extern "C" bool pd_sets_save(const struct pl_set *s, char *path_out, size_t path_len)
{
	char path[512];
	if (!pd_sets_path_for(s->name, path, sizeof(path)))
		return false;
	char *json = pd_set_write(s);
	if (!json)
		return false;
	const bool ok = write_file(path, json);
	pd_set_free(json);
	if (ok && path_out)
		snprintf(path_out, path_len, "%s", path);
	return ok;
}

extern "C" bool pd_sets_delete(const char *name)
{
	char path[512];
	if (!pd_sets_path_for(name, path, sizeof(path)))
		return false;
	return os_unlink(path) == 0;
}

extern "C" bool pd_sets_dirty(const struct pl_set *s)
{
	char path[512];
	if (!pd_sets_path_for(s->name, path, sizeof(path)))
		return true;
	const std::string disk = read_file(path);
	if (disk.empty())
		return true;
	char *json = pd_set_write(s);
	const bool dirty = !json || disk != json;
	pd_set_free(json);
	return dirty;
}
