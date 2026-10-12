// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/account_groups/account_groups.h"

#include "data/data_changes.h"
#include "data/data_user.h"
#include "lang/lang_instance.h"
#include "main/main_session.h"
#include "settings.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace AyuAccountGroups {
namespace {

constexpr auto kFileVersion = 1;

struct AccountData {
	QString label;
	GroupId group = 0;
};

struct State {
	bool loaded = false;
	std::vector<Group> groups;
	base::flat_map<uint64, AccountData> accounts;
	GroupId nextId = 1;
	rpl::event_stream<> changes;
};

[[nodiscard]] State &Instance() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString FilePath() {
	return cWorkingDir() + u"tdata/ayu_account_groups.json"_q;
}

[[nodiscard]] QString Normalized(const QString &text, int limit) {
	auto result = text;
	result.replace(QChar('\n'), QChar(' '));
	result.replace(QChar('\r'), QChar(' '));
	result.replace(QChar('\t'), QChar(' '));
	result = result.trimmed();
	if (result.size() > limit) {
		result = result.mid(0, limit).trimmed();
	}
	return result;
}

[[nodiscard]] bool HasGroup(const State &state, GroupId id) {
	return id > 0
		&& (ranges::find(state.groups, id, &Group::id) != end(state.groups));
}

void Load(State &state) {
	const auto path = FilePath();
	auto file = QFile(path);
	if (!file.exists()) {
		return;
	} else if (!file.open(QIODevice::ReadOnly)) {
		LOG(("AyuGram Account Groups: could not open '%1'.").arg(path));
		return;
	}
	const auto bytes = file.readAll();
	file.close();

	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(bytes, &error);
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		// Keep the broken file aside, so nothing the user typed is lost.
		LOG(("AyuGram Account Groups: bad file, moving it aside."));
		const auto backup = path + u".broken"_q;
		QFile::remove(backup);
		QFile::rename(path, backup);
		return;
	}
	const auto root = document.object();
	const auto groups = root.value(u"groups"_q).toArray();
	for (const auto &value : groups) {
		const auto object = value.toObject();
		const auto id = object.value(u"id"_q).toInt();
		const auto name = Normalized(
			object.value(u"name"_q).toString(),
			kMaxGroupNameLength);
		if (id <= 0 || name.isEmpty() || HasGroup(state, id)) {
			continue;
		}
		state.groups.push_back({
			.id = id,
			.name = name,
			.collapsed = object.value(u"collapsed"_q).toBool(),
		});
		state.nextId = std::max(state.nextId, id + 1);
	}
	state.nextId = std::max(
		state.nextId,
		GroupId(root.value(u"nextGroupId"_q).toInt()));

	const auto accounts = root.value(u"accounts"_q).toObject();
	for (auto i = accounts.begin(); i != accounts.end(); ++i) {
		auto ok = false;
		const auto key = i.key().toULongLong(&ok);
		if (!ok || !key) {
			continue;
		}
		const auto object = i.value().toObject();
		auto data = AccountData{
			.label = Normalized(
				object.value(u"label"_q).toString(),
				kMaxLabelLength),
			.group = GroupId(object.value(u"group"_q).toInt()),
		};
		if (!HasGroup(state, data.group)) {
			data.group = 0;
		}
		if (!data.label.isEmpty() || data.group) {
			state.accounts[key] = std::move(data);
		}
	}
}

void Save(const State &state) {
	auto groups = QJsonArray();
	for (const auto &group : state.groups) {
		auto object = QJsonObject();
		object.insert(u"id"_q, group.id);
		object.insert(u"name"_q, group.name);
		object.insert(u"collapsed"_q, group.collapsed);
		groups.append(object);
	}
	auto accounts = QJsonObject();
	for (const auto &[key, data] : state.accounts) {
		auto object = QJsonObject();
		if (!data.label.isEmpty()) {
			object.insert(u"label"_q, data.label);
		}
		if (data.group) {
			object.insert(u"group"_q, data.group);
		}
		if (!object.isEmpty()) {
			accounts.insert(QString::number(key), object);
		}
	}
	auto root = QJsonObject();
	root.insert(u"version"_q, kFileVersion);
	root.insert(u"nextGroupId"_q, state.nextId);
	root.insert(u"groups"_q, groups);
	root.insert(u"accounts"_q, accounts);

	// QSaveFile writes a temporary file and atomically replaces the old one,
	// so a crash in the middle of saving can't leave a half-written file.
	QDir().mkpath(cWorkingDir() + u"tdata"_q);
	const auto path = FilePath();
	auto file = QSaveFile(path);
	if (!file.open(QIODevice::WriteOnly)) {
		LOG(("AyuGram Account Groups: could not write '%1'.").arg(path));
		return;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if (!file.commit()) {
		LOG(("AyuGram Account Groups: could not save '%1'.").arg(path));
	}
}

[[nodiscard]] State &Loaded() {
	auto &state = Instance();
	if (!state.loaded) {
		state.loaded = true;
		Load(state);
	}
	return state;
}

void SaveAndNotify(State &state) {
	Save(state);
	state.changes.fire({});
}

void Cleanup(State &state, uint64 accountKey) {
	const auto i = state.accounts.find(accountKey);
	if (i != end(state.accounts)
		&& i->second.label.isEmpty()
		&& !i->second.group) {
		state.accounts.erase(i);
	}
}

} // namespace

uint64 AccountKey(not_null<Main::Session*> session) {
	return session->uniqueId();
}

QString Label(uint64 accountKey) {
	const auto &state = Loaded();
	const auto i = state.accounts.find(accountKey);
	return (i != end(state.accounts)) ? i->second.label : QString();
}

void SetLabel(uint64 accountKey, const QString &label) {
	auto &state = Loaded();
	const auto normalized = Normalized(label, kMaxLabelLength);
	if (Label(accountKey) == normalized) {
		return;
	}
	state.accounts[accountKey].label = normalized;
	Cleanup(state, accountKey);
	SaveAndNotify(state);
}

std::vector<Group> Groups() {
	return Loaded().groups;
}

std::optional<Group> FindGroup(GroupId id) {
	const auto &state = Loaded();
	const auto i = ranges::find(state.groups, id, &Group::id);
	if (i == end(state.groups)) {
		return std::nullopt;
	}
	return *i;
}

bool GroupNameTaken(const QString &name, GroupId except) {
	const auto normalized = Normalized(name, kMaxGroupNameLength);
	for (const auto &group : Loaded().groups) {
		if (group.id != except
			&& !group.name.compare(normalized, Qt::CaseInsensitive)) {
			return true;
		}
	}
	return false;
}

GroupId GroupOf(uint64 accountKey) {
	const auto &state = Loaded();
	const auto i = state.accounts.find(accountKey);
	return (i != end(state.accounts) && HasGroup(state, i->second.group))
		? i->second.group
		: 0;
}

void SetGroup(uint64 accountKey, GroupId id) {
	auto &state = Loaded();
	if (!HasGroup(state, id)) {
		id = 0;
	}
	if (GroupOf(accountKey) == id) {
		return;
	}
	state.accounts[accountKey].group = id;
	Cleanup(state, accountKey);
	SaveAndNotify(state);
}

GroupId CreateGroup(const QString &name) {
	auto &state = Loaded();
	const auto normalized = Normalized(name, kMaxGroupNameLength);
	if (normalized.isEmpty()) {
		return 0;
	}
	const auto id = state.nextId++;
	state.groups.push_back({ .id = id, .name = normalized });
	SaveAndNotify(state);
	return id;
}

void RenameGroup(GroupId id, const QString &name) {
	auto &state = Loaded();
	const auto normalized = Normalized(name, kMaxGroupNameLength);
	const auto i = ranges::find(state.groups, id, &Group::id);
	if (normalized.isEmpty()
		|| i == end(state.groups)
		|| i->name == normalized) {
		return;
	}
	i->name = normalized;
	SaveAndNotify(state);
}

void DeleteGroup(GroupId id) {
	auto &state = Loaded();
	const auto i = ranges::find(state.groups, id, &Group::id);
	if (i == end(state.groups)) {
		return;
	}
	state.groups.erase(i);
	auto emptied = std::vector<uint64>();
	for (auto &[key, data] : state.accounts) {
		if (data.group == id) {
			data.group = 0;
			emptied.push_back(key);
		}
	}
	for (const auto key : emptied) {
		Cleanup(state, key);
	}
	SaveAndNotify(state);
}

bool CanMoveGroup(GroupId id, int delta) {
	const auto &state = Loaded();
	const auto i = ranges::find(state.groups, id, &Group::id);
	if (i == end(state.groups) || !delta) {
		return false;
	}
	const auto index = int(i - begin(state.groups));
	const auto moved = index + delta;
	return (moved >= 0) && (moved < int(state.groups.size()));
}

void MoveGroup(GroupId id, int delta) {
	if (!CanMoveGroup(id, delta)) {
		return;
	}
	auto &state = Loaded();
	const auto i = ranges::find(state.groups, id, &Group::id);
	const auto index = int(i - begin(state.groups));
	auto group = std::move(*i);
	state.groups.erase(i);
	state.groups.insert(begin(state.groups) + index + delta, std::move(group));
	SaveAndNotify(state);
}

void SetGroupCollapsed(GroupId id, bool collapsed) {
	auto &state = Loaded();
	const auto i = ranges::find(state.groups, id, &Group::id);
	if (i == end(state.groups) || i->collapsed == collapsed) {
		return;
	}
	i->collapsed = collapsed;
	Save(state);
}

rpl::producer<> Changes() {
	return Instance().changes.events();
}

QString DisplayName(not_null<Main::Session*> session) {
	const auto label = Label(AccountKey(session));
	return label.isEmpty() ? session->user()->name() : label;
}

rpl::producer<QString> DisplayNameValue(not_null<Main::Session*> session) {
	const auto user = session->user();
	const auto key = AccountKey(session);
	auto name = rpl::single(
		user->name()
	) | rpl::then(session->changes().realtimeNameUpdates(
		user
	) | rpl::map([=] {
		return user->name();
	}));
	auto label = rpl::single(
		rpl::empty
	) | rpl::then(
		Changes()
	) | rpl::map([=] {
		return Label(key);
	});
	return rpl::combine(
		std::move(name),
		std::move(label)
	) | rpl::map([](const QString &name, const QString &label) {
		return label.isEmpty() ? name : label;
	}) | rpl::distinct_until_changed();
}

bool PersianUi() {
	const auto persian = [](const QString &id) {
		return id.startsWith(u"fa"_q, Qt::CaseInsensitive);
	};
	const auto &instance = Lang::GetInstance();
	return persian(instance.id()) || persian(instance.baseId());
}

QString Tr(const char *english, const char *persian) {
	return QString::fromUtf8(PersianUi() ? persian : english);
}

} // namespace AyuAccountGroups
