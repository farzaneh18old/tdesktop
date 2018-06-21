/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/export_api_wrap.h"

#include "export/export_settings.h"
#include "export/data/export_data_types.h"
#include "export/output/export_output_result.h"
#include "export/output/export_output_file.h"
#include "mtproto/rpc_sender.h"
#include "base/value_ordering.h"
#include "base/bytes.h"

#include <set>
#include <deque>

namespace Export {
namespace {

constexpr auto kUserpicsSliceLimit = 100;
constexpr auto kFileChunkSize = 128 * 1024;
constexpr auto kFileRequestsCount = 2;
constexpr auto kFileNextRequestDelay = TimeMs(20);
constexpr auto kChatsSliceLimit = 100;
constexpr auto kMessagesSliceLimit = 100;
constexpr auto kTopPeerSliceLimit = 100;
constexpr auto kFileMaxSize = 1500 * 1024 * 1024;
constexpr auto kLocationCacheSize = 100'000;

struct LocationKey {
	uint64 type;
	uint64 id;

	inline bool operator<(const LocationKey &other) const {
		return std::tie(type, id) < std::tie(other.type, other.id);
	}
};

std::tuple<const uint64 &, const uint64 &> value_ordering_helper(const LocationKey &value) {
	return std::tie(
		value.type,
		value.id);
}

LocationKey ComputeLocationKey(const Data::FileLocation &value) {
	auto result = LocationKey();
	result.type = value.dcId;
	value.data.match([&](const MTPDinputFileLocation &data) {
		result.type |= (1ULL << 24);
		result.type |= (uint64(uint32(data.vlocal_id.v)) << 32);
		result.id = data.vvolume_id.v;
	}, [&](const MTPDinputDocumentFileLocation &data) {
		result.type |= (2ULL << 24);
		result.id = data.vid.v;
	}, [&](const MTPDinputSecureFileLocation &data) {
		result.type |= (3ULL << 24);
		result.id = data.vid.v;
	}, [&](const MTPDinputEncryptedFileLocation &data) {
		result.type |= (4ULL << 24);
		result.id = data.vid.v;
	});
	return result;
}

Settings::Type SettingsFromDialogsType(Data::DialogInfo::Type type) {
	using DialogType = Data::DialogInfo::Type;
	switch (type) {
	case DialogType::Personal:
		return Settings::Type::PersonalChats;
	case DialogType::Bot:
		return Settings::Type::BotChats;
	case DialogType::PrivateGroup:
		return Settings::Type::PrivateGroups;
	case DialogType::PublicGroup:
		return Settings::Type::PublicGroups;
	case DialogType::PrivateChannel:
		return Settings::Type::PrivateChannels;
	case DialogType::PublicChannel:
		return Settings::Type::PublicChannels;
	}
	return Settings::Type(0);
}

} // namespace

class ApiWrap::LoadedFileCache {
public:
	using Location = Data::FileLocation;

	LoadedFileCache(int limit);

	void save(const Location &location, const QString &relativePath);
	base::optional<QString> find(const Location &location) const;

private:
	int _limit = 0;
	std::map<LocationKey, QString> _map;
	std::deque<LocationKey> _list;

};

struct ApiWrap::StartProcess {
	FnMut<void(StartInfo)> done;

	enum class Step {
		UserpicsCount,
		SplitRanges,
		DialogsCount,
		LeftChannelsCount,
	};
	std::deque<Step> steps;
	int splitIndex = 0;
	StartInfo info;
};

struct ApiWrap::ContactsProcess {
	FnMut<void(Data::ContactsList&&)> done;

	Data::ContactsList result;

	int topPeersOffset = 0;
};

struct ApiWrap::UserpicsProcess {
	FnMut<bool(Data::UserpicsInfo&&)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::UserpicsSlice&&)> handleSlice;
	FnMut<void()> finish;

	int processed = 0;
	base::optional<Data::UserpicsSlice> slice;
	uint64 maxId = 0;
	bool lastSlice = false;
	int fileIndex = -1;
};

struct ApiWrap::FileProcess {
	FileProcess(const QString &path, Output::Stats *stats);

	Output::File file;
	QString relativePath;

	Fn<bool(FileProgress)> progress;
	FnMut<void(const QString &relativePath)> done;

	Data::FileLocation location;
	int offset = 0;
	int size = 0;

	struct Request {
		int offset = 0;
		QByteArray bytes;
	};
	std::deque<Request> requests;
};

struct ApiWrap::FileProgress {
	int ready = 0;
	int total = 0;
};

struct ApiWrap::ChatsProcess {
	Fn<bool(int count)> progress;
	FnMut<void(Data::DialogsInfo&&)> done;

	Data::DialogsInfo info;
	int processedCount = 0;
	std::map<Data::PeerId, int> indexByPeer;
};

struct ApiWrap::LeftChannelsProcess : ChatsProcess {
	int fullCount = 0;
	int offset = 0;
	bool finished = false;
};

struct ApiWrap::DialogsProcess : ChatsProcess {
	int splitIndexPlusOne = 0;
	Data::TimeId offsetDate = 0;
	int32 offsetId = 0;
	MTPInputPeer offsetPeer = MTP_inputPeerEmpty();
};

struct ApiWrap::ChatProcess {
	Data::DialogInfo info;

	FnMut<bool(const Data::DialogInfo &)> start;
	Fn<bool(DownloadProgress)> fileProgress;
	Fn<bool(Data::MessagesSlice&&)> handleSlice;
	FnMut<void()> done;

	int localSplitIndex = 0;
	int32 largestIdPlusOne = 1;

	Data::ParseMediaContext context;
	base::optional<Data::MessagesSlice> slice;
	bool lastSlice = false;
	int fileIndex = -1;
};

ApiWrap::LoadedFileCache::LoadedFileCache(int limit) : _limit(limit) {
	Expects(limit >= 0);
}

void ApiWrap::LoadedFileCache::save(
		const Location &location,
		const QString &relativePath) {
	const auto key = ComputeLocationKey(location);
	_map[key] = relativePath;
	_list.push_back(key);
	if (_list.size() > _limit) {
		const auto key = _list.front();
		_list.pop_front();
		_map.erase(key);
	}
}

base::optional<QString> ApiWrap::LoadedFileCache::find(
		const Location &location) const {
	const auto key = ComputeLocationKey(location);
	if (const auto i = _map.find(key); i != end(_map)) {
		return i->second;
	}
	return base::none;
}

ApiWrap::FileProcess::FileProcess(const QString &path, Output::Stats *stats)
: file(path, stats) {
}

template <typename Request>
auto ApiWrap::mainRequest(Request &&request) {
	Expects(_takeoutId.has_value());

	return std::move(_mtp.request(MTPInvokeWithTakeout<Request>(
		MTP_long(*_takeoutId),
		std::forward<Request>(request)
	)).fail([=](RPCError &&result) {
		error(std::move(result));
	}).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)));
}

template <typename Request>
auto ApiWrap::splitRequest(int index, Request &&request) {
	Expects(index < _splits.size());

	//if (index == _splits.size() - 1) {
	//	return mainRequest(std::forward<Request>(request));
	//}
	return mainRequest(MTPInvokeWithMessagesRange<Request>(
		_splits[index],
		std::forward<Request>(request)));
}

auto ApiWrap::fileRequest(const Data::FileLocation &location, int offset) {
	Expects(location.dcId != 0);
	Expects(_takeoutId.has_value());

	return std::move(_mtp.request(MTPInvokeWithTakeout<MTPupload_GetFile>(
		MTP_long(*_takeoutId),
		MTPupload_GetFile(
			location.data,
			MTP_int(offset),
			MTP_int(kFileChunkSize))
	)).fail([=](RPCError &&result) {
		error(std::move(result));
	}).toDC(MTP::ShiftDcId(location.dcId, MTP::kExportMediaDcShift)));
}

ApiWrap::ApiWrap(Fn<void(FnMut<void()>)> runner)
: _mtp(std::move(runner))
, _fileCache(std::make_unique<LoadedFileCache>(kLocationCacheSize)) {
}

rpl::producer<RPCError> ApiWrap::errors() const {
	return _errors.events();
}

rpl::producer<Output::Result> ApiWrap::ioErrors() const {
	return _ioErrors.events();
}

void ApiWrap::startExport(
		const Settings &settings,
		Output::Stats *stats,
		FnMut<void(StartInfo)> done) {
	Expects(_settings == nullptr);
	Expects(_startProcess == nullptr);

	_settings = std::make_unique<Settings>(settings);
	_stats = stats;
	_startProcess = std::make_unique<StartProcess>();
	_startProcess->done = std::move(done);

	using Step = StartProcess::Step;
	if (_settings->types & Settings::Type::Userpics) {
		_startProcess->steps.push_back(Step::UserpicsCount);
	}
	if (_settings->types & Settings::Type::NonChannelChatsMask) {
		_startProcess->steps.push_back(Step::SplitRanges);
	}
	if (_settings->types & Settings::Type::AnyChatsMask) {
		_startProcess->steps.push_back(Step::DialogsCount);
	}
	if (_settings->types & Settings::Type::GroupsChannelsMask) {
		_startProcess->steps.push_back(Step::LeftChannelsCount);
	}
	startMainSession([=] {
		sendNextStartRequest();
	});
}

void ApiWrap::sendNextStartRequest() {
	Expects(_startProcess != nullptr);

	auto &steps = _startProcess->steps;
	if (steps.empty()) {
		finishStartProcess();
		return;
	}
	using Step = StartProcess::Step;
	const auto step = steps.front();
	steps.pop_front();
	switch (step) {
	case Step::UserpicsCount:
		return requestUserpicsCount();
	case Step::SplitRanges:
		return requestSplitRanges();
	case Step::DialogsCount:
		return requestDialogsCount();
	case Step::LeftChannelsCount:
		return requestLeftChannelsCount();
	}
	Unexpected("Step in ApiWrap::sendNextStartRequest.");
}

void ApiWrap::requestUserpicsCount() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0),  // offset
		MTP_long(0), // max_id
		MTP_int(0)   // limit
	)).done([=](const MTPphotos_Photos &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		_startProcess->info.userpicsCount = result.match(
		[](const MTPDphotos_photos &data) {
			return int(data.vphotos.v.size());
		}, [](const MTPDphotos_photosSlice &data) {
			return data.vcount.v;
		});

		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestSplitRanges() {
	Expects(_startProcess != nullptr);

	mainRequest(MTPmessages_GetSplitRanges(
	)).done([=](const MTPVector<MTPMessageRange> &result) {
		_splits = result.v;
		sendNextStartRequest();
	}).send();
}

void ApiWrap::requestDialogsCount() {
	Expects(_startProcess != nullptr);

	splitRequest(_startProcess->splitIndex, MTPmessages_GetDialogs(
		MTP_flags(0),
		MTP_int(0), // offset_date
		MTP_int(0), // offset_id
		MTP_inputPeerEmpty(), // offset_peer
		MTP_int(1)
	)).done([=](const MTPmessages_Dialogs &result) {
		Expects(_settings != nullptr);
		Expects(_startProcess != nullptr);

		_startProcess->info.dialogsCount += result.match(
		[](const MTPDmessages_dialogs &data) {
			return int(data.vdialogs.v.size());
		}, [](const MTPDmessages_dialogsSlice &data) {
			return data.vcount.v;
		});

		if (++_startProcess->splitIndex >= _splits.size()) {
			sendNextStartRequest();
		} else {
			requestDialogsCount();
		}
	}).send();
}

void ApiWrap::requestLeftChannelsCount() {
	Expects(_startProcess != nullptr);
	Expects(_leftChannelsProcess == nullptr);

	_leftChannelsProcess = std::make_unique<LeftChannelsProcess>();
	requestLeftChannelsSliceGeneric([=] {
		Expects(_startProcess != nullptr);
		Expects(_leftChannelsProcess != nullptr);

		_startProcess->info.leftChannelsCount
			= _leftChannelsProcess->fullCount;
		sendNextStartRequest();
	});
}

void ApiWrap::finishStartProcess() {
	Expects(_startProcess != nullptr);

	const auto process = base::take(_startProcess);
	process->done(process->info);
}

void ApiWrap::requestLeftChannelsList(
		Fn<bool(int count)> progress,
		FnMut<void(Data::DialogsInfo&&)> done) {
	Expects(_leftChannelsProcess != nullptr);

	validateSplits();

	_leftChannelsProcess->progress = std::move(progress);
	_leftChannelsProcess->done = std::move(done);
	requestLeftChannelsSlice();
}

void ApiWrap::requestLeftChannelsSlice() {
	requestLeftChannelsSliceGeneric([=] {
		Expects(_leftChannelsProcess != nullptr);

		if (_leftChannelsProcess->finished) {
			const auto process = base::take(_leftChannelsProcess);
			Data::FinalizeLeftChannelsInfo(process->info, *_settings);
			process->done(std::move(process->info));
		} else {
			requestLeftChannelsSlice();
		}
	});
}

void ApiWrap::requestDialogsList(
		Fn<bool(int count)> progress,
		FnMut<void(Data::DialogsInfo&&)> done) {
	Expects(_dialogsProcess == nullptr);

	validateSplits();

	_dialogsProcess = std::make_unique<DialogsProcess>();
	_dialogsProcess->splitIndexPlusOne = _splits.size();
	_dialogsProcess->progress = std::move(progress);
	_dialogsProcess->done = std::move(done);

	requestDialogsSlice();
}

void ApiWrap::validateSplits() {
	if (_splits.empty()) {
		_splits.push_back(MTP_messageRange(
			MTP_int(0),
			MTP_int(std::numeric_limits<int>::max())));
	}
}

void ApiWrap::startMainSession(FnMut<void()> done) {
	const auto sizeLimit = _settings->media.sizeLimit;
	const auto hasFiles = (_settings->media.types != 0) && (sizeLimit > 0);

	using Type = Settings::Type;
	using Flag = MTPaccount_InitTakeoutSession::Flag;
	const auto flags = Flag(0)
		| (_settings->types & Type::Contacts ? Flag::f_contacts : Flag(0))
		| (hasFiles ? Flag::f_files : Flag(0))
		| (sizeLimit < kFileMaxSize ? Flag::f_file_max_size : Flag(0))
		| (_settings->types & (Type::PersonalChats | Type::BotChats)
			? Flag::f_message_users
			: Flag(0))
		| (_settings->types & Type::PrivateGroups
			? (Flag::f_message_chats | Flag::f_message_megagroups)
			: Flag(0))
		| (_settings->types & Type::PublicGroups
			? Flag::f_message_megagroups
			: Flag(0))
		| (_settings->types & (Type::PrivateChannels | Type::PublicChannels)
			? Flag::f_message_channels
			: Flag(0));

	_mtp.request(MTPaccount_InitTakeoutSession(
		MTP_flags(flags),
		MTP_int(sizeLimit)
	)).done([=, done = std::move(done)](
			const MTPaccount_Takeout &result) mutable {
		_takeoutId = result.match([](const MTPDaccount_takeout &data) {
			return data.vid.v;
		});
		done();
	}).fail([=](RPCError &&result) {
		error(std::move(result));
	}).toDC(MTP::ShiftDcId(0, MTP::kExportDcShift)).send();
}

void ApiWrap::requestPersonalInfo(FnMut<void(Data::PersonalInfo&&)> done) {
	mainRequest(MTPusers_GetFullUser(
		_user
	)).done([=, done = std::move(done)](const MTPUserFull &result) mutable {
		Expects(result.type() == mtpc_userFull);

		const auto &full = result.c_userFull();
		if (full.vuser.type() == mtpc_user) {
			done(Data::ParsePersonalInfo(result));
		} else {
			error("Bad user type.");
		}
	}).send();
}

void ApiWrap::requestUserpics(
		FnMut<bool(Data::UserpicsInfo&&)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::UserpicsSlice&&)> slice,
		FnMut<void()> finish) {
	Expects(_userpicsProcess == nullptr);

	_userpicsProcess = std::make_unique<UserpicsProcess>();
	_userpicsProcess->start = std::move(start);
	_userpicsProcess->fileProgress = std::move(progress);
	_userpicsProcess->handleSlice = std::move(slice);
	_userpicsProcess->finish = std::move(finish);

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0), // offset
		MTP_long(_userpicsProcess->maxId),
		MTP_int(kUserpicsSliceLimit)
	)).done([=](const MTPphotos_Photos &result) mutable {
		Expects(_userpicsProcess != nullptr);

		auto startInfo = result.match(
		[](const MTPDphotos_photos &data) {
			return Data::UserpicsInfo{ data.vphotos.v.size() };
		}, [](const MTPDphotos_photosSlice &data) {
			return Data::UserpicsInfo{ data.vcount.v };
		});
		if (!_userpicsProcess->start(std::move(startInfo))) {
			return;
		}

		handleUserpicsSlice(result);
	}).send();
}

void ApiWrap::handleUserpicsSlice(const MTPphotos_Photos &result) {
	Expects(_userpicsProcess != nullptr);

	result.match([&](const auto &data) {
		if constexpr (MTPDphotos_photos::Is<decltype(data)>()) {
			_userpicsProcess->lastSlice = true;
		}
		loadUserpicsFiles(Data::ParseUserpicsSlice(
			data.vphotos,
			_userpicsProcess->processed));
	});
}

void ApiWrap::loadUserpicsFiles(Data::UserpicsSlice &&slice) {
	Expects(_userpicsProcess != nullptr);
	Expects(!_userpicsProcess->slice.has_value());

	if (slice.list.empty()) {
		_userpicsProcess->lastSlice = true;
	}
	_userpicsProcess->slice = std::move(slice);
	_userpicsProcess->fileIndex = -1;
	loadNextUserpic();
}

void ApiWrap::loadNextUserpic() {
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());

	auto &list = _userpicsProcess->slice->list;
	while (true) {
		const auto index = ++_userpicsProcess->fileIndex;
		if (index >= list.size()) {
			break;
		}
		const auto ready = processFileLoad(
			list[index].image.file,
			[=](FileProgress value) { return loadUserpicProgress(value); },
			[=](const QString &path) { loadUserpicDone(path); });
		if (!ready) {
			return;
		}
	}
	finishUserpicsSlice();
}

void ApiWrap::finishUserpicsSlice() {
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());

	auto slice = *base::take(_userpicsProcess->slice);
	if (!slice.list.empty()) {
		_userpicsProcess->processed += slice.list.size();
		_userpicsProcess->maxId = slice.list.back().id;
		if (!_userpicsProcess->handleSlice(std::move(slice))) {
			return;
		}
	}
	if (_userpicsProcess->lastSlice) {
		finishUserpics();
		return;
	}

	mainRequest(MTPphotos_GetUserPhotos(
		_user,
		MTP_int(0), // offset
		MTP_long(_userpicsProcess->maxId),
		MTP_int(kUserpicsSliceLimit)
	)).done([=](const MTPphotos_Photos &result) {
		handleUserpicsSlice(result);
	}).send();
}

bool ApiWrap::loadUserpicProgress(FileProgress progress) {
	Expects(_fileProcess != nullptr);
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());
	Expects((_userpicsProcess->fileIndex >= 0)
		&& (_userpicsProcess->fileIndex
			< _userpicsProcess->slice->list.size()));

	return _userpicsProcess->fileProgress(DownloadProgress{
		_fileProcess->relativePath,
		_userpicsProcess->fileIndex,
		progress.ready,
		progress.total });
}

void ApiWrap::loadUserpicDone(const QString &relativePath) {
	Expects(_userpicsProcess != nullptr);
	Expects(_userpicsProcess->slice.has_value());
	Expects((_userpicsProcess->fileIndex >= 0)
		&& (_userpicsProcess->fileIndex
			< _userpicsProcess->slice->list.size()));

	const auto index = _userpicsProcess->fileIndex;
	auto &file = _userpicsProcess->slice->list[index].image.file;
	file.relativePath = relativePath;
	loadNextUserpic();
}

void ApiWrap::finishUserpics() {
	Expects(_userpicsProcess != nullptr);

	base::take(_userpicsProcess)->finish();
}

void ApiWrap::requestContacts(FnMut<void(Data::ContactsList&&)> done) {
	Expects(_contactsProcess == nullptr);

	_contactsProcess = std::make_unique<ContactsProcess>();
	_contactsProcess->done = std::move(done);
	mainRequest(MTPcontacts_GetSaved(
	)).done([=](const MTPVector<MTPSavedContact> &result) {
		_contactsProcess->result = Data::ParseContactsList(result);
		requestTopPeersSlice();
	}).send();
}

void ApiWrap::requestTopPeersSlice() {
	Expects(_contactsProcess != nullptr);

	using Flag = MTPcontacts_GetTopPeers::Flag;
	mainRequest(MTPcontacts_GetTopPeers(
		MTP_flags(Flag::f_correspondents
			| Flag::f_bots_inline
			| Flag::f_phone_calls),
		MTP_int(_contactsProcess->topPeersOffset),
		MTP_int(kTopPeerSliceLimit),
		MTP_int(0) // hash
	)).done([=](const MTPcontacts_TopPeers &result) {
		Expects(_contactsProcess != nullptr);

		if (!Data::AppendTopPeers(_contactsProcess->result, result)) {
			error("Unexpected data in ApiWrap::requestTopPeersSlice.");
			return;
		}

		const auto offset = _contactsProcess->topPeersOffset;
		const auto loaded = result.match(
		[](const MTPDcontacts_topPeersNotModified &data) {
			return true;
		}, [&](const MTPDcontacts_topPeers &data) {
			for (const auto &category : data.vcategories.v) {
				const auto loaded = category.match(
				[&](const MTPDtopPeerCategoryPeers &data) {
					return offset + data.vpeers.v.size() >= data.vcount.v;
				});
				if (!loaded) {
					return false;
				}
			}
			return true;
		});

		if (loaded) {
			auto process = base::take(_contactsProcess);
			process->done(std::move(process->result));
		} else {
			_contactsProcess->topPeersOffset = std::max(std::max(
				_contactsProcess->result.correspondents.size(),
				_contactsProcess->result.inlineBots.size()),
				_contactsProcess->result.phoneCalls.size());
			requestTopPeersSlice();
		}
	}).send();
}

void ApiWrap::requestSessions(FnMut<void(Data::SessionsList&&)> done) {
	mainRequest(MTPaccount_GetAuthorizations(
	)).done([=, done = std::move(done)](
			const MTPaccount_Authorizations &result) mutable {
		auto list = Data::ParseSessionsList(result);
		mainRequest(MTPaccount_GetWebAuthorizations(
		)).done([=, done = std::move(done), list = std::move(list)](
				const MTPaccount_WebAuthorizations &result) mutable {
			list.webList = Data::ParseWebSessionsList(result).webList;
			done(std::move(list));
		}).send();
	}).send();
}

void ApiWrap::requestMessages(
		const Data::DialogInfo &info,
		FnMut<bool(const Data::DialogInfo &)> start,
		Fn<bool(DownloadProgress)> progress,
		Fn<bool(Data::MessagesSlice&&)> slice,
		FnMut<void()> done) {
	Expects(_chatProcess == nullptr);

	_chatProcess = std::make_unique<ChatProcess>();
	_chatProcess->info = info;
	_chatProcess->start = std::move(start);
	_chatProcess->fileProgress = std::move(progress);
	_chatProcess->handleSlice = std::move(slice);
	_chatProcess->done = std::move(done);

	requestMessagesCount(0);
}

void ApiWrap::requestMessagesCount(int localSplitIndex) {
	Expects(_chatProcess != nullptr);
	Expects(localSplitIndex < _chatProcess->info.splits.size());

	requestChatMessages(
		_chatProcess->info.splits[localSplitIndex],
		0, // offset_id
		0, // add_offset
		1, // limit
		[=](const MTPmessages_Messages &result) {
		Expects(_chatProcess != nullptr);

		const auto count = result.match(
		[](const MTPDmessages_messages &data) {
			return data.vmessages.v.size();
		}, [](const MTPDmessages_messagesSlice &data) {
			return data.vcount.v;
		}, [](const MTPDmessages_channelMessages &data) {
			return data.vcount.v;
		}, [](const MTPDmessages_messagesNotModified &data) {
			return -1;
		});
		if (count < 0) {
			error("Unexpected messagesNotModified received.");
			return;
		}
		_chatProcess->info.messagesCountPerSplit[localSplitIndex] = count;
		if (localSplitIndex + 1 < _chatProcess->info.splits.size()) {
			requestMessagesCount(localSplitIndex + 1);
		} else if (_chatProcess->start(_chatProcess->info)) {
			requestMessagesSlice();
		}
	});
}

void ApiWrap::finishExport(FnMut<void()> done) {
	const auto guard = gsl::finally([&] { _takeoutId = base::none; });

	mainRequest(MTPaccount_FinishTakeoutSession(
		MTP_flags(MTPaccount_FinishTakeoutSession::Flag::f_success)
	)).done(std::move(done)).send();
}

void ApiWrap::cancelExportFast() {
	if (_takeoutId.has_value()) {
		const auto requestId = mainRequest(MTPaccount_FinishTakeoutSession(
			MTP_flags(0)
		)).send();
		_mtp.request(requestId).detach();
	}
}

void ApiWrap::requestDialogsSlice() {
	Expects(_dialogsProcess != nullptr);

	const auto splitIndex = _dialogsProcess->splitIndexPlusOne - 1;
	splitRequest(splitIndex, MTPmessages_GetDialogs(
		MTP_flags(0),
		MTP_int(_dialogsProcess->offsetDate),
		MTP_int(_dialogsProcess->offsetId),
		_dialogsProcess->offsetPeer,
		MTP_int(kChatsSliceLimit)
	)).done([=](const MTPmessages_Dialogs &result) {
		auto finished = result.match(
		[](const MTPDmessages_dialogs &data) {
			return true;
		}, [](const MTPDmessages_dialogsSlice &data) {
			return data.vdialogs.v.isEmpty();
		});

		auto info = Data::ParseDialogsInfo(result);
		_dialogsProcess->processedCount += info.list.size();
		const auto last = info.list.empty()
			? Data::DialogInfo()
			: info.list.back();
		appendDialogsSlice(std::move(info));

		if (!_dialogsProcess->progress(_dialogsProcess->processedCount)) {
			return;
		}

		if (!finished && last.topMessageDate > 0) {
			_dialogsProcess->offsetId = last.topMessageId;
			_dialogsProcess->offsetDate = last.topMessageDate;
			_dialogsProcess->offsetPeer = last.input;
		} else if (--_dialogsProcess->splitIndexPlusOne > 0) {
			_dialogsProcess->offsetId = 0;
			_dialogsProcess->offsetDate = 0;
			_dialogsProcess->offsetPeer = MTP_inputPeerEmpty();
		} else {
			finishDialogsList();
			return;
		}
		requestDialogsSlice();
	}).send();
}

void ApiWrap::appendDialogsSlice(Data::DialogsInfo &&info) {
	Expects(_dialogsProcess != nullptr);
	Expects(_dialogsProcess->splitIndexPlusOne <= _splits.size());

	appendChatsSlice(
		*_dialogsProcess,
		std::move(info),
		_dialogsProcess->splitIndexPlusOne - 1);
}

void ApiWrap::finishDialogsList() {
	Expects(_dialogsProcess != nullptr);

	const auto process = base::take(_dialogsProcess);

	ranges::reverse(process->info.list);
	Data::FinalizeDialogsInfo(process->info, *_settings);

	process->done(std::move(process->info));
}

void ApiWrap::requestLeftChannelsSliceGeneric(FnMut<void()> done) {
	Expects(_leftChannelsProcess != nullptr);

	mainRequest(MTPchannels_GetLeftChannels(
		MTP_int(_leftChannelsProcess->offset)
	)).done([=, done = std::move(done)](
			const MTPmessages_Chats &result) mutable {
		Expects(_leftChannelsProcess != nullptr);

		appendLeftChannelsSlice(Data::ParseLeftChannelsInfo(result));

		const auto process = _leftChannelsProcess.get();
		process->offset += result.match(
		[](const auto &data) {
			return int(data.vchats.v.size());
		});

		process->fullCount = result.match(
		[](const MTPDmessages_chats &data) {
			return int(data.vchats.v.size());
		}, [](const MTPDmessages_chatsSlice &data) {
			return data.vcount.v;
		});

		process->finished = result.match(
		[](const MTPDmessages_chats &data) {
			return true;
		}, [](const MTPDmessages_chatsSlice &data) {
			return data.vchats.v.isEmpty();
		});

		if (process->progress) {
			if (!process->progress(process->info.list.size())) {
				return;
			}
		}

		done();
	}).send();
}

void ApiWrap::appendLeftChannelsSlice(Data::DialogsInfo &&info) {
	Expects(_leftChannelsProcess != nullptr);
	Expects(!_splits.empty());

	appendChatsSlice(
		*_leftChannelsProcess,
		std::move(info),
		_splits.size() - 1);
}

void ApiWrap::appendChatsSlice(
		ChatsProcess &to,
		Data::DialogsInfo &&info,
		int splitIndex) {
	Expects(_settings != nullptr);

	const auto types = _settings->types;
	auto filtered = ranges::view::all(
		info.list
	) | ranges::view::filter([&](const Data::DialogInfo &info) {
		return (types & SettingsFromDialogsType(info.type)) != 0;
	});
	auto &list = to.info.list;
	list.reserve(list.size() + info.list.size());
	for (auto &info : filtered) {
		const auto nextIndex = list.size();
		const auto [i, ok] = to.indexByPeer.emplace(info.peerId, nextIndex);
		if (ok) {
			list.push_back(std::move(info));
		}
		list[i->second].splits.push_back(splitIndex);
		list[i->second].messagesCountPerSplit.push_back(0);
	}
}

void ApiWrap::requestMessagesSlice() {
	Expects(_chatProcess != nullptr);

	requestChatMessages(
		_chatProcess->info.splits[_chatProcess->localSplitIndex],
		_chatProcess->largestIdPlusOne,
		-kMessagesSliceLimit,
		kMessagesSliceLimit,
		[=](const MTPmessages_Messages &result) {
		Expects(_chatProcess != nullptr);

		result.match([&](const MTPDmessages_messagesNotModified &data) {
			error("Unexpected messagesNotModified received.");
		}, [&](const auto &data) {
			if constexpr (MTPDmessages_messages::Is<decltype(data)>()) {
				_chatProcess->lastSlice = true;
			}
			loadMessagesFiles(Data::ParseMessagesSlice(
				_chatProcess->context,
				data.vmessages,
				data.vusers,
				data.vchats,
				_chatProcess->info.relativePath));
		});
	});
}

void ApiWrap::requestChatMessages(
		int splitIndex,
		int offsetId,
		int addOffset,
		int limit,
		FnMut<void(MTPmessages_Messages&&)> done) {
	if (_chatProcess->info.onlyMyMessages) {
		splitRequest(splitIndex, MTPmessages_Search(
			MTP_flags(MTPmessages_Search::Flag::f_from_id),
			_chatProcess->info.input,
			MTP_string(""), // query
			_user,
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0), // min_date
			MTP_int(0), // max_date
			MTP_int(offsetId),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(0), // max_id
			MTP_int(0), // min_id
			MTP_int(0) // hash
		)).done(std::move(done)).send();
	} else {
		splitRequest(splitIndex, MTPmessages_GetHistory(
			_chatProcess->info.input,
			MTP_int(offsetId),
			MTP_int(0), // offset_date
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(0), // max_id
			MTP_int(0), // min_id
			MTP_int(0)  // hash
		)).done(std::move(done)).send();
	}
}

void ApiWrap::loadMessagesFiles(Data::MessagesSlice &&slice) {
	Expects(_chatProcess != nullptr);
	Expects(!_chatProcess->slice.has_value());

	if (slice.list.empty()) {
		_chatProcess->lastSlice = true;
	}
	_chatProcess->slice = std::move(slice);
	_chatProcess->fileIndex = -1;

	loadNextMessageFile();
}

void ApiWrap::loadNextMessageFile() {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());

	auto &list = _chatProcess->slice->list;
	while (true) {
		const auto index = ++_chatProcess->fileIndex;
		if (index >= list.size()) {
			break;
		}
		const auto fileProgress = [=](FileProgress value) {
			return loadMessageFileProgress(value);
		};
		const auto ready = processFileLoad(
			list[index].file(),
			fileProgress,
			[=](const QString &path) { loadMessageFileDone(path); },
			&list[index]);
		if (!ready) {
			return;
		}
	}
	finishMessagesSlice();
}

void ApiWrap::finishMessagesSlice() {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());

	auto slice = *base::take(_chatProcess->slice);
	if (!slice.list.empty()) {
		_chatProcess->largestIdPlusOne = slice.list.back().id + 1;
		if (!_chatProcess->handleSlice(std::move(slice))) {
			return;
		}
	}
	if (_chatProcess->lastSlice
		&& (++_chatProcess->localSplitIndex
			< _chatProcess->info.splits.size())) {
		_chatProcess->lastSlice = false;
		_chatProcess->largestIdPlusOne = 1;
	}
	if (!_chatProcess->lastSlice) {
		requestMessagesSlice();
	} else {
		finishMessages();
	}
}

bool ApiWrap::loadMessageFileProgress(FileProgress progress) {
	Expects(_fileProcess != nullptr);
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());
	Expects((_chatProcess->fileIndex >= 0)
		&& (_chatProcess->fileIndex < _chatProcess->slice->list.size()));

	return _chatProcess->fileProgress(DownloadProgress{
		_fileProcess->relativePath,
		_chatProcess->fileIndex,
		progress.ready,
		progress.total });
}

void ApiWrap::loadMessageFileDone(const QString &relativePath) {
	Expects(_chatProcess != nullptr);
	Expects(_chatProcess->slice.has_value());
	Expects((_chatProcess->fileIndex >= 0)
		&& (_chatProcess->fileIndex < _chatProcess->slice->list.size()));

	const auto index = _chatProcess->fileIndex;
	_chatProcess->slice->list[index].file().relativePath = relativePath;
	loadNextMessageFile();
}

void ApiWrap::finishMessages() {
	Expects(_chatProcess != nullptr);
	Expects(!_chatProcess->slice.has_value());

	const auto process = base::take(_chatProcess);
	process->done();
}

bool ApiWrap::processFileLoad(
		Data::File &file,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done,
		Data::Message *message) {
	using SkipReason = Data::File::SkipReason;

	if (!file.relativePath.isEmpty()) {
		return true;
	} else if (!file.location) {
		file.skipReason = SkipReason::Unavailable;
		return true;
	} else if (writePreloadedFile(file)) {
		return !file.relativePath.isEmpty();
	}

	using Type = MediaSettings::Type;
	const auto type = message ? message->media.content.match(
	[&](const Data::Document &data) {
		if (data.isSticker) {
			return Type::Sticker;
		} else if (data.isVideoMessage) {
			return Type::VideoMessage;
		} else if (data.isVoiceMessage) {
			return Type::VoiceMessage;
		} else if (data.isAnimated) {
			return Type::GIF;
		} else if (data.isVideoFile) {
			return Type::Video;
		} else {
			return Type::File;
		}
	}, [](const auto &data) {
		return Type::Photo;
	}) : Type::Photo;

	if ((_settings->media.types & type) != type) {
		file.skipReason = SkipReason::FileType;
		return true;
	} else if (file.size >= _settings->media.sizeLimit) {
		file.skipReason = SkipReason::FileSize;
		return true;
	}
	loadFile(file, std::move(progress), std::move(done));
	return false;
}

bool ApiWrap::writePreloadedFile(Data::File &file) {
	Expects(_settings != nullptr);

	using namespace Output;

	if (const auto path = _fileCache->find(file.location)) {
		file.relativePath = *path;
		return true;
	} else if (!file.content.isEmpty()) {
		const auto process = prepareFileProcess(file);
		auto &output = _fileProcess->file;
		if (const auto result = output.writeBlock(file.content)) {
			file.relativePath = process->relativePath;
			_fileCache->save(file.location, file.relativePath);
		} else {
			ioError(result);
		}
		return true;
	}
	return false;
}

void ApiWrap::loadFile(
		const Data::File &file,
		Fn<bool(FileProgress)> progress,
		FnMut<void(QString)> done) {
	Expects(_fileProcess == nullptr);
	Expects(file.location.dcId != 0);

	_fileProcess = prepareFileProcess(file);
	_fileProcess->progress = std::move(progress);
	_fileProcess->done = std::move(done);

	if (_fileProcess->progress) {
		const auto progress = FileProgress{
			_fileProcess->file.size(),
			_fileProcess->size
		};
		if (!_fileProcess->progress(progress)) {
			return;
		}
	}

	loadFilePart();
}

auto ApiWrap::prepareFileProcess(const Data::File &file) const
-> std::unique_ptr<FileProcess> {
	Expects(_settings != nullptr);

	const auto relativePath = Output::File::PrepareRelativePath(
		_settings->path,
		file.suggestedPath);
	auto result = std::make_unique<FileProcess>(
		_settings->path + relativePath,
		_stats);
	result->relativePath = relativePath;
	result->location = file.location;
	result->size = file.size;
	return result;
}

void ApiWrap::loadFilePart() {
	if (!_fileProcess
		|| _fileProcess->requests.size() >= kFileRequestsCount
		|| (_fileProcess->size > 0
			&& _fileProcess->offset >= _fileProcess->size)) {
		return;
	}

	const auto offset = _fileProcess->offset;
	_fileProcess->requests.push_back({ offset });
	fileRequest(
		_fileProcess->location,
		_fileProcess->offset
	).done([=](const MTPupload_File &result) {
		filePartDone(offset, result);
	}).send();
	_fileProcess->offset += kFileChunkSize;

	if (_fileProcess->size > 0
		&& _fileProcess->requests.size() < kFileRequestsCount) {
		//const auto runner = _runner;
		//crl::on_main([=] {
		//	QTimer::singleShot(kFileNextRequestDelay, [=] {
		//		runner([=] {
		//			loadFilePart();
		//		});
		//	});
		//});
	}
}

void ApiWrap::filePartDone(int offset, const MTPupload_File &result) {
	Expects(_fileProcess != nullptr);
	Expects(!_fileProcess->requests.empty());

	if (result.type() == mtpc_upload_fileCdnRedirect) {
		error("Cdn redirect is not supported.");
		return;
	}
	const auto &data = result.c_upload_file();
	if (data.vbytes.v.isEmpty()) {
		if (_fileProcess->size > 0) {
			error("Empty bytes received in file part.");
			return;
		}
	} else {
		using Request = FileProcess::Request;
		auto &requests = _fileProcess->requests;
		const auto i = ranges::find(
			requests,
			offset,
			[](const Request &request) { return request.offset; });
		Assert(i != end(requests));

		i->bytes = data.vbytes.v;

		auto &file = _fileProcess->file;
		while (!requests.empty() && !requests.front().bytes.isEmpty()) {
			const auto &bytes = requests.front().bytes;
			if (const auto result = file.writeBlock(bytes); !result) {
				ioError(result);
				return;
			}
			requests.pop_front();
		}

		if (_fileProcess->progress) {
			_fileProcess->progress(FileProgress{
				file.size(),
				_fileProcess->size });
		}

		if (!requests.empty()
			|| !_fileProcess->size
			|| _fileProcess->size > _fileProcess->offset) {
			loadFilePart();
			return;
		}
	}

	auto process = base::take(_fileProcess);
	const auto relativePath = process->relativePath;
	_fileCache->save(process->location, relativePath);
	process->done(process->relativePath);
}

void ApiWrap::error(RPCError &&error) {
	_errors.fire(std::move(error));
}

void ApiWrap::error(const QString &text) {
	error(MTP_rpc_error(MTP_int(0), MTP_string("API_ERROR: " + text)));
}

void ApiWrap::ioError(const Output::Result &result) {
	_ioErrors.fire_copy(result);
}

ApiWrap::~ApiWrap() = default;

} // namespace Export