// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/ai/ai_conversation.h"

#include "ayu/features/account_groups/account_groups.h"
#include "ayu/features/ai/ai_core.h"
#include "ayu/features/ayu_tools/ayu_tools.h"
#include "ayu/features/ayu_tools/history_loader.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "main/main_session.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace AyuConversation {
namespace {

using AyuAccountGroups::Tr;

constexpr auto kTick = crl::time(5000);
constexpr auto kContext = 30;
const auto kFile = u"ayu_ai_conversations.json"_q;

struct State {
	bool loaded = false;
	std::vector<Conversation> list;
	base::flat_set<uint64> busy;
	base::flat_map<uint64, QString> status;
	rpl::event_stream<> changes;
	std::unique_ptr<base::Timer> timer;
};

[[nodiscard]] State &Instance() {
	static auto result = State();
	return result;
}

void Write() {
	auto list = QJsonArray();
	for (const auto &c : Instance().list) {
		auto participants = QJsonArray();
		for (const auto &p : c.participants) {
			auto object = QJsonObject();
			object.insert(u"account"_q, QString::number(p.account));
			object.insert(u"provider"_q, QString::number(p.providerId));
			object.insert(u"persona"_q, p.persona);
			participants.append(object);
		}
		auto object = QJsonObject();
		object.insert(u"id"_q, QString::number(c.id));
		object.insert(u"enabled"_q, c.enabled);
		object.insert(u"title"_q, c.title);
		object.insert(u"peer"_q, QString::number(c.peer.value));
		object.insert(u"topicRoot"_q, QString::number(c.topicRootId.bare));
		object.insert(u"topic"_q, c.topic);
		object.insert(u"participants"_q, participants);
		object.insert(u"minGap"_q, c.minGap);
		object.insert(u"maxGap"_q, c.maxGap);
		object.insert(u"maxMessages"_q, c.maxMessages);
		object.insert(u"showService"_q, c.showService);
		object.insert(u"sent"_q, c.sent);
		object.insert(u"lastSpeaker"_q, c.lastSpeaker);
		list.append(object);
	}
	auto object = QJsonObject();
	object.insert(u"conversations"_q, list);
	AyuTools::SaveJson(kFile, object);
}

[[nodiscard]] std::vector<Conversation> &Loaded() {
	auto &state = Instance();
	if (!state.loaded) {
		state.loaded = true;
		const auto object = AyuTools::LoadJson(kFile);
		for (const auto &value : object.value(u"conversations"_q).toArray()) {
			const auto o = value.toObject();
			auto c = Conversation{
				.id = o.value(u"id"_q).toString().toULongLong(),
				.enabled = o.value(u"enabled"_q).toBool(),
				.title = o.value(u"title"_q).toString(),
				.peer = PeerId(o.value(u"peer"_q).toString().toULongLong()),
				.topicRootId = MsgId(
					o.value(u"topicRoot"_q).toString().toLongLong()),
				.topic = o.value(u"topic"_q).toString(),
				.minGap = o.value(u"minGap"_q).toInt(60),
				.maxGap = o.value(u"maxGap"_q).toInt(180),
				.maxMessages = o.value(u"maxMessages"_q).toInt(30),
				.showService = o.value(u"showService"_q).toBool(),
				.sent = o.value(u"sent"_q).toInt(),
				.lastSpeaker = o.value(u"lastSpeaker"_q).toInt(-1),
			};
			for (const auto &p : o.value(u"participants"_q).toArray()) {
				const auto po = p.toObject();
				c.participants.push_back({
					.account = po.value(u"account"_q).toString().toULongLong(),
					.providerId = po.value(u"provider"_q).toString().toULongLong(),
					.persona = po.value(u"persona"_q).toString(),
				});
			}
			if (c.id && c.peer) {
				state.list.push_back(std::move(c));
			}
		}
	}
	return state.list;
}

[[nodiscard]] Conversation *FindPtr(uint64 id) {
	auto &list = Loaded();
	const auto i = ranges::find(list, id, &Conversation::id);
	return (i != end(list)) ? &*i : nullptr;
}

void Notify() {
	Write();
	Instance().changes.fire({});
}

void SetStatus(uint64 id, const QString &text) {
	Instance().status[id] = text;
	Instance().changes.fire({});
}

[[nodiscard]] int RandomBetween(int from, int till) {
	if (till <= from) {
		return from;
	}
	return from + int(base::RandomValue<uint32>() % uint32(till - from + 1));
}

void Delay(uint64 id, int seconds) {
	if (const auto c = FindPtr(id)) {
		c->nextAt = base::unixtime::now() + seconds;
	}
}

[[nodiscard]] int PickSpeaker(const Conversation &c) {
	const auto count = int(c.participants.size());
	if (count <= 1) {
		return 0;
	}
	auto index = RandomBetween(0, count - 2);
	if (c.lastSpeaker >= 0 && index >= c.lastSpeaker) {
		++index;
	}
	return std::clamp(index, 0, count - 1);
}

void Step(Conversation conversation) {
	const auto id = conversation.id;
	auto &state = Instance();
	if (conversation.participants.empty()) {
		SetStatus(id, Tr("Add participants.", "شرکت‌کننده اضافه کن."));
		Delay(id, 60);
		return;
	}
	const auto index = PickSpeaker(conversation);
	const auto participant = conversation.participants[index];
	const auto session = AyuTools::FindSession(participant.account);
	if (!session) {
		SetStatus(id, Tr(
			"An account of this conversation is not logged in.",
			"یکی از اکانت‌های این گفتگو وارد نیست."));
		if (const auto c = FindPtr(id)) {
			c->lastSpeaker = index;
		}
		Delay(id, 30);
		return;
	}
	const auto peer = session->data().peerLoaded(conversation.peer);
	if (!peer) {
		SetStatus(id, Tr(
			"An account is not in this chat: ",
			"یه اکانت توی این چت نیست: ")
			+ session->user()->name());
		if (const auto c = FindPtr(id)) {
			c->lastSpeaker = index;
		}
		Delay(id, 60);
		return;
	}
	state.busy.emplace(id);
	const auto release = [=] {
		Instance().busy.remove(id);
	};
	const auto name = session->user()->name();
	// Who takes part: only their messages are read and answered.
	struct Member {
		PeerId peer = 0;
		QString name;
		QString persona;
	};
	auto members = std::vector<Member>();
	for (const auto &p : conversation.participants) {
		if (const auto s = AyuTools::FindSession(p.account)) {
			members.push_back({
				.peer = s->userPeerId(),
				.name = s->user()->name(),
				.persona = p.persona.trimmed(),
			});
		}
	}
	const auto me = session->userPeerId();
	AyuTools::LoadRecent(session, peer, 60, [=](
			std::vector<AyuTools::RawMessage> all) {
		auto list = std::vector<AyuTools::RawMessage>();
		for (const auto &message : all) {
			if (!message.service
				&& ranges::contains(members, message.from, &Member::peer)) {
				list.push_back(message);
			}
		}
		if (int(list.size()) > kContext) {
			list.erase(list.begin(), list.end() - kContext);
		}
		auto others = QStringList();
		for (const auto &member : members) {
			if (member.peer != me) {
				others.push_back(u"- "_q
					+ member.name
					+ (member.persona.isEmpty()
						? QString()
						: (u": "_q + member.persona)));
			}
		}
		auto system = AyuAi::BaseSystemPrompt();
		system += u"\n\nYou take part in a planned discussion in a Telegram "
			"group as the member \""_q + name + u"\".\n"_q
			+ u"Your role, opinion and side: "_q
			+ (participant.persona.trimmed().isEmpty()
				? u"be yourself, a friendly member."_q
				: participant.persona.trimmed())
			+ u"\nThe topic and rules of the discussion: "_q
			+ (conversation.topic.trimmed().isEmpty()
				? u"a natural friendly chat."_q
				: conversation.topic.trimmed())
			+ u"\nThe other members of this discussion:\n"_q
			+ (others.isEmpty() ? u"(nobody yet)"_q : others.join('\n'))
			+ u"\nTalk ONLY with these members and ONLY about the topic. "
			"Other people in the group are not part of it: never answer "
			"them and never comment on their messages. Write ONE short "
			"natural chat message that continues the discussion: answer the "
			"last point of the other members, agree or disagree as your role "
			"says, bring a new argument, don't repeat earlier messages. If "
			"nobody said anything yet, start the topic. Use the language of "
			"the topic. Write only the message text, without your name."_q;
		const auto context = AyuTools::FormatForPrompt(session, list, 8000);
		const auto text = u"The discussion so far (old to new, your own "
			"messages are signed \""_q + name + u"\"):\n"_q
			+ (context.trimmed().isEmpty() ? u"(nothing yet)"_q : context)
			+ u"\n\nWrite your next message."_q;
		auto replyTo = MsgId();
		for (auto i = list.rbegin(); i != list.rend(); ++i) {
			if (i->from != me) {
				if (base::RandomValue<uint32>() % 3) {
					replyTo = i->id;
				}
				break;
			}
		}
		AyuAi::Complete(
			AyuAi::Request{
				.providerId = participant.providerId,
				.system = system,
				.messages = { { .role = u"user"_q, .text = text } },
				.temperature = 0.9,
				.maxTokens = 400,
			},
			[=](QString answer) {
				release();
				const auto c = FindPtr(id);
				if (!c || !c->enabled) {
					return;
				}
				answer = answer.trimmed();
				if (answer.startsWith(name + ':')) {
					answer = answer.mid(name.size() + 1).trimmed();
				}
				if (answer.isEmpty()) {
					Delay(id, 30);
					return;
				}
				if (c->showService) {
					const auto provider = AyuAi::FindProvider(
						participant.providerId);
					if (provider) {
						answer = u"["_q + provider->name + u"] "_q + answer;
					}
				}
				if (const auto strong = AyuTools::FindSession(participant.account)) {
					if (const auto peer = strong->data().peerLoaded(c->peer)) {
						AyuTools::SendText(
							strong,
							peer,
							c->topicRootId,
							TextWithTags{ answer },
							replyTo);
					}
				}
				++c->sent;
				c->lastSpeaker = index;
				c->nextAt = base::unixtime::now()
					+ RandomBetween(c->minGap, c->maxGap);
				if (c->maxMessages > 0 && c->sent >= c->maxMessages) {
					c->enabled = false;
					SetStatus(id, Tr("Finished.", "تموم شد."));
				} else {
					SetStatus(id, Tr("Last message by ", "آخرین پیام از ")
						+ name);
				}
				Notify();
			},
			[=](QString error) {
				release();
				SetStatus(id, name + u": "_q + error);
				Delay(id, 90);
			});
	}, [=](QString error) {
		release();
		SetStatus(id, error);
		Delay(id, 60);
	});
}

void Tick() {
	const auto now = base::unixtime::now();
	const auto list = Loaded();
	for (const auto &c : list) {
		if (!c.enabled
			|| Instance().busy.contains(c.id)
			|| c.nextAt > now) {
			continue;
		}
		Step(c);
	}
}

} // namespace

std::vector<Conversation> List() {
	return Loaded();
}

std::optional<Conversation> Find(uint64 id) {
	if (const auto c = FindPtr(id)) {
		return *c;
	}
	return std::nullopt;
}

uint64 Save(Conversation conversation) {
	auto &list = Loaded();
	if (!conversation.id) {
		conversation.id = base::RandomValue<uint64>() | 1;
	}
	conversation.minGap = std::max(conversation.minGap, 10);
	conversation.maxGap = std::max(conversation.maxGap, conversation.minGap);
	const auto i = ranges::find(list, conversation.id, &Conversation::id);
	if (i != end(list)) {
		*i = conversation;
	} else {
		list.push_back(conversation);
	}
	Notify();
	return conversation.id;
}

void Remove(uint64 id) {
	auto &list = Loaded();
	list.erase(ranges::remove(list, id, &Conversation::id), end(list));
	Instance().status.remove(id);
	Notify();
}

void StopAll() {
	auto changed = false;
	for (auto &c : Loaded()) {
		if (c.enabled) {
			c.enabled = false;
			changed = true;
			Instance().status[c.id] = Tr("Stopped.", "متوقف شد.");
		}
	}
	if (changed) {
		Notify();
	}
}

void Restart(uint64 id) {
	if (const auto c = FindPtr(id)) {
		c->enabled = true;
		c->sent = 0;
		c->nextAt = base::unixtime::now();
		SetStatus(id, Tr("Starting…", "در حال شروع…"));
		Notify();
	}
}

rpl::producer<> Changes() {
	return Instance().changes.events();
}

QString Status(uint64 id) {
	const auto i = Instance().status.find(id);
	return (i != end(Instance().status)) ? i->second : QString();
}

QString Summary(const Conversation &c) {
	auto result = c.title.trimmed().isEmpty()
		? Tr("Conversation", "گفتگو")
		: c.title.trimmed();
	result += u" • "_q
		+ QString::number(c.participants.size())
		+ Tr(" accounts", " اکانت")
		+ u" • "_q
		+ QString::number(c.sent)
		+ (c.maxMessages > 0
			? (u"/"_q + QString::number(c.maxMessages))
			: QString())
		+ Tr(" messages", " پیام");
	if (!c.enabled) {
		result += u" • "_q + Tr("off", "خاموش");
	}
	return result;
}

void Init() {
	auto &state = Instance();
	if (state.timer) {
		return;
	}
	// Conversations never continue by themselves after a restart.
	auto changed = false;
	for (auto &c : Loaded()) {
		if (c.enabled) {
			c.enabled = false;
			changed = true;
			state.status[c.id] = Tr(
				"Stopped because AyuGram was closed. Press \"Start now\" to continue.",
				"چون ایوگرام بسته شد متوقف شد. برای ادامه «همین الان شروع کن» رو بزن.");
		}
	}
	if (changed) {
		Write();
	}
	state.timer = std::make_unique<base::Timer>([] { Tick(); });
	state.timer->callEach(kTick);
}

} // namespace AyuConversation
