/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "statistics/chart_widget.h"

#include "base/qt/qt_key_modifiers.h"
#include "statistics/linear_chart_view.h"
#include "ui/abstract_button.h"
#include "ui/effects/animation_value_f.h"
#include "ui/rect.h"
#include "styles/style_boxes.h"

namespace Statistic {

namespace {

constexpr auto kHeightLimitsUpdateTimeout = crl::time(320);

[[nodiscard]] int FindMaxValue(
		Data::StatisticalChart &chartData,
		int startXIndex,
		int endXIndex) {
	auto maxValue = 0;
	for (auto &l : chartData.lines) {
		const auto lineMax = l.segmentTree.rMaxQ(startXIndex, endXIndex);
		maxValue = std::max(lineMax, maxValue);
	}
	return maxValue;
}

[[nodiscard]] int FindMinValue(
		Data::StatisticalChart &chartData,
		int startXIndex,
		int endXIndex) {
	auto minValue = std::numeric_limits<int>::max();
	for (auto &l : chartData.lines) {
		const auto lineMin = l.segmentTree.rMinQ(startXIndex, endXIndex);
		minValue = std::min(lineMin, minValue);
	}
	return minValue;
}

void PaintHorizontalLines(
		QPainter &p,
		const ChartHorizontalLinesData &horizontalLine,
		const QRect &r) {
	const auto alpha = p.opacity();
	p.setOpacity(horizontalLine.alpha);
	for (const auto &line : horizontalLine.lines) {
		const auto lineRect = QRect(
			0,
			r.y() + r.height() * line.relativeValue,
			r.x() + r.width(),
			st::lineWidth);
		p.fillRect(lineRect, st::boxTextFg);
	}
	p.setOpacity(alpha);
}

void PaintCaptionsToHorizontalLines(
		QPainter &p,
		const ChartHorizontalLinesData &horizontalLine,
		const QRect &r) {
	const auto alpha = p.opacity();
	p.setOpacity(horizontalLine.alpha);
	p.setFont(st::boxTextFont->f);
	p.setPen(st::boxTextFg);
	for (const auto &line : horizontalLine.lines) {
		p.drawText(10, r.y() + r.height() * line.relativeValue, line.caption);
	}
	p.setOpacity(alpha);
}

} // namespace

class ChartWidget::Footer final : public Ui::AbstractButton {
public:
	Footer(not_null<Ui::RpWidget*> parent);

	[[nodiscard]] rpl::producer<Limits> xPercentageLimitsChange() const;
	[[nodiscard]] rpl::producer<> userInteractionFinished() const;

private:
	not_null<Ui::AbstractButton*> _left;
	not_null<Ui::AbstractButton*> _right;

	rpl::event_stream<Limits> _xPercentageLimitsChange;
	rpl::event_stream<> _userInteractionFinished;

	struct {
		int x = 0;
		int leftLimit = 0;
		int rightLimit = 0;
		int diffX = 0;
	} _start;

};

ChartWidget::Footer::Footer(not_null<Ui::RpWidget*> parent)
: Ui::AbstractButton(parent)
, _left(Ui::CreateChild<Ui::AbstractButton>(this))
, _right(Ui::CreateChild<Ui::AbstractButton>(this)) {
	sizeValue(
	) | rpl::start_with_next([=](const QSize &s) {
		_left->resize(st::colorSliderWidth, s.height());
		_right->resize(st::colorSliderWidth, s.height());
	}, _left->lifetime());
	_left->paintRequest(
	) | rpl::start_with_next([=] {
		auto p = QPainter(_left);
		p.setOpacity(0.3);
		p.fillRect(_left->rect(), st::boxTextFg);
	}, _left->lifetime());
	_right->paintRequest(
	) | rpl::start_with_next([=] {
		auto p = QPainter(_right);
		p.setOpacity(0.3);
		p.fillRect(_right->rect(), st::boxTextFg);
	}, _right->lifetime());

	sizeValue(
	) | rpl::take(2) | rpl::start_with_next([=] {
		_left->moveToLeft(0, 0);
		_right->moveToRight(0, 0);
	}, _left->lifetime());

	const auto handleDrag = [&](
			not_null<Ui::AbstractButton*> side,
			Fn<int()> leftLimit,
			Fn<int()> rightLimit) {
		side->events(
		) | rpl::filter([=](not_null<QEvent*> e) {
			return (e->type() == QEvent::MouseButtonPress)
				|| (e->type() == QEvent::MouseButtonRelease)
				|| ((e->type() == QEvent::MouseMove) && side->isDown());
		}) | rpl::start_with_next([=](not_null<QEvent*> e) {
			const auto pos = static_cast<QMouseEvent*>(e.get())->pos();
			switch (e->type()) {
			case QEvent::MouseMove: {
				if (base::IsCtrlPressed()) {
					const auto diff = (pos.x() - _start.x);
					_left->move(_left->x() + diff, side->y());
					_right->move(_right->x() + diff, side->y());
				} else {
					_start.diffX = pos.x() - _start.x;
					const auto nextX = std::clamp(
						side->x() + (pos.x() - _start.x),
						_start.leftLimit,
						_start.rightLimit);
					side->move(nextX, side->y());
				}
				_xPercentageLimitsChange.fire({
					.min = _left->x() / float64(width()),
					.max = rect::right(_right) / float64(width()),
				});
			} break;
			case QEvent::MouseButtonPress: {
				_start.x = pos.x();
				_start.leftLimit = leftLimit();
				_start.rightLimit = rightLimit();
			} break;
			case QEvent::MouseButtonRelease: {
				_userInteractionFinished.fire({});
				_xPercentageLimitsChange.fire({
					.min = _left->x() / float64(width()),
					.max = rect::right(_right) / float64(width()),
				});
				_start = {};
			} break;
			}
		}, side->lifetime());
	};
	handleDrag(
		_left,
		[=] { return 0; },
		[=] { return _right->x() - _left->width(); });
	handleDrag(
		_right,
		[=] { return rect::right(_left); },
		[=] { return width() - _right->width(); });
}

rpl::producer<Limits> ChartWidget::Footer::xPercentageLimitsChange() const {
	return _xPercentageLimitsChange.events();
}

rpl::producer<> ChartWidget::Footer::userInteractionFinished() const {
	return _userInteractionFinished.events();
}

ChartWidget::ChartAnimationController::ChartAnimationController(
	Fn<void()> &&updateCallback)
: _animation(std::move(updateCallback)) {
}

void ChartWidget::ChartAnimationController::setXPercentageLimits(
		Data::StatisticalChart &chartData,
		Limits xPercentageLimits,
		crl::time now) {
	if ((_animValueXMin.to() == xPercentageLimits.min)
		&& (_animValueXMax.to() == xPercentageLimits.max)) {
		return;
	}
	start();
	_animValueXMin.start(xPercentageLimits.min);
	_animValueXMax.start(xPercentageLimits.max);
	_lastUserInteracted = now;

	{
		const auto startXIndex = chartData.findStartIndex(
			_animValueXMin.to());
		const auto endXIndex = chartData.findEndIndex(
			startXIndex,
			_animValueXMax.to());
		_finalHeightLimits = {
			float64(FindMinValue(chartData, startXIndex, endXIndex)),
			float64(FindMaxValue(chartData, startXIndex, endXIndex)),
		};
	}
	_animValueYMin = anim::value(
		_animValueYMin.current(),
		_finalHeightLimits.min);
	_animValueYMax = anim::value(
		_animValueYMax.current(),
		_finalHeightLimits.max);

	{
		auto k = (_animValueYMax.current() - _animValueYMin.current())
			/ float64(_finalHeightLimits.max - _finalHeightLimits.min);
		if (k > 1.) {
			k = 1. / k;
		}
		constexpr auto kDtHeightSpeed1 = 0.03 / 2;
		constexpr auto kDtHeightSpeed2 = 0.03 / 2;
		constexpr auto kDtHeightSpeed3 = 0.045 / 2;
		constexpr auto kDtHeightSpeedThreshold1 = 0.7;
		constexpr auto kDtHeightSpeedThreshold2 = 0.1;
		constexpr auto kDtHeightInstantThreshold = 0.97;
		_dtYSpeed = (k > kDtHeightSpeedThreshold1)
			? kDtHeightSpeed1
			: (k < kDtHeightSpeedThreshold2)
			? kDtHeightSpeed2
			: kDtHeightSpeed3;
		if (k < kDtHeightInstantThreshold) {
			_dtCurrent = { 0., 0. };
		}
	}
}

void ChartWidget::ChartAnimationController::start() {
	if (!_animation.animating()) {
		_animation.start();
	}
}

void ChartWidget::ChartAnimationController::finish() {
	_animation.stop();
	_animValueXMin.finish();
	_animValueXMax.finish();
	_animValueYMin.finish();
	_animValueYMax.finish();
	_animValueYAlpha.finish();
}

void ChartWidget::ChartAnimationController::resetAlpha() {
	_alphaAnimationStartedAt = 0;
	_animValueYAlpha = anim::value(0., 1.);
}

void ChartWidget::ChartAnimationController::tick(
		crl::time now,
		std::vector<ChartHorizontalLinesData> &horizontalLines) {
	if (!_animation.animating()) {
		return;
	}
	constexpr auto kExpandingDelay = crl::time(100);
	constexpr auto kXExpandingDuration = 200.;
	constexpr auto kAlphaExpandingDuration = 200.;

	if (!_yAnimationStartedAt
			&& ((now - _lastUserInteracted) >= kExpandingDelay)) {
		_heightAnimationStarts.fire({});
		_yAnimationStartedAt = _lastUserInteracted + kExpandingDelay;
	}
	if (!_alphaAnimationStartedAt) {
		_alphaAnimationStartedAt = now;
	}

	_dtCurrent.min = std::min(_dtCurrent.min + _dtYSpeed, 1.);
	_dtCurrent.max = std::min(_dtCurrent.max + _dtYSpeed, 1.);

	const auto dtX = std::min(
		(now - _animation.started()) / kXExpandingDuration,
		1.);
	const auto dtAlpha = std::min(
		(now - _alphaAnimationStartedAt) / kAlphaExpandingDuration,
		1.);

	const auto isFinished = [](const anim::value &anim) {
		return anim.current() == anim.to();
	};

	const auto xFinished = isFinished(_animValueXMin)
		&& isFinished(_animValueXMax);
	const auto yFinished = isFinished(_animValueYMin)
		&& isFinished(_animValueYMax);
	const auto alphaFinished = isFinished(_animValueYAlpha);

	if (xFinished && yFinished && alphaFinished) {
		const auto &lines = horizontalLines.back().lines;
		if ((lines.front().absoluteValue == _animValueYMin.to())
			&& (lines.back().absoluteValue == _animValueYMax.to())) {
			_animation.stop();
		}
	}
	if (xFinished) {
		_animValueXMin.finish();
		_animValueXMax.finish();
	} else {
		_animValueXMin.update(dtX, anim::linear);
		_animValueXMax.update(dtX, anim::linear);
	}
	if (_yAnimationStartedAt) {
		_animValueYMin.update(_dtCurrent.min, anim::easeInCubic);
		_animValueYMax.update(_dtCurrent.max, anim::easeInCubic);
		_animValueYAlpha.update(dtAlpha, anim::easeInCubic);

		for (auto &horizontalLine : horizontalLines) {
			horizontalLine.computeRelative(
				_animValueYMax.current(),
				_animValueYMin.current());
		}
	}

	if (dtAlpha >= 0. && dtAlpha <= 1.) {
		const auto value = _animValueYAlpha.current();

		for (auto &horizontalLine : horizontalLines) {
			horizontalLine.alpha = horizontalLine.fixedAlpha * (1. - value);
		}
		horizontalLines.back().alpha = value;
		if (value == 1.) {
			while (horizontalLines.size() > 1) {
				const auto startIt = begin(horizontalLines);
				if (!startIt->alpha) {
					horizontalLines.erase(startIt);
				} else {
					break;
				}
			}
		}
	}

	if (yFinished && alphaFinished) {
		_alphaAnimationStartedAt = 0;
		_yAnimationStartedAt = 0;
	}
}

Limits ChartWidget::ChartAnimationController::currentXLimits() const {
	return { _animValueXMin.current(), _animValueXMax.current() };
}

Limits ChartWidget::ChartAnimationController::currentHeightLimits() const {
	return { _animValueYMin.current(), _animValueYMax.current() };
}

Limits ChartWidget::ChartAnimationController::finalHeightLimits() const {
	return _finalHeightLimits;
}

auto ChartWidget::ChartAnimationController::heightAnimationStarts() const
-> rpl::producer<> {
	return _heightAnimationStarts.events();
}

ChartWidget::ChartWidget(not_null<Ui::RpWidget*> parent)
: Ui::RpWidget(parent)
, _footer(std::make_unique<Footer>(this))
, _animationController([=] { update(); }) {
	sizeValue(
	) | rpl::start_with_next([=](const QSize &s) {
		_footer->setGeometry(
			0,
			s.height() - st::countryRowHeight,
			s.width(),
			st::countryRowHeight);
	}, _footer->lifetime());
	_footer->paintRequest(
	) | rpl::start_with_next([=, limits = Limits{ 0., 1. }] {
		auto p = QPainter(_footer.get());

		if (_chartData) {
			const auto startXIndex2 = 0;
			const auto endXIndex2 = int(_chartData.xPercentage.size() - 1);
			const auto limitsY = Limits{
				float64(FindMinValue(_chartData, startXIndex2, endXIndex2)),
				float64(FindMaxValue(_chartData, startXIndex2, endXIndex2)),
			};
			p.fillRect(_footer->rect(), st::boxBg);
			Statistic::PaintLinearChartView(
				p,
				_chartData,
				limits,
				limitsY,
				_footer->rect());
		}
	}, _footer->lifetime());

	rpl::merge(
		_animationController.heightAnimationStarts(),
		_footer->userInteractionFinished()
	) | rpl::start_with_next([=] {
		_animationController.resetAlpha();
		addHorizontalLine(_animationController.finalHeightLimits(), true);
		_animationController.start();
	}, _footer->lifetime());

	_footer->xPercentageLimitsChange(
	) | rpl::start_with_next([=](Limits xPercentageLimits) {
		const auto now = crl::now();
		_animationController.setXPercentageLimits(
			_chartData,
			xPercentageLimits,
			now);
		if ((now - _lastHeightLimitsChanged) < kHeightLimitsUpdateTimeout) {
			return;
		}
		_lastHeightLimitsChanged = now;
		_animationController.resetAlpha();
		addHorizontalLine(_animationController.finalHeightLimits(), true);
	}, _footer->lifetime());
	resize(width(), st::confirmMaxHeight + st::countryRowHeight * 2);
}

void ChartWidget::setChartData(Data::StatisticalChart chartData) {
	_chartData = chartData;

	_animationController.setXPercentageLimits(
		_chartData,
		{ _chartData.xPercentage.front(), _chartData.xPercentage.back() },
		0);
	_animationController.finish();
	addHorizontalLine(_animationController.finalHeightLimits(), false);
	update();
}

void ChartWidget::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);

	_animationController.tick(crl::now(), _horizontalLines);

	const auto r = rect();
	const auto captionRect = r;
	const auto chartRectBottom = st::lineWidth
		+ _footer->height()
		+ st::countryRowHeight;
	const auto chartRect = r
		- QMargins{ 0, st::boxTextFont->height, 0, chartRectBottom };

	p.fillRect(r, st::boxBg);

	for (auto &horizontalLine : _horizontalLines) {
		PaintHorizontalLines(p, horizontalLine, chartRect);
	}

	if (_chartData) {
		Statistic::PaintLinearChartView(
			p,
			_chartData,
			_animationController.currentXLimits(),
			_animationController.currentHeightLimits(),
			chartRect);
	}

	for (auto &horizontalLine : _horizontalLines) {
		PaintCaptionsToHorizontalLines(p, horizontalLine, chartRect);
	}
}

void ChartWidget::addHorizontalLine(Limits newHeight, bool animated) {
	const auto newLinesData = ChartHorizontalLinesData(
		newHeight.max,
		newHeight.min,
		true);
	if (!animated) {
		_horizontalLines.clear();
	}
	for (auto &horizontalLine : _horizontalLines) {
		horizontalLine.fixedAlpha = horizontalLine.alpha;
	}
	_horizontalLines.push_back(newLinesData);
	if (!animated) {
		_horizontalLines.back().alpha = 1.;
	}
}

} // namespace Statistic