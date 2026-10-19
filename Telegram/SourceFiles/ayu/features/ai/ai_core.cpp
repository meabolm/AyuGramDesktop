// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/ai/ai_core.h"

#include "apiwrap.h"
#include "ayu/features/account_groups/account_groups.h"
#include "ayu/features/ayu_tools/ayu_tools.h"
#include "ayu/features/ayu_tools/history_loader.h"
#include "base/call_delayed.h"
#include "base/random.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "mtproto/mtproto_proxy_data.h"

#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkProxyFactory>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace AyuAi {
namespace {

using AyuAccountGroups::Tr;

constexpr auto kFileName = "ayu_ai.json";
constexpr auto kTickInterval = crl::time(30 * 1000);
constexpr auto kCheckInterval = TimeId(120);
constexpr auto kRequestTimeout = 120 * 1000;
constexpr auto kFreshMessage = TimeId(180);
constexpr auto kHotWindow = TimeId(24 * 60 * 60);
constexpr auto kMaxReactionsPerCheck = 2;

struct Runtime {
	std::deque<TimeId> actions;
	base::flat_set<MsgId> handled;
	TimeId lastCheck = 0;
	bool busy = false;
	QString status;
};

struct State {
	bool loaded = false;
	std::vector<Provider> providers;
	uint64 defaultProvider = 0;
	std::vector<Task> tasks;
	base::flat_map<uint64, Runtime> runtime;
	base::flat_set<not_null<Main::Session*>> watched;
	rpl::event_stream<> changes;
	std::unique_ptr<base::Timer> timer;
	rpl::lifetime lifetime;
};

[[nodiscard]] State &Instance() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString KindKey(ProviderKind kind) {
	switch (kind) {
	case ProviderKind::DeepSeek: return u"deepseek"_q;
	case ProviderKind::OpenAI: return u"openai"_q;
	case ProviderKind::Gemini: return u"gemini"_q;
	case ProviderKind::Claude: return u"claude"_q;
	case ProviderKind::Grok: return u"grok"_q;
	case ProviderKind::Groq: return u"groq"_q;
	case ProviderKind::OpenRouter: return u"openrouter"_q;
	case ProviderKind::Mistral: return u"mistral"_q;
	case ProviderKind::Qwen: return u"qwen"_q;
	case ProviderKind::Kimi: return u"kimi"_q;
	case ProviderKind::Ollama: return u"ollama"_q;
	case ProviderKind::Custom: break;
	}
	return u"custom"_q;
}

[[nodiscard]] ProviderKind KindFromKey(const QString &key) {
	for (const auto &preset : Presets()) {
		if (KindKey(preset.kind) == key) {
			return preset.kind;
		}
	}
	return ProviderKind::Custom;
}

void Save() {
	const auto &state = Instance();
	auto providers = QJsonArray();
	for (const auto &provider : state.providers) {
		auto object = QJsonObject();
		object.insert(u"id"_q, QString::number(provider.id));
		object.insert(u"kind"_q, KindKey(provider.kind));
		object.insert(u"name"_q, provider.name);
		object.insert(u"baseUrl"_q, provider.baseUrl);
		object.insert(u"apiKey"_q, provider.apiKey);
		object.insert(u"model"_q, provider.model);
		object.insert(u"vision"_q, provider.vision);
		providers.append(object);
	}
	auto tasks = QJsonArray();
	for (const auto &task : state.tasks) {
		auto object = QJsonObject();
		object.insert(u"id"_q, QString::number(task.id));
		object.insert(u"enabled"_q, task.enabled);
		object.insert(u"account"_q, QString::number(task.account));
		object.insert(u"peer"_q, QString::number(task.peer.value));
		object.insert(u"topic"_q, QString::number(task.topicRootId.bare));
		object.insert(u"kind"_q, int(task.kind));
		object.insert(u"provider"_q, QString::number(task.providerId));
		object.insert(u"title"_q, task.title);
		object.insert(u"instructions"_q, task.instructions);
		object.insert(u"contextCount"_q, task.contextCount);
		object.insert(u"maxPerHour"_q, task.maxPerHour);
		object.insert(u"onlyWhenAddressed"_q, task.onlyWhenAddressed);
		object.insert(u"minReplies"_q, task.minReplies);
		object.insert(u"minReactions"_q, task.minReactions);
		object.insert(u"timeOfDay"_q, task.timeOfDay);
		object.insert(u"daily"_q, task.daily);
		object.insert(u"runAt"_q, task.runAt);
		object.insert(u"toSavedMessages"_q, task.toSavedMessages);
		object.insert(u"lastRun"_q, task.lastRun);
		tasks.append(object);
	}
	auto root = QJsonObject();
	root.insert(u"version"_q, 1);
	root.insert(u"defaultProvider"_q, QString::number(state.defaultProvider));
	root.insert(u"providers"_q, providers);
	root.insert(u"tasks"_q, tasks);
	AyuTools::SaveJson(QString::fromLatin1(kFileName), root);
}

State &Loaded() {
	auto &state = Instance();
	if (state.loaded) {
		return state;
	}
	state.loaded = true;
	const auto root = AyuTools::LoadJson(QString::fromLatin1(kFileName));
	for (const auto &value : root.value(u"providers"_q).toArray()) {
		const auto o = value.toObject();
		auto provider = Provider{
			.id = o.value(u"id"_q).toString().toULongLong(),
			.kind = KindFromKey(o.value(u"kind"_q).toString()),
			.name = o.value(u"name"_q).toString(),
			.baseUrl = o.value(u"baseUrl"_q).toString(),
			.apiKey = o.value(u"apiKey"_q).toString(),
			.model = o.value(u"model"_q).toString(),
			.vision = o.value(u"vision"_q).toBool(),
		};
		if (provider.id) {
			state.providers.push_back(std::move(provider));
		}
	}
	state.defaultProvider = root.value(
		u"defaultProvider"_q).toString().toULongLong();
	for (const auto &value : root.value(u"tasks"_q).toArray()) {
		const auto o = value.toObject();
		auto task = Task{
			.id = o.value(u"id"_q).toString().toULongLong(),
			.enabled = o.value(u"enabled"_q).toBool(true),
			.account = o.value(u"account"_q).toString().toULongLong(),
			.peer = PeerId(o.value(u"peer"_q).toString().toULongLong()),
			.topicRootId = MsgId(o.value(u"topic"_q).toString().toLongLong()),
			.kind = TaskKind(std::clamp(o.value(u"kind"_q).toInt(), 0, 3)),
			.providerId = o.value(u"provider"_q).toString().toULongLong(),
			.title = o.value(u"title"_q).toString(),
			.instructions = o.value(u"instructions"_q).toString(),
			.contextCount = o.value(u"contextCount"_q).toInt(30),
			.maxPerHour = o.value(u"maxPerHour"_q).toInt(6),
			.onlyWhenAddressed = o.value(u"onlyWhenAddressed"_q).toBool(),
			.minReplies = o.value(u"minReplies"_q).toInt(5),
			.minReactions = o.value(u"minReactions"_q).toInt(5),
			.timeOfDay = o.value(u"timeOfDay"_q).toInt(9 * 60),
			.daily = o.value(u"daily"_q).toBool(true),
			.runAt = TimeId(o.value(u"runAt"_q).toInt()),
			.toSavedMessages = o.value(u"toSavedMessages"_q).toBool(),
			.lastRun = TimeId(o.value(u"lastRun"_q).toInt()),
		};
		if (task.id && task.account && task.peer) {
			state.tasks.push_back(std::move(task));
		}
	}
	return state;
}

void Notify() {
	Save();
	Instance().changes.fire({});
}

[[nodiscard]] Task *TaskPtr(uint64 id) {
	auto &state = Loaded();
	const auto i = ranges::find(state.tasks, id, &Task::id);
	return (i != end(state.tasks)) ? &*i : nullptr;
}

void SetStatus(uint64 taskId, const QString &status) {
	auto &state = Loaded();
	state.runtime[taskId].status = QLocale().toString(
		QTime::currentTime(),
		QLocale::ShortFormat) + u" — "_q + status;
	state.changes.fire({});
}

[[nodiscard]] bool CanAct(const Task &task) {
	auto &runtime = Loaded().runtime[task.id];
	const auto now = base::unixtime::now();
	while (!runtime.actions.empty()
		&& (now - runtime.actions.front()) > 3600) {
		runtime.actions.pop_front();
	}
	const auto limit = (task.maxPerHour > 0)
		? std::min(task.maxPerHour, 120)
		: 120;
	return int(runtime.actions.size()) < limit;
}

void Acted(uint64 taskId) {
	Loaded().runtime[taskId].actions.push_back(base::unixtime::now());
}

// Network.

class ProxyFactory final : public QNetworkProxyFactory {
public:
	QList<QNetworkProxy> queryProxy(
			const QNetworkProxyQuery &query) override {
		const auto host = query.url().host();
		if (host == u"localhost"_q || host == u"127.0.0.1"_q) {
			return { QNetworkProxy(QNetworkProxy::NoProxy) };
		}
		if (Core::IsAppLaunched()) {
			const auto &proxy = Core::App().settings().proxy();
			if (proxy.isEnabled()) {
				const auto selected = proxy.selected();
				using Type = MTP::ProxyData::Type;
				if (selected.type == Type::Socks5
					|| selected.type == Type::Http) {
					return { MTP::ToNetworkProxy(selected) };
				}
			}
		}
		return QNetworkProxyFactory::systemProxyForQuery(query);
	}
};

[[nodiscard]] QNetworkAccessManager &Network() {
	static const auto result = [] {
		const auto manager = new QNetworkAccessManager();
		manager->setProxyFactory(new ProxyFactory());
		return manager;
	}();
	return *result;
}

[[nodiscard]] QString ParseError(int status, const QByteArray &body) {
	const auto document = QJsonDocument::fromJson(body);
	auto message = QString();
	if (document.isObject()) {
		const auto error = document.object().value(u"error"_q);
		if (error.isObject()) {
			message = error.toObject().value(u"message"_q).toString();
		} else if (error.isString()) {
			message = error.toString();
		}
		if (message.isEmpty()) {
			message = document.object().value(u"message"_q).toString();
		}
	} else if (document.isArray() && !document.array().isEmpty()) {
		const auto first = document.array().first().toObject();
		message = first.value(u"error"_q).toObject().value(
			u"message"_q).toString();
	}
	if (message.isEmpty()) {
		message = QString::fromUtf8(body.left(300));
	}
	if (status == 401 || status == 403) {
		message = Tr(
			"The API key is wrong. Copy the full key right when you create "
			"it on the service site (later it is shown hidden with ***), "
			"then paste it here again.\n",
			"کلید API اشتباهه. کلید کامل فقط همون لحظه ساختنش توی سایت "
			"نشون داده میشه (بعدش با *** مخفی میشه). یه کلید جدید بساز، "
			"همون لحظه کپیش کن و اینجا بذار.\n")
			+ message;
	} else if (status == 402) {
		message = Tr("Not enough credit. ", "اعتبار حساب کافی نیست. ")
			+ message;
	} else if (status == 429) {
		message = Tr("Too many requests or quota is over. ", "درخواست زیاد است یا سهمیه تمام شده. ")
			+ message;
	}
	return status
		? (u"HTTP "_q + QString::number(status) + u": "_q + message)
		: message;
}

[[nodiscard]] QString ParseContent(const QByteArray &body) {
	const auto document = QJsonDocument::fromJson(body);
	const auto choices = document.object().value(u"choices"_q).toArray();
	if (choices.isEmpty()) {
		return QString();
	}
	const auto message = choices.first().toObject().value(
		u"message"_q).toObject();
	const auto content = message.value(u"content"_q);
	if (content.isString()) {
		return content.toString();
	}
	auto result = QString();
	for (const auto &part : content.toArray()) {
		result += part.toObject().value(u"text"_q).toString();
	}
	return result;
}

[[nodiscard]] QString CleanAnswer(QString text) {
	// Reasoning models may include their thoughts.
	const auto think = text.indexOf(u"</think>"_q);
	if (think >= 0) {
		text = text.mid(think + 8);
	}
	return text.trimmed();
}

[[nodiscard]] bool IsSkip(const QString &text) {
	const auto simple = text.trimmed().toUpper();
	return simple.isEmpty()
		|| simple == u"SKIP"_q
		|| simple.startsWith(u"SKIP."_q)
		|| simple.startsWith(u"[SKIP]"_q);
}

// Engine.

[[nodiscard]] QString ChatName(not_null<PeerData*> peer) {
	return peer->name();
}

[[nodiscard]] QString TaskSystemPrompt(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const Task &task) {
	auto result = BaseSystemPrompt();
	result += u"\n\nYou act on behalf of the Telegram user \""_q
		+ session->user()->name()
		+ u"\" in the chat \""_q
		+ ChatName(peer)
		+ u"\". Write like a real person in a chat: natural, short, "
			"no markdown headings, in the language of the conversation "
			"unless told otherwise. Do not say that you are an AI unless "
			"the instructions allow it."_q;
	if (!task.instructions.trimmed().isEmpty()) {
		result += u"\n\nInstructions from the account owner:\n"_q
			+ task.instructions.trimmed();
	}
	return result;
}

void SendReply(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		const Task &task,
		MsgId replyTo,
		const QString &text) {
	AyuTools::SendText(
		session,
		peer,
		task.topicRootId,
		TextWithTags{ text },
		replyTo);
}

void FinishBusy(uint64 taskId) {
	Loaded().runtime[taskId].busy = false;
}

void MarkRun(uint64 taskId) {
	if (const auto task = TaskPtr(taskId)) {
		task->lastRun = base::unixtime::now();
		Save();
	}
}

void RunAutoReply(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		Task task,
		AyuTools::RawMessage target,
		Fn<void(QString)> report) {
	const auto taskId = task.id;
	Loaded().runtime[taskId].busy = true;
	const auto count = std::clamp(task.contextCount, 5, 100);
	AyuTools::LoadRecent(session, peer, count, [=](
			std::vector<AyuTools::RawMessage> messages) {
		const auto context = AyuTools::FormatForPrompt(session, messages);
		auto request = Request{
			.providerId = task.providerId,
			.system = TaskSystemPrompt(session, peer, task),
		};
		request.messages.push_back({
			.role = u"user"_q,
			.text = u"Recent messages of the chat (oldest first):\n"_q
				+ context
				+ u"\n\nThe new message to consider:\n[#"_q
				+ QString::number(target.id.bare)
				+ u"] "_q
				+ AyuTools::PeerName(session, target.from)
				+ u": "_q
				+ AyuTools::ShortText(target, 2000)
				+ u"\n\nFollowing the instructions, write only the text of "
					"your reply to this new message. If according to the "
					"instructions you should not reply, answer exactly: SKIP"_q,
		});
		Complete(std::move(request), [=](QString answer) {
			FinishBusy(taskId);
			answer = CleanAnswer(answer);
			if (IsSkip(answer)) {
				SetStatus(taskId, Tr("Skipped a message.", "از یک پیام رد شد."));
				if (report) {
					report(Tr("The AI decided not to reply.", "هوش مصنوعی تصمیم گرفت جواب ندهد."));
				}
				return;
			}
			SendReply(session, peer, task, target.id, answer);
			Acted(taskId);
			MarkRun(taskId);
			SetStatus(taskId, Tr("Replied.", "جواب داد."));
			if (report) {
				report(Tr("Reply sent.", "جواب ارسال شد."));
			}
		}, [=](QString error) {
			FinishBusy(taskId);
			SetStatus(taskId, Tr("Error: ", "خطا: ") + error);
			if (report) {
				report(Tr("Error: ", "خطا: ") + error);
			}
		});
	}, [=](QString error) {
		FinishBusy(taskId);
		SetStatus(taskId, Tr("Error: ", "خطا: ") + error);
		if (report) {
			report(Tr("Error: ", "خطا: ") + error);
		}
	});
}

void RunHotReply(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		Task task,
		Fn<void(QString)> report) {
	const auto taskId = task.id;
	auto &runtime = Loaded().runtime[taskId];
	runtime.busy = true;
	runtime.lastCheck = base::unixtime::now();
	AyuTools::LoadRecent(session, peer, 100, [=](
			std::vector<AyuTools::RawMessage> messages) {
		const auto now = base::unixtime::now();
		auto counts = base::flat_map<MsgId, int>();
		auto answered = base::flat_set<MsgId>();
		for (const auto &message : messages) {
			if (message.replyTo) {
				++counts[message.replyTo];
				if (message.out) {
					answered.emplace(message.replyTo);
				}
			}
		}
		auto &runtime = Loaded().runtime[taskId];
		const AyuTools::RawMessage *best = nullptr;
		auto bestCount = 0;
		for (const auto &message : messages) {
			const auto i = counts.find(message.id);
			const auto count = (i != end(counts)) ? i->second : 0;
			if (message.out
				|| message.service
				|| count < std::max(task.minReplies, 1)
				|| (now - message.date) > kHotWindow
				|| answered.contains(message.id)
				|| runtime.handled.contains(message.id)) {
				continue;
			}
			if (count > bestCount) {
				best = &message;
				bestCount = count;
			}
		}
		if (!best) {
			runtime.busy = false;
			if (report) {
				report(Tr("No message has enough replies yet.", "فعلاً پیامی با ریپلی کافی نیست."));
			}
			return;
		}
		const auto target = *best;
		runtime.handled.emplace(target.id);
		auto replies = std::vector<AyuTools::RawMessage>();
		for (const auto &message : messages) {
			if (message.replyTo == target.id) {
				replies.push_back(message);
			}
		}
		auto request = Request{
			.providerId = task.providerId,
			.system = TaskSystemPrompt(session, peer, task),
		};
		request.messages.push_back({
			.role = u"user"_q,
			.text = u"This message got "_q
				+ QString::number(bestCount)
				+ u" replies in the chat:\n"_q
				+ AyuTools::PeerName(session, target.from)
				+ u": "_q
				+ AyuTools::ShortText(target, 2000)
				+ u"\n\nThe replies:\n"_q
				+ AyuTools::FormatForPrompt(session, replies, 6000)
				+ u"\n\nWrite only the text of your own reply to that "
					"message, following the instructions. If you should "
					"not reply, answer exactly: SKIP"_q,
		});
		Complete(std::move(request), [=](QString answer) {
			FinishBusy(taskId);
			answer = CleanAnswer(answer);
			if (IsSkip(answer)) {
				SetStatus(taskId, Tr("Skipped a hot message.", "از یک پیام داغ رد شد."));
				if (report) {
					report(Tr("The AI decided not to reply.", "هوش مصنوعی تصمیم گرفت جواب ندهد."));
				}
				return;
			}
			SendReply(session, peer, task, target.id, answer);
			Acted(taskId);
			MarkRun(taskId);
			SetStatus(taskId, Tr("Replied to a hot message.", "روی پیام داغ ریپلای زد."));
			if (report) {
				report(Tr("Reply sent.", "ریپلای ارسال شد."));
			}
		}, [=](QString error) {
			FinishBusy(taskId);
			SetStatus(taskId, Tr("Error: ", "خطا: ") + error);
			if (report) {
				report(Tr("Error: ", "خطا: ") + error);
			}
		});
	}, [=](QString error) {
		FinishBusy(taskId);
		if (report) {
			report(Tr("Error: ", "خطا: ") + error);
		}
	});
}

void RunCopyReaction(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		Task task,
		Fn<void(QString)> report) {
	const auto taskId = task.id;
	auto &runtime = Loaded().runtime[taskId];
	runtime.busy = true;
	runtime.lastCheck = base::unixtime::now();
	AyuTools::LoadRecent(session, peer, 60, [=](
			std::vector<AyuTools::RawMessage> messages) {
		const auto now = base::unixtime::now();
		const auto premium = session->user()->isPremium();
		auto sent = 0;
		for (auto i = messages.rbegin(); i != messages.rend(); ++i) {
			const auto &message = *i;
			auto &runtime = Loaded().runtime[taskId];
			if (sent >= kMaxReactionsPerCheck) {
				break;
			} else if (message.out
				|| (now - message.date) > kHotWindow
				|| runtime.handled.contains(message.id)
				|| message.reactionsTotal() < std::max(task.minReactions, 1)) {
				continue;
			}
			const auto chosen = ranges::any_of(
				message.reactions,
				&AyuTools::ReactionCount::chosen);
			if (chosen) {
				runtime.handled.emplace(message.id);
				continue;
			}
			const AyuTools::ReactionCount *top = nullptr;
			for (const auto &reaction : message.reactions) {
				if (reaction.customId && !premium) {
					continue;
				}
				if (!top || reaction.count > top->count) {
					top = &reaction;
				}
			}
			if (!top) {
				continue;
			}
			const auto current = TaskPtr(taskId);
			if (!current || !CanAct(*current)) {
				break;
			}
			runtime.handled.emplace(message.id);
			const auto reaction = top->customId
				? MTP_reactionCustomEmoji(MTP_long(top->customId))
				: MTP_reactionEmoji(MTP_string(top->emoji));
			using Flag = MTPmessages_SendReaction::Flag;
			session->api().request(MTPmessages_SendReaction(
				MTP_flags(Flag::f_reaction),
				peer->input(),
				MTP_int(message.id.bare),
				MTP_vector<MTPReaction>(1, reaction)
			)).done([=](const MTPUpdates &result) {
				session->api().applyUpdates(result);
			}).send();
			Acted(taskId);
			++sent;
		}
		FinishBusy(taskId);
		if (sent) {
			MarkRun(taskId);
			SetStatus(taskId, Tr("Reacted to %1 message(s).", "روی %1 پیام ری‌اکشن زد.").arg(sent));
		}
		if (report) {
			report(sent
				? Tr("Reactions sent: %1", "ری‌اکشن ارسال شد: %1").arg(sent)
				: Tr("No new popular message right now.", "فعلاً پیام پرری‌اکشن جدیدی نیست."));
		}
	}, [=](QString error) {
		FinishBusy(taskId);
		if (report) {
			report(Tr("Error: ", "خطا: ") + error);
		}
	});
}

void RunPost(
		not_null<Main::Session*> session,
		not_null<PeerData*> peer,
		Task task,
		Fn<void(QString)> report) {
	const auto taskId = task.id;
	Loaded().runtime[taskId].busy = true;
	MarkRun(taskId);
	const auto generate = [=](QString context) {
		auto request = Request{
			.providerId = task.providerId,
			.system = TaskSystemPrompt(session, peer, task),
		};
		auto text = QString();
		if (!context.isEmpty()) {
			text += u"Recent messages of the chat (oldest first):\n"_q
				+ context
				+ u"\n\n"_q;
		}
		text += u"Current date and time: "_q
			+ QLocale::c().toString(
				QDateTime::currentDateTime(),
				u"yyyy-MM-dd HH:mm, dddd"_q)
			+ u"\n\nDo the task from the instructions now. Write only the "
				"text of the message that should be sent."_q;
		request.messages.push_back({ .role = u"user"_q, .text = text });
		Complete(std::move(request), [=](QString answer) {
			FinishBusy(taskId);
			answer = CleanAnswer(answer);
			if (IsSkip(answer)) {
				SetStatus(taskId, Tr("Nothing to send.", "چیزی برای ارسال نبود."));
				if (report) {
					report(Tr("The AI had nothing to send.", "هوش مصنوعی چیزی برای ارسال نداشت."));
				}
				return;
			}
			if (task.toSavedMessages) {
				const auto title = task.title.trimmed().isEmpty()
					? TaskKindName(task.kind)
					: task.title.trimmed();
				AyuTools::SendText(
					session,
					session->user(),
					0,
					TextWithTags{ u"\U0001F916 "_q
						+ title
						+ u" — "_q
						+ ChatName(peer)
						+ u"\n\n"_q
						+ answer });
			} else {
				AyuTools::SendText(
					session,
					peer,
					task.topicRootId,
					TextWithTags{ answer });
			}
			Acted(taskId);
			if (const auto current = TaskPtr(taskId)) {
				if (!current->daily) {
					current->enabled = false;
				}
				Notify();
			}
			SetStatus(taskId, Tr("Done.", "انجام شد."));
			if (report) {
				report(Tr("Done, the message was sent.", "انجام شد و پیام ارسال شد."));
			}
		}, [=](QString error) {
			FinishBusy(taskId);
			SetStatus(taskId, Tr("Error: ", "خطا: ") + error);
			if (report) {
				report(Tr("Error: ", "خطا: ") + error);
			}
		});
	};
	if (task.contextCount > 0) {
		AyuTools::LoadRecent(
			session,
			peer,
			std::clamp(task.contextCount, 1, 100),
			[=](std::vector<AyuTools::RawMessage> messages) {
				generate(AyuTools::FormatForPrompt(session, messages));
			},
			[=](QString) { generate(QString()); });
	} else {
		generate(QString());
	}
}

[[nodiscard]] TimeId TodayAt(int minutes) {
	const auto date = QDate::currentDate();
	const auto time = QTime(
		std::clamp(minutes / 60, 0, 23),
		std::clamp(minutes % 60, 0, 59));
	return TimeId(QDateTime(date, time).toSecsSinceEpoch());
}

[[nodiscard]] bool PostDue(const Task &task, TimeId now) {
	if (task.daily) {
		const auto at = TodayAt(task.timeOfDay);
		return (now >= at) && (task.lastRun < at) && (now - at) < 3 * 3600;
	}
	return task.runAt
		&& (now >= task.runAt)
		&& (task.lastRun < task.runAt)
		&& (now - task.runAt) < 6 * 3600;
}

void Tick() {
	const auto now = base::unixtime::now();
	const auto tasks = Loaded().tasks;
	for (const auto &task : tasks) {
		if (!task.enabled) {
			continue;
		}
		auto &runtime = Loaded().runtime[task.id];
		if (runtime.busy) {
			continue;
		}
		const auto session = AyuTools::FindSession(task.account);
		const auto peer = session
			? session->data().peerLoaded(task.peer)
			: nullptr;
		if (!peer) {
			continue;
		}
		switch (task.kind) {
		case TaskKind::Post:
			if (PostDue(task, now)) {
				RunPost(session, peer, task, nullptr);
			}
			break;
		case TaskKind::HotReply:
			if ((now - runtime.lastCheck) >= kCheckInterval
				&& CanAct(task)) {
				RunHotReply(session, peer, task, nullptr);
			}
			break;
		case TaskKind::CopyReaction:
			if ((now - runtime.lastCheck) >= kCheckInterval
				&& CanAct(task)) {
				RunCopyReaction(session, peer, task, nullptr);
			}
			break;
		case TaskKind::AutoReply:
			break;
		}
	}
}

[[nodiscard]] AyuTools::RawMessage RawFromItem(not_null<HistoryItem*> item) {
	auto result = AyuTools::RawMessage{
		.id = item->id,
		.date = item->date(),
		.from = item->from()->id,
		.text = item->originalText().text,
		.replyTo = item->replyToId(),
		.out = item->out(),
	};
	if (item->media() && result.text.isEmpty()) {
		result.media = AyuTools::MediaKind::Other;
	}
	return result;
}

void HandleNewItem(
		not_null<Main::Session*> session,
		not_null<HistoryItem*> item) {
	if (item->out()
		|| item->isService()
		|| !item->isRegular()
		|| (base::unixtime::now() - item->date()) > kFreshMessage) {
		return;
	}
	const auto peer = item->history()->peer;
	const auto tasks = TasksFor(session->uniqueId(), peer->id);
	for (const auto &task : tasks) {
		if (!task.enabled
			|| task.kind != TaskKind::AutoReply
			|| (task.topicRootId && item->topicRootId() != task.topicRootId)
			|| Loaded().runtime[task.id].busy
			|| !CanAct(task)) {
			continue;
		}
		if (task.onlyWhenAddressed) {
			const auto replied = item->replyToId()
				? session->data().message(peer->id, item->replyToId())
				: nullptr;
			if (!item->mentionsMe() && !(replied && replied->out())) {
				continue;
			}
		}
		auto &runtime = Loaded().runtime[task.id];
		runtime.busy = true;
		const auto target = RawFromItem(item);
		const auto taskId = task.id;
		const auto delay = crl::time(3000 + base::RandomIndex(6000));
		base::call_delayed(delay, session, [=] {
			const auto current = TaskPtr(taskId);
			if (!current || !current->enabled) {
				FinishBusy(taskId);
				return;
			}
			RunAutoReply(session, peer, *current, target, nullptr);
		});
	}
}

void Watch(not_null<Main::Session*> session) {
	auto &state = Instance();
	if (state.watched.contains(session)) {
		return;
	}
	state.watched.emplace(session);
	session->lifetime().add([=] {
		Instance().watched.remove(session);
	});
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		HandleNewItem(session, item);
	}, session->lifetime());
}

void WatchAccounts() {
	auto &state = Instance();
	auto &domain = Core::App().domain();
	rpl::single(rpl::empty) | rpl::then(
		domain.accountsChanges()
	) | rpl::on_next([=] {
		for (const auto &[index, account] : Core::App().domain().accounts()) {
			account->sessionValue(
			) | rpl::on_next([=](Main::Session *session) {
				if (session) {
					Watch(session);
				}
			}, Instance().lifetime);
		}
	}, state.lifetime);
}

} // namespace

const std::vector<Preset> &Presets() {
	static const auto result = std::vector<Preset>{
		{
			.kind = ProviderKind::DeepSeek,
			.name = u"DeepSeek"_q,
			.baseUrl = u"https://api.deepseek.com/v1"_q,
			.model = u"deepseek-chat"_q,
			.models = { u"deepseek-chat"_q, u"deepseek-reasoner"_q },
			.keysUrl = u"https://platform.deepseek.com/api_keys"_q,
		},
		{
			.kind = ProviderKind::OpenAI,
			.name = u"ChatGPT (OpenAI)"_q,
			.baseUrl = u"https://api.openai.com/v1"_q,
			.model = u"gpt-4o-mini"_q,
			.models = {
				u"gpt-4o-mini"_q,
				u"gpt-4o"_q,
				u"gpt-4.1-mini"_q,
				u"gpt-4.1"_q,
			},
			.keysUrl = u"https://platform.openai.com/api-keys"_q,
			.vision = true,
		},
		{
			.kind = ProviderKind::Gemini,
			.name = u"Gemini (Google)"_q,
			.baseUrl = u"https://generativelanguage.googleapis.com/v1beta/openai"_q,
			.model = u"gemini-2.5-flash"_q,
			.models = {
				u"gemini-2.5-flash"_q,
				u"gemini-2.5-pro"_q,
				u"gemini-2.0-flash"_q,
			},
			.keysUrl = u"https://aistudio.google.com/apikey"_q,
			.vision = true,
		},
		{
			.kind = ProviderKind::Claude,
			.name = u"Claude (Anthropic)"_q,
			.baseUrl = u"https://api.anthropic.com/v1"_q,
			.model = u"claude-sonnet-5-5"_q,
			.models = {
				u"claude-sonnet-5-5"_q,
				u"claude-haiku-5-5"_q,
				u"claude-opus-5-5"_q,
			},
			.keysUrl = u"https://console.anthropic.com/settings/keys"_q,
			.vision = true,
		},
		{
			.kind = ProviderKind::Grok,
			.name = u"Grok (xAI)"_q,
			.baseUrl = u"https://api.x.ai/v1"_q,
			.model = u"grok-3-mini"_q,
			.models = { u"grok-3-mini"_q, u"grok-4"_q },
			.keysUrl = u"https://console.x.ai"_q,
		},
		{
			.kind = ProviderKind::Groq,
			.name = u"Groq"_q,
			.baseUrl = u"https://api.groq.com/openai/v1"_q,
			.model = u"llama-3.3-70b-versatile"_q,
			.models = {
				u"llama-3.3-70b-versatile"_q,
				u"openai/gpt-oss-120b"_q,
			},
			.keysUrl = u"https://console.groq.com/keys"_q,
		},
		{
			.kind = ProviderKind::OpenRouter,
			.name = u"OpenRouter"_q,
			.baseUrl = u"https://openrouter.ai/api/v1"_q,
			.model = u"deepseek/deepseek-chat"_q,
			.models = {
				u"deepseek/deepseek-chat"_q,
				u"openai/gpt-4o-mini"_q,
				u"google/gemini-2.5-flash"_q,
			},
			.keysUrl = u"https://openrouter.ai/keys"_q,
			.vision = true,
		},
		{
			.kind = ProviderKind::Mistral,
			.name = u"Mistral"_q,
			.baseUrl = u"https://api.mistral.ai/v1"_q,
			.model = u"mistral-small-latest"_q,
			.models = {
				u"mistral-small-latest"_q,
				u"mistral-large-latest"_q,
			},
			.keysUrl = u"https://console.mistral.ai/api-keys"_q,
		},
		{
			.kind = ProviderKind::Qwen,
			.name = u"Qwen (Alibaba)"_q,
			.baseUrl = u"https://dashscope-intl.aliyuncs.com/compatible-mode/v1"_q,
			.model = u"qwen-plus"_q,
			.models = { u"qwen-plus"_q, u"qwen-max"_q, u"qwen-vl-plus"_q },
			.keysUrl = u"https://modelstudio.console.alibabacloud.com"_q,
		},
		{
			.kind = ProviderKind::Kimi,
			.name = u"Kimi (Moonshot)"_q,
			.baseUrl = u"https://api.moonshot.ai/v1"_q,
			.model = u"kimi-k2-0905-preview"_q,
			.models = { u"kimi-k2-0905-preview"_q, u"moonshot-v1-8k"_q },
			.keysUrl = u"https://platform.moonshot.ai/console/api-keys"_q,
		},
		{
			.kind = ProviderKind::Ollama,
			.name = u"Ollama"_q,
			.baseUrl = u"http://localhost:11434/v1"_q,
			.model = u"llama3.1"_q,
			.models = { u"llama3.1"_q, u"qwen2.5"_q, u"gemma3"_q },
			.keysUrl = u"https://ollama.com"_q,
		},
		{
			.kind = ProviderKind::Custom,
			.name = u"Custom (OpenAI compatible)"_q,
		},
	};
	return result;
}

const Preset &PresetFor(ProviderKind kind) {
	const auto &list = Presets();
	const auto i = ranges::find(list, kind, &Preset::kind);
	return (i != end(list)) ? *i : list.back();
}

std::vector<Provider> Providers() {
	return Loaded().providers;
}

std::optional<Provider> FindProvider(uint64 id) {
	const auto &state = Loaded();
	if (!id) {
		id = state.defaultProvider;
	}
	const auto i = ranges::find(state.providers, id, &Provider::id);
	if (i != end(state.providers)) {
		return *i;
	} else if (!state.providers.empty()) {
		return state.providers.front();
	}
	return std::nullopt;
}

uint64 DefaultProviderId() {
	const auto provider = FindProvider(0);
	return provider ? provider->id : 0;
}

void SetDefaultProvider(uint64 id) {
	Loaded().defaultProvider = id;
	Notify();
}

uint64 SaveProvider(Provider provider) {
	auto &state = Loaded();
	if (!provider.id) {
		provider.id = base::RandomValue<uint64>() | 1;
	}
	const auto i = ranges::find(state.providers, provider.id, &Provider::id);
	if (i != end(state.providers)) {
		*i = provider;
	} else {
		state.providers.push_back(provider);
	}
	if (!state.defaultProvider) {
		state.defaultProvider = provider.id;
	}
	Notify();
	return provider.id;
}

void RemoveProvider(uint64 id) {
	auto &state = Loaded();
	const auto i = ranges::find(state.providers, id, &Provider::id);
	if (i == end(state.providers)) {
		return;
	}
	state.providers.erase(i);
	if (state.defaultProvider == id) {
		state.defaultProvider = state.providers.empty()
			? 0
			: state.providers.front().id;
	}
	Notify();
}

Fn<void()> Complete(
		Request request,
		Fn<void(QString)> done,
		Fn<void(QString)> failed) {
	const auto provider = FindProvider(request.providerId);
	if (!provider) {
		failed(Tr(
			"No AI service is added yet. Add one in Settings > AyuGram > Other > AI services.",
			"هنوز سرویس هوش مصنوعی اضافه نشده. از تنظیمات > ایوگرام > سایر > سرویس‌های هوش مصنوعی اضافه کن."));
		return [] {};
	}
	return CompleteWith(
		*provider,
		std::move(request),
		std::move(done),
		std::move(failed));
}

QString CleanApiKey(QString key) {
	static const auto invisible = QRegularExpression(
		u"[\\s\\x{200B}-\\x{200F}\\x{202A}-\\x{202E}\\x{2066}-\\x{2069}\\x{FEFF}\"'`]"_q);
	key.remove(invisible);
	if (key.startsWith(u"Bearer"_q, Qt::CaseInsensitive)) {
		key = key.mid(6);
	}
	return key;
}

Fn<void()> CompleteWith(
		Provider providerValue,
		Request request,
		Fn<void(QString)> done,
		Fn<void(QString)> failed) {
	const auto provider = &providerValue;
	auto base = provider->baseUrl.trimmed();
	while (base.endsWith('/')) {
		base.chop(1);
	}
	if (base.isEmpty()) {
		failed(Tr("The service address is empty.", "آدرس سرویس خالی است."));
		return [] {};
	}
	const auto url = base.endsWith(u"/chat/completions"_q)
		? base
		: (base + u"/chat/completions"_q);

	auto messages = QJsonArray();
	if (!request.system.isEmpty()) {
		auto system = QJsonObject();
		system.insert(u"role"_q, u"system"_q);
		system.insert(u"content"_q, request.system);
		messages.append(system);
	}
	for (const auto &message : request.messages) {
		auto object = QJsonObject();
		object.insert(u"role"_q, message.role);
		if (!message.imageJpeg.isEmpty() && provider->vision) {
			auto parts = QJsonArray();
			auto text = QJsonObject();
			text.insert(u"type"_q, u"text"_q);
			text.insert(u"text"_q, message.text);
			parts.append(text);
			auto image = QJsonObject();
			image.insert(u"type"_q, u"image_url"_q);
			auto imageUrl = QJsonObject();
			imageUrl.insert(
				u"url"_q,
				u"data:image/jpeg;base64,"_q
					+ QString::fromLatin1(message.imageJpeg.toBase64()));
			image.insert(u"image_url"_q, imageUrl);
			parts.append(image);
			object.insert(u"content"_q, parts);
		} else {
			object.insert(u"content"_q, message.text);
		}
		messages.append(object);
	}
	auto body = QJsonObject();
	body.insert(u"model"_q, provider->model.trimmed());
	body.insert(u"messages"_q, messages);
	body.insert(u"temperature"_q, request.temperature);
	body.insert(u"max_tokens"_q, request.maxTokens);
	body.insert(u"stream"_q, false);

	auto networkRequest = QNetworkRequest(QUrl(url));
	networkRequest.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	networkRequest.setTransferTimeout(kRequestTimeout);
	const auto key = CleanApiKey(provider->apiKey);
	if (!key.isEmpty()) {
		networkRequest.setRawHeader(
			"Authorization",
			"Bearer " + key.toUtf8());
		if (provider->kind == ProviderKind::Claude) {
			networkRequest.setRawHeader("x-api-key", key.toUtf8());
			networkRequest.setRawHeader("anthropic-version", "2023-06-01");
		}
	}
	if (provider->kind == ProviderKind::OpenRouter) {
		networkRequest.setRawHeader("X-Title", "AyuGram");
	}

	const auto reply = Network().post(
		networkRequest,
		QJsonDocument(body).toJson(QJsonDocument::Compact));
	const auto cancelled = std::make_shared<bool>(false);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		if (*cancelled) {
			return;
		}
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto data = reply->readAll();
		if (reply->error() != QNetworkReply::NoError
			&& (status < 200 || status >= 300)) {
			failed(status
				? ParseError(status, data)
				: reply->errorString());
			return;
		}
		const auto content = CleanAnswer(ParseContent(data));
		if (content.isEmpty()) {
			failed(Tr("Empty answer: ", "جواب خالی: ")
				+ ParseError(0, data));
			return;
		}
		done(content);
	});
	const auto weak = QPointer<QNetworkReply>(reply);
	return [=] {
		*cancelled = true;
		if (const auto strong = weak.data()) {
			if (strong->isRunning()) {
				strong->abort();
			}
		}
	};
}

std::vector<Task> Tasks() {
	return Loaded().tasks;
}

std::vector<Task> TasksFor(uint64 account, PeerId peer) {
	auto result = std::vector<Task>();
	for (const auto &task : Loaded().tasks) {
		if (task.account == account && task.peer == peer) {
			result.push_back(task);
		}
	}
	return result;
}

std::optional<Task> FindTask(uint64 id) {
	if (const auto task = TaskPtr(id)) {
		return *task;
	}
	return std::nullopt;
}

uint64 SaveTask(Task task) {
	auto &state = Loaded();
	if (!task.id) {
		task.id = base::RandomValue<uint64>() | 1;
	}
	const auto i = ranges::find(state.tasks, task.id, &Task::id);
	if (i != end(state.tasks)) {
		// Editing the time of a one-time post lets it run again.
		if (i->runAt != task.runAt || i->timeOfDay != task.timeOfDay) {
			task.lastRun = 0;
		}
		*i = task;
	} else {
		state.tasks.push_back(task);
	}
	Notify();
	return task.id;
}

void RemoveTask(uint64 id) {
	auto &state = Loaded();
	const auto i = ranges::find(state.tasks, id, &Task::id);
	if (i != end(state.tasks)) {
		state.tasks.erase(i);
		state.runtime.remove(id);
		Notify();
	}
}

rpl::producer<> Changes() {
	return Instance().changes.events();
}

void RunTaskNow(uint64 id, Fn<void(QString)> report) {
	const auto task = FindTask(id);
	if (!task) {
		return;
	}
	const auto session = AyuTools::FindSession(task->account);
	const auto peer = session
		? session->data().peerLoaded(task->peer)
		: nullptr;
	if (!peer) {
		report(Tr("The account of this task is not logged in.", "اکانت این وظیفه وارد نیست."));
		return;
	} else if (Loaded().runtime[id].busy) {
		report(Tr("The task is already running.", "این وظیفه در حال اجراست."));
		return;
	}
	switch (task->kind) {
	case TaskKind::Post:
		RunPost(session, peer, *task, report);
		break;
	case TaskKind::HotReply:
		RunHotReply(session, peer, *task, report);
		break;
	case TaskKind::CopyReaction:
		RunCopyReaction(session, peer, *task, report);
		break;
	case TaskKind::AutoReply:
		AyuTools::LoadRecent(session, peer, 20, [=](
				std::vector<AyuTools::RawMessage> messages) {
			for (auto i = messages.rbegin(); i != messages.rend(); ++i) {
				if (!i->out && !i->service) {
					RunAutoReply(session, peer, *task, *i, report);
					return;
				}
			}
			report(Tr("No message to reply to.", "پیامی برای جواب دادن نیست."));
		}, [=](QString error) {
			report(Tr("Error: ", "خطا: ") + error);
		});
		break;
	}
}

QString LastStatus(uint64 taskId) {
	auto &state = Loaded();
	const auto i = state.runtime.find(taskId);
	return (i != end(state.runtime)) ? i->second.status : QString();
}

QString TaskKindName(TaskKind kind) {
	switch (kind) {
	case TaskKind::AutoReply:
		return Tr("Smart replies", "جواب‌دهی هوشمند");
	case TaskKind::HotReply:
		return Tr("Reply to hot messages", "ریپلای روی پیام‌های داغ");
	case TaskKind::CopyReaction:
		return Tr("Join popular reactions", "همراهی با ری‌اکشن پرطرفدار");
	case TaskKind::Post:
		return Tr("Scheduled AI message", "پیام زمان‌دار هوش مصنوعی");
	}
	return QString();
}

QString TaskKindAbout(TaskKind kind) {
	switch (kind) {
	case TaskKind::AutoReply:
		return Tr(
			"Reads new messages of this chat and replies when your "
			"instructions say so. For example: answer questions about my "
			"shop politely, or joke with the members.",
			"پیام‌های جدید این گفتگو رو می‌خونه و هر جا دستور تو بگه جواب "
			"می‌ده. مثلاً: به سؤال‌ها درباره فروشگاهم مؤدبانه جواب بده، یا با "
			"اعضا شوخی کن.");
	case TaskKind::HotReply:
		return Tr(
			"When a message gets many replies, writes your own reply to it.",
			"وقتی یه پیام ریپلای زیادی بگیره، تو هم روش ریپلای می‌زنه.");
	case TaskKind::CopyReaction:
		return Tr(
			"When a message gets many reactions, adds the same reaction "
			"that most people chose. Doesn't need an AI service.",
			"وقتی یه پیام ری‌اکشن زیادی بگیره، همون ری‌اکشنی که بیشتر زدن "
			"رو می‌زنه. به سرویس هوش مصنوعی نیاز نداره.");
	case TaskKind::Post:
		return Tr(
			"At the chosen time (once or every day) the AI does the task "
			"and sends the result here or to your Saved Messages. For "
			"example: write a good morning message, or summarize today's "
			"messages of the group for me.",
			"سر ساعت انتخابی (یک بار یا هر روز) هوش مصنوعی کار رو انجام می‌ده "
			"و نتیجه رو همین‌جا یا توی پیام‌های ذخیره‌شده‌ات می‌فرسته. مثلاً: "
			"یه پیام صبح بخیر بنویس، یا پیام‌های امروز گروه رو برام خلاصه کن.");
	}
	return QString();
}

QString TaskSummary(const Task &task) {
	auto result = task.title.trimmed().isEmpty()
		? TaskKindName(task.kind)
		: task.title.trimmed();
	if (task.kind == TaskKind::Post) {
		const auto time = QTime(task.timeOfDay / 60, task.timeOfDay % 60);
		result += u" • "_q + (task.daily
			? Tr("every day at ", "هر روز ساعت ")
			: Tr("once at ", "یک بار ساعت "))
			+ QLocale().toString(time, QLocale::ShortFormat);
	}
	if (!task.enabled) {
		result += u" • "_q + Tr("off", "خاموش");
	}
	return result;
}

QString BaseSystemPrompt() {
	return u"You are a helpful assistant built into the Telegram client "
		"AyuGram. Answer in the language the user writes in (often "
		"Persian). Be accurate and to the point."_q;
}

void Init() {
	auto &state = Loaded();
	if (state.timer) {
		return;
	}
	state.timer = std::make_unique<base::Timer>([] { Tick(); });
	state.timer->callEach(kTickInterval);
	crl::on_main([] {
		if (Core::IsAppLaunched()) {
			WatchAccounts();
		}
	});
}

} // namespace AyuAi
