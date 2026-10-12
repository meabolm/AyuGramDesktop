// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "ayu/features/account_groups/account_groups.h"
#include "ui/widgets/buttons.h"

namespace style {
struct SettingsButton;
} // namespace style

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class Show;
namespace Menu {
struct MenuCallback;
} // namespace Menu
} // namespace Ui

namespace AyuAccountGroups {

// "Set account name…" and "Category ▸" items of an account context menu.
void FillAccountMenu(
	const Ui::Menu::MenuCallback &addAction,
	std::shared_ptr<Ui::Show> show,
	not_null<Main::Session*> session);

// Context menu of a category header.
void FillGroupMenu(
	const Ui::Menu::MenuCallback &addAction,
	std::shared_ptr<Ui::Show> show,
	GroupId id);

// Clickable category header for the accounts list.
class GroupHeader final : public Ui::RippleButton {
public:
	GroupHeader(
		QWidget *parent,
		const style::SettingsButton &st,
		const QString &name,
		int count,
		bool collapsed);

	void setCollapsed(bool collapsed);
	[[nodiscard]] bool collapsed() const;
	[[nodiscard]] rpl::producer<bool> collapsedValue() const;

	// Space at the right side reserved for an unread badge.
	void setRightSkip(int skip);

protected:
	void paintEvent(QPaintEvent *e) override;
	int resizeGetHeight(int newWidth) override;

private:
	void paintChevron(QPainter &p);

	const style::SettingsButton &_st;
	const QString _name;
	const QString _count;
	int _countWidth = 0;
	int _rightSkip = 0;
	rpl::variable<bool> _collapsed;

};

} // namespace AyuAccountGroups
