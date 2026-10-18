// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/repeat_schedule/repeat_schedule.h"

#include "ayu/features/account_groups/account_groups.h"

#include "apiwrap.h"
#include "ayu/features/ayu_tools/ayu_tools.h"
#include "ayu/features/ayu_tools/history_loader.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"

#include <QtCore/QJsonArray>

namespace AyuRepeat {
namespace {

constexpr auto kFileName = "ayu_repeat_messages.json";
constexpr auto kMaxScheduledPerChat = 100;
constexpr auto kKeepFree = 2;
constexpr auto kMaxAhead = TimeId(364 * 24 * 60 * 60);
constexpr auto kCheckInterval = crl::time(30 * 60 * 1000);
constexpr auto kFirstCheckDelay = crl::time(25 * 1000);
constexpr auto kAfterRegisterDelay = crl::time(6 * 1000);
constexpr auto kSendDelay = crl::time(450);
constexpr auto kDeletedAfter = TimeId(20 * 60);

struct State {
	bool loaded = false;
	std::vector<Job> jobs;
	base::flat_set<uint64> busy;
	rpl::event_stream<> changes;
	std::unique_ptr<base::Timer> timer;
	base::has_weak_ptr guard;
};

[[nodiscard]] State &Instance() {
	static auto result = State();
	return result;
}

void Save() {
	const auto &state = Instance();
	auto list = QJsonArray();
	for (const auto &job : state.jobs) {
		auto object = QJsonObject();
		object.insert(u"id"_q, QString::number(job.id));
		object.insert(u"account"_q, QString::number(job.account));
		object.insert(u"peer"_q, QString::number(job.peer.value));
		object.insert(u"topic"_q, QString::number(job.topicRootId.bare));
		object.insert(u"text"_q, job.text.text);
		object.insert(u"tags"_q, AyuTools::TagsToJson(job.text.tags));
		object.insert(u"period"_q, job.period);
		object.insert(u"first"_q, job.first);
		object.insert(u"created"_q, job.created);
		object.insert(u"planned"_q, job.planned);
		object.insert(u"silent"_q, job.silent);
		list.append(object);
	}
	auto root = QJsonObject();
	root.insert(u"version"_q, 1);
	root.insert(u"jobs"_q, list);
	AyuTools::SaveJson(QString::fromLatin1(kFileName), root);
}

State &Loaded() {
	auto &state = Instance();
	if (state.loaded) {
		return state;
	}
	state.loaded = true;
	const auto root = AyuTools::LoadJson(QString::fromLatin1(kFileName));
	for (const auto &value : root.value(u"jobs"_q).toArray()) {
		const auto object = value.toObject();
		auto job = Job{
			.id = object.value(u"id"_q).toString().toULongLong(),
			.account = object.value(u"account"_q).toString().toULongLong(),
			.peer = PeerId(object.value(u"peer"_q).toString().toULongLong()),
			.topicRootId = MsgId(
				object.value(u"topic"_q).toString().toLongLong()),
			.text = {
				object.value(u"text"_q).toString(),
				AyuTools::TagsFromJson(object.value(u"tags"_q).toArray()),
			},
			.period = TimeId(object.value(u"period"_q).toInt()),
			.first = TimeId(object.value(u"first"_q).toInt()),
			.created = TimeId(object.value(u"created"_q).toInt()),
			.planned = TimeId(object.value(u"planned"_q).toInt()),
			.silent = object.value(u"silent"_q).toBool(),
		};
		if (job.id && job.account && job.peer && job.period > 0) {
			state.jobs.push_back(std::move(job));
		}
	}
	return state;
}

[[nodiscard]] QString Normalized(const QString &text) {
	return text.trimmed();
}

[[nodiscard]] bool IsCopy(
		const Job &job,
		const AyuTools::RawMessage &message) {
	return Normalized(message.text) == Normalized(job.text.text);
}

void Remove(uint64 jobId) {
	auto &state = Loaded();
	const auto i = ranges::find(state.jobs, jobId, &Job::id);
	if (i != end(state.jobs)) {
		state.jobs.erase(i);
		Save();
		state.changes.fire({});
	}
}

void SendCopies(
		not_null<Main::Session*> session,
		Job job,
		std::vector<TimeId> dates,
		Fn<void()> finished) {
	if (dates.empty()) {
		finished();
		return;
	}
	const auto peer = session->data().peerLoaded(job.peer);
	if (!peer) {
		finished();
		return;
	}
	const auto date = dates.front();
	dates.erase(begin(dates));
	AyuTools::SendText(
		session,
		peer,
		job.topicRootId,
		job.text,
		0,
		Api::SendOptions{ .scheduled = date, .silent = job.silent });
	base::call_delayed(kSendDelay, session, [=] {
		SendCopies(session, job, dates, finished);
	});
}

void TopUpJob(not_null<Main::Session*> session, const Job &job) {
	auto &state = Loaded();
	if (state.busy.contains(job.id)) {
		return;
	}
	const auto peer = session->data().peerLoaded(job.peer);
	if (!peer) {
		return;
	}
	state.busy.emplace(job.id);
	const auto jobId = job.id;
	const auto release = [=] {
		Instance().busy.remove(jobId);
	};
	AyuTools::LoadScheduled(session, peer, [=](
			std::vector<AyuTools::RawMessage> scheduled) {
		auto &state = Loaded();
		const auto i = ranges::find(state.jobs, jobId, &Job::id);
		if (i == end(state.jobs)) {
			release();
			return;
		}
		const auto job = *i;
		const auto now = base::unixtime::now();
		auto copies = 0;
		auto last = TimeId();
		for (const auto &message : scheduled) {
			if (IsCopy(job, message) && message.date > now) {
				++copies;
				last = std::max(last, message.date);
			}
		}
		const auto deleted = job.planned
			? (job.planned > now + 60)
			: ((now - job.created) > kDeletedAfter);
		if (!copies && deleted) {
			// All the scheduled copies were deleted: the user stopped it.
			release();
			Remove(jobId);
			return;
		}
		auto next = copies ? (last + job.period) : job.first;
		while (next <= now + 60) {
			next += job.period;
		}
		const auto capacity = kMaxScheduledPerChat
			- kKeepFree
			- int(scheduled.size());
		auto dates = std::vector<TimeId>();
		while (int(dates.size()) < capacity && next <= now + kMaxAhead) {
			dates.push_back(next);
			next += job.period;
		}
		if (!dates.empty()) {
			i->planned = dates.back();
			Save();
		}
		SendCopies(session, job, std::move(dates), release);
	}, [=](const QString &error) {
		LOG(("AyuRepeat: could not load scheduled messages: %1"
			).arg(error));
		release();
	});
}

void CheckAll() {
	for (const auto &session : AyuTools::Sessions()) {
		TopUp(session);
	}
}

} // namespace

void Init() {
	auto &state = Loaded();
	if (state.timer) {
		return;
	}
	state.timer = std::make_unique<base::Timer>([] { CheckAll(); });
	state.timer->callEach(kCheckInterval);
	base::call_delayed(kFirstCheckDelay, &state.guard, [] {
		CheckAll();
	});
}

bool Intercept(
		not_null<Main::Session*> session,
		Api::MessageToSend &message) {
	auto &options = message.action.options;
	if (!options.scheduled
		|| options.scheduleRepeatPeriod <= 0
		|| (session->user()->isPremium()
			&& options.scheduleRepeatPeriod >= 24 * 60 * 60)
		|| message.textWithTags.text.trimmed().isEmpty()) {
		return false;
	}
	auto &state = Loaded();
	const auto history = message.action.history;
	auto job = Job{
		.id = base::RandomValue<uint64>() | 1,
		.account = session->uniqueId(),
		.peer = history->peer->id,
		.topicRootId = message.action.replyTo.topicRootId,
		.text = message.textWithTags,
		.period = options.scheduleRepeatPeriod,
		.first = options.scheduled,
		.created = base::unixtime::now(),
		.silent = options.silent,
	};
	options.scheduleRepeatPeriod = 0;
	state.jobs.push_back(job);
	Save();
	state.changes.fire({});

	const auto jobId = job.id;
	base::call_delayed(kAfterRegisterDelay, session, [=] {
		const auto &state = Loaded();
		const auto i = ranges::find(state.jobs, jobId, &Job::id);
		if (i != end(state.jobs)) {
			TopUpJob(session, *i);
		}
	});
	return true;
}

std::vector<Job> Jobs() {
	return Loaded().jobs;
}

rpl::producer<> Changes() {
	return Instance().changes.events();
}

void Stop(uint64 jobId, bool deleteScheduled) {
	auto &state = Loaded();
	const auto i = ranges::find(state.jobs, jobId, &Job::id);
	if (i == end(state.jobs)) {
		return;
	}
	const auto job = *i;
	Remove(jobId);
	if (!deleteScheduled) {
		return;
	}
	const auto session = AyuTools::FindSession(job.account);
	const auto peer = session
		? session->data().peerLoaded(job.peer)
		: nullptr;
	if (!peer) {
		return;
	}
	AyuTools::LoadScheduled(session, peer, [=](
			std::vector<AyuTools::RawMessage> scheduled) {
		auto ids = QVector<MTPint>();
		for (const auto &message : scheduled) {
			if (IsCopy(job, message)) {
				ids.push_back(MTP_int(message.id.bare));
			}
		}
		if (ids.isEmpty()) {
			return;
		}
		session->api().request(MTPmessages_DeleteScheduledMessages(
			peer->input(),
			MTP_vector<MTPint>(ids)
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
		}).send();
	});
}

void TopUp(not_null<Main::Session*> session) {
	const auto key = session->uniqueId();
	for (const auto &job : Loaded().jobs) {
		if (job.account == key) {
			TopUpJob(session, job);
		}
	}
}

QString PeriodText(TimeId period) {
	const auto day = TimeId(24 * 60 * 60);
	if (period < 60 * 60) {
		return AyuAccountGroups::Tr("Every %1 min", "هر %1 دقیقه")
			.arg(std::max(period / 60, 1));
	} else if (period < day) {
		return AyuAccountGroups::Tr("Every %1 h", "هر %1 ساعت")
			.arg(period / 3600);
	} else if (period <= day) {
		return tr::lng_schedule_repeat_daily(tr::now);
	} else if (period <= 7 * day) {
		return tr::lng_schedule_repeat_weekly(tr::now);
	} else if (period <= 14 * day) {
		return tr::lng_schedule_repeat_biweekly(tr::now);
	} else if (period <= 30 * day) {
		return tr::lng_schedule_repeat_monthly(tr::now);
	} else if (period <= 91 * day) {
		return tr::lng_schedule_repeat_every_month(tr::now, lt_count, 3);
	} else if (period <= 182 * day) {
		return tr::lng_schedule_repeat_every_month(tr::now, lt_count, 6);
	}
	return tr::lng_schedule_repeat_yearly(tr::now);
}

} // namespace AyuRepeat
