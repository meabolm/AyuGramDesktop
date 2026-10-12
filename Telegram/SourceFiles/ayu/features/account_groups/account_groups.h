// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Local account labels and categories.
//
// Everything here is purely local and cosmetic: the data lives in its own
// file (tdata/ayu_account_groups.json) and never touches the encrypted
// account data or anything that is sent to Telegram.
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace AyuAccountGroups {

using GroupId = int32;

struct Group {
	GroupId id = 0;
	QString name;
	bool collapsed = false;
};

inline constexpr auto kMaxLabelLength = 64;
inline constexpr auto kMaxGroupNameLength = 48;

// Stable key of an account: user id (with the test DC shift).
[[nodiscard]] uint64 AccountKey(not_null<Main::Session*> session);

// Custom account names. An empty label means "use the Telegram name".
[[nodiscard]] QString Label(uint64 accountKey);
void SetLabel(uint64 accountKey, const QString &label);

// Categories, in display order.
[[nodiscard]] std::vector<Group> Groups();
[[nodiscard]] std::optional<Group> FindGroup(GroupId id);
[[nodiscard]] bool GroupNameTaken(const QString &name, GroupId except = 0);
[[nodiscard]] GroupId GroupOf(uint64 accountKey); // 0 means no category.
void SetGroup(uint64 accountKey, GroupId id);
GroupId CreateGroup(const QString &name);
void RenameGroup(GroupId id, const QString &name);
void DeleteGroup(GroupId id); // Accounts inside move to "no category".
void MoveGroup(GroupId id, int delta);
[[nodiscard]] bool CanMoveGroup(GroupId id, int delta);

// Only remembers the state, doesn't fire Changes().
void SetGroupCollapsed(GroupId id, bool collapsed);

// Fired after labels or categories were changed.
[[nodiscard]] rpl::producer<> Changes();

// The custom label if set, otherwise the Telegram name.
[[nodiscard]] QString DisplayName(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<QString> DisplayNameValue(
	not_null<Main::Session*> session);

// The strings of this feature exist in English and Persian only.
[[nodiscard]] bool PersianUi();
[[nodiscard]] QString Tr(const char *english, const char *persian);

} // namespace AyuAccountGroups
