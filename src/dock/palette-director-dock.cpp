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
Palette Director dock (spec 6, mock v2 dock panel).

View + command surface only (design D10): it polls the selected filter host on a ~30 Hz
QTimer (pdhost::read_view copies the snapshot + log/timeline rings under the host's light
view mutex) and sends commands with pdhost::command into the host's command queue, which
the Director's worker thread drains. Every Qt call happens on the UI thread. Closing the
dock only stops the poll timer; the Director keeps running.
*/

#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QComboBox>
#include <QDockWidget>
#include <QMainWindow>
#include <QDir>
#include <QPixmap>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include "palette-lock-host.h"
#include "palette-lock-host.hpp"

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

void fill_ramp(QPainter &p, const QRectF &r, const struct pl_palette &pal)
{
	QLinearGradient g(r.topLeft(), r.topRight());
	if (pal.mode == PL_MODE_DUOTONE || pal.n_colours < 2) {
		g.setColorAt(0.0, qc(pal.colours[0]));
		g.setColorAt(1.0, qc(pal.colours[std::max(0, pal.n_colours - 1)]));
	} else {
		for (int i = 0; i < pal.n_colours; i++)
			g.setColorAt(static_cast<double>(i) / (pal.n_colours - 1), qc(pal.colours[i]));
	}
	QPainterPath path;
	path.addRoundedRect(r, 3, 3);
	p.fillPath(path, g);
}

/* ---- palette pad (mock .pad) ---- */
class PadWidget : public QWidget {
public:
	explicit PadWidget(QWidget *parent) : QWidget(parent)
	{
		setMinimumSize(118, 62);
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
		const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
		QPainterPath bg;
		bg.addRoundedRect(r, 6, 6);
		p.fillPath(bg, QColor(0x1f, 0x21, 0x28));
		QPen pen(live ? QColor(0x4c, 0xd0, 0x7d)
			      : (next ? QColor(0x7a, 0x81, 0x99) : QColor(0x3a, 0x3e, 0x4a)));
		pen.setWidthF(live ? 2.0 : 1.0);
		if (next && !live)
			pen.setStyle(Qt::DashLine);
		p.setPen(pen);
		p.drawPath(bg);

		QFont f = font();
		f.setBold(true);
		p.setFont(f);
		p.setPen(QColor(0xe6, 0xe6, 0xea));
		const QRectF nameR(r.left() + 6, r.top() + 4, r.width() - 46, 16);
		p.drawText(nameR, Qt::AlignLeft | Qt::AlignVCenter,
			   p.fontMetrics().elidedText(QString::fromUtf8(pal.name), Qt::ElideRight, (int)nameR.width()));

		QFont bf = font();
		bf.setBold(true);
		bf.setPointSizeF(std::max(6.0, bf.pointSizeF() * 0.75));
		p.setFont(bf);
		if (live || next) {
			p.setPen(live ? QColor(0x4c, 0xd0, 0x7d) : QColor(0x9a, 0x9c, 0xab));
			p.drawText(QRectF(r.right() - 40, r.top() + 4, 36, 14), Qt::AlignRight | Qt::AlignVCenter,
				   live ? "LIVE" : "NEXT");
		}

		fill_ramp(p, QRectF(r.left() + 6, r.top() + 23, r.width() - 12, 15), pal);

		QFont sf = font();
		sf.setPointSizeF(std::max(6.0, sf.pointSizeF() * 0.78));
		p.setFont(sf);
		p.setPen(QColor(0x9a, 0x9c, 0xab));
		const QString sub = QString("%1%2 %3 Ctrl+%4")
					    .arg(breakdown ? QString("breakdown %1 ").arg(kDot) : QString())
					    .arg(pal.mode == PL_MODE_DUOTONE ? "duotone" : "gradient")
					    .arg(kDot)
					    .arg(index + 1);
		p.drawText(QRectF(r.left() + 6, r.top() + 40, r.width() - 12, 14), Qt::AlignLeft | Qt::AlignVCenter,
			   sub);

		if (live) {
			const double w = r.width() * std::clamp(pfade, 0.0f, 1.0f);
			p.fillRect(QRectF(r.left(), r.bottom() - 3, w, 3), QColor(0x4c, 0xd0, 0x7d));
		}
	}
};

/* ---- 64-beat timeline (mock drawTimeline) ---- */
class TimelineWidget : public QWidget {
public:
	explicit TimelineWidget(QWidget *parent) : QWidget(parent) { setMinimumHeight(110); }

	std::vector<pdhost::TimelineSample> tl;
	double now = 0.0;
	int phrase = 32;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const double W = width(), H = height();
		p.fillRect(rect(), QColor(0x14, 0x15, 0x1a));
		auto X = [&](double b) {
			return W - (now - b) / 64.0 * W;
		};
		const QColor sec[3] = {QColor(80, 140, 255, 36), QColor(255, 182, 72, 56), QColor(255, 92, 122, 64)};
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
		p.setPen(QPen(QColor(255, 255, 255, 72), 1.0));
		p.drawPath(bass);
		const int ph = std::max(1, phrase);
		for (double b = std::ceil((now - 64.0) / ph) * ph; b <= now; b += ph)
			p.fillRect(QRectF(X(b), 10, 1, H), QColor(255, 255, 255, 128));
		p.setPen(QPen(QColor(0x4c, 0xd0, 0x7d), 2.0));
		p.drawPath(mix);
	}
};

const char *kChipStyle = "QLabel{padding:2px 8px;border-radius:9px;background:#14151a;"
			 "border:1px solid rgba(255,255,255,40);font-weight:600;color:%1;}";

void set_chip(QLabel *l, const QString &text, const char *colour)
{
	if (l->text() != text)
		l->setText(text);
	const QString ss = QString(kChipStyle).arg(colour);
	if (l->styleSheet() != ss)
		l->setStyleSheet(ss);
}

QWidget *stat(QWidget *parent, const QString &key, QLabel **val)
{
	auto *w = new QWidget(parent);
	auto *v = new QVBoxLayout(w);
	v->setContentsMargins(0, 0, 0, 0);
	v->setSpacing(0);
	auto *k = new QLabel(key, w);
	k->setStyleSheet("color:#9a9cab;font-size:10px;");
	*val = new QLabel("-", w);
	(*val)->setStyleSheet("font-size:15px;font-weight:600;");
	v->addWidget(k);
	v->addWidget(*val);
	return w;
}

/* ---- the dock ---- */
class PaletteDirectorDock : public QWidget {
public:
	explicit PaletteDirectorDock(QWidget *parent = nullptr) : QWidget(parent)
	{
		setObjectName("PaletteDirectorDock");
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		outer->addWidget(scroll);
		auto *body = new QWidget(scroll);
		scroll->setWidget(body);
		auto *v = new QVBoxLayout(body);
		v->setContentsMargins(8, 8, 8, 8);
		v->setSpacing(8);

		/* instance picker */
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
		empty->setStyleSheet("color:#9a9cab;");
		v->addWidget(empty);

		/* status */
		auto *statusRow = new QHBoxLayout();
		secChip = new QLabel(body);
		lockChip = new QLabel(body);
		stateChip = new QLabel(body);
		statusRow->addWidget(secChip);
		statusRow->addWidget(lockChip);
		statusRow->addWidget(stateChip);
		statusRow->addStretch(1);
		for (int i = 0; i < 4; i++) {
			beatsL[i] = new QLabel(body);
			beatsL[i]->setFixedSize(12, 12);
			statusRow->addWidget(beatsL[i]);
		}
		v->addLayout(statusRow);

		auto *stats = new QHBoxLayout();
		stats->addWidget(stat(body, "BPM", &sBpm));
		stats->addWidget(stat(body, T("Dock.Confidence"), &sConf));
		stats->addWidget(stat(body, T("Dock.BarPhrase"), &sBar));
		stats->addWidget(stat(body, T("Dock.Lock"), &sMix));
		stats->addWidget(stat(body, T("Dock.Cooldown"), &sCool));
		stats->addStretch(1);
		v->addLayout(stats);
		sAudio = new QLabel(body);
		sAudio->setStyleSheet("color:#9a9cab;font-size:10px;");
		v->addWidget(sAudio);

		/* pads */
		auto *padHead = new QHBoxLayout();
		auto *ph = new QLabel(T("Dock.PalettesTap"), body);
		ph->setStyleSheet("color:#9a9cab;font-size:11px;");
		nextLbl = new QLabel(body);
		nextLbl->setStyleSheet("color:#9a9cab;font-size:11px;");
		padHead->addWidget(ph);
		padHead->addStretch(1);
		padHead->addWidget(nextLbl);
		v->addLayout(padHead);
		padGrid = new QGridLayout();
		padGrid->setSpacing(6);
		v->addLayout(padGrid);
		for (int i = 0; i < PL_MAX_PALETTES; i++) {
			pads[i] = new PadWidget(body);
			pads[i]->index = i;
			pads[i]->on_tap = [this](int idx) {
				send(PD_CMD_GO_LIVE, idx);
			};
			pads[i]->hide();
		}

		/* controls */
		auto *ctrls = new QGridLayout();
		ctrls->setSpacing(6);
		bForceOff = new QPushButton(T("Dock.ForceOff"), body);
		bForceOn = new QPushButton(T("Dock.ForceOn"), body);
		bPause = new QPushButton(T("Dock.Pause"), body);
		bPrev = new QPushButton(T("Dock.Prev"), body);
		bNext = new QPushButton(T("Dock.Next"), body);
		bRotate = new QPushButton(T("Dock.Rotate"), body);
		bForceOn->setCheckable(true);
		bPause->setCheckable(true);
		ctrls->addWidget(bForceOff, 0, 0);
		ctrls->addWidget(bForceOn, 0, 1);
		ctrls->addWidget(bPause, 0, 2);
		ctrls->addWidget(bPrev, 1, 0);
		ctrls->addWidget(bNext, 1, 1);
		ctrls->addWidget(bRotate, 1, 2);
		v->addLayout(ctrls);
		bPanic = new QPushButton(T("Dock.Panic"), body);
		bPanic->setStyleSheet(
			"QPushButton{border:1px solid #7a3344;color:#ffb3c1;padding:8px;font-weight:600;}");
		v->addWidget(bPanic);

		QObject::connect(bForceOff, &QPushButton::pressed, this, [this] { send(PD_CMD_FORCE_OFF); });
		/* the host logs every command with its origin ("dock: ...") */
		QObject::connect(bForceOff, &QPushButton::released, this, [this] { send(PD_CMD_RELEASE_FORCE); });
		QObject::connect(bForceOn, &QPushButton::clicked, this, [this] { send(PD_CMD_FORCE_ON); });
		QObject::connect(bPause, &QPushButton::clicked, this, [this] { send(PD_CMD_TOGGLE_PAUSE); });
		QObject::connect(bPrev, &QPushButton::clicked, this, [this] { send(PD_CMD_PREV); });
		QObject::connect(bNext, &QPushButton::clicked, this, [this] { send(PD_CMD_NEXT); });
		QObject::connect(bRotate, &QPushButton::clicked, this, [this] { send(PD_CMD_ROTATE_NOW); });
		QObject::connect(bPanic, &QPushButton::clicked, this, [this] { send(PD_CMD_PANIC); });

		/* timeline + log */
		timeline = new TimelineWidget(body);
		v->addWidget(timeline);
		auto *legend = new QLabel(T("Dock.TimelineLegend"), body);
		legend->setStyleSheet("color:#9a9cab;font-size:10px;");
		legend->setWordWrap(true);
		v->addWidget(legend);
		logText = new QPlainTextEdit(body);
		logText->setReadOnly(true);
		logText->setMinimumHeight(118);
		logText->setStyleSheet(
			"QPlainTextEdit{font-family:Consolas,monospace;font-size:11px;background:#14151a;}");
		v->addWidget(logText, 1);

		timer = new QTimer(this);
		timer->setInterval(33); // ~30 Hz
		QObject::connect(timer, &QTimer::timeout, this, [this] { poll(); });
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

public:
	/* Test hook (env PD_DOCK_SNAPSHOT_DIR): render the dock offscreen to PNG without showing
	 * it. Used for sandbox evidence when the OBS window can't be driven interactively. */
	void snapshot_offscreen(const QString &dir)
	{
		if (isVisible())
			return;
		resize(560, 900);
		poll();
		const QString path = QDir(dir).filePath(QString("dock-%1.png").arg(++snap_n, 3, 10, QChar('0')));
		grab().save(path);
		blog(LOG_INFO, "[palette-director] dock offscreen snapshot %s (dock hidden, director running)",
		     path.toUtf8().constData());
	}

	/* Test hook: press the dock's own controls (same code path as a mouse click). */
	void test_press(int what)
	{
		switch (what) {
		case 0:
			blog(LOG_INFO, "[palette-director] dock test: tap pad 5");
			pads[4]->on_tap(4);
			break;
		case 1:
			bNext->click();
			break;
		case 2:
			bPrev->click();
			break;
		case 3:
			bForceOn->click();
			break;
		case 4:
			bForceOn->click();
			break;
		case 5:
			emit bForceOff->pressed();
			break;
		case 6:
			emit bForceOff->released();
			break;
		case 7:
			bPause->click();
			break;
		case 8:
			bPause->click();
			break;
		case 9:
			bRotate->click();
			break;
		case 10:
			bPanic->click();
			break;
		case 11:
			bPause->click();
			break;
		}
	}

	void test_press_chain(int i)
	{
		if (i > 11)
			return;
		test_press(i);
		QTimer::singleShot(700, this, [this, i] { test_press_chain(i + 1); });
	}

private:
	int snap_n = 0;

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
		blog(LOG_INFO, "[palette-director] dock: attached to '%s'", label.toUtf8().constData());
	}

	void refresh_instances()
	{
		const auto list = pdhost::list_instances();
		std::string sig;
		for (const auto &ii : list)
			sig += std::to_string(ii.id) + "|" + ii.parent_name + "|" + ii.filter_name + ";";
		if (sig == inst_sig)
			return;
		inst_sig = sig;
		rebuilding = true;
		picker->clear();
		int sel = -1;
		for (const auto &ii : list) {
			const QString label =
				QString::fromUtf8(ii.parent_name.empty() ? "(no source)" : ii.parent_name.c_str()) +
				" / " + QString::fromUtf8(ii.filter_name.c_str());
			picker->addItem(label, QVariant::fromValue<qulonglong>(ii.id));
			if (ii.id == selected)
				sel = picker->count() - 1;
		}
		if (sel < 0 && picker->count() > 0)
			sel = 0;
		if (sel >= 0)
			picker->setCurrentIndex(sel);
		rebuilding = false;
		QStringList names;
		for (int i = 0; i < picker->count(); i++)
			names << picker->itemText(i);
		blog(LOG_INFO, "[palette-director] dock: instance list changed, %d Palette Lock filter(s): [%s]",
		     (int)list.size(), names.join("; ").toUtf8().constData());
		if (sel >= 0)
			attach(picker->itemData(sel).toULongLong(), picker->itemText(sel));
		else
			selected = 0;
	}

	void poll()
	{
		refresh_instances();
		const bool have = selected && pdhost::read_view(selected, view, log_cursor);
		empty->setVisible(!have);
		if (!have)
			return;
		const pd::DirectorState &st = view.st;
		const struct pl_set &set = view.set;

		static const char *secName[3] = {"GROOVE", "BREAKDOWN", "DROP"};
		static const char *secCol[3] = {"#9fd3ff", "#ffb648", "#ff5c7a"};
		const int si = std::clamp(static_cast<int>(st.section), 0, 2);
		set_chip(secChip, secName[si], secCol[si]);
		const float mix = view.mix;
		set_chip(lockChip, mix > 0.99f ? "LOCK ON" : (mix < 0.01f ? "LOCK OFF" : "FADING"),
			 mix > 0.5f ? "#4cd07d" : "#9a9cab");
		const char *state = "DIRECTOR RUNNING";
		if (!st.enabled)
			state = "DIRECTOR OFF";
		else if (st.paused)
			state = "DIRECTOR PAUSED";
		if (st.force_off)
			state = "FORCED OFF";
		else if (st.forced_on)
			state = "FORCED ON";
		set_chip(stateChip, state, "#e6e6ea");
		const int bt = static_cast<int>(st.beat % 4);
		for (int i = 0; i < 4; i++) {
			const bool on = i == bt && st.beat_frac < 0.35f;
			const char *ss = on ? "background:#4f7cff;border-radius:6px;"
					    : "background:#333744;border-radius:6px;";
			if (beatsL[i]->styleSheet() != ss)
				beatsL[i]->setStyleSheet(ss);
		}
		sBpm->setText(st.silent ? QString("%1 (fixed)").arg(st.bpm, 0, 'f', 0)
					: QString::number(st.bpm, 'f', 1));
		sConf->setText(st.silent ? QString("-") : QString("%1%").arg(std::lround(st.confidence * 100.0f)));
		const int pb = set.phrase_beats == 16 ? 16 : 32;
		const double b = view.beats;
		sBar->setText(QString("%1/%2 %3 %4")
				      .arg(static_cast<int>(std::fmod(b, pb) / 4.0) + 1)
				      .arg(pb / 4)
				      .arg(kDot)
				      .arg(static_cast<long long>(b / pb) + 1));
		sMix->setText(QString::number(mix, 'f', 2));
		sCool->setText(QString::number(st.cooldown_phrases_left));
		sAudio->setText(view.audio_name.empty()
					? T("Dock.NoAudio")
					: QString("%1: %2%3")
						  .arg(T("AudioSource"), QString::fromUtf8(view.audio_name.c_str()),
						       view.audio_attached ? "" : " (not found)"));
		bForceOn->setChecked(st.forced_on);
		bPause->setChecked(st.paused);
		bPause->setText(st.paused ? T("Dock.Resume") : T("Dock.Pause"));

		/* pads */
		const int bd = pl_breakdown_index(&set);
		std::string psig = std::to_string(set.n_palettes);
		for (int i = 0; i < set.n_palettes; i++)
			psig += std::string("|") + set.palettes[i].name;
		if (psig != pads_sig) {
			pads_sig = psig;
			for (int i = 0; i < PL_MAX_PALETTES; i++) {
				padGrid->removeWidget(pads[i]);
				pads[i]->hide();
			}
			for (int i = 0; i < set.n_palettes; i++) {
				padGrid->addWidget(pads[i], i / 3, i % 3);
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
		QString trig;
		switch (set.trigger) {
		case PL_TRIGGER_PHRASES:
			trig = QString("every %1 phrases").arg(set.every_phrases);
			break;
		case PL_TRIGGER_RANDOM:
			trig = QString("random %1%/phrase").arg(std::lround(set.change_chance * 100.0f));
			break;
		case PL_TRIGGER_MANUAL:
			trig = "manual only";
			break;
		default:
			trig = "next drop";
			break;
		}
		nextLbl->setText(QString("changes on: %1").arg(trig));

		/* timeline */
		timeline->tl = view.timeline;
		timeline->now = view.beats;
		timeline->phrase = pb;
		timeline->update();

		/* log: newest first (mock logEv) */
		if (!view.log.empty()) {
			for (const auto &l : view.log) {
				log_cursor = std::max(log_cursor, l.seq);
				lines.prepend(QString("phrase %1 %2 %3")
						      .arg(static_cast<qulonglong>(l.phrase + 1), 3)
						      .arg(kDot)
						      .arg(QString::fromUtf8(l.text.c_str())));
			}
			while (lines.size() > 60)
				lines.removeLast();
			logText->setPlainText(lines.join("\n"));
		}
	}

	QComboBox *picker = nullptr;
	QLabel *secChip = nullptr, *lockChip = nullptr, *stateChip = nullptr, *beatsL[4] = {};
	QLabel *sBpm = nullptr, *sConf = nullptr, *sBar = nullptr, *sMix = nullptr, *sCool = nullptr;
	QLabel *sAudio = nullptr, *nextLbl = nullptr, *empty = nullptr;
	QGridLayout *padGrid = nullptr;
	PadWidget *pads[PL_MAX_PALETTES] = {};
	QPushButton *bForceOff = nullptr, *bForceOn = nullptr, *bPause = nullptr, *bPrev = nullptr, *bNext = nullptr,
		    *bRotate = nullptr, *bPanic = nullptr;
	TimelineWidget *timeline = nullptr;
	QPlainTextEdit *logText = nullptr;
	QStringList lines;
	QTimer *timer = nullptr;

	pdhost::DockView view;
	uint64_t selected = 0;
	uint64_t log_cursor = 0;
	std::string inst_sig;
	std::string pads_sig;
	bool rebuilding = false;
};

} // namespace

#endif

#include "palette-director-dock.h"

void palette_director_dock_load(void)
{
#if defined(ENABLE_FRONTEND_API) && defined(ENABLE_QT)
	auto *dock = new PaletteDirectorDock();
	if (!obs_frontend_add_dock_by_id("palette-director-dock", obs_module_text("PaletteDirector"), dock)) {
		blog(LOG_ERROR, "[palette-director] dock id already in use; dock not added");
		delete dock;
		return;
	}
	blog(LOG_INFO, "[palette-director] dock registered (obs_frontend_add_dock_by_id)");

	/* Sandbox test hooks (unset in normal use). PD_DOCK_SNAPSHOT_DIR: offscreen PNGs every 2 s while
	 * the dock is hidden, plus Qt grabs of the OBS main window. PD_DOCK_TEST_CYCLE=1: show the dock,
	 * close it (QDockWidget::close, same as its X button), then reopen it, to prove the Director is
	 * unaffected. Both only drive the UI on the UI thread via QTimer. */
	const QString snapDir = qEnvironmentVariable("PD_DOCK_SNAPSHOT_DIR");
	if (!snapDir.isEmpty()) {
		auto *t = new QTimer(dock);
		t->setInterval(2000);
		QObject::connect(t, &QTimer::timeout, dock, [dock, snapDir] { dock->snapshot_offscreen(snapDir); });
		t->start();
	}
	if (qEnvironmentVariableIntValue("PD_DOCK_TEST_CYCLE") == 1) {
		auto step = [dock, snapDir](int ms, int what) {
			QTimer::singleShot(ms, dock, [dock, snapDir, what] {
				auto *dw = qobject_cast<QDockWidget *>(dock->parentWidget());
				auto *mw = static_cast<QMainWindow *>(obs_frontend_get_main_window());
				if (!dw) {
					blog(LOG_WARNING, "[palette-director] dock test: no QDockWidget parent");
					return;
				}
				if (what == 0) {
					dw->setFloating(false);
					if (mw && mw->dockWidgetArea(dw) == Qt::NoDockWidgetArea)
						mw->addDockWidget(Qt::RightDockWidgetArea, dw);
					dw->show();
					dw->raise();
					if (mw)
						mw->resizeDocks({dw}, {560}, Qt::Horizontal);
					blog(LOG_INFO,
					     "[palette-director] dock test: dock opened (docked, right area)");
				} else if (what == 1) {
					dw->close();
					blog(LOG_INFO,
					     "[palette-director] dock test: dock closed (QDockWidget::close)");
				} else if (what == 2) {
					dw->show();
					dw->raise();
					blog(LOG_INFO, "[palette-director] dock test: dock reopened");
				} else if (mw && !snapDir.isEmpty()) {
					const QString path =
						QDir(snapDir).filePath(QString("obs-window-%1.png").arg(what));
					mw->grab().save(path);
					blog(LOG_INFO, "[palette-director] dock test: OBS main window grabbed to %s",
					     path.toUtf8().constData());
				}
			});
		};
		step(15000, 0);  // open
		step(45000, 10); // grab while docked + live
		/* chained so the presses stay in order even if the UI thread stalls */
		QTimer::singleShot(46000, dock, [dock] { dock->test_press_chain(0); });
		step(56000, 12); // grab after the dock controls (chain ends ~54 s)
		step(60000, 1);  // close
		step(90000, 2);  // reopen
		step(100000, 11);
	}
#endif
}
