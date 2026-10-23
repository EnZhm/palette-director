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

/*
Palette Director dock (spec 6, mock v2 dock panel; v1 amendments phase C: the main UI).

Top section (always visible): instance picker, status line, beat lights, palette pads (up to
16), controls incl. PANIC, 64-beat timeline and a "now" sentence. Below it a collapsible
QTabWidget (Set, Palettes, Look, Changes, Director, Log) whose open/closed state and tab are
remembered in the dock's own config file (plugin_config/palette-director/dock.json).

Threading: everything here runs on the UI thread.
  - Live view: a ~30 Hz QTimer polls pdhost::read_view (copy under the host's view mutex).
  - Commands (go live, next, panic, ...): pdhost::command -> host command queue -> worker.
  - Edits: palette_lock_commit / palette_lock_set_audio / palette_lock_set_action write the
    filter's settings and call obs_source_update, exactly the path the properties window
    uses; the filter then pushes the set into the Director. No new cross-thread paths.
Closing the dock only stops the poll timer; the Director keeps running.
*/

#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <QAbstractItemModel>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QEvent>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QListWidget>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "palette-lock-filter.h"
#include "palette-lock-host.h"
#include "palette-lock-host.hpp"
#include "sets/set-store.h"

namespace {

QColor qc(const struct pl_colour &c)
{
	return QColor::fromRgbF(std::clamp(c.r, 0.0f, 1.0f), std::clamp(c.g, 0.0f, 1.0f), std::clamp(c.b, 0.0f, 1.0f));
}

QString T(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const QString kDot = QString::fromUtf8("\xC2\xB7");

QString hex_of(const struct pl_colour &c)
{
	char h[PL_HEX_LEN];
	pl_colour_to_hex(c, h);
	return QString::fromLatin1(h);
}

/* colour at position t (0..1) exactly as the shader maps it (duotone: smoothstep of first+last) */
QColor ramp_at(const struct pl_palette &pal, double t)
{
	if (pal.n_colours < 2)
		return qc(pal.colours[0]);
	auto lerp = [](const pl_colour &a, const pl_colour &b, double f) {
		return QColor::fromRgbF(static_cast<float>(std::clamp(a.r + (b.r - a.r) * f, 0.0, 1.0)),
					static_cast<float>(std::clamp(a.g + (b.g - a.g) * f, 0.0, 1.0)),
					static_cast<float>(std::clamp(a.b + (b.b - a.b) * f, 0.0, 1.0)));
	};
	if (pal.mode == PL_MODE_DUOTONE)
		return lerp(pal.colours[0], pal.colours[pal.n_colours - 1], t * t * (3.0 - 2.0 * t));
	const double x = t * (pal.n_colours - 1);
	const int i = std::min(static_cast<int>(std::floor(x)), pal.n_colours - 2);
	return lerp(pal.colours[i], pal.colours[i + 1], x - i);
}

void fill_ramp(QPainter &p, const QRectF &r, const struct pl_palette &pal, double radius = 3.0)
{
	QLinearGradient g(r.topLeft(), r.topRight());
	const int stops = pal.mode == PL_MODE_DUOTONE ? 16 : std::max(2, pal.n_colours);
	for (int i = 0; i < stops; i++) {
		const double t = static_cast<double>(i) / (stops - 1);
		g.setColorAt(t, ramp_at(pal, t));
	}
	QPainterPath path;
	path.addRoundedRect(r, radius, radius);
	p.fillPath(path, g);
}

/* RAII strong reference to the selected filter source */
struct SrcRef {
	obs_source_t *s = nullptr;
	explicit SrcRef(uint64_t id) : s(id ? pdhost::filter_source(id) : nullptr) {}
	~SrcRef()
	{
		if (s)
			obs_source_release(s);
	}
	SrcRef(const SrcRef &) = delete;
	SrcRef &operator=(const SrcRef &) = delete;
};

/* ---- palette pad ---- */
class PadWidget : public QWidget {
public:
	explicit PadWidget(QWidget *parent) : QWidget(parent)
	{
		setMinimumSize(132, 74);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		setCursor(Qt::PointingHandCursor);
	}

	struct pl_palette pal = {};
	int index = 0;
	bool live = false, next = false, breakdown = false;
	float pfade = 1.0f;
	std::function<void(int)> on_tap;

protected:
	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && on_tap)
			on_tap(index);
	}

	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QPalette &pl = palette();
		const QColor hl = pl.color(QPalette::Highlight);
		const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
		QPainterPath bg;
		bg.addRoundedRect(r, 7, 7);
		p.fillPath(bg, pl.color(QPalette::Base));
		QPen pen(live ? hl : (next ? pl.color(QPalette::WindowText) : pl.color(QPalette::Mid)));
		pen.setWidthF(live ? 3.0 : (next ? 1.5 : 1.0));
		if (next && !live)
			pen.setStyle(Qt::DashLine);
		p.setPen(pen);
		p.drawPath(bg);

		QFont f = font();
		f.setBold(true);
		p.setFont(f);
		p.setPen(pl.color(QPalette::Text));
		const QRectF nameR(r.left() + 8, r.top() + 5, r.width() - 54, 18);
		p.drawText(nameR, Qt::AlignLeft | Qt::AlignVCenter,
			   p.fontMetrics().elidedText(QString::fromUtf8(pal.name), Qt::ElideRight, (int)nameR.width()));

		QFont bf = font();
		bf.setBold(true);
		bf.setPointSizeF(std::max(6.0, bf.pointSizeF() * 0.78));
		p.setFont(bf);
		if (live || next) {
			p.setPen(live ? hl : pl.color(QPalette::WindowText));
			p.drawText(QRectF(r.right() - 48, r.top() + 5, 42, 16), Qt::AlignRight | Qt::AlignVCenter,
				   live ? "LIVE" : "NEXT");
		}

		fill_ramp(p, QRectF(r.left() + 8, r.top() + 26, r.width() - 16, 20), pal);

		QFont sf = font();
		sf.setPointSizeF(std::max(6.0, sf.pointSizeF() * 0.8));
		p.setFont(sf);
		p.setPen(pl.color(QPalette::PlaceholderText));
		QString sub = pal.mode == PL_MODE_DUOTONE ? QString("duo") : QString("grad");
		if (breakdown)
			sub += QString(" %1 breakdown").arg(kDot);
		if (index < PL_MAX_PALETTE_HOTKEYS)
			sub += QString(" %1 Ctrl+%2").arg(kDot).arg(index + 1);
		p.drawText(QRectF(r.left() + 8, r.top() + 49, r.width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter,
			   sub);

		if (live) {
			const double w = (r.width() - 8) * std::clamp(pfade, 0.0f, 1.0f);
			p.fillRect(QRectF(r.left() + 4, r.bottom() - 5, w, 3), hl);
		}
	}
};

/* ---- large ramp preview (palette editor) ---- */
class RampWidget : public QWidget {
public:
	explicit RampWidget(QWidget *parent) : QWidget(parent) { setMinimumHeight(44); }
	struct pl_palette pal = {};

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		if (pal.n_colours >= 2)
			fill_ramp(p, QRectF(rect()).adjusted(1, 1, -1, -1), pal, 5.0);
	}
};

/* ---- 4 beat lights ---- */
class BeatLights : public QWidget {
public:
	explicit BeatLights(QWidget *parent) : QWidget(parent) { setFixedSize(4 * 20, 16); }
	int on = -1;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		for (int i = 0; i < 4; i++) {
			p.setPen(Qt::NoPen);
			p.setBrush(i == on ? palette().color(QPalette::Highlight) : palette().color(QPalette::Mid));
			p.drawEllipse(QRectF(i * 20 + 2, 1, 14, 14));
		}
	}
};

/* ---- v1.0.1 audio meter: input level (dBFS after gain, CLIP > -1 dBFS), bass bar with the kick
 * threshold marked, kick light. Fed from the DockView snapshot (copied under the host's view mutex). */
class AudioMeter : public QWidget {
public:
	explicit AudioMeter(QWidget *parent) : QWidget(parent)
	{
		setMinimumHeight(34);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}
	float db = -120.0f, bass = 0.0f, thr = 0.4f;
	bool clip = false, silent = true;
	int kick_flash = 0; // frames left to light the kick lamp

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const QPalette &pl = palette();
		const double W = width(), lamp = 26.0, lbl = 46.0;
		const double bw = std::max(40.0, W - lamp - lbl - 10.0);
		QFont f = font();
		f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.8));
		f.setBold(true);
		p.setFont(f);
		p.setPen(pl.color(QPalette::PlaceholderText));
		p.drawText(QRectF(0, 0, lbl, 15), Qt::AlignLeft | Qt::AlignVCenter, "LEVEL");
		p.drawText(QRectF(0, 18, lbl, 15), Qt::AlignLeft | Qt::AlignVCenter, "BASS");
		/* level: -60..0 dBFS */
		const QRectF lr(lbl, 2, bw, 11);
		p.fillRect(lr, pl.color(QPalette::Base));
		const double lv = std::clamp((static_cast<double>(db) + 60.0) / 60.0, 0.0, 1.0);
		const QColor lc = clip ? QColor(255, 60, 90)
				       : (db > -6.0f ? QColor(255, 190, 40) : QColor(60, 200, 110));
		p.fillRect(QRectF(lr.left(), lr.top(), lr.width() * lv, lr.height()), lc);
		for (int t = -48; t < 0; t += 12) {
			const double x = lr.left() + lr.width() * (t + 60.0) / 60.0;
			p.fillRect(QRectF(x, lr.top(), 1, lr.height()), pl.color(QPalette::Mid));
		}
		p.setPen(pl.color(QPalette::Text));
		p.drawText(lr.adjusted(4, -1, -4, 1), Qt::AlignRight | Qt::AlignVCenter,
			   clip ? QString("CLIP %1 dBFS").arg(db, 0, 'f', 1)
				: (db <= -119.0f ? QString("no signal") : QString("%1 dBFS").arg(db, 0, 'f', 1)));
		/* bass bar with the kick threshold */
		const QRectF br(lbl, 19, bw, 11);
		p.fillRect(br, pl.color(QPalette::Base));
		p.fillRect(QRectF(br.left(), br.top(), br.width() * std::clamp(bass, 0.0f, 1.0f), br.height()),
			   pl.color(QPalette::Highlight));
		const double tx = br.left() + br.width() * std::clamp(thr, 0.0f, 1.0f);
		p.fillRect(QRectF(tx - 1, br.top() - 2, 2, br.height() + 4), QColor(255, 60, 90));
		/* kick light */
		const QRectF kr(W - lamp + 2, 4, lamp - 6, lamp - 6);
		p.setPen(QPen(pl.color(QPalette::Mid), 1));
		p.setBrush(kick_flash > 0 ? QColor(255, 60, 90) : pl.color(QPalette::Base));
		p.drawEllipse(kr);
		p.setPen(pl.color(QPalette::PlaceholderText));
		p.drawText(QRectF(W - lamp - 4, 22, lamp + 4, 12), Qt::AlignCenter, "KICK");
	}
};

/* ---- 64-beat timeline (mock drawTimeline) ---- */
class TimelineWidget : public QWidget {
public:
	explicit TimelineWidget(QWidget *parent) : QWidget(parent) { setMinimumHeight(96); }

	std::vector<pdhost::TimelineSample> tl;
	double now = 0.0;
	int phrase = 32;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const double W = width(), H = height();
		const QPalette &pl = palette();
		p.fillRect(rect(), pl.color(QPalette::Base));
		auto X = [&](double b) {
			return W - (now - b) / 64.0 * W;
		};
		const QColor sec[3] = {QColor(80, 140, 255, 46), QColor(255, 182, 72, 70), QColor(255, 92, 122, 80)};
		p.setRenderHint(QPainter::Antialiasing, false); // abutting translucent bands: no seams
		for (size_t i = 0; i < tl.size(); i++) {
			const int x0 = static_cast<int>(std::floor(X(tl[i].beat)));
			const int x1 = i + 1 < tl.size() ? static_cast<int>(std::floor(X(tl[i + 1].beat)))
							 : static_cast<int>(W);
			if (x1 <= x0)
				continue;
			p.fillRect(QRect(x0, 10, x1 - x0, static_cast<int>(H) - 10),
				   sec[std::clamp(tl[i].section, 0, 2)]);
			p.fillRect(QRect(x0, 0, x1 - x0, 8), QColor::fromRgbF(tl[i].r, tl[i].g, tl[i].b));
		}
		p.setRenderHint(QPainter::Antialiasing, true);
		QPainterPath bass, mix;
		for (size_t i = 0; i < tl.size(); i++) {
			const double x = X(tl[i].beat);
			const double yb = H - 4 - std::min(1.0f, tl[i].bass) * H * 0.3;
			const double ym = 16 + (1.0 - tl[i].mix) * (H * 0.5);
			if (i == 0) {
				bass.moveTo(x, yb);
				mix.moveTo(x, ym);
			} else {
				bass.lineTo(x, yb);
				mix.lineTo(x, ym);
			}
		}
		QColor bc = pl.color(QPalette::Text);
		bc.setAlpha(90);
		p.setPen(QPen(bc, 1.0));
		p.drawPath(bass);
		QColor tc = pl.color(QPalette::Text);
		tc.setAlpha(140);
		const int ph = std::max(1, phrase);
		for (double b = std::ceil((now - 64.0) / ph) * ph; b <= now; b += ph)
			p.fillRect(QRectF(X(b), 10, 1, H), tc);
		p.setPen(QPen(pl.color(QPalette::Highlight), 2.0));
		p.drawPath(mix);
	}
};

/* In a scrolling dock, a wheel over a spin box / combo / slider must scroll the dock, not
 * silently change a setting. Such widgets only take the wheel once they have focus. */
class WheelGuard : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *o, QEvent *e) override
	{
		if (e->type() == QEvent::Wheel) {
			auto *w = qobject_cast<QWidget *>(o);
			if (w && !w->hasFocus()) {
				e->ignore();
				return true;
			}
		}
		return QObject::eventFilter(o, e);
	}
};

void guard_wheel(QWidget *root, QObject *guard)
{
	for (QWidget *w : root->findChildren<QWidget *>()) {
		if (qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QComboBox *>(w) ||
		    qobject_cast<QAbstractSlider *>(w)) {
			w->setFocusPolicy(Qt::StrongFocus);
			w->installEventFilter(guard);
		}
	}
}

/* coloured status chip: tinted background over the theme, theme text colour */
void set_chip(QLabel *l, const QString &text, const QColor &tint)
{
	if (l->text() != text)
		l->setText(text);
	const QString ss = QString("QLabel{padding:3px 10px;border-radius:10px;font-weight:700;"
				   "background:rgba(%1,%2,%3,90);border:1px solid rgba(%1,%2,%3,200);}")
				   .arg(tint.red())
				   .arg(tint.green())
				   .arg(tint.blue());
	if (l->styleSheet() != ss)
		l->setStyleSheet(ss);
}

QLabel *help_label(const char *key, QWidget *parent)
{
	auto *l = new QLabel(T(key), parent);
	l->setWordWrap(true);
	QFont f = l->font();
	f.setPointSizeF(std::max(6.0, f.pointSizeF() * 0.88));
	l->setFont(f);
	l->setForegroundRole(QPalette::PlaceholderText);
	return l;
}

/* slider with a live value label; model value = slider / scale */
struct ValueSlider {
	QSlider *s = nullptr;
	QLabel *v = nullptr;
	double scale = 100.0;
	bool percent = true;
	void show_value() const
	{
		if (percent)
			v->setText(QString("%1%").arg(s->value()));
		else
			v->setText(QString::number(s->value() / scale, 'f', scale <= 2.0 ? 1 : 2));
	}
};

/* ---- the dock ---- */
class PaletteDirectorDock : public QWidget {
public:
	explicit PaletteDirectorDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName("PaletteDirectorDock");
		pl_set_default(&model);
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		scroll->setFrameShape(QFrame::NoFrame);
		outer->addWidget(scroll);
		body = new QWidget(scroll);
		scroll->setWidget(body);
		auto *v = new QVBoxLayout(body);
		v->setContentsMargins(8, 8, 8, 8);
		v->setSpacing(8);

		build_top(v);
		build_tabs(v);
		v->addStretch(1);
		guard_wheel(tabs, new WheelGuard(this));
		picker->setFocusPolicy(Qt::StrongFocus);
		picker->installEventFilter(new WheelGuard(this));

		timer = new QTimer(this);
		timer->setInterval(33); // ~30 Hz
		QObject::connect(timer, &QTimer::timeout, this, [this] { poll(); });
		load_dock_config();
	}

	void select_filter(obs_source_t *filter)
	{
		const uint64_t id = pdhost::id_for_filter(filter);
		if (!id)
			return;
		want_select = id;
		refresh_instances(true);
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		blog(LOG_INFO, "[palette-director] dock shown: 30 Hz poll started");
		pads_sig.clear();
		timer->start();
		poll();
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		timer->stop();
		blog(LOG_INFO, "[palette-director] dock hidden: poll stopped (directors keep running)");
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		pads_sig.clear(); // re-flow the pad grid
	}

	/* ---------------------------------------------------------------- top section */
	void build_top(QVBoxLayout *v)
	{
		auto *pickRow = new QHBoxLayout();
		pickRow->addWidget(new QLabel(T("Dock.Instance"), body));
		picker = new QComboBox(body);
		picker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		pickRow->addWidget(picker, 1);
		v->addLayout(pickRow);
		QObject::connect(picker, &QComboBox::currentIndexChanged, this, [this](int i) {
			if (rebuilding || i < 0)
				return;
			attach(picker->itemData(i).toULongLong(), picker->itemText(i));
		});

		empty = new QLabel(T("Dock.NoInstance"), body);
		empty->setWordWrap(true);
		v->addWidget(empty);

		live_area = new QWidget(body);
		auto *lv = new QVBoxLayout(live_area);
		lv->setContentsMargins(0, 0, 0, 0);
		lv->setSpacing(8);
		v->addWidget(live_area);

		/* status line */
		auto *statusRow = new QHBoxLayout();
		secChip = new QLabel(live_area);
		lockChip = new QLabel(live_area);
		stateChip = new QLabel(live_area);
		statusRow->addWidget(secChip);
		statusRow->addWidget(lockChip);
		statusRow->addWidget(stateChip);
		statusRow->addStretch(1);
		beats = new BeatLights(live_area);
		statusRow->addWidget(beats);
		lv->addLayout(statusRow);

		statusText = new QLabel(live_area);
		QFont sf = statusText->font();
		sf.setPointSizeF(sf.pointSizeF() * 1.15);
		sf.setBold(true);
		statusText->setFont(sf);
		statusText->setWordWrap(true);
		lv->addWidget(statusText);
		sAudio = new QLabel(live_area);
		sAudio->setForegroundRole(QPalette::PlaceholderText);
		lv->addWidget(sAudio);
		meter = new AudioMeter(live_area);
		meter->setToolTip(T("Dock.MeterHelp"));
		lv->addWidget(meter);

		/* pads */
		auto *ph = new QLabel(T("Dock.PalettesTap"), live_area);
		ph->setForegroundRole(QPalette::PlaceholderText);
		ph->setWordWrap(true);
		lv->addWidget(ph);
		padGrid = new QGridLayout();
		padGrid->setSpacing(6);
		lv->addLayout(padGrid);
		for (int i = 0; i < PL_MAX_PALETTES; i++) {
			pads[i] = new PadWidget(live_area);
			pads[i]->index = i;
			pads[i]->on_tap = [this](int idx) {
				send(PD_CMD_GO_LIVE, idx);
			};
			pads[i]->hide();
		}

		/* controls */
		auto *ctrls = new QGridLayout();
		ctrls->setSpacing(6);
		bForceOff = new QPushButton(T("Dock.ForceOff"), live_area);
		bForceOn = new QPushButton(T("Dock.ForceOn"), live_area);
		bPause = new QPushButton(T("Dock.Pause"), live_area);
		bPrev = new QPushButton(T("Dock.Prev"), live_area);
		bNext = new QPushButton(T("Dock.Next"), live_area);
		bRotate = new QPushButton(T("Dock.Rotate"), live_area);
		bForceOn->setCheckable(true);
		bPause->setCheckable(true);
		/* let the dock get narrow without a horizontal scrollbar (labels may clip, never overflow) */
		for (QPushButton *b : {bForceOff, bForceOn, bPause, bPrev, bNext, bRotate})
			b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		ctrls->addWidget(bForceOff, 0, 0);
		ctrls->addWidget(bForceOn, 0, 1);
		ctrls->addWidget(bPause, 0, 2);
		ctrls->addWidget(bPrev, 1, 0);
		ctrls->addWidget(bNext, 1, 1);
		ctrls->addWidget(bRotate, 1, 2);
		lv->addLayout(ctrls);
		bPanic = new QPushButton(T("Dock.Panic"), live_area);
		/* v1.0.1: PANIC is a toggle and shows its live state; the red outline fills while active */
		bPanic->setCheckable(true);
		bPanic->setStyleSheet("QPushButton{border:2px solid rgba(255,70,100,220);padding:8px;font-weight:700;}"
				      "QPushButton:checked{background:rgba(255,60,90,200);color:white;}");
		bForceOn->setStyleSheet("QPushButton:checked{background:rgba(80,140,255,170);font-weight:700;}");
		lv->addWidget(bPanic);

		QObject::connect(bForceOff, &QPushButton::pressed, this, [this] { send(PD_CMD_FORCE_OFF); });
		/* the host logs every command with its origin ("dock: ...") */
		QObject::connect(bForceOff, &QPushButton::released, this, [this] { send(PD_CMD_RELEASE_FORCE); });
		QObject::connect(bForceOn, &QPushButton::clicked, this, [this] { send(PD_CMD_FORCE_ON); });
		QObject::connect(bPause, &QPushButton::clicked, this, [this] { send(PD_CMD_TOGGLE_PAUSE); });
		QObject::connect(bPrev, &QPushButton::clicked, this, [this] { send(PD_CMD_PREV); });
		QObject::connect(bNext, &QPushButton::clicked, this, [this] { send(PD_CMD_NEXT); });
		QObject::connect(bRotate, &QPushButton::clicked, this, [this] { send(PD_CMD_ROTATE_NOW); });
		QObject::connect(bPanic, &QPushButton::clicked, this, [this] { send(PD_CMD_PANIC); });

		timeline = new TimelineWidget(live_area);
		lv->addWidget(timeline);
		auto *legend = new QLabel(T("Dock.TimelineLegend"), live_area);
		legend->setForegroundRole(QPalette::PlaceholderText);
		legend->setWordWrap(true);
		lv->addWidget(legend);

		nowLbl = new QLabel(live_area);
		nowLbl->setWordWrap(true);
		QFont nf = nowLbl->font();
		nf.setBold(true);
		nowLbl->setFont(nf);
		nowLbl->setFrameShape(QFrame::StyledPanel);
		nowLbl->setContentsMargins(6, 4, 6, 4);
		lv->addWidget(nowLbl);
	}

	/* ---------------------------------------------------------------- tabs */
	void build_tabs(QVBoxLayout *v)
	{
		tabsToggle = new QToolButton(body);
		tabsToggle->setCheckable(true);
		tabsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		tabsToggle->setText(T("Dock.Settings"));
		tabsToggle->setArrowType(Qt::RightArrow);
		v->addWidget(tabsToggle);
		tabs = new QTabWidget(body);
		tabs->setVisible(false);
		v->addWidget(tabs);
		QObject::connect(tabsToggle, &QToolButton::toggled, this, [this](bool on) {
			tabs->setVisible(on);
			tabsToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
			save_dock_config();
		});
		QObject::connect(tabs, &QTabWidget::currentChanged, this, [this](int) { save_dock_config(); });

		build_set_tab();
		build_palettes_tab();
		build_look_tab();
		build_changes_tab();
		build_director_tab();
		build_log_tab();
	}

	void build_set_tab()
	{
		auto *w = new QWidget(tabs);
		auto *v = new QVBoxLayout(w);
		auto *row = new QHBoxLayout();
		row->addWidget(new QLabel(T("Set"), w));
		setSel = new QComboBox(w);
		row->addWidget(setSel, 1);
		v->addLayout(row);
		QObject::connect(setSel, &QComboBox::activated, this, [this](int i) {
			const QString name = setSel->itemText(i);
			SrcRef r(selected);
			if (!r.s || name == QString::fromUtf8(model.name))
				return;
			palette_lock_select_set(r.s, name.toUtf8().constData());
			after_set_change();
		});
		auto *btns = new QGridLayout();
		const char *acts[6][2] = {{"set.new", "New"},    {"set.dup", "Duplicate"}, {"set.ren", "Rename"},
					  {"set.del", "Delete"}, {"set.save", "Save"},     {"set.load", "LoadFile"}};
		for (int i = 0; i < 6; i++) {
			auto *b = new QPushButton(T(acts[i][1]), w);
			b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			const std::string act = acts[i][0];
			btns->addWidget(b, i / 3, i % 3);
			setBtns[i] = b;
			QObject::connect(b, &QPushButton::clicked, this, [this, act] {
				SrcRef r(selected);
				if (r.s) {
					blog(LOG_INFO, "[palette-director] dock: set action %s", act.c_str());
					palette_lock_set_action(r.s, act.c_str());
				}
				after_set_change();
			});
		}
		v->addLayout(btns);
		auto *form = new QFormLayout();
		synPl = new QLineEdit(w);
		form->addRow(T("SynesthesiaPlaylist"), synPl);
		v->addLayout(form);
		QObject::connect(synPl, &QLineEdit::editingFinished, this, [this] {
			struct pl_set ns = model;
			const QByteArray t = synPl->text().toUtf8();
			if (strcmp(ns.synesthesia_playlist, t.constData()) == 0)
				return;
			snprintf(ns.synesthesia_playlist, sizeof(ns.synesthesia_playlist), "%s", t.constData());
			commit(ns, -1, "Synesthesia playlist -> '" + synPl->text() + "'");
		});
		setPath = new QLabel(w);
		setPath->setWordWrap(true);
		setPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
		v->addWidget(setPath);
		v->addWidget(help_label("Dock.SetHelp", w));
		v->addStretch(1);
		tabs->addTab(w, T("Dock.Tab.Set"));
	}

	void build_palettes_tab()
	{
		auto *w = new QWidget(tabs);
		auto *v = new QVBoxLayout(w);
		palList = new QListWidget(w);
		palList->setDragDropMode(QAbstractItemView::InternalMove);
		palList->setDefaultDropAction(Qt::MoveAction);
		palList->setMinimumHeight(150);
		v->addWidget(palList);
		QObject::connect(palList, &QListWidget::currentRowChanged, this, [this](int row) {
			if (filling || row < 0)
				return;
			sel_pal = row;
			fill_editor();
		});
		QObject::connect(palList->model(), &QAbstractItemModel::rowsMoved, this,
				 [this] { QTimer::singleShot(0, this, [this] { on_palettes_reordered(); }); });

		auto *prow = new QGridLayout();
		bPalLive = new QPushButton(T("Dock.PalLive"), w);
		bPalBd = new QPushButton(T("Dock.PalBreakdown"), w);
		bPalNew = new QPushButton(T("Dock.PalNew"), w);
		bPalDup = new QPushButton(T("Duplicate"), w);
		bPalDel = new QPushButton(T("Delete"), w);
		int k = 0;
		for (auto *b : {bPalLive, bPalBd, bPalNew, bPalDup, bPalDel}) {
			b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			prow->addWidget(b, k / 3, k % 3);
			k++;
		}
		v->addLayout(prow);

		auto *form = new QFormLayout();
		palName = new QLineEdit(w);
		form->addRow(T("Dock.PalName"), palName);
		palMode = new QComboBox(w);
		palMode->addItem(T("Dock.ModeGradient"), PL_MODE_GRADIENT);
		palMode->addItem(T("Dock.ModeDuotone"), PL_MODE_DUOTONE);
		form->addRow(T("Dock.PalMode"), palMode);
		v->addLayout(form);
		ramp = new RampWidget(w);
		v->addWidget(ramp);

		swList = new QListWidget(w);
		swList->setViewMode(QListView::ListMode);
		swList->setFlow(QListView::LeftToRight);
		swList->setWrapping(false);
		swList->setIconSize(QSize(40, 40));
		swList->setFixedHeight(64);
		swList->setDragDropMode(QAbstractItemView::InternalMove);
		swList->setDefaultDropAction(Qt::MoveAction);
		swList->setSpacing(3);
		v->addWidget(swList);
		QObject::connect(swList->model(), &QAbstractItemModel::rowsMoved, this,
				 [this] { QTimer::singleShot(0, this, [this] { on_swatches_reordered(); }); });
		QObject::connect(swList, &QListWidget::currentRowChanged, this, [this](int row) {
			if (filling)
				return;
			sel_sw = row;
			fill_swatch_fields();
		});
		QObject::connect(swList, &QListWidget::itemClicked, this, [this] { pick_colour(); });

		auto *srow = new QHBoxLayout();
		swHex = new QLineEdit(w);
		swHex->setPlaceholderText("#RRGGBB");
		swHex->setMaxLength(7);
		swHex->setMaximumWidth(100);
		srow->addWidget(new QLabel(T("Dock.Hex"), w));
		srow->addWidget(swHex);
		bSwPick = new QPushButton(T("Dock.SwPick"), w);
		bSwRemove = new QPushButton(T("Dock.SwRemove"), w);
		srow->addWidget(bSwPick);
		srow->addWidget(bSwRemove);
		srow->addStretch(1);
		v->addLayout(srow);
		for (QPushButton *b : {bSwPick, bSwRemove})
			b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
		auto *crow = new QHBoxLayout();
		bSwAdd = new QPushButton(T("Dock.SwAdd"), w);
		bSwSort = new QPushButton(T("Dock.SwSort"), w);
		bSwRev = new QPushButton(T("Dock.SwReverse"), w);
		for (QPushButton *b : {bSwAdd, bSwSort, bSwRev}) {
			b->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
			crow->addWidget(b);
		}
		v->addLayout(crow);
		v->addWidget(help_label("Dock.PalettesHelp", w));

		QObject::connect(swHex, &QLineEdit::editingFinished, this, [this] { apply_hex(swHex->text()); });
		QObject::connect(bSwPick, &QPushButton::clicked, this, [this] { pick_colour(); });
		QObject::connect(bSwRemove, &QPushButton::clicked, this, [this] {
			struct pl_set ns = model;
			if (!valid_pal() || !pl_remove_colour(&ns.palettes[sel_pal], sel_sw))
				return;
			commit(ns, -1, QString("palette '%1' colour %2 removed").arg(pal_name()).arg(sel_sw + 1));
		});
		QObject::connect(bSwAdd, &QPushButton::clicked, this, [this] {
			struct pl_set ns = model;
			if (!valid_pal() || !pl_add_colour(&ns.palettes[sel_pal]))
				return;
			sel_sw = ns.palettes[sel_pal].n_colours - 1;
			commit(ns, -1, QString("palette '%1' colour added").arg(pal_name()));
		});
		QObject::connect(bSwSort, &QPushButton::clicked, this, [this] {
			struct pl_set ns = model;
			if (!valid_pal())
				return;
			pl_sort_colours(&ns.palettes[sel_pal]);
			commit(ns, -1, QString("palette '%1' sorted dark -> bright").arg(pal_name()));
		});
		QObject::connect(bSwRev, &QPushButton::clicked, this, [this] {
			struct pl_set ns = model;
			if (!valid_pal())
				return;
			pl_reverse_colours(&ns.palettes[sel_pal]);
			commit(ns, -1, QString("palette '%1' reversed").arg(pal_name()));
		});
		QObject::connect(palName, &QLineEdit::editingFinished, this, [this] {
			if (!valid_pal())
				return;
			const QByteArray nn = palName->text().trimmed().toUtf8();
			if (strcmp(nn.constData(), model.palettes[sel_pal].name) == 0)
				return;
			struct pl_set ns = model;
			const QString old = pal_name();
			if (!pl_rename_palette(&ns, sel_pal, nn.constData())) {
				palName->setText(old); // unique, non-empty name rule
				palName->setToolTip(T("Dock.NameRule"));
				blog(LOG_INFO,
				     "[palette-director] dock: rename of '%s' rejected (names must be unique)",
				     old.toUtf8().constData());
				return;
			}
			commit(ns, -1, QString("palette '%1' renamed to '%2'").arg(old, palName->text().trimmed()));
		});
		QObject::connect(palMode, &QComboBox::activated, this, [this](int i) {
			if (!valid_pal())
				return;
			struct pl_set ns = model;
			ns.palettes[sel_pal].mode = palMode->itemData(i).toInt();
			commit(ns, -1,
			       QString("palette '%1' mode -> %2").arg(pal_name(), i ? "duotone" : "gradient map"));
		});
		QObject::connect(bPalNew, &QPushButton::clicked, this, [this] {
			struct pl_set ns = model;
			int idx = -1;
			char nm[PL_NAME_MAX];
			snprintf(nm, sizeof(nm), "Palette %d", ns.n_palettes + 1);
			if (!pl_add_palette(&ns, nm, &idx))
				return;
			sel_pal = idx;
			commit(ns, -1, QString("palette '%1' added").arg(QString::fromUtf8(ns.palettes[idx].name)));
		});
		QObject::connect(bPalLive, &QPushButton::clicked, this, [this] {
			if (valid_pal())
				send(PD_CMD_GO_LIVE, sel_pal);
		});
		QObject::connect(bPalDup, &QPushButton::clicked, this, [this] {
			if (!valid_pal())
				return;
			struct pl_set ns = model;
			int live = view.st.live_palette, edit = sel_pal, idx = -1;
			const int live_before = live;
			const QString src = pal_name();
			if (!pl_duplicate_palette(&ns, sel_pal, &live, &edit, &idx))
				return;
			sel_pal = idx;
			commit(ns, live != live_before ? live : -1, QString("palette '%1' duplicated").arg(src));
		});
		QObject::connect(bPalDel, &QPushButton::clicked, this, [this] {
			if (!valid_pal())
				return;
			struct pl_set ns = model;
			int live = view.st.live_palette, edit = sel_pal;
			const QString gone = pal_name();
			if (!pl_delete_palette(&ns, sel_pal, &live, &edit))
				return;
			sel_pal = std::min(sel_pal, ns.n_palettes - 1);
			commit(ns, live, QString("palette '%1' deleted").arg(gone)); // indices changed: re-sync
		});
		QObject::connect(bPalBd, &QPushButton::clicked, this, [this] {
			if (!valid_pal())
				return;
			struct pl_set ns = model;
			const bool is_bd = pl_breakdown_index(&ns) == sel_pal;
			snprintf(ns.breakdown, sizeof(ns.breakdown), "%s", is_bd ? "" : ns.palettes[sel_pal].name);
			commit(ns, -1,
			       is_bd ? QString("breakdown palette cleared")
				     : QString("breakdown palette -> '%1'").arg(pal_name()));
		});
		tabs->addTab(w, T("Dock.Tab.Palettes"));
	}

	ValueSlider make_slider(QFormLayout *form, QWidget *w, const char *label, const char *help, int lo, int hi,
				bool percent, std::function<void(struct pl_set &, double)> set, const char *log_name,
				double scale = 100.0)
	{
		ValueSlider vs;
		vs.scale = scale;
		vs.s = new QSlider(Qt::Horizontal, w);
		vs.s->setRange(lo, hi);
		vs.v = new QLabel(w);
		vs.v->setMinimumWidth(44);
		vs.percent = percent;
		auto *row = new QHBoxLayout();
		row->addWidget(vs.s, 1);
		row->addWidget(vs.v);
		auto *box = new QVBoxLayout();
		box->setSpacing(1);
		box->addLayout(row);
		box->addWidget(help_label(help, w));
		form->addRow(T(label), box);
		QSlider *s = vs.s;
		const ValueSlider copy = vs;
		QObject::connect(s, &QSlider::valueChanged, this, [this, copy, set, log_name, s](int val) {
			copy.show_value();
			if (filling)
				return;
			struct pl_set ns = model;
			set(ns, val / copy.scale);
			/* while dragging apply silently; the release logs the final value */
			commit(ns, -1,
			       s->isSliderDown() ? QString() : QString("%1 -> %2").arg(log_name, copy.v->text()));
		});
		QObject::connect(s, &QSlider::sliderReleased, this, [copy, log_name] {
			blog(LOG_INFO, "[palette-director] dock edit: %s -> %s", log_name,
			     copy.v->text().toUtf8().constData());
		});
		return vs;
	}

	void build_look_tab()
	{
		auto *w = new QWidget(tabs);
		auto *form = new QFormLayout(w);
		form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
		lkCoverage = make_slider(
			form, w, "Coverage", "Coverage.Help", 0, 100, true,
			[](struct pl_set &s, double v) { s.strength = static_cast<float>(v); }, "coverage");
		lkBlack = make_slider(
			form, w, "InputBlack", "InputBlack.Help", 0, 50, true,
			[](struct pl_set &s, double v) { s.input_black = static_cast<float>(v); }, "input black");
		lkWhite = make_slider(
			form, w, "InputWhite", "InputWhite.Help", 50, 100, true,
			[](struct pl_set &s, double v) { s.input_white = static_cast<float>(v); }, "input white");
		lkBsrc = new QComboBox(w);
		lkBsrc->addItem(T("BrightnessFrom.Perceived"), PL_BSRC_PERCEIVED);
		lkBsrc->addItem(T("BrightnessFrom.Max"), PL_BSRC_MAX);
		lkBsrc->addItem(T("BrightnessFrom.Average"), PL_BSRC_AVERAGE);
		auto *bbox = new QVBoxLayout();
		bbox->setSpacing(1);
		bbox->addWidget(lkBsrc);
		bbox->addWidget(help_label("BrightnessFrom.Help", w));
		form->addRow(T("BrightnessFrom"), bbox);
		QObject::connect(lkBsrc, &QComboBox::activated, this, [this](int i) {
			struct pl_set ns = model;
			ns.brightness_source = lkBsrc->itemData(i).toInt();
			commit(ns, -1, "brightness from -> " + lkBsrc->itemText(i));
		});
		lkContrast = make_slider(
			form, w, "Contrast", "Contrast.Help", 50, 200, false,
			[](struct pl_set &s, double v) { s.contrast = static_cast<float>(v); }, "contrast");
		lkKeep = make_slider(
			form, w, "KeepBrightness", "KeepBrightness.Help", 0, 100, true,
			[](struct pl_set &s, double v) { s.preserve = static_cast<float>(v); }, "keep brightness");
		tabs->addTab(w, T("Dock.Tab.Look"));
	}

	void build_changes_tab()
	{
		auto *w = new QWidget(tabs);
		auto *form = new QFormLayout(w);
		chTrigger = new QComboBox(w);
		chTrigger->addItem(T("ChangeOn.Drop"), PL_TRIGGER_DROP);
		chTrigger->addItem(T("ChangeOn.Phrases"), PL_TRIGGER_PHRASES);
		chTrigger->addItem(T("ChangeOn.Random"), PL_TRIGGER_RANDOM);
		chTrigger->addItem(T("ChangeOn.Manual"), PL_TRIGGER_MANUAL);
		form->addRow(T("ChangeOn"), chTrigger);
		chEvery = new QSpinBox(w);
		chEvery->setRange(1, 16);
		form->addRow(T("EveryN"), chEvery);
		chChance = make_slider(
			form, w, "ChancePerPhrase", "Dock.ChanceHelp", 0, 100, true,
			[](struct pl_set &s, double v) { s.change_chance = static_cast<float>(v); }, "change chance");
		chOrder = new QComboBox(w);
		chOrder->addItem(T("Order.Sequence"), PL_ORDER_SEQUENCE);
		chOrder->addItem(T("Order.Shuffle"), PL_ORDER_SHUFFLE);
		form->addRow(T("Order"), chOrder);
		chXfade = new QDoubleSpinBox(w);
		chXfade->setRange(0.0, 16.0);
		chXfade->setSingleStep(0.5);
		chXfade->setDecimals(1);
		form->addRow(T("Dock.Crossfade"), chXfade);
		chBd = new QComboBox(w);
		form->addRow(T("BreakdownPalette"), chBd);

		QObject::connect(chTrigger, &QComboBox::activated, this, [this](int i) {
			struct pl_set ns = model;
			ns.trigger = chTrigger->itemData(i).toInt();
			commit(ns, -1, "change on -> " + chTrigger->itemText(i));
		});
		QObject::connect(chEvery, &QSpinBox::valueChanged, this, [this](int n) {
			if (filling)
				return;
			struct pl_set ns = model;
			ns.every_phrases = n;
			commit(ns, -1, QString("every %1 phrases").arg(n));
		});
		QObject::connect(chOrder, &QComboBox::activated, this, [this](int i) {
			struct pl_set ns = model;
			ns.order = chOrder->itemData(i).toInt();
			commit(ns, -1, "order -> " + chOrder->itemText(i));
		});
		QObject::connect(chXfade, &QDoubleSpinBox::valueChanged, this, [this](double b) {
			if (filling)
				return;
			struct pl_set ns = model;
			ns.crossfade_beats = static_cast<float>(b);
			commit(ns, -1, QString("crossfade -> %1 beats").arg(b));
		});
		QObject::connect(chBd, &QComboBox::activated, this, [this](int i) {
			struct pl_set ns = model;
			snprintf(ns.breakdown, sizeof(ns.breakdown), "%s",
				 chBd->itemData(i).toString().toUtf8().constData());
			commit(ns, -1, "breakdown palette -> " + chBd->itemText(i));
		});
		tabs->addTab(w, T("Dock.Tab.Changes"));
	}

	QCheckBox *add_check(QFormLayout *form, QWidget *w, const char *label, bool pl_set::*field,
			     const char *log_name)
	{
		auto *c = new QCheckBox(T(label), w);
		form->addRow(c);
		QObject::connect(c, &QCheckBox::toggled, this, [this, field, log_name](bool on) {
			if (filling)
				return;
			struct pl_set ns = model;
			ns.*field = on;
			commit(ns, -1, QString("%1 -> %2").arg(log_name, on ? "on" : "off"));
		});
		return c;
	}

	QSpinBox *add_int(QFormLayout *form, QWidget *w, const char *label, int lo, int hi, int pl_set::*field,
			  const char *log_name)
	{
		auto *sb = new QSpinBox(w);
		sb->setRange(lo, hi);
		form->addRow(T(label), sb);
		QObject::connect(sb, &QSpinBox::valueChanged, this, [this, field, log_name](int n) {
			if (filling)
				return;
			struct pl_set ns = model;
			ns.*field = n;
			commit(ns, -1, QString("%1 -> %2").arg(log_name).arg(n));
		});
		return sb;
	}

	void build_director_tab()
	{
		auto *w = new QWidget(tabs);
		auto *form = new QFormLayout(w);
		drEnabled = add_check(form, w, "Dock.DirEnabled", &pl_set::dir_enabled, "director enabled");
		drAudio = new QComboBox(w);
		form->addRow(T("AudioSource"), drAudio);
		QObject::connect(drAudio, &QComboBox::activated, this, [this](int i) {
			SrcRef r(selected);
			if (!r.s)
				return;
			audio_setting = drAudio->itemData(i).toString();
			palette_lock_set_audio(r.s, audio_setting.toUtf8().constData());
		});
		drBpm = new QSpinBox(w);
		drBpm->setRange(60, 200);
		form->addRow(T("FallbackBpm"), drBpm);
		QObject::connect(drBpm, &QSpinBox::valueChanged, this, [this](int n) {
			if (filling)
				return;
			struct pl_set ns = model;
			ns.fallback_bpm = static_cast<float>(n);
			commit(ns, -1, QString("fallback BPM -> %1").arg(n));
		});
		drPhrase = new QComboBox(w);
		drPhrase->addItem("16 beats", 16);
		drPhrase->addItem("32 beats", 32);
		form->addRow(T("PhraseLength"), drPhrase);
		QObject::connect(drPhrase, &QComboBox::activated, this, [this](int i) {
			struct pl_set ns = model;
			ns.phrase_beats = drPhrase->itemData(i).toInt();
			commit(ns, -1, "phrase length -> " + drPhrase->itemText(i));
		});
		drFade = new QDoubleSpinBox(w);
		drFade->setRange(0.25, 8.0);
		drFade->setSingleStep(0.25);
		form->addRow(T("LockFade"), drFade);
		QObject::connect(drFade, &QDoubleSpinBox::valueChanged, this, [this](double b) {
			if (filling)
				return;
			struct pl_set ns = model;
			ns.lock_fade_beats = static_cast<float>(b);
			commit(ns, -1, QString("lock fade -> %1 beats").arg(b));
		});
		/* v1.0.1 audio sensitivity */
		drGain = make_slider(
			form, w, "InputGain", "InputGain.Help", -24, 48, false,
			[](struct pl_set &s, double v) { s.input_gain_db = static_cast<float>(v); }, "input gain dB",
			2.0);
		drSens = make_slider(
			form, w, "KickSensitivity", "KickSensitivity.Help", 0, 100, true,
			[](struct pl_set &s, double v) { s.kick_sensitivity = static_cast<float>(v); },
			"kick sensitivity");

		auto *hdr = new QLabel(T("LockBehaviours"), w);
		QFont hf = hdr->font();
		hf.setBold(true);
		hdr->setFont(hf);
		form->addRow(hdr);
		drRandom = add_check(form, w, "RandomOff", &pl_set::random_off, "random off");
		drRandChance = make_slider(
			form, w, "ChancePerPhrase", "Dock.RandomOffHelp", 0, 100, true,
			[](struct pl_set &s, double v) { s.random_chance = static_cast<float>(v); },
			"random-off chance");
		drBarsMin = add_int(form, w, "BarsOffMin", 1, 8, &pl_set::bars_min, "bars off min");
		drBarsMax = add_int(form, w, "BarsOffMax", 1, 16, &pl_set::bars_max, "bars off max");
		drCool = add_int(form, w, "Cooldown", 0, 8, &pl_set::cooldown_phrases, "cooldown");
		drOffBd = add_check(form, w, "OffOnBreakdown", &pl_set::off_on_breakdown, "off on breakdown");
		drOffDrop = add_check(form, w, "OffOnDrop", &pl_set::off_on_drop, "off on drop");
		drDropBars = add_int(form, w, "RawBarsAtDrop", 1, 8, &pl_set::drop_bars, "raw bars at drop");
		drBreath = make_slider(
			form, w, "Breathing", "Dock.BreathingHelp", 0, 100, true,
			[](struct pl_set &s, double v) { s.breathing = static_cast<float>(v); }, "breathing");
		drRotation = add_check(form, w, "Rotation", &pl_set::rotation, "rotation");
		auto *seedRow = new QHBoxLayout();
		drSeed = new QSpinBox(w);
		drSeed->setRange(0, 99999);
		auto *reroll = new QPushButton(T("Reroll"), w);
		seedRow->addWidget(drSeed, 1);
		seedRow->addWidget(reroll);
		form->addRow(T("Seed"), seedRow);
		QObject::connect(drSeed, &QSpinBox::valueChanged, this, [this](int n) {
			if (filling)
				return;
			struct pl_set ns = model;
			ns.seed = static_cast<unsigned>(n);
			commit(ns, -1, QString("seed -> %1").arg(n));
		});
		QObject::connect(reroll, &QPushButton::clicked, this, [this] {
			struct pl_set ns = model;
			ns.seed = static_cast<unsigned>(rand() % 100000);
			commit(ns, -1, QString("seed rerolled -> %1").arg(ns.seed));
		});
		tabs->addTab(w, T("Dock.Tab.Director"));
	}

	void build_log_tab()
	{
		logText = new QPlainTextEdit(tabs);
		logText->setReadOnly(true);
		logText->setMinimumHeight(220);
		QFont mf("Consolas");
		mf.setStyleHint(QFont::Monospace);
		logText->setFont(mf);
		tabs->addTab(logText, T("Dock.Tab.Log"));
	}

	/* ---------------------------------------------------------------- dock config */
	void load_dock_config()
	{
		char *path = obs_module_config_path("dock.json");
		obs_data_t *d = path ? obs_data_create_from_json_file(path) : nullptr;
		bfree(path);
		const bool open = d ? obs_data_get_bool(d, "tabs_open") : false;
		const int tab = d ? static_cast<int>(obs_data_get_int(d, "tab")) : 0;
		obs_data_release(d);
		loading_cfg = true;
		tabs->setCurrentIndex(std::clamp(tab, 0, tabs->count() - 1));
		tabsToggle->setChecked(open);
		loading_cfg = false;
	}

	void save_dock_config()
	{
		if (loading_cfg)
			return;
		char *dir = obs_module_config_path("");
		if (dir)
			os_mkdirs(dir);
		bfree(dir);
		char *path = obs_module_config_path("dock.json");
		if (!path)
			return;
		obs_data_t *d = obs_data_create();
		obs_data_set_bool(d, "tabs_open", tabsToggle->isChecked());
		obs_data_set_int(d, "tab", tabs->currentIndex());
		obs_data_save_json_safe(d, path, "tmp", "bak");
		obs_data_release(d);
		bfree(path);
	}

public:
	/* ---------------------------------------------------------------- test hooks */
#if defined(PD_TEST_HOOKS)
	void snapshot_offscreen(const QString &dir)
	{
		if (isVisible())
			return;
		resize(600, 1100);
		poll();
		const QString path = QDir(dir).filePath(QString("dock-%1.png").arg(++snap_n, 3, 10, QChar('0')));
		grab().save(path);
		blog(LOG_INFO, "[palette-director] dock offscreen snapshot %s (dock hidden, director running)",
		     path.toUtf8().constData());
	}

	void grab_to(const QString &dir, const QString &name)
	{
		if (dir.isEmpty())
			return;
		const QString path = QDir(dir).filePath(name);
		/* grab the scrolled body so the whole dock (top section + open tab) is in the image,
		 * flattened onto the theme's window colour (the body itself is transparent) */
		QPixmap pm(body->size() * body->devicePixelRatioF());
		pm.setDevicePixelRatio(body->devicePixelRatioF());
		pm.fill(palette().color(QPalette::Window));
		body->render(&pm, QPoint(), QRegion(), QWidget::DrawChildren);
		pm.save(path);
		blog(LOG_INFO, "[palette-director] dock test: dock grabbed to %s", path.toUtf8().constData());
	}

	void test_open_tabs(int tab)
	{
		tabsToggle->setChecked(true);
		tabs->setCurrentIndex(tab);
		blog(LOG_INFO, "[palette-director] dock test: tab '%s' shown", tabs->tabText(tab).toUtf8().constData());
	}

	QString tab_name(int i) const { return tabs->tabText(i); }

	/* edit a swatch through the editor's own widgets (select palette + swatch, type hex) */
	void test_edit_swatch(int pal, int sw, const QString &hex)
	{
		if (pal < 0)
			pal = view.st.live_palette; // edit the live palette so the change shows in the preview
		tabsToggle->setChecked(true);
		tabs->setCurrentIndex(1);
		palList->setCurrentRow(pal);
		swList->setCurrentRow(sw);
		swHex->setText(hex);
		blog(LOG_INFO, "[palette-director] dock test: palette %d swatch %d hex typed %s", pal + 1, sw + 1,
		     hex.toUtf8().constData());
		emit swHex->editingFinished();
	}

	void test_coverage(int pct)
	{
		tabsToggle->setChecked(true);
		tabs->setCurrentIndex(2);
		blog(LOG_INFO, "[palette-director] dock test: coverage slider set to %d", pct);
		lkCoverage.s->setValue(pct);
	}

	/* press a Set-tab button (0 New, 1 Duplicate, 2 Rename, 3 Delete, 4 Save, 5 Load file) */
	void test_set_button(int i)
	{
		tabsToggle->setChecked(true);
		tabs->setCurrentIndex(0);
		blog(LOG_INFO, "[palette-director] dock test: Set tab button '%s' clicked",
		     setBtns[i]->text().toUtf8().constData());
		setBtns[i]->click();
	}

	void test_open_filters()
	{
		SrcRef r(selected);
		obs_source_t *parent = r.s ? obs_filter_get_parent(r.s) : nullptr;
		if (!parent)
			return;
		blog(LOG_INFO, "[palette-director] dock test: opening the Filters dialog of '%s'",
		     obs_source_get_name(parent));
		obs_frontend_open_source_filters(parent);
	}

	void test_tap_pad(int i)
	{
		blog(LOG_INFO, "[palette-director] dock test: tap pad %d", i + 1);
		if (i >= 0 && i < PL_MAX_PALETTES && pads[i]->on_tap)
			pads[i]->on_tap(i);
	}

private:
	int snap_n = 0;
#endif

private:
	/* ---------------------------------------------------------------- model + edits */
	bool valid_pal() const { return sel_pal >= 0 && sel_pal < model.n_palettes; }
	bool valid_sw() const { return valid_pal() && sel_sw >= 0 && sel_sw < model.palettes[sel_pal].n_colours; }
	QString pal_name() const { return valid_pal() ? QString::fromUtf8(model.palettes[sel_pal].name) : QString(); }

	void commit(const struct pl_set &ns, int force_live, const QString &why)
	{
		SrcRef r(selected);
		if (!r.s)
			return;
		palette_lock_commit(r.s, &ns, force_live, why.toUtf8().constData());
		model = ns;
		have_model = true;
		fill_editors();
	}

	void after_set_change()
	{
		reload_model(true);
		refresh_set_list();
	}

	void reload_model(bool force)
	{
		SrcRef r(selected);
		if (!r.s)
			return;
		struct pl_set m;
		int edit = 0, live = 0;
		if (!palette_lock_read_set(r.s, &m, &edit, &live))
			return;
		const char *a = palette_lock_audio(r.s);
		const QString audio = QString::fromUtf8(a ? a : "");
		bfree((void *)a);
		if (!force && have_model && memcmp(&m, &model, sizeof(m)) == 0 && audio == audio_setting)
			return;
		model = m;
		have_model = true;
		audio_setting = audio;
		fill_editors();
	}

	void apply_hex(const QString &text)
	{
		if (!valid_sw())
			return;
		static const QRegularExpression re("^#?[0-9A-Fa-f]{6}$");
		QString t = text.trimmed();
		if (!re.match(t).hasMatch()) {
			fill_swatch_fields();
			return;
		}
		if (!t.startsWith('#'))
			t.prepend('#');
		struct pl_set ns = model;
		const QByteArray hb = t.toUpper().toLatin1();
		ns.palettes[sel_pal].colours[sel_sw] = pl_colour_from_hex(hb.constData());
		if (hex_of(ns.palettes[sel_pal].colours[sel_sw]) == hex_of(model.palettes[sel_pal].colours[sel_sw]))
			return;
		commit(ns, -1,
		       QString("palette '%1' colour %2 -> %3").arg(pal_name()).arg(sel_sw + 1).arg(t.toUpper()));
	}

	void pick_colour()
	{
		if (!valid_sw())
			return;
		const QColor c =
			QColorDialog::getColor(qc(model.palettes[sel_pal].colours[sel_sw]), this, T("Dock.SwPick"));
		if (c.isValid())
			apply_hex(c.name());
	}

	void on_palettes_reordered()
	{
		if (filling || palList->count() != model.n_palettes)
			return;
		struct pl_set ns = model;
		int new_live = -1, new_sel = sel_pal;
		bool changed = false;
		for (int i = 0; i < palList->count(); i++) {
			const int old = palList->item(i)->data(Qt::UserRole).toInt();
			if (old < 0 || old >= model.n_palettes)
				return;
			ns.palettes[i] = model.palettes[old];
			changed = changed || old != i;
			if (old == view.st.live_palette)
				new_live = i;
			if (old == sel_pal)
				new_sel = i;
		}
		if (!changed)
			return;
		sel_pal = new_sel;
		/* indices changed: always re-sync the Director's live palette to the same palette */
		commit(ns, new_live,
		       QString("palettes reordered (live stays '%1')")
			       .arg(new_live >= 0 ? QString::fromUtf8(ns.palettes[new_live].name) : QString("-")));
	}

	void on_swatches_reordered()
	{
		if (filling || !valid_pal() || swList->count() != model.palettes[sel_pal].n_colours)
			return;
		struct pl_set ns = model;
		bool changed = false;
		int new_sel = sel_sw;
		for (int i = 0; i < swList->count(); i++) {
			const int old = swList->item(i)->data(Qt::UserRole).toInt();
			ns.palettes[sel_pal].colours[i] = model.palettes[sel_pal].colours[old];
			changed = changed || old != i;
			if (old == sel_sw)
				new_sel = i;
		}
		sel_sw = new_sel;
		if (changed)
			commit(ns, -1, QString("palette '%1' colours reordered").arg(pal_name()));
	}

	/* ---------------------------------------------------------------- editors <- model */
	static void set_combo_data(QComboBox *c, const QVariant &d)
	{
		const int i = c->findData(d);
		if (i >= 0 && c->currentIndex() != i)
			c->setCurrentIndex(i);
	}
	static void set_slider(const ValueSlider &vs, double v)
	{
		if (vs.s->isSliderDown())
			return;
		const int iv = static_cast<int>(std::lround(v * vs.scale));
		if (vs.s->value() != iv)
			vs.s->setValue(iv);
		vs.show_value();
	}
	static void set_spin(QSpinBox *sb, int v)
	{
		if (!sb->hasFocus() && sb->value() != v)
			sb->setValue(v);
	}
	static void set_dspin(QDoubleSpinBox *sb, double v)
	{
		if (!sb->hasFocus() && std::fabs(sb->value() - v) > 1e-4)
			sb->setValue(v);
	}
	static void set_check(QCheckBox *c, bool v)
	{
		if (c->isChecked() != v)
			c->setChecked(v);
	}

	/* hide a form row (label + field) by one of its field widgets */
	static bool layout_has(QLayout *l, QWidget *w)
	{
		for (int k = 0; k < l->count(); k++) {
			QLayoutItem *c = l->itemAt(k);
			if (c->widget() == w || (c->layout() && layout_has(c->layout(), w)))
				return true;
		}
		return false;
	}
	static void set_row_visible(QWidget *field, bool vis)
	{
		auto *form = qobject_cast<QFormLayout *>(field->parentWidget()->layout());
		if (!form)
			return;
		for (int r = 0; r < form->rowCount(); r++) {
			QLayoutItem *it = form->itemAt(r, QFormLayout::FieldRole);
			if (!it)
				continue;
			if (it->widget() == field || (it->layout() && layout_has(it->layout(), field))) {
				if (form->isRowVisible(r) != vis)
					form->setRowVisible(r, vis);
				return;
			}
		}
	}

	void fill_editors()
	{
		filling = true;
		const struct pl_set &s = model;
		/* set tab */
		if (!synPl->hasFocus())
			synPl->setText(QString::fromUtf8(s.synesthesia_playlist));
		update_set_path();
		/* palettes tab */
		if (sel_pal < 0 || sel_pal >= s.n_palettes)
			sel_pal = std::clamp(view.st.live_palette, 0, s.n_palettes - 1);
		const int bd = pl_breakdown_index(&s);
		const int live = view.st.live_palette;
		std::string sig;
		for (int i = 0; i < s.n_palettes; i++) {
			sig += std::string(s.palettes[i].name) + (i == bd ? "*B" : "") + (i == live ? "*L" : "") +
			       (s.palettes[i].mode ? "*D" : "") + ":";
			for (int j = 0; j < s.palettes[i].n_colours; j++)
				sig += hex_of(s.palettes[i].colours[j]).toStdString();
			sig += "|";
		}
		if (sig != pal_list_sig) {
			pal_list_sig = sig;
			palList->clear();
			for (int i = 0; i < s.n_palettes; i++) {
				QString label = QString("%1. %2").arg(i + 1).arg(QString::fromUtf8(s.palettes[i].name));
				label += QString("   %1 %2")
						 .arg(kDot, s.palettes[i].mode == PL_MODE_DUOTONE ? "duo" : "grad");
				if (i == live)
					label += QString("   %1 LIVE").arg(kDot);
				if (i == bd)
					label += QString("   %1 breakdown").arg(kDot);
				auto *it = new QListWidgetItem(label, palList);
				it->setData(Qt::UserRole, i);
				QPixmap pm(48, 14);
				pm.fill(Qt::transparent);
				{
					QPainter p(&pm);
					p.setRenderHint(QPainter::Antialiasing);
					fill_ramp(p, QRectF(0, 0, 48, 14), s.palettes[i], 2.0);
				}
				it->setIcon(QIcon(pm));
				if (i == live) {
					QFont f = it->font();
					f.setBold(true);
					it->setFont(f);
				}
			}
		}
		if (palList->currentRow() != sel_pal)
			palList->setCurrentRow(sel_pal);
		fill_editor_inner();
		/* look */
		set_slider(lkCoverage, s.strength);
		set_slider(lkBlack, s.input_black);
		set_slider(lkWhite, s.input_white);
		set_combo_data(lkBsrc, s.brightness_source);
		set_slider(lkContrast, s.contrast);
		set_slider(lkKeep, s.preserve);
		/* changes: show only the controls that apply to the trigger */
		set_combo_data(chTrigger, s.trigger);
		set_spin(chEvery, s.every_phrases);
		set_row_visible(chEvery, s.trigger == PL_TRIGGER_PHRASES);
		set_slider(chChance, s.change_chance);
		set_row_visible(chChance.s, s.trigger == PL_TRIGGER_RANDOM);
		set_combo_data(chOrder, s.order);
		set_dspin(chXfade, s.crossfade_beats);
		std::string bsig = std::string(s.breakdown) + "#";
		for (int i = 0; i < s.n_palettes; i++)
			bsig += std::string(s.palettes[i].name) + "|";
		if (bsig != bd_sig) {
			bd_sig = bsig;
			chBd->clear();
			chBd->addItem(T("None"), QString());
			for (int i = 0; i < s.n_palettes; i++)
				chBd->addItem(QString::fromUtf8(s.palettes[i].name),
					      QString::fromUtf8(s.palettes[i].name));
		}
		set_combo_data(chBd, QString::fromUtf8(bd >= 0 ? s.breakdown : ""));
		/* director */
		set_check(drEnabled, s.dir_enabled);
		set_spin(drBpm, static_cast<int>(std::lround(s.fallback_bpm)));
		set_combo_data(drPhrase, s.phrase_beats);
		set_dspin(drFade, s.lock_fade_beats);
		set_check(drRandom, s.random_off);
		set_slider(drRandChance, s.random_chance);
		set_spin(drBarsMin, s.bars_min);
		set_spin(drBarsMax, s.bars_max);
		set_spin(drCool, s.cooldown_phrases);
		set_row_visible(drRandChance.s, s.random_off);
		set_row_visible(drBarsMin, s.random_off);
		set_row_visible(drBarsMax, s.random_off);
		set_row_visible(drCool, s.random_off);
		set_check(drOffBd, s.off_on_breakdown);
		set_check(drOffDrop, s.off_on_drop);
		set_spin(drDropBars, s.drop_bars);
		set_row_visible(drDropBars, s.off_on_drop);
		set_slider(drBreath, s.breathing);
		set_slider(drGain, s.input_gain_db);
		set_slider(drSens, s.kick_sensitivity);
		set_check(drRotation, s.rotation);
		set_spin(drSeed, static_cast<int>(s.seed));
		refresh_audio_list();
		filling = false;
	}

	void fill_editor()
	{
		filling = true;
		fill_editor_inner();
		filling = false;
	}

	void fill_editor_inner()
	{
		const bool ok = valid_pal();
		for (QWidget *wdg :
		     {static_cast<QWidget *>(palName), static_cast<QWidget *>(palMode), static_cast<QWidget *>(swList),
		      static_cast<QWidget *>(swHex), static_cast<QWidget *>(bPalLive), static_cast<QWidget *>(bPalDup),
		      static_cast<QWidget *>(bPalDel), static_cast<QWidget *>(bPalBd)})
			wdg->setEnabled(ok);
		bPalNew->setEnabled(model.n_palettes < PL_MAX_PALETTES);
		if (!ok)
			return;
		const struct pl_palette &p = model.palettes[sel_pal];
		if (!palName->hasFocus())
			palName->setText(QString::fromUtf8(p.name));
		set_combo_data(palMode, p.mode);
		ramp->pal = p;
		ramp->update();
		bPalDup->setEnabled(model.n_palettes < PL_MAX_PALETTES);
		bPalDel->setEnabled(model.n_palettes > 1);
		bPalLive->setEnabled(sel_pal != view.st.live_palette);
		bPalBd->setText(pl_breakdown_index(&model) == sel_pal ? T("Dock.PalBreakdownClear")
								      : T("Dock.PalBreakdown"));
		std::string sig = std::to_string(sel_pal) + ":" + std::to_string(p.mode) + ":";
		for (int j = 0; j < p.n_colours; j++)
			sig += hex_of(p.colours[j]).toStdString();
		if (sig != sw_sig) {
			sw_sig = sig;
			swList->clear();
			for (int j = 0; j < p.n_colours; j++) {
				const bool unused = p.mode == PL_MODE_DUOTONE && j > 0 && j < p.n_colours - 1;
				QPixmap pm(40, 40);
				pm.fill(Qt::transparent);
				{
					QPainter pp(&pm);
					pp.setRenderHint(QPainter::Antialiasing);
					QColor c = qc(p.colours[j]);
					if (unused)
						c.setAlpha(70); // duotone uses only the first and last colour
					pp.setBrush(c);
					pp.setPen(QPen(palette().color(QPalette::Mid), 1));
					pp.drawRoundedRect(QRectF(1, 1, 38, 38), 5, 5);
				}
				auto *it = new QListWidgetItem(QIcon(pm), QString(), swList);
				it->setData(Qt::UserRole, j);
				it->setToolTip(hex_of(p.colours[j]) +
					       (unused ? QString(" (unused in duotone)") : QString()));
				it->setSizeHint(QSize(46, 46));
			}
		}
		if (sel_sw >= p.n_colours)
			sel_sw = p.n_colours - 1;
		if (sel_sw < 0)
			sel_sw = 0;
		if (swList->currentRow() != sel_sw)
			swList->setCurrentRow(sel_sw);
		bSwAdd->setEnabled(p.n_colours < PL_MAX_COLOURS);
		bSwRemove->setEnabled(p.n_colours > 2);
		fill_swatch_fields();
	}

	void fill_swatch_fields()
	{
		if (!valid_sw())
			return;
		if (!swHex->hasFocus())
			swHex->setText(hex_of(model.palettes[sel_pal].colours[sel_sw]));
	}

	void update_set_path()
	{
		char path[600];
		if (!pd_sets_path_for(model.name, path, sizeof(path)))
			snprintf(path, sizeof(path), "(no Documents folder)");
		const bool dirty = pd_sets_dirty(&model);
		const QString t = QString("%1%2").arg(QString::fromUtf8(path), dirty ? T("Dock.Unsaved") : QString());
		if (setPath->text() != t)
			setPath->setText(t);
		setPath->setForegroundRole(dirty ? QPalette::Highlight : QPalette::WindowText);
		is_dirty = dirty;
	}

	void refresh_set_list()
	{
		struct pd_set_entry list[PD_SETS_MAX];
		const int n = pd_sets_list(list, PD_SETS_MAX);
		QStringList names;
		bool listed = false;
		for (int i = 0; i < n; i++) {
			names << QString::fromUtf8(list[i].name);
			listed = listed || strcmp(list[i].name, model.name) == 0;
		}
		if (!listed)
			names << QString::fromUtf8(model.name);
		QStringList cur;
		for (int i = 0; i < setSel->count(); i++)
			cur << setSel->itemText(i);
		setSel->blockSignals(true);
		if (cur != names) {
			setSel->clear();
			setSel->addItems(names);
		}
		if (setSel->currentText() != QString::fromUtf8(model.name))
			setSel->setCurrentText(QString::fromUtf8(model.name));
		setSel->blockSignals(false);
		setBtns[3]->setEnabled(names.size() > 1);
	}

	static bool enum_audio(void *param, obs_source_t *src)
	{
		auto *out = static_cast<QStringList *>(param);
		if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0)
			return true;
		const char *n = obs_source_get_name(src);
		if (n && n[0])
			*out << QString::fromUtf8(n);
		return true;
	}

	void refresh_audio_list()
	{
		QStringList names;
		obs_enum_sources(enum_audio, &names);
		if (!audio_setting.isEmpty() && !names.contains(audio_setting))
			names << audio_setting;
		QStringList cur;
		for (int i = 1; i < drAudio->count(); i++)
			cur << drAudio->itemData(i).toString();
		drAudio->blockSignals(true);
		if (cur != names || drAudio->count() == 0) {
			drAudio->clear();
			drAudio->addItem(T("AudioSource.None"), QString());
			for (const QString &n : names)
				drAudio->addItem(n, n);
		}
		const int i = drAudio->findData(audio_setting);
		if (i >= 0 && drAudio->currentIndex() != i)
			drAudio->setCurrentIndex(i);
		drAudio->blockSignals(false);
	}

	/* ---------------------------------------------------------------- live view */
	void send(int cmd, int idx = -1)
	{
		if (selected)
			pdhost::command(selected, cmd, idx);
	}

	void attach(uint64_t id, const QString &label)
	{
		if (id == selected)
			return;
		selected = id;
		log_cursor = 0;
		lines.clear();
		logText->clear();
		pads_sig.clear();
		pal_list_sig.clear();
		sw_sig.clear();
		bd_sig.clear();
		sel_pal = -1;
		sel_sw = 0;
		have_model = false;
		reload_model(true);
		refresh_set_list();
		blog(LOG_INFO, "[palette-director] dock: attached to '%s'", label.toUtf8().constData());
	}

	void refresh_instances(bool force = false)
	{
		const auto list = pdhost::list_instances();
		std::string sig;
		for (const auto &ii : list)
			sig += std::to_string(ii.id) + "|" + ii.parent_name + "|" + ii.filter_name + ";";
		if (sig == inst_sig && !force)
			return;
		const bool changed = sig != inst_sig;
		inst_sig = sig;
		rebuilding = true;
		picker->clear();
		int sel = -1;
		const uint64_t want = want_select ? want_select : selected;
		for (const auto &ii : list) {
			const QString label =
				QString::fromUtf8(ii.parent_name.empty() ? "(no source)" : ii.parent_name.c_str()) +
				" / " + QString::fromUtf8(ii.filter_name.c_str());
			picker->addItem(label, QVariant::fromValue<qulonglong>(ii.id));
			if (ii.id == want)
				sel = picker->count() - 1;
		}
		want_select = 0;
		if (sel < 0 && picker->count() > 0)
			sel = 0;
		if (sel >= 0)
			picker->setCurrentIndex(sel);
		rebuilding = false;
		if (changed) {
			QStringList names;
			for (int i = 0; i < picker->count(); i++)
				names << picker->itemText(i);
			blog(LOG_INFO,
			     "[palette-director] dock: instance list changed, %d Palette Lock filter(s): [%s]",
			     (int)list.size(), names.join("; ").toUtf8().constData());
		}
		if (sel >= 0)
			attach(picker->itemData(sel).toULongLong(), picker->itemText(sel));
		else
			selected = 0;
	}

	QString next_text(const pd::DirectorState &st, const struct pl_set &set) const
	{
		if (st.panic)
			return T("Dock.NowPanic");
		if (!st.enabled)
			return T("Dock.NowDirectorOff");
		if (st.paused)
			return T("Dock.NowPaused");
		if (st.section == pd::Section::Breakdown && pl_breakdown_index(&set) >= 0)
			return T("Dock.NowBreakdown");
		/* before the first shuffle pass is drawn the next pick is not known yet */
		const QString nn =
			(st.next_palette >= 0 && st.next_palette < set.n_palettes)
				? QString::fromUtf8(set.palettes[st.next_palette].name)
				: (set.order == PL_ORDER_SHUFFLE ? QString("a shuffled palette") : QString("?"));
		switch (set.trigger) {
		case PL_TRIGGER_PHRASES: {
			const int every = std::max(1, set.every_phrases);
			const int left = every - static_cast<int>(st.phrase % static_cast<uint64_t>(every));
			return QString("next: %1 in %2 phrase%3").arg(nn).arg(left).arg(left == 1 ? "" : "s");
		}
		case PL_TRIGGER_RANDOM:
			return QString("next: %1 maybe at the next phrase (%2%)")
				.arg(nn)
				.arg(std::lround(set.change_chance * 100.0f));
		case PL_TRIGGER_MANUAL:
			return QString("next: manual only (tap a pad)");
		default:
			return QString("next: %1 on the next drop").arg(nn);
		}
	}

	void poll()
	{
		refresh_instances();
		const bool have = selected && pdhost::read_view(selected, view, log_cursor);
		empty->setVisible(!have);
		live_area->setVisible(have);
		tabsToggle->setEnabled(have);
		if (!have)
			return;
		if (++slow_tick % 8 == 0)
			reload_model(false); // ~4 Hz: pick up edits made in the properties window / websocket
		if (slow_tick % 90 == 0)
			refresh_set_list();
		const pd::DirectorState &st = view.st;
		const struct pl_set &set = view.set;

		static const char *secName[3] = {"GROOVE", "BREAKDOWN", "DROP"};
		static const QColor secCol[3] = {QColor(80, 140, 255), QColor(255, 170, 40), QColor(255, 60, 100)};
		const int si = std::clamp(static_cast<int>(st.section), 0, 2);
		set_chip(secChip, secName[si], secCol[si]);
		const float mix = view.mix;
		const QString lockTxt = mix > 0.99f ? "LOCK ON" : (mix < 0.01f ? "LOCK OFF" : "FADING");
		set_chip(lockChip, lockTxt, mix > 0.5f ? QColor(60, 200, 110) : QColor(140, 140, 150));
		QString state = "DIRECTOR RUNNING";
		QColor stc(60, 200, 110);
		if (!st.enabled) {
			state = "DIRECTOR OFF";
			stc = QColor(140, 140, 150);
		} else if (st.paused) {
			state = "DIRECTOR PAUSED";
			stc = QColor(255, 170, 40);
		}
		if (st.panic) {
			state = "PANIC";
			stc = QColor(255, 60, 100);
		} else if (st.force_off) {
			state = "FORCED OFF";
			stc = QColor(255, 60, 100);
		} else if (st.forced_on) {
			state = "FORCED ON";
			stc = QColor(80, 140, 255);
		}
		set_chip(stateChip, state, stc);
		beats->on = st.beat_frac < 0.35f ? static_cast<int>(st.beat % 4) : -1;
		beats->update();

		const int pb = set.phrase_beats == 16 ? 16 : 32;
		const double b = view.beats;
		const QString bpm = st.silent ? QString("%1 BPM (fixed)").arg(st.bpm, 0, 'f', 0)
					      : QString("%1 BPM (%2%)")
							.arg(st.bpm, 0, 'f', 1)
							.arg(std::lround(st.confidence * 100.0f));
		statusText->setText(QString("%1  %2  bar %3/%4 %5 phrase %6  %7  coverage %8%")
					    .arg(bpm, kDot)
					    .arg(static_cast<int>(std::fmod(b, pb) / 4.0) + 1)
					    .arg(pb / 4)
					    .arg(kDot)
					    .arg(static_cast<long long>(b / pb) + 1)
					    .arg(kDot)
					    .arg(std::lround(set.strength * 100.0f)));
		sAudio->setText(view.audio_name.empty()
					? T("Dock.NoAudio")
					: QString("%1: %2%3   %4 cooldown %5")
						  .arg(T("AudioSource"), QString::fromUtf8(view.audio_name.c_str()),
						       view.audio_attached ? QString() : QString(" (not found)"), kDot)
						  .arg(st.cooldown_phrases_left));
		/* v1.0.1 audio meter */
		meter->db = st.input_db;
		meter->clip = st.clip;
		meter->bass = st.bass_level;
		meter->thr = st.kick_threshold;
		meter->silent = st.silent;
		if (st.kicks != kicks_seen) {
			kicks_seen = st.kicks;
			meter->kick_flash = 4; // ~130 ms at 30 Hz
		} else if (meter->kick_flash > 0) {
			meter->kick_flash--;
		}
		meter->update();
		bForceOn->setChecked(st.forced_on);
		bForceOn->setText(st.forced_on ? T("Dock.ForceOnActive") : T("Dock.ForceOn"));
		bPause->setChecked(st.paused);
		bPause->setText(st.paused ? T("Dock.Resume") : T("Dock.Pause"));
		bPanic->setChecked(st.panic);
		bPanic->setText(st.panic ? T("Dock.PanicActive") : T("Dock.Panic"));

		/* pads: flow grid of up to 16, 2..4 columns by width */
		const int cols = std::clamp(live_area->width() / 150, 2, 4);
		const int bd = pl_breakdown_index(&set);
		std::string psig = std::to_string(set.n_palettes) + "/" + std::to_string(cols);
		for (int i = 0; i < set.n_palettes; i++)
			psig += std::string("|") + set.palettes[i].name;
		if (psig != pads_sig) {
			pads_sig = psig;
			for (int i = 0; i < PL_MAX_PALETTES; i++) {
				padGrid->removeWidget(pads[i]);
				pads[i]->hide();
			}
			for (int i = 0; i < set.n_palettes; i++) {
				padGrid->addWidget(pads[i], i / cols, i % cols);
				pads[i]->show();
			}
		}
		for (int i = 0; i < set.n_palettes; i++) {
			PadWidget *pw = pads[i];
			pw->pal = set.palettes[i];
			pw->live = i == st.live_palette;
			pw->next = i == st.next_palette && i != st.live_palette;
			pw->breakdown = i == bd;
			pw->pfade = st.prev_palette >= 0 ? st.pfade : 1.0f;
			pw->update();
		}

		/* timeline */
		timeline->tl = view.timeline;
		timeline->now = view.beats;
		timeline->phrase = pb;
		timeline->update();

		/* "now" sentence */
		const QString liveName = (st.live_palette >= 0 && st.live_palette < set.n_palettes)
						 ? QString::fromUtf8(set.palettes[st.live_palette].name)
						 : QString("?");
		QString lockWord = mix > 0.99f ? "lock ON" : (mix < 0.01f ? "lock OFF (raw source)" : "lock fading");
		if (st.panic)
			lockWord = "lock held ON by PANIC";
		else if (st.force_off)
			lockWord = "lock forced OFF";
		else if (st.forced_on)
			lockWord = "lock held ON by Force on (press Force on to release)";
		nowLbl->setText(QString("Live: %1 %2 %3 %2 %4%5")
					.arg(liveName, kDot, next_text(st, set), lockWord,
					     is_dirty ? QString(" %1 set has unsaved changes").arg(kDot) : QString()));

		/* palette list markers follow the live palette */
		if (have_model && live_seen != st.live_palette) {
			live_seen = st.live_palette;
			fill_editors();
		}

		/* log: newest first (mock logEv); the Log tab keeps the full history (200 lines) */
		if (!view.log.empty()) {
			for (const auto &l : view.log) {
				log_cursor = std::max(log_cursor, l.seq);
				lines.prepend(QString("phrase %1 %2 %3")
						      .arg(static_cast<qulonglong>(l.phrase + 1), 3)
						      .arg(kDot)
						      .arg(QString::fromUtf8(l.text.c_str())));
			}
			while (lines.size() > 200)
				lines.removeLast();
			logText->setPlainText(lines.join("\n"));
		}
	}

	/* widgets */
	QWidget *body = nullptr, *live_area = nullptr;
	QComboBox *picker = nullptr;
	QLabel *secChip = nullptr, *lockChip = nullptr, *stateChip = nullptr;
	BeatLights *beats = nullptr;
	QLabel *statusText = nullptr, *sAudio = nullptr, *empty = nullptr, *nowLbl = nullptr;
	QGridLayout *padGrid = nullptr;
	PadWidget *pads[PL_MAX_PALETTES] = {};
	QPushButton *bForceOff = nullptr, *bForceOn = nullptr, *bPause = nullptr, *bPrev = nullptr, *bNext = nullptr,
		    *bRotate = nullptr, *bPanic = nullptr;
	TimelineWidget *timeline = nullptr;
	QToolButton *tabsToggle = nullptr;
	QTabWidget *tabs = nullptr;
	/* set tab */
	QComboBox *setSel = nullptr;
	QPushButton *setBtns[6] = {};
	QLineEdit *synPl = nullptr;
	QLabel *setPath = nullptr;
	/* palettes tab */
	QListWidget *palList = nullptr, *swList = nullptr;
	QPushButton *bPalNew = nullptr, *bPalLive = nullptr, *bPalDup = nullptr, *bPalDel = nullptr, *bPalBd = nullptr;
	QLineEdit *palName = nullptr, *swHex = nullptr;
	QComboBox *palMode = nullptr;
	RampWidget *ramp = nullptr;
	QPushButton *bSwPick = nullptr, *bSwRemove = nullptr, *bSwAdd = nullptr, *bSwSort = nullptr, *bSwRev = nullptr;
	/* look tab */
	ValueSlider lkCoverage, lkBlack, lkWhite, lkContrast, lkKeep;
	QComboBox *lkBsrc = nullptr;
	/* changes tab */
	QComboBox *chTrigger = nullptr, *chOrder = nullptr, *chBd = nullptr;
	QSpinBox *chEvery = nullptr;
	ValueSlider chChance;
	QDoubleSpinBox *chXfade = nullptr;
	/* director tab */
	QCheckBox *drEnabled = nullptr, *drRandom = nullptr, *drOffBd = nullptr, *drOffDrop = nullptr,
		  *drRotation = nullptr;
	QComboBox *drAudio = nullptr, *drPhrase = nullptr;
	QSpinBox *drBpm = nullptr, *drBarsMin = nullptr, *drBarsMax = nullptr, *drCool = nullptr, *drDropBars = nullptr,
		 *drSeed = nullptr;
	QDoubleSpinBox *drFade = nullptr;
	ValueSlider drRandChance, drBreath, drGain, drSens;
	AudioMeter *meter = nullptr;
	uint32_t kicks_seen = 0;
	/* log tab */
	QPlainTextEdit *logText = nullptr;
	QStringList lines;
	QTimer *timer = nullptr;

	/* state */
	pdhost::DockView view;
	struct pl_set model;
	bool have_model = false;
	QString audio_setting;
	uint64_t selected = 0, want_select = 0;
	uint64_t log_cursor = 0;
	std::string inst_sig, pads_sig, pal_list_sig, sw_sig, bd_sig;
	int sel_pal = -1, sel_sw = 0, live_seen = -2;
	unsigned slow_tick = 0;
	bool rebuilding = false, filling = false, loading_cfg = false, is_dirty = false;
};

PaletteDirectorDock *g_dock = nullptr;

} // namespace

#endif

#include "palette-director-dock.h"

bool palette_director_dock_available(void)
{
#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)
	return g_dock != nullptr;
#else
	return false;
#endif
}

void palette_director_dock_show(obs_source_t *filter)
{
#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)
	if (!g_dock)
		return;
	auto *dw = qobject_cast<QDockWidget *>(g_dock->parentWidget());
	if (dw) {
		dw->show();
		dw->raise();
		dw->activateWindow();
	}
	g_dock->select_filter(filter);
	blog(LOG_INFO, "[palette-director] dock: opened from the filter properties ('%s')",
	     filter ? obs_source_get_name(filter) : "?");
#else
	(void)filter;
#endif
}

void palette_director_dock_load(void)
{
#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)
	auto *dock = new PaletteDirectorDock();
	if (!obs_frontend_add_dock_by_id("palette-director-dock", obs_module_text("PaletteDirector"), dock)) {
		blog(LOG_ERROR, "[palette-director] dock id already in use; dock not added");
		delete dock;
		return;
	}
	g_dock = dock;
	blog(LOG_INFO, "[palette-director] dock registered (obs_frontend_add_dock_by_id)");

#if defined(PD_TEST_HOOKS)
	/* Sandbox test hooks, compiled only with -DPD_TEST_HOOKS=ON (never in release builds).
	 * PD_DOCK_SNAPSHOT_DIR: where grabs go (and, without the cycle, offscreen PNGs every 2 s).
	 * PD_DOCK_TEST_CYCLE=1: open the dock docked right, grab the live section, show and grab
	 * every tab, edit a swatch through the palette editor, move the Coverage slider and tap
	 * pad 9. Everything runs on the UI thread via QTimer. */
	blog(LOG_INFO, "[palette-director] test hooks compiled in (PD_TEST_HOOKS)");
	const QString snapDir = qEnvironmentVariable("PD_DOCK_SNAPSHOT_DIR");
	if (!snapDir.isEmpty() && qEnvironmentVariableIntValue("PD_DOCK_TEST_CYCLE") != 1) {
		auto *t = new QTimer(dock);
		t->setInterval(2000);
		QObject::connect(t, &QTimer::timeout, dock, [dock, snapDir] { dock->snapshot_offscreen(snapDir); });
		t->start();
	}
	if (qEnvironmentVariableIntValue("PD_DOCK_TEST_CYCLE") == 1) {
		auto grab_main = [snapDir](const QString &name) {
			auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (!mw || snapDir.isEmpty())
				return;
			const QString path = QDir(snapDir).filePath(name);
			mw->grab().save(path);
			blog(LOG_INFO, "[palette-director] dock test: OBS main window grabbed to %s",
			     path.toUtf8().constData());
		};
		auto at = [dock](int ms, std::function<void()> fn) {
			QTimer::singleShot(ms, dock, std::move(fn));
		};
		at(15000, [dock] {
			auto *dw = qobject_cast<QDockWidget *>(dock->parentWidget());
			auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
			if (!dw)
				return;
			dw->setFloating(false);
			if (mw && mw->dockWidgetArea(dw) == Qt::NoDockWidgetArea)
				mw->addDockWidget(Qt::RightDockWidgetArea, dw);
			dw->show();
			dw->raise();
			if (mw)
				mw->resizeDocks({dw}, {660}, Qt::Horizontal);
			blog(LOG_INFO,
			     "[palette-director] dock test: dock opened (docked, right area), body min width %d",
			     dock->minimumSizeHint().width());
		});
		at(40000, [dock, snapDir, grab_main] {
			dock->grab_to(snapDir, "v1a-dock-live.png");
			grab_main("v1a-obs-live.png");
		});
		/* palette editor: change the live palette's 3rd colour to green, visible in the preview
		 * (done before the synthetic track's first breakdown so the live palette is stable) */
		at(43000, [dock] { dock->test_edit_swatch(-1, 2, "#00FF66"); });
		at(44500, [dock, snapDir, grab_main] {
			dock->grab_to(snapDir, "v1a-palette-edit.png");
			grab_main("v1a-obs-palette-edit.png");
		});
		at(47000, [dock] { dock->test_edit_swatch(-1, 2, "#FF00A0"); }); // back to the bundled colour
		int t = 50000;
		for (int i = 0; i < 6; i++) {
			at(t, [dock, i] { dock->test_open_tabs(i); });
			at(t + 2500, [dock, snapDir, i] {
				dock->grab_to(snapDir,
					      QString("v1a-tab-%1-%2.png").arg(i).arg(dock->tab_name(i).toLower()));
			});
			t += 5000;
		}
		at(t, [dock] { dock->test_coverage(80); });
		at(t + 2500, [dock, snapDir, grab_main] {
			dock->grab_to(snapDir, "v1a-look-coverage80.png");
			grab_main("v1a-obs-coverage80.png");
		});
		t += 10000;
		at(t, [dock] { dock->test_tap_pad(8); });
		at(t + 3000, [dock, snapDir] { dock->grab_to(snapDir, "v1a-pad9-live.png"); });
		t += 8000;
		at(t, [dock] { dock->test_coverage(100); });
		/* set lifecycle through the Set tab's real buttons; with PD_UI_AUTO=1 the prompt /
		 * confirm dialogs take their answers from PD_UI_AUTO_ANSWERS (logged) */
		at(t + 1500, [dock] { dock->test_set_button(1); }); // Duplicate
		at(t + 3000, [dock] { dock->test_set_button(4); }); // Save
		at(t + 4000, [dock, snapDir] { dock->grab_to(snapDir, "v1a-set-duplicated.png"); });
		at(t + 5000, [dock] { dock->test_set_button(3); }); // Delete (confirm) -> previous set
		at(t + 6500, [dock, snapDir] { dock->grab_to(snapDir, "v1a-set-after-delete.png"); });
		/* unsaved-changes confirm: make the set dirty, press New, answer "no" -> nothing changes */
		at(t + 7000, [dock] { dock->test_coverage(90); });
		at(t + 8000, [dock] { dock->test_set_button(0); }); // New -> "Discard unsaved changes?" -> no
		at(t + 9000, [dock] { dock->test_coverage(100); });
		t += 9500;
		at(t + 1000, [dock] { dock->test_open_tabs(2); });
		/* the filter's slim properties window (Filters dialog of the filter's parent source) */
		at(t + 4000, [dock] { dock->test_open_filters(); });
	}
#endif
#endif
}
