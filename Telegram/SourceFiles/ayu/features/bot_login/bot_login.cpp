// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/bot_login/bot_login.h"

#include "apiwrap.h"
#include "ayu/features/ayu_tools/ayu_tools.h"
#include "base/call_delayed.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "main/main_session.h"
#include "mtproto/sender.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>

namespace AyuBotLogin {
namespace {

constexpr auto kBatch = 100;
constexpr auto kMaxBatches = 60;
constexpr auto kSaveDelay = crl::time(3000);

struct SavedPeer {
	uint64 hash = 0;
	MsgId last = 0;
};

struct Saved {
	bool loaded = false;
	bool dirty = false;
	MsgId maxGlobal = 0;
	base::flat_map<PeerId, SavedPeer> peers;
};

struct Job {
	not_null<Main::Session*> session;
	mtpRequestId id = 0;
	mtpRequestId current = 0;
	bool cancelled = false;

	PeerId peer = 0;
	ChannelData *channel = nullptr;

	int wantDown = 0;
	int wantUp = 0;
	int skip = 0;
	MsgId nextDown = 0;
	MsgId nextUp = 0;
	MsgId upLimit = 0;
	bool upDone = false;
	bool downDone = false;
	bool reachedStart = false;
	int batches = 0;

	// Looking for the newest message id when it is unknown.
	bool probing = false;
	bool binary = false;
	int probeStep = 0;
	int probeEmpty = 0;
	MsgId probeLow = 0;
	MsgId probeHigh = 0;

	QVector<MTPMessage> down;
	QVector<MTPMessage> up;
	QVector<MTPUser> users;
	QVector<MTPChat> chats;

	MTP::DoneHandler done;
	MTP::FailHandler fail;
};

struct State {
	base::flat_map<not_null<MTP::Instance*>, not_null<Main::Session*>> bots;
	base::flat_map<uint64, Saved> saved;
	base::flat_map<mtpRequestId, std::shared_ptr<Job>> jobs;
};

[[nodiscard]] State &Instance() {
	static auto result = State();
	return result;
}

[[nodiscard]] QString FileName(not_null<Main::Session*> session) {
	return u"ayu_bot_%1.json"_q.arg(session->userId().bare);
}

[[nodiscard]] Saved &SavedFor(not_null<Main::Session*> session) {
	auto &result = Instance().saved[session->userId().bare];
	if (!result.loaded) {
		result.loaded = true;
		const auto object = AyuTools::LoadJson(FileName(session));
		result.maxGlobal = MsgId(
			object.value(u"max"_q).toString().toLongLong());
		for (const auto &value : object.value(u"peers"_q).toArray()) {
			const auto o = value.toObject();
			const auto id = PeerId(o.value(u"id"_q).toString().toULongLong());
			if (!id) {
				continue;
			}
			result.peers[id] = SavedPeer{
				.hash = o.value(u"hash"_q).toString().toULongLong(),
				.last = MsgId(o.value(u"last"_q).toString().toLongLong()),
			};
		}
	}
	return result;
}

void Save(not_null<Main::Session*> session) {
	auto &saved = SavedFor(session);
	if (!saved.dirty) {
		return;
	}
	saved.dirty = false;
	auto peers = QJsonArray();
	for (const auto &[id, peer] : saved.peers) {
		auto object = QJsonObject();
		object.insert(u"id"_q, QString::number(id.value));
		object.insert(u"hash"_q, QString::number(peer.hash));
		object.insert(u"last"_q, QString::number(peer.last.bare));
		peers.append(object);
	}
	auto object = QJsonObject();
	object.insert(u"max"_q, QString::number(saved.maxGlobal.bare));
	object.insert(u"peers"_q, peers);
	AyuTools::SaveJson(FileName(session), object);
}

void MarkDirty(not_null<Main::Session*> session) {
	auto &saved = SavedFor(session);
	if (saved.dirty) {
		return;
	}
	saved.dirty = true;
	base::call_delayed(kSaveDelay, session, [=] {
		Save(session);
	});
}

void Remember(not_null<HistoryItem*> item) {
	if (!item->isRegular()) {
		return;
	}
	const auto session = &item->history()->session();
	const auto peer = item->history()->peer;
	auto &saved = SavedFor(session);
	auto &entry = saved.peers[peer->id];
	if (const auto user = peer->asUser()) {
		entry.hash = user->accessHash();
	} else if (const auto channel = peer->asChannel()) {
		entry.hash = channel->accessHash();
	}
	entry.last = std::max(entry.last, item->id);
	if (!peer->isChannel()) {
		saved.maxGlobal = std::max(saved.maxGlobal, item->id);
	}
	MarkDirty(session);
}

[[nodiscard]] PeerId PeerFromInput(
		not_null<Main::Session*> session,
		const MTPInputPeer &peer) {
	return peer.match([&](const MTPDinputPeerSelf &) {
		return session->userPeerId();
	}, [](const MTPDinputPeerUser &data) {
		return peerFromUser(data.vuser_id());
	}, [](const MTPDinputPeerChat &data) {
		return peerFromChat(data.vchat_id());
	}, [](const MTPDinputPeerChannel &data) {
		return peerFromChannel(data.vchannel_id());
	}, [](const MTPDinputPeerUserFromMessage &data) {
		return peerFromUser(data.vuser_id());
	}, [](const MTPDinputPeerChannelFromMessage &data) {
		return peerFromChannel(data.vchannel_id());
	}, [](const MTPDinputPeerEmpty &) {
		return PeerId();
	});
}

[[nodiscard]] MsgId KnownTop(const std::shared_ptr<Job> &job) {
	const auto session = job->session;
	auto result = MsgId(0);
	if (const auto history = session->data().historyLoaded(job->peer)) {
		if (const auto last = history->lastMessage()) {
			if (last->isRegular()) {
				result = last->id;
			}
		}
	}
	auto &saved = SavedFor(session);
	if (job->channel) {
		const auto i = saved.peers.find(job->peer);
		if (i != end(saved.peers)) {
			result = std::max(result, i->second.last);
		}
	} else {
		result = std::max(result, saved.maxGlobal);
	}
	return result;
}

void Finish(const std::shared_ptr<Job> &job);
void Request(const std::shared_ptr<Job> &job);

void Fail(const std::shared_ptr<Job> &job, const MTP::Error &error) {
	Instance().jobs.remove(job->id);
	if (job->cancelled || !job->fail) {
		return;
	}
	auto fail = base::take(job->fail);
	job->done = nullptr;
	fail(error, MTP::Response{ .requestId = job->id });
}

void SendBatch(
		const std::shared_ptr<Job> &job,
		MsgId from,
		MsgId till,
		Fn<void(const QVector<MTPMessage>&)> handle) {
	auto ids = QVector<MTPInputMessage>();
	ids.reserve((till - from).bare + 1);
	for (auto id = from; id <= till; ++id) {
		ids.push_back(MTP_inputMessageID(MTP_int(id.bare)));
	}
	++job->batches;
	const auto onDone = [=](const MTP::Response &response) {
		if (job->cancelled) {
			return true;
		}
		auto result = MTPmessages_Messages();
		auto begin = response.reply.constData();
		if (!result.read(begin, begin + response.reply.size())) {
			return false;
		}
		auto list = QVector<MTPMessage>();
		result.match([&](const MTPDmessages_messagesNotModified &) {
		}, [&](const auto &data) {
			for (const auto &user : data.vusers().v) {
				job->users.push_back(user);
			}
			for (const auto &chat : data.vchats().v) {
				job->chats.push_back(chat);
			}
			list = data.vmessages().v;
		});
		handle(list);
		return true;
	};
	const auto onFail = [=](const MTP::Error &error, const MTP::Response &) {
		if (!job->cancelled) {
			Fail(job, error);
		}
		return true;
	};
	auto &instance = job->session->mtp();
	job->current = job->channel
		? instance.send(
			MTPchannels_GetMessages(
				job->channel->inputChannel(),
				MTP_vector<MTPInputMessage>(ids)),
			MTP::DoneHandler(onDone),
			MTP::FailHandler(onFail))
		: instance.send(
			MTPmessages_GetMessages(MTP_vector<MTPInputMessage>(ids)),
			MTP::DoneHandler(onDone),
			MTP::FailHandler(onFail));
}

[[nodiscard]] MsgId MaxExisting(const QVector<MTPMessage> &list) {
	auto result = MsgId(0);
	for (const auto &message : list) {
		if (message.type() != mtpc_messageEmpty) {
			result = std::max(result, IdFromMessage(message));
		}
	}
	return result;
}

void StartScan(const std::shared_ptr<Job> &job, MsgId top) {
	job->probing = false;
	job->binary = false;
	job->nextDown = top - 1;
	job->nextUp = top;
	job->upLimit = top - 1; // Nothing newer is known.
	if (job->wantUp <= 0) {
		job->upDone = true;
	}
	if (job->nextDown < 1 || job->wantDown + job->skip <= 0) {
		job->downDone = true;
		job->reachedStart = (job->nextDown < 1);
	}
	Request(job);
}

void Probe(const std::shared_ptr<Job> &job) {
	if (job->binary) {
		if (job->probeHigh - job->probeLow <= kBatch) {
			// Check the last window and start.
			const auto from = job->probeLow + 1;
			const auto till = std::max(from, job->probeHigh);
			SendBatch(job, from, till, [=](const QVector<MTPMessage> &list) {
				const auto found = std::max(job->probeLow, MaxExisting(list));
				if (!job->channel) {
					auto &saved = SavedFor(job->session);
					if (found > saved.maxGlobal) {
						saved.maxGlobal = found;
						MarkDirty(job->session);
					}
				}
				StartScan(job, found + 1);
			});
			return;
		}
		const auto mid = MsgId((job->probeLow.bare + job->probeHigh.bare) / 2);
		SendBatch(job, mid, mid + kBatch - 1, [=](
				const QVector<MTPMessage> &list) {
			if (const auto found = MaxExisting(list)) {
				job->probeLow = found;
			} else {
				job->probeHigh = mid;
			}
			Probe(job);
		});
		return;
	}
	const auto from = (job->probeStep == 0)
		? MsgId(1)
		: MsgId(int64(kBatch) << job->probeStep);
	SendBatch(job, from, from + kBatch - 1, [=](
			const QVector<MTPMessage> &list) {
		if (const auto found = MaxExisting(list)) {
			job->probeLow = std::max(job->probeLow, found);
			job->probeEmpty = 0;
		} else if (++job->probeEmpty >= 2 || job->probeStep >= 30) {
			job->binary = true;
			job->probeHigh = from;
			if (!job->probeLow) {
				StartScan(job, 1); // No messages at all.
				return;
			}
		}
		++job->probeStep;
		Probe(job);
	});
}

// Checks for messages newer than the known top before loading.
void ProbeUp(const std::shared_ptr<Job> &job, MsgId top, int left) {
	SendBatch(job, top, top + kBatch - 1, [=](
			const QVector<MTPMessage> &list) {
		const auto found = MaxExisting(list);
		if (!found || left <= 1) {
			const auto result = std::max(top, found + 1);
			if (!job->channel) {
				auto &saved = SavedFor(job->session);
				if (result - 1 > saved.maxGlobal) {
					saved.maxGlobal = result - 1;
					MarkDirty(job->session);
				}
			}
			StartScan(job, result);
		} else {
			ProbeUp(job, found + 1, left - 1);
		}
	});
}

void Request(const std::shared_ptr<Job> &job) {
	if (job->cancelled) {
		return;
	}
	const auto exhausted = (job->batches >= kMaxBatches);
	if (!job->upDone && !exhausted) {
		const auto from = job->nextUp;
		const auto till = std::min(from + kBatch - 1, job->upLimit);
		if (till < from) {
			job->upDone = true;
		} else {
			SendBatch(job, from, till, [=](const QVector<MTPMessage> &list) {
				for (const auto &message : list) {
					if (message.type() != mtpc_messageEmpty
						&& PeerFromMessage(message) == job->peer) {
						job->up.push_back(message);
					}
				}
				job->nextUp = till + 1;
				if (int(job->up.size()) >= job->wantUp
					|| job->nextUp > job->upLimit) {
					job->upDone = true;
				}
				Request(job);
			});
			return;
		}
	}
	if (!job->downDone && !exhausted) {
		const auto till = job->nextDown;
		const auto from = std::max(MsgId(1), till - kBatch + 1);
		SendBatch(job, from, till, [=](const QVector<MTPMessage> &list) {
			for (const auto &message : list) {
				if (message.type() != mtpc_messageEmpty
					&& PeerFromMessage(message) == job->peer) {
					job->down.push_back(message);
				}
			}
			job->nextDown = from - 1;
			if (job->nextDown < 1) {
				job->downDone = true;
				job->reachedStart = true;
			} else if (int(job->down.size()) >= job->wantDown + job->skip) {
				job->downDone = true;
			}
			Request(job);
		});
		return;
	}
	Finish(job);
}

void Finish(const std::shared_ptr<Job> &job) {
	Instance().jobs.remove(job->id);
	if (job->cancelled || !job->done) {
		return;
	}
	const auto byIdDesc = [](const MTPMessage &a, const MTPMessage &b) {
		return IdFromMessage(a) > IdFromMessage(b);
	};
	auto down = job->down;
	std::sort(down.begin(), down.end(), byIdDesc);
	if (job->skip > 0) {
		down = down.mid(std::min(job->skip, int(down.size())));
	}
	if (int(down.size()) > job->wantDown) {
		down.resize(std::max(job->wantDown, 0));
	}
	auto up = job->up;
	std::sort(up.begin(), up.end(), [](const MTPMessage &a, const MTPMessage &b) {
		return IdFromMessage(a) < IdFromMessage(b);
	});
	if (int(up.size()) > job->wantUp) {
		up.resize(std::max(job->wantUp, 0));
	}
	std::reverse(up.begin(), up.end());
	auto messages = up + down;

	const auto count = int(messages.size())
		+ ((job->reachedStart || job->downDone && down.isEmpty()) ? 0 : 100000);
	const auto result = MTP_messages_messagesSlice(
		MTP_flags(0),
		MTP_int(count),
		MTPint(),
		MTPint(),
		MTPSearchPostsFlood(),
		MTP_vector<MTPMessage>(messages),
		MTP_vector<MTPForumTopic>(),
		MTP_vector<MTPChat>(job->chats),
		MTP_vector<MTPUser>(job->users));
	auto reply = mtpBuffer();
	result.write(reply);
	auto done = base::take(job->done);
	job->fail = nullptr;
	done(MTP::Response{ .reply = std::move(reply), .requestId = job->id });
}

mtpRequestId SendHistory(
		not_null<MTP::Instance*> instance,
		const MTPmessages_GetHistory &request,
		MTP::DoneHandler &&done,
		MTP::FailHandler &&fail) {
	const auto &state = Instance();
	const auto i = state.bots.find(instance);
	Assert(i != end(state.bots));
	const auto session = i->second;

	auto buffer = mtpBuffer();
	request.write(buffer);
	const mtpPrime *from = buffer.constData();
	const auto end = from + buffer.size();
	++from; // Constructor id.
	auto peer = MTPInputPeer();
	auto offsetId = MTPint();
	auto offsetDate = MTPint();
	auto addOffset = MTPint();
	auto limit = MTPint();
	const auto parsed = peer.read(from, end)
		&& offsetId.read(from, end)
		&& offsetDate.read(from, end)
		&& addOffset.read(from, end)
		&& limit.read(from, end);

	const auto id = MTP::details::GetNextRequestId();
	const auto job = std::make_shared<Job>(Job{
		.session = session,
		.id = id,
		.done = std::move(done),
		.fail = std::move(fail),
	});
	Instance().jobs.emplace(id, job);
	if (!parsed) {
		crl::on_main(session, [=] {
			Fail(job, MTP::Error::Local("BOT_HISTORY", "Bad request."));
		});
		return id;
	}
	job->peer = PeerFromInput(session, peer);
	if (peerIsChannel(job->peer)) {
		job->channel = session->data().channelLoaded(
			peerToChannel(job->peer));
		if (!job->channel) {
			crl::on_main(session, [=] {
				Fail(job, MTP::Error::Local("BOT_HISTORY", "No channel."));
			});
			return id;
		}
	}
	const auto wanted = std::clamp(limit.v, 1, 100);
	const auto add = addOffset.v;
	if (add >= 0) {
		job->wantDown = wanted;
		job->skip = add;
		job->wantUp = 0;
	} else {
		job->wantUp = std::min(-add, wanted);
		job->wantDown = wanted - job->wantUp;
	}
	const auto offset = MsgId(offsetId.v);
	crl::on_main(session, [=] {
		if (job->cancelled) {
			return;
		}
		const auto known = KnownTop(job);
		if (offset) {
			// Messages newer than the offset may exist up to the known top.
			job->nextDown = offset - 1;
			job->nextUp = offset;
			job->upLimit = std::max(known, offset) + kBatch;
			if (job->wantUp <= 0) {
				job->upDone = true;
			}
			if (job->nextDown < 1 || job->wantDown + job->skip <= 0) {
				job->downDone = true;
				job->reachedStart = (job->nextDown < 1);
			}
			Request(job);
		} else if (known) {
			ProbeUp(job, known + 1, 20);
		} else {
			job->probing = true;
			Probe(job);
		}
	});
	return id;
}

void Watch(not_null<Main::Session*> session) {
	auto &state = Instance();
	const auto instance = &session->mtp();
	if (state.bots.contains(instance)) {
		return;
	}
	state.bots.emplace(instance, session);
	session->lifetime().add([=] {
		Save(session);
		Instance().bots.remove(instance);
	});
	session->data().newItemAdded(
	) | rpl::on_next([=](not_null<HistoryItem*> item) {
		Remember(item);
	}, session->lifetime());
}

void ApplyMessages(
		not_null<Main::Session*> session,
		const MTPmessages_Messages &result) {
	result.match([](const MTPDmessages_messagesNotModified &) {
	}, [&](const auto &data) {
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		session->data().processMessages(
			data.vmessages(),
			NewMessageType::Existing);
		for (const auto &message : data.vmessages().v) {
			if (message.type() == mtpc_messageEmpty) {
				continue;
			}
			const auto peerId = PeerFromMessage(message);
			const auto history = session->data().history(peerId);
			const auto id = IdFromMessage(message);
			const auto last = history->lastMessage();
			if (!last || last->id < id) {
				history->applyDialogTopMessage(id);
				history->setUnreadCount(0);
				history->setInboxReadTill(id);
			}
		}
	});
	session->data().chatsListChanged(nullptr);
}

void RestoreMessages(not_null<Main::Session*> session) {
	const auto &saved = SavedFor(session);
	auto global = QVector<MTPInputMessage>();
	for (const auto &[peerId, peer] : saved.peers) {
		if (!peer.last) {
			continue;
		} else if (peerIsChannel(peerId)) {
			const auto channel = session->data().channelLoaded(
				peerToChannel(peerId));
			if (!channel) {
				continue;
			}
			session->api().request(MTPchannels_GetMessages(
				channel->inputChannel(),
				MTP_vector<MTPInputMessage>(
					1,
					MTP_inputMessageID(MTP_int(peer.last.bare))))
			).done([=](const MTPmessages_Messages &result) {
				ApplyMessages(session, result);
			}).send();
		} else {
			global.push_back(MTP_inputMessageID(MTP_int(peer.last.bare)));
		}
	}
	while (!global.isEmpty()) {
		const auto part = global.mid(0, kBatch);
		global = global.mid(part.size());
		session->api().request(MTPmessages_GetMessages(
			MTP_vector<MTPInputMessage>(part)
		)).done([=](const MTPmessages_Messages &result) {
			ApplyMessages(session, result);
		}).send();
	}
}

void RestoreChats(not_null<Main::Session*> session) {
	const auto &saved = SavedFor(session);
	auto users = QVector<MTPInputUser>();
	auto chats = QVector<MTPlong>();
	auto channels = QVector<MTPInputChannel>();
	for (const auto &[peerId, peer] : saved.peers) {
		if (peerIsUser(peerId)) {
			users.push_back(MTP_inputUser(
				MTP_long(peerToUser(peerId).bare),
				MTP_long(peer.hash)));
		} else if (peerIsChat(peerId)) {
			chats.push_back(MTP_long(peerToChat(peerId).bare));
		} else if (peerIsChannel(peerId)) {
			channels.push_back(MTP_inputChannel(
				MTP_long(peerToChannel(peerId).bare),
				MTP_long(peer.hash)));
		}
	}
	const auto waiting = std::make_shared<int>(0);
	const auto finished = [=] {
		if (--*waiting == 0) {
			RestoreMessages(session);
		}
	};
	const auto start = [&](auto &&request) {
		++*waiting;
		request.fail(finished).send();
	};
	while (!users.isEmpty()) {
		const auto part = users.mid(0, kBatch);
		users = users.mid(part.size());
		start(session->api().request(MTPusers_GetUsers(
			MTP_vector<MTPInputUser>(part)
		)).done([=](const MTPVector<MTPUser> &result) {
			session->data().processUsers(result);
			finished();
		}));
	}
	if (!chats.isEmpty()) {
		start(session->api().request(MTPmessages_GetChats(
			MTP_vector<MTPlong>(chats)
		)).done([=](const MTPmessages_Chats &result) {
			result.match([&](const auto &data) {
				session->data().processChats(data.vchats());
			});
			finished();
		}));
	}
	while (!channels.isEmpty()) {
		const auto part = channels.mid(0, kBatch);
		channels = channels.mid(part.size());
		start(session->api().request(MTPchannels_GetChannels(
			MTP_vector<MTPInputChannel>(part)
		)).done([=](const MTPmessages_Chats &result) {
			result.match([&](const auto &data) {
				session->data().processChats(data.vchats());
			});
			finished();
		}));
	}
	if (!*waiting) {
		RestoreMessages(session);
	}
}

} // namespace

void Init() {
	auto &hook = MTP::BotHistoryHook();
	if (hook.applies) {
		return;
	}
	hook.applies = [](not_null<MTP::Instance*> instance) {
		return Instance().bots.contains(instance);
	};
	hook.send = SendHistory;
	MTP::details::BotCancelHook() = [](mtpRequestId requestId) {
		auto &jobs = Instance().jobs;
		const auto i = jobs.find(requestId);
		if (i == end(jobs)) {
			return;
		}
		const auto job = i->second;
		jobs.erase(i);
		job->cancelled = true;
		job->done = nullptr;
		job->fail = nullptr;
		if (const auto current = base::take(job->current)) {
			job->session->mtp().cancel(current);
		}
	};
}

bool IsBot(not_null<Main::Session*> session) {
	return session->user()->isBot();
}

void SessionStarted(not_null<Main::Session*> session) {
	if (!IsBot(session)) {
		return;
	}
	Init();
	const auto first = !Instance().bots.contains(&session->mtp());
	Watch(session);
	if (first) {
		RestoreChats(session);
	}
}

} // namespace AyuBotLogin
