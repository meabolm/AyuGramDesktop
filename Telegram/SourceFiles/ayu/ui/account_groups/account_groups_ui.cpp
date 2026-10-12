// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/account_groups/account_groups_ui.h"

#include "data/data_user.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/layers/show.h"
#include "ui/painter.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/popup_menu.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace AyuAccountGroups {
namespace {

struct NameBoxArgs {
	QString title;
	QString placeholder;
	QString initial;
	QString about;
	int maxLength = 0;
	bool allowEmpty = false;
	Fn<bool(const QString&)> valid;
	Fn<void(const QString&)> done;
};

void NameBox(not_null<Ui::GenericBox*> box, NameBoxArgs &&args) {
	box->setTitle(args.title);

	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		rpl::single(args.placeholder),
		args.initial));
	if (args.maxLength > 0) {
		field->setMaxLength(args.maxLength);
	}
	if (!args.about.isEmpty()) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(box, args.about, st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
	}

	box->setFocusCallback([=] {
		field->setFocusFast();
		field->selectAll();
	});

	const auto allowEmpty = args.allowEmpty;
	const auto valid = std::move(args.valid);
	const auto done = std::move(args.done);
	const auto submit = [=] {
		const auto text = field->getLastText().trimmed();
		if ((text.isEmpty() && !allowEmpty)
			|| (!text.isEmpty() && valid && !valid(text))) {
			field->showError();
			return;
		}
		box->closeBox();
		done(text);
	};
	field->submits(
	) | rpl::on_next([=](auto) {
		submit();
	}, field->lifetime());

	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

[[nodiscard]] QString Quoted(const QString &text) {
	return PersianUi()
		? (QString::fromUtf8("«") + text + QString::fromUtf8("»"))
		: (QString::fromUtf8("“") + text + QString::fromUtf8("”"));
}

void ShowGroupNameBox(
		std::shared_ptr<Ui::Show> show,
		QString title,
		QString initial,
		GroupId except,
		Fn<void(const QString&)> done) {
	show->showBox(Box(NameBox, NameBoxArgs{
		.title = std::move(title),
		.placeholder = Tr("Category name", "نام دسته"),
		.initial = std::move(initial),
		.maxLength = kMaxGroupNameLength,
		.valid = [=](const QString &name) {
			return !GroupNameTaken(name, except);
		},
		.done = std::move(done),
	}));
}

} // namespace

void FillAccountMenu(
		const Ui::Menu::MenuCallback &addAction,
		std::shared_ptr<Ui::Show> show,
		not_null<Main::Session*> session) {
	const auto key = AccountKey(session);
	const auto original = session->user()->name();

	addAction(Tr("Set account name…", "نام‌گذاری حساب…"), [=] {
		show->showBox(Box(NameBox, NameBoxArgs{
			.title = Tr("Account name", "نام حساب"),
			.placeholder = Tr("Custom name", "نام دلخواه"),
			.initial = Label(key),
			.about = Tr(
				"Only you see this name, only in this app on this computer. "
				"Nothing changes in your Telegram account. "
				"Leave it empty to show the original name:",
				"این نام فقط برای شما و فقط در همین برنامه روی همین کامپیوتر "
				"نمایش داده می‌شود و چیزی در حساب تلگرام تغییر نمی‌کند. "
				"اگر خالی بماند، نام اصلی نمایش داده می‌شود:")
				+ ' '
				+ Quoted(original),
			.maxLength = kMaxLabelLength,
			.allowEmpty = true,
			.done = [=](const QString &label) {
				SetLabel(key, label);
			},
		}));
	}, &st::menuIconEdit);

	const auto current = GroupOf(key);
	addAction(Ui::Menu::MenuCallback::Args{
		.text = Tr("Category", "دسته‌بندی"),
		.handler = nullptr,
		.icon = &st::menuIconAddToFolder,
		.fillSubmenu = [=](not_null<Ui::PopupMenu*> menu) {
			const auto check = [&](bool checked) {
				return checked ? &st::mediaPlayerMenuCheck : nullptr;
			};
			menu->addAction(Tr("No category", "بدون دسته"), [=] {
				SetGroup(key, 0);
			}, check(!current));
			for (const auto &group : Groups()) {
				const auto id = group.id;
				menu->addAction(
					Ui::Text::FixAmpersandInAction(group.name),
					[=] { SetGroup(key, id); },
					check(current == id));
			}
			menu->addSeparator();
			menu->addAction(Tr("New category…", "دسته جدید…"), [=] {
				ShowGroupNameBox(
					show,
					Tr("New category", "دسته جدید"),
					QString(),
					0,
					[=](const QString &name) {
						if (const auto id = CreateGroup(name)) {
							SetGroup(key, id);
						}
					});
			}, &st::menuIconAdd);
		},
	});
}

void FillGroupMenu(
		const Ui::Menu::MenuCallback &addAction,
		std::shared_ptr<Ui::Show> show,
		GroupId id) {
	const auto group = FindGroup(id);
	if (!group) {
		return;
	}
	const auto name = group->name;

	addAction(Tr("Rename category…", "تغییر نام دسته…"), [=] {
		ShowGroupNameBox(
			show,
			Tr("Rename category", "تغییر نام دسته"),
			name,
			id,
			[=](const QString &name) { RenameGroup(id, name); });
	}, &st::menuIconEdit);

	if (CanMoveGroup(id, -1)) {
		addAction(Tr("Move up", "انتقال به بالا"), [=] {
			MoveGroup(id, -1);
		}, &st::menuIconReorder);
	}
	if (CanMoveGroup(id, 1)) {
		addAction(Tr("Move down", "انتقال به پایین"), [=] {
			MoveGroup(id, 1);
		}, &st::menuIconReorder);
	}

	addAction({
		.text = Tr("Delete category", "حذف دسته"),
		.handler = [=] {
			show->showBox(Ui::MakeConfirmBox({
				.text = Tr(
					"Delete the category %1? "
					"The accounts in it stay logged in, "
					"they just move to “No category”.",
					"دسته %1 حذف شود؟ "
					"حساب‌های داخل آن از سیستم خارج نمی‌شوند "
					"و فقط به «بدون دسته» منتقل می‌شوند.").arg(Quoted(name)),
				.confirmed = [=](Fn<void()> close) {
					close();
					DeleteGroup(id);
				},
				.confirmText = tr::lng_box_delete(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		},
		.icon = &st::menuIconDeleteAttention,
		.isAttention = true,
	});
}

GroupHeader::GroupHeader(
	QWidget *parent,
	const style::SettingsButton &st,
	const QString &name,
	int count,
	bool collapsed)
: RippleButton(parent, st.ripple)
, _st(st)
, _name(name)
, _count(QString::number(count))
, _countWidth(st::normalFont->width(_count))
, _collapsed(collapsed) {
	_collapsed.changes(
	) | rpl::on_next([=] {
		update();
	}, lifetime());
	resize(width(), resizeGetHeight(width()));
}

void GroupHeader::setCollapsed(bool collapsed) {
	_collapsed = collapsed;
}

bool GroupHeader::collapsed() const {
	return _collapsed.current();
}

rpl::producer<bool> GroupHeader::collapsedValue() const {
	return _collapsed.value();
}

void GroupHeader::setRightSkip(int skip) {
	if (_rightSkip != skip) {
		_rightSkip = skip;
		update();
	}
}

int GroupHeader::resizeGetHeight(int newWidth) {
	return _st.height
		+ _st.padding.top()
		+ _st.padding.bottom()
		- style::ConvertScale(8);
}

void GroupHeader::paintEvent(QPaintEvent *e) {
	auto p = Painter(this);

	const auto over = (isOver() || isDown()) && !isDisabled();
	p.fillRect(e->rect(), over ? _st.textBgOver : _st.textBg);
	paintRipple(p, 0, 0);

	paintChevron(p);

	const auto outer = width();
	const auto &font = _st.style.font;
	const auto left = _st.padding.left();
	const auto right = _st.padding.right() + _rightSkip;
	const auto top = (height() - font->height) / 2;
	const auto countSkip = 2 * font->spacew;
	const auto available = std::max(
		outer - left - right - countSkip - _countWidth,
		0);
	const auto name = font->elided(_name, available);
	const auto nameWidth = font->width(name);

	p.setFont(font);
	p.setPen(st::windowActiveTextFg);
	p.drawTextLeft(left, top, outer, name, nameWidth);

	p.setFont(st::normalFont);
	p.setPen(st::windowSubTextFg);
	p.drawTextLeft(
		left + nameWidth + countSkip,
		top + font->ascent - st::normalFont->ascent,
		outer,
		_count,
		_countWidth);
}

void GroupHeader::paintChevron(QPainter &p) {
	const auto rtl = style::RightToLeft();
	const auto size = style::ConvertScaleExact(4.);
	const auto center = _st.iconLeft + st::settingsIconAdd.width() / 2.;
	const auto x = rtl ? (width() - center) : center;
	const auto y = height() / 2.;

	auto hq = PainterHighQualityEnabler(p);
	auto pen = st::windowSubTextFg->p;
	pen.setWidthF(style::ConvertScaleExact(1.5));
	pen.setCapStyle(Qt::RoundCap);
	pen.setJoinStyle(Qt::RoundJoin);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);

	auto path = QPainterPath();
	if (_collapsed.current()) {
		// Points towards the text, like a closed folder in a tree.
		const auto direction = rtl ? -1. : 1.;
		path.moveTo(x - direction * size / 2., y - size);
		path.lineTo(x + direction * size / 2., y);
		path.lineTo(x - direction * size / 2., y + size);
	} else {
		path.moveTo(x - size, y - size / 2.);
		path.lineTo(x, y + size / 2.);
		path.lineTo(x + size, y - size / 2.);
	}
	p.drawPath(path);
}

} // namespace AyuAccountGroups
