// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
//
// Repeating scheduled messages without Telegram Premium.
//
// Premium accounts use the native repeat of Telegram. For the others the
// message is scheduled on the server many times ahead (as far as Telegram
// allows: 100 scheduled messages per chat, one year ahead), so it is sent
// even when the computer is off. The list is topped up while the app runs.
#pragma once

#include "api/api_common.h"

namespace Main {
class Session;
} // namespace Main

namespace AyuRepeat {

struct Job {
	uint64 id = 0;
	uint64 account = 0;
	PeerId peer = 0;
	MsgId topicRootId = 0;
	TextWithTags text;
	TimeId period = 0;
	TimeId first = 0;
	TimeId created = 0;
	TimeId planned = 0; // The date of the last scheduled copy.
	bool silent = false;
};

void Init();

// Called from ApiWrap::sendMessage. Returns true if the message was a
// repeating one on a non-premium account and was registered as a job:
// the repeat period is then cleared from the message, which is sent as
// the first scheduled copy.
bool Intercept(
	not_null<Main::Session*> session,
	Api::MessageToSend &message);

[[nodiscard]] std::vector<Job> Jobs();
[[nodiscard]] rpl::producer<> Changes();

// Stops the job; with `deleteScheduled` also deletes its scheduled copies.
void Stop(uint64 jobId, bool deleteScheduled);

// Fills the scheduled copies of all jobs of the session now.
void TopUp(not_null<Main::Session*> session);

[[nodiscard]] QString PeriodText(TimeId period);

} // namespace AyuRepeat
