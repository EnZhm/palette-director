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

#include "pd-ui.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <cstdlib>
#include <string>

#if defined(ENABLE_QT)
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#if defined(ENABLE_FRONTEND_API)
#include <obs-frontend-api.h>
#endif
#endif

/*
Sandbox test hook: PD_UI_AUTO (env) makes the helpers non-interactive. Each helper takes the
next '|' separated answer from PD_UI_AUTO_ANSWERS (env) or from pd_ui_set_answers():
"y"/"n" for confirms, text for prompts, a path for the file picker, "-" = cancel.
Every auto answer is logged. Unset in normal use.
*/
namespace {
std::string g_answers;
bool g_answers_init = false;

bool auto_mode()
{
	const char *e = getenv("PD_UI_AUTO");
	return e && *e == '1';
}

bool next_answer(std::string &out)
{
	if (!g_answers_init) {
		const char *e = getenv("PD_UI_AUTO_ANSWERS");
		g_answers = e ? e : "";
		g_answers_init = true;
	}
	if (g_answers.empty())
		return false;
	const size_t bar = g_answers.find('|');
	out = g_answers.substr(0, bar);
	g_answers = bar == std::string::npos ? std::string() : g_answers.substr(bar + 1);
	return true;
}

#if defined(ENABLE_QT)
QWidget *parent_window()
{
#if defined(ENABLE_FRONTEND_API)
	return static_cast<QWidget *>(obs_frontend_get_main_window());
#else
	return nullptr;
#endif
}
#endif
} // namespace

extern "C" void pd_ui_set_answers(const char *answers)
{
	g_answers = answers ? answers : "";
	g_answers_init = true;
}

extern "C" bool pd_ui_confirm(const char *title, const char *text)
{
	if (auto_mode()) {
		std::string a;
		const bool yes = !next_answer(a) || a == "y";
		blog(LOG_INFO, "[palette-director] ui (auto): confirm '%s' -> %s", text, yes ? "yes" : "no");
		return yes;
	}
#if defined(ENABLE_QT)
	return QMessageBox::question(parent_window(), QString::fromUtf8(title), QString::fromUtf8(text),
				     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
#else
	(void)title;
	(void)text;
	return true;
#endif
}

extern "C" char *pd_ui_prompt(const char *title, const char *label, const char *def)
{
	if (auto_mode()) {
		std::string a;
		if (!next_answer(a))
			a = def ? def : "";
		blog(LOG_INFO, "[palette-director] ui (auto): prompt '%s' -> '%s'", label, a.c_str());
		return a == "-" ? nullptr : bstrdup(a.c_str());
	}
#if defined(ENABLE_QT)
	bool ok = false;
	const QString s = QInputDialog::getText(parent_window(), QString::fromUtf8(title), QString::fromUtf8(label),
						QLineEdit::Normal, QString::fromUtf8(def ? def : ""), &ok)
				  .trimmed();
	if (!ok || s.isEmpty())
		return nullptr;
	return bstrdup(s.toUtf8().constData());
#else
	(void)title;
	(void)label;
	return def ? bstrdup(def) : nullptr;
#endif
}

extern "C" char *pd_ui_open_json(const char *title, const char *start_dir)
{
	if (auto_mode()) {
		std::string a;
		if (!next_answer(a))
			a = "-";
		blog(LOG_INFO, "[palette-director] ui (auto): open file -> '%s'", a.c_str());
		return a == "-" ? nullptr : bstrdup(a.c_str());
	}
#if defined(ENABLE_QT)
	const QString p = QFileDialog::getOpenFileName(parent_window(), QString::fromUtf8(title),
						       QString::fromUtf8(start_dir ? start_dir : ""),
						       "Palette Director set (*.json)");
	if (p.isEmpty())
		return nullptr;
	return bstrdup(p.toUtf8().constData());
#else
	(void)title;
	(void)start_dir;
	return nullptr;
#endif
}

extern "C" void pd_ui_message(const char *title, const char *text)
{
	blog(LOG_WARNING, "[palette-director] %s: %s", title, text);
	if (auto_mode())
		return;
#if defined(ENABLE_QT)
	QMessageBox::warning(parent_window(), QString::fromUtf8(title), QString::fromUtf8(text));
#endif
}
