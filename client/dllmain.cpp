#include "NativeBridge.h"
#include "pch.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pugixml\pugixml.hpp"

#define ASIO_STANDALONE
#include <asio.hpp>

namespace fs = std::filesystem;

static std::string username = "Player";
static std::string ip = "46.62.255.79";
static std::string port = "40000";
static HANDLE g_initThread = NULL;
static std::atomic<int> g_localPlayerId{0};
static std::atomic<bool> g_shutdown{false};
static std::atomic<bool> g_run{false};

static asio::io_context io;
static asio::ip::udp::resolver resolver(io);
static asio::ip::udp::socket theSocket(io);
static asio::ip::udp::endpoint serverEndpoint;

static bool PinClientModule()
{
	HMODULE pinnedModule = nullptr;
	return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&PinClientModule), &pinnedModule) != FALSE;
}

struct ParsedHalves
{
	std::vector<std::string> first;
	std::vector<std::string> second;
};

struct RemotePlayerChunks
{
	std::string username;

	std::vector<std::string> update1A;
	std::vector<std::string> update1B;
	std::vector<std::string> update2A;
	std::vector<std::string> update2B;

	std::vector<std::string> lastUpdate1;
	std::vector<std::string> lastUpdate2;
	std::vector<std::string> lastUpdate3;
	std::vector<std::string> lastUpdate4;

	int update1ASequence = 0;
	int update1BSequence = 0;

	int update1AMovementSequence = 0;
	int update1BMovementSequence = 0;

	int update2ASequence = 0;
	int update2BSequence = 0;

	int lastPushed1Sequence = 0;
	int lastPushed2Sequence = 0;
};

static std::mutex remoteMu;
static std::unordered_map<int, RemotePlayerChunks> remotePlayers;

static fs::path getExecutablePath()
{
	char buffer[MAX_PATH]{};
	GetModuleFileNameA(NULL, buffer, MAX_PATH);
	return fs::path(buffer).parent_path().parent_path();
}

static ParsedHalves ParseValuesSplitHalf(const std::string &input)
{
	static const std::string kStartMarker = "_s";
	static const std::string kEndMarker = "_e";
	static const std::string kHalfMarker = "half";

	std::istringstream iss(input);
	std::string word;
	ParsedHalves out;
	std::vector<std::string> *current = &out.first;

	if (!(iss >> word))
		return out;

	bool inBlock = false;
	std::string blockAccum;

	while (iss >> word)
	{
		if (!inBlock)
		{
			if (word == kStartMarker)
			{
				inBlock = true;
				blockAccum.clear();
				continue;
			}

			if (word == kHalfMarker)
			{
				current = &out.second;
				continue;
			}

			if (word == kEndMarker)
			{
				current->push_back(word);
				continue;
			}

			current->push_back(word);
		}
		else
		{
			if (word == kEndMarker)
			{
				current->push_back(blockAccum);
				inBlock = false;
				blockAccum.clear();
			}
			else
			{
				if (!blockAccum.empty())
					blockAccum += ' ';
				blockAccum += word;
			}
		}
	}

	if (inBlock && !blockAccum.empty())
		current->push_back(blockAccum);

	return out;
}

static std::string EscapeField(const std::string &s)
{
	std::string out;
	out.reserve(s.size());

	for (char c : s)
	{
		if (c == '\\')
			out += "\\\\";
		else if (c == '\t')
			out += "\\t";
		else if (c == '\n')
			out += "\\n";
		else if (c == '\r')
			out += "\\r";
		else
			out += c;
	}

	return out;
}

static std::string BuildPacket(const std::string &opcode, const std::string &id, const std::vector<std::string> &fields)
{
	std::string packet = opcode + "\t" + id;
	for (const auto &f : fields)
	{
		packet += "\t";
		packet += EscapeField(f);
	}
	return packet;
}

static std::vector<std::string> SplitTabs(const std::string &s)
{
	std::vector<std::string> parts;
	std::string cur;
	bool esc = false;

	for (char c : s)
	{
		if (esc)
		{
			if (c == 't')
				cur += '\t';
			else if (c == 'n')
				cur += '\n';
			else if (c == 'r')
				cur += '\r';
			else if (c == '\\')
				cur += '\\';
			else
				cur += c;
			esc = false;
		}
		else if (c == '\\')
		{
			esc = true;
		}
		else if (c == '\t')
		{
			parts.push_back(cur);
			cur.clear();
		}
		else
		{
			cur += c;
		}
	}

	parts.push_back(cur);
	return parts;
}

static bool ParsePositiveInt(const std::string &text, int &value)
{
	try
	{
		value = std::stoi(text);
		return value > 0;
	}
	catch (...)
	{
		value = 0;
		return false;
	}
}

static bool ExtractMovementPrefix(const ParsedHalves &halves, std::vector<std::string> &movement)
{
	if (halves.first.size() < 7)
		return false;
	movement.assign(halves.first.begin(), halves.first.begin() + 7);
	return true;
}

static bool RemoveMovementPrefix(ParsedHalves &halves, std::vector<std::string> &movement)
{
	if (!ExtractMovementPrefix(halves, movement))
		return false;
	halves.first.erase(halves.first.begin(), halves.first.begin() + 7);
	return true;
}

static void CloseOnlineSession()
{
	wo_native::SetConnected(false);
	try
	{
		if (theSocket.is_open())
			theSocket.close();
	}
	catch (...)
	{
	}
}

static void SendUdpPacket(const std::string &packet, const char *label)
{
	if (!wo_native::IsConnected() || !theSocket.is_open())
		return;

	try
	{
		theSocket.send(asio::buffer(packet));
	}
	catch (const std::exception &e)
	{
		std::cout << "Send error (" << label << "): " << e.what() << "\n";
	}
}

static std::string BuildLocalPacketId()
{
	const int localPlayerId = g_localPlayerId.load();
	if (localPlayerId > 0)
		return std::to_string(localPlayerId) + "\t" + EscapeField(username);
	return EscapeField(username);
}

static int g_movementSequence = static_cast<int>(((GetTickCount64() / 20ULL) % 1000000000ULL) + 1ULL);

static int g_update1Sequence = static_cast<int>(((GetTickCount64() / 20ULL) % 1000000000ULL) + 1ULL);

static int g_update2Sequence = static_cast<int>(((GetTickCount64() / 20ULL) % 1000000000ULL) + 1ULL);

static constexpr int kSequenceSpan = 2000000000;

static constexpr ULONGLONG kScriptStallThresholdMs = 750;
static constexpr ULONGLONG kKeepaliveIntervalMs = 1000;

static constexpr ULONGLONG kMenuTransitionGraceMs = 500;
static constexpr size_t kClientInGameField = 7;
static constexpr size_t kMenuNameField = 47;

static std::vector<std::string> g_cachedMovement;
static ParsedHalves g_cachedUpdate1;
static bool g_haveCachedUpdate1 = false;
static std::string g_presence = "none";
static ULONGLONG g_lastScriptActivityTick = 0;
static ULONGLONG g_lastKeepaliveTick = 0;
static ULONGLONG g_menuTransitionDeadlineTick = 0;

static void ResetTransportWatchdog()
{
	g_cachedMovement.clear();
	g_cachedUpdate1 = ParsedHalves();
	g_haveCachedUpdate1 = false;
	g_presence = "none";
	g_lastScriptActivityTick = 0;
	g_lastKeepaliveTick = 0;
	g_menuTransitionDeadlineTick = 0;
}

static bool IsTruthyField(const std::string &value)
{
	return value == "true" || value == "1" || value == "True" || value == "TRUE";
}

static bool ScriptTransportStalled(ULONGLONG now)
{
	return g_lastScriptActivityTick != 0 && now >= g_lastScriptActivityTick && (now - g_lastScriptActivityTick) >= kScriptStallThresholdMs;
}

static bool CachedClientIsInGame()
{
	return g_haveCachedUpdate1 && g_cachedUpdate1.first.size() > kClientInGameField && IsTruthyField(g_cachedUpdate1.first[kClientInGameField]);
}

static std::string EffectivePresence(ULONGLONG now)
{
	if (!g_presence.empty() && g_presence != "none")
		return g_presence;

	if (ScriptTransportStalled(now) && CachedClientIsInGame())
		return "IngameMenu";

	return "none";
}

static void ApplyPresenceToUpdate1(ParsedHalves &halves, ULONGLONG now)
{
	if (halves.first.size() <= kMenuNameField)
		return;

	const std::string presence = EffectivePresence(now);
	if (presence != "none" || ScriptTransportStalled(now))
		halves.first[kMenuNameField] = presence;
}

static int NextSequence(int &sequence)
{
	if (sequence <= 0 || sequence >= kSequenceSpan)
		sequence = 1;
	else
		++sequence;

	return sequence;
}

static int NextMovementSequence()
{
	return NextSequence(g_movementSequence);
}

static void SendMovementPacket(const std::string &packetId, const std::vector<std::string> &movement)
{
	if (movement.size() < 7)
		return;

	const int sequence = NextMovementSequence();

	std::vector<std::string> fields;
	fields.reserve(8);

	fields.push_back(std::to_string(sequence));
	fields.insert(fields.end(), movement.begin(), movement.begin() + 7);

	SendUdpPacket(BuildPacket("MOVE", packetId, fields), "MOVE");
}

static void SendMovementPayload(const std::string &payload)
{
	ParsedHalves halves = ParseValuesSplitHalf(payload);
	std::vector<std::string> movement;
	if (!ExtractMovementPrefix(halves, movement))
		return;

	g_cachedMovement = movement;
	SendMovementPacket(BuildLocalPacketId(), movement);
}

static void SendUpdate1Halves(ParsedHalves halves, ULONGLONG now)
{
	if (halves.first.size() < 7)
		return;

	ApplyPresenceToUpdate1(halves, now);

	const std::string packetId = BuildLocalPacketId();
	const int movementSequence = NextMovementSequence();
	const int updateSequence = NextSequence(g_update1Sequence);

	if (!halves.first.empty())
	{
		halves.first.insert(halves.first.begin(), std::to_string(movementSequence));

		halves.first.insert(halves.first.begin(), std::to_string(updateSequence));

		SendUdpPacket(BuildPacket("UPDATE1A", packetId, halves.first), "UPDATE1A");
	}

	if (!halves.second.empty())
	{
		halves.second.insert(halves.second.begin(), std::to_string(movementSequence));

		halves.second.insert(halves.second.begin(), std::to_string(updateSequence));

		SendUdpPacket(BuildPacket("UPDATE1B", packetId, halves.second), "UPDATE1B");
	}
}

static void SendUpdate1(const std::string &payload)
{
	ParsedHalves halves = ParseValuesSplitHalf(payload);

	if (halves.first.size() < 7)
		return;

	g_cachedMovement.assign(halves.first.begin(), halves.first.begin() + 7);

	const ULONGLONG now = GetTickCount64();

	if (halves.first.size() > kMenuNameField && !halves.first[kMenuNameField].empty())
	{
		const std::string incomingPresence = halves.first[kMenuNameField];
		const bool transitionPending = g_menuTransitionDeadlineTick != 0 && now < g_menuTransitionDeadlineTick;

		if (transitionPending && incomingPresence == "IngameMenu" && !g_presence.empty() && g_presence != "none" && g_presence != "IngameMenu")
		{
			halves.first[kMenuNameField] = g_presence;
		}
		else
		{
			g_presence = incomingPresence;
			g_menuTransitionDeadlineTick = 0;
		}
	}

	g_cachedUpdate1 = halves;
	g_haveCachedUpdate1 = true;

	SendUpdate1Halves(std::move(halves), now);
}

static void SendUpdate2(const std::string &payload)
{
	ParsedHalves halves = ParseValuesSplitHalf(payload);

	std::vector<std::string> movement;

	if (!RemoveMovementPrefix(halves, movement))
		return;

	g_cachedMovement = movement;

	const std::string packetId = BuildLocalPacketId();

	const int updateSequence = NextSequence(g_update2Sequence);

	if (!halves.first.empty())
	{
		halves.first.insert(halves.first.begin(), std::to_string(updateSequence));

		SendUdpPacket(BuildPacket("UPDATE2A", packetId, halves.first), "UPDATE2A");
	}

	if (!halves.second.empty())
	{
		halves.second.insert(halves.second.begin(), std::to_string(updateSequence));

		SendUdpPacket(BuildPacket("UPDATE2B", packetId, halves.second), "UPDATE2B");
	}
}

static void SendCombined(const std::string &payload, const char *opcode)
{
	ParsedHalves halves = ParseValuesSplitHalf(payload);
	std::vector<std::string> movement;
	if (!RemoveMovementPrefix(halves, movement))
		return;

	g_cachedMovement = movement;

	std::vector<std::string> fields;
	fields.reserve(halves.first.size() + halves.second.size());
	fields.insert(fields.end(), halves.first.begin(), halves.first.end());
	fields.insert(fields.end(), halves.second.begin(), halves.second.end());

	if (!fields.empty())
		SendUdpPacket(BuildPacket(opcode, BuildLocalPacketId(), fields), opcode);
}

static std::string PayloadTag(const std::string &payload)
{
	const size_t space = payload.find(' ');
	return space == std::string::npos ? payload : payload.substr(0, space);
}

static void ProcessPresence(const std::string &payload, ULONGLONG now)
{
	const size_t space = payload.find(' ');
	std::string presence = (space == std::string::npos) ? "none" : payload.substr(space + 1);

	if (presence.empty() || presence.size() > 64)
		presence = "none";

	g_menuTransitionDeadlineTick = 0;
	g_presence = presence;

	if (g_haveCachedUpdate1 && g_cachedUpdate1.first.size() > kMenuNameField)
		g_cachedUpdate1.first[kMenuNameField] = presence;

	if (g_haveCachedUpdate1)
		SendUpdate1Halves(g_cachedUpdate1, now);
}

static void ProcessMenuTransition(const std::string &payload, ULONGLONG now)
{
	const size_t space = payload.find(' ');
	const std::string closingMenu = (space == std::string::npos) ? std::string() : payload.substr(space + 1);

	if (!closingMenu.empty() && closingMenu != g_presence)
		return;

	g_menuTransitionDeadlineTick = now + kMenuTransitionGraceMs;
}

static bool CommitDeferredMenuPresenceIfDue()
{
	if (g_menuTransitionDeadlineTick == 0)
		return false;

	const ULONGLONG now = GetTickCount64();
	if (now < g_menuTransitionDeadlineTick)
		return false;

	g_menuTransitionDeadlineTick = 0;

	if (g_presence == "IngameMenu")
		return false;

	g_presence = "IngameMenu";

	if (g_haveCachedUpdate1 && g_cachedUpdate1.first.size() > kMenuNameField)
	{
		g_cachedUpdate1.first[kMenuNameField] = g_presence;
		SendUpdate1Halves(g_cachedUpdate1, now);
		return true;
	}

	return false;
}

static bool SendTransportKeepaliveIfDue()
{
	if (!wo_native::IsConnected() || g_lastScriptActivityTick == 0)
		return false;

	const ULONGLONG now = GetTickCount64();
	if (!ScriptTransportStalled(now))
		return false;

	if (g_lastKeepaliveTick != 0 && now >= g_lastKeepaliveTick && (now - g_lastKeepaliveTick) < kKeepaliveIntervalMs)
	{
		return false;
	}

	bool sent = false;
	const std::string packetId = BuildLocalPacketId();

	if (g_cachedMovement.size() >= 7)
	{
		SendMovementPacket(packetId, g_cachedMovement);
		sent = true;
	}

	if (g_haveCachedUpdate1)
	{
		SendUpdate1Halves(g_cachedUpdate1, now);
		sent = true;
	}

	if (sent)
		g_lastKeepaliveTick = now;

	return sent;
}

static void ProcessOutbound(const std::string &payload)
{
	const std::string tag = PayloadTag(payload);

	if (tag == "offline")
	{
		ResetTransportWatchdog();
		return;
	}

	const bool scriptTransportPayload = tag == "move" || tag == "wo" || tag == "wo2" || tag == "wo3" || tag == "wo4" || tag == "presence" || tag == "presence_transition";

	if (!scriptTransportPayload)
		return;

	const ULONGLONG now = GetTickCount64();
	g_lastScriptActivityTick = now;
	g_lastKeepaliveTick = 0;

	if (tag == "move")
		SendMovementPayload(payload);
	else if (tag == "wo")
		SendUpdate1(payload);
	else if (tag == "wo2")
		SendUpdate2(payload);
	else if (tag == "wo3")
		SendCombined(payload, "UPDATE3");
	else if (tag == "wo4")
		SendCombined(payload, "UPDATE4");
	else if (tag == "presence")
		ProcessPresence(payload, now);
	else if (tag == "presence_transition")
		ProcessMenuTransition(payload, now);
}

static void QueueMovement(int playerId, const std::string &playerUsername, const std::vector<std::string> &movement)
{
	if (playerId <= 0 || playerUsername.empty() || movement.size() < 8)
		return;

	wo_native::InboundMessage message;
	message.opcode = wo_native::InboundOpcode::Move;
	message.playerId = playerId;
	message.sender = playerUsername;

	// sequence + x y z w heading speed area
	message.fields.assign(movement.begin(), movement.begin() + 8);

	wo_native::PushInbound(std::move(message));
}

static void QueueUpdate1(int playerId, const std::string &playerUsername, const std::vector<std::string> &update1A, const std::vector<std::string> &update1B)
{
	if (playerId <= 0 || playerUsername.empty())
		return;

	wo_native::InboundMessage message;
	message.opcode = wo_native::InboundOpcode::Update1;
	message.playerId = playerId;
	message.sender = playerUsername;
	message.fields.reserve(update1A.size() + update1B.size());
	message.fields.insert(message.fields.end(), update1A.begin(), update1A.end());
	message.fields.insert(message.fields.end(), update1B.begin(), update1B.end());

	if (!message.fields.empty())
		wo_native::PushInbound(std::move(message));
}

static void QueueUpdate2(int playerId, const std::string &playerUsername, const std::vector<std::string> &update2A, const std::vector<std::string> &update2B)
{
	if (playerId <= 0 || playerUsername.empty())
		return;

	wo_native::InboundMessage message;
	message.opcode = wo_native::InboundOpcode::Update2;
	message.playerId = playerId;
	message.sender = playerUsername;
	message.fields.reserve(update2A.size() + update2B.size());
	message.fields.insert(message.fields.end(), update2A.begin(), update2A.end());
	message.fields.insert(message.fields.end(), update2B.begin(), update2B.end());

	if (!message.fields.empty())
		wo_native::PushInbound(std::move(message));
}

static void QueueUpdate3(int playerId, const std::string &playerUsername, std::vector<std::string> &&fields)
{
	if (playerId <= 0 || playerUsername.empty() || fields.size() < 6)
		return;

	wo_native::InboundMessage message;
	message.opcode = wo_native::InboundOpcode::Update3;
	message.playerId = playerId;
	message.sender = playerUsername;
	message.fields = std::move(fields);
	wo_native::PushInbound(std::move(message));
}

static void QueueUpdate4(int playerId, const std::string &playerUsername, std::vector<std::string> &&fields)
{
	if (playerId <= 0 || playerUsername.empty() || fields.size() < 16)
		return;

	wo_native::InboundMessage message;
	message.opcode = wo_native::InboundOpcode::Update4;
	message.playerId = playerId;
	message.sender = playerUsername;
	message.fields = std::move(fields);
	wo_native::PushInbound(std::move(message));
}

static void HandleServerPacket(const std::string &msg)
{
	auto parts = SplitTabs(msg);
	if (parts.empty())
		return;

	if (parts[0] == "ERROR")
	{
		if (parts.size() >= 2 && parts[1] == "USERNAME_TAKEN")
			wo_native::PushControl(wo_native::InboundOpcode::UsernameTaken);
		else if (parts.size() >= 2 && parts[1] == "BANNED")
			wo_native::PushControl(wo_native::InboundOpcode::Banned);
		else if (parts.size() >= 2 && parts[1] == "NOT_WHITELISTED")
			wo_native::PushControl(wo_native::InboundOpcode::NotWhitelisted);
		CloseOnlineSession();
		return;
	}

	if (parts[0] == "KICK")
	{
		wo_native::PushControl(wo_native::InboundOpcode::Kicked);
		CloseOnlineSession();
		return;
	}

	const std::string &opcode = parts[0];
	if (opcode != "MOVE" && opcode != "UPDATE1A" && opcode != "UPDATE1B" && opcode != "UPDATE2A" && opcode != "UPDATE2B" && opcode != "UPDATE3" && opcode != "UPDATE4")
	{
		return;
	}

	if (parts.size() < 3)
		return;

	int playerId = 0;
	if (!ParsePositiveInt(parts[1], playerId))
		return;

	std::string playerUsername = parts[2];
	if (playerUsername.empty())
		return;

	if (playerUsername == username)
	{
		g_localPlayerId.store(playerId);
		wo_native::SetLocalId(playerId);
	}

	std::vector<std::string> fields(parts.begin() + 3, parts.end());

	if (opcode == "MOVE")
	{
		QueueMovement(playerId, playerUsername, fields);
		return;
	}

	if (fields.empty())
		return;

	int packetSequence = 0;
	int movementSequence = 0;

	if (opcode == "UPDATE1A" || opcode == "UPDATE1B" || opcode == "UPDATE2A" || opcode == "UPDATE2B")
	{
		if (fields.empty() || !ParsePositiveInt(fields[0], packetSequence))
		{
			return;
		}

		fields.erase(fields.begin());
	}

	if (opcode == "UPDATE1A" || opcode == "UPDATE1B")
	{
		if (fields.empty() || !ParsePositiveInt(fields[0], movementSequence))
		{
			return;
		}

		fields.erase(fields.begin());
	}

	if (opcode == "UPDATE3" || opcode == "UPDATE4")
	{
		bool changed = false;
		{
			std::lock_guard<std::mutex> lock(remoteMu);
			auto &rp = remotePlayers[playerId];
			rp.username = playerUsername;
			std::vector<std::string> &last = (opcode == "UPDATE3") ? rp.lastUpdate3 : rp.lastUpdate4;
			if (fields != last)
			{
				last = fields;
				changed = true;
			}
		}

		if (!changed)
			return;

		if (opcode == "UPDATE3")
			QueueUpdate3(playerId, playerUsername, std::move(fields));
		else
			QueueUpdate4(playerId, playerUsername, std::move(fields));
		return;
	}

	bool push1 = false;
	bool push2 = false;
	std::vector<std::string> u1a;
	std::vector<std::string> u1b;
	std::vector<std::string> u2a;
	std::vector<std::string> u2b;
	std::string pushUsername;

	{
		std::lock_guard<std::mutex> lock(remoteMu);
		auto &rp = remotePlayers[playerId];
		rp.username = playerUsername;

		if (opcode == "UPDATE1A" && packetSequence > rp.lastPushed1Sequence)
		{
			rp.update1A = std::move(fields);
			rp.update1ASequence = packetSequence;
			rp.update1AMovementSequence = movementSequence;
		}
		else if (opcode == "UPDATE1B" && packetSequence > rp.lastPushed1Sequence)
		{
			rp.update1B = std::move(fields);
			rp.update1BSequence = packetSequence;
			rp.update1BMovementSequence = movementSequence;
		}
		else if (opcode == "UPDATE2A" && packetSequence > rp.lastPushed2Sequence)
		{
			rp.update2A = std::move(fields);
			rp.update2ASequence = packetSequence;
		}
		else if (opcode == "UPDATE2B" && packetSequence > rp.lastPushed2Sequence)
		{
			rp.update2B = std::move(fields);
			rp.update2BSequence = packetSequence;
		}

		if (rp.update1ASequence > rp.lastPushed1Sequence && rp.update1ASequence == rp.update1BSequence && rp.update1AMovementSequence == rp.update1BMovementSequence)
		{
			rp.lastPushed1Sequence = rp.update1ASequence;

			u1a = rp.update1A;
			u1b = rp.update1B;

			pushUsername = rp.username;
			push1 = true;
		}

		if (rp.update2ASequence > rp.lastPushed2Sequence && rp.update2ASequence == rp.update2BSequence)
		{
			rp.lastPushed2Sequence = rp.update2ASequence;

			u2a = rp.update2A;
			u2b = rp.update2B;

			pushUsername = rp.username;
			push2 = true;
		}
	}

	if (push1)
		QueueUpdate1(playerId, pushUsername, u1a, u1b);
	if (push2)
		QueueUpdate2(playerId, pushUsername, u2a, u2b);
}

static void SenderThread()
{
	std::string payload;
	bool wasConnected = false;

	while (g_run.load())
	{
		bool worked = false;
		const bool connected = wo_native::IsConnected();

		if (connected && !wasConnected)
			ResetTransportWatchdog();
		else if (!connected && wasConnected)
			ResetTransportWatchdog();

		wasConnected = connected;

		while (g_run.load() && wo_native::PopOutbound(payload))
		{
			if (wo_native::IsConnected())
				ProcessOutbound(payload);
			worked = true;
		}

		if (wo_native::IsConnected() && CommitDeferredMenuPresenceIfDue())
			worked = true;

		if (wo_native::IsConnected() && SendTransportKeepaliveIfDue())
			worked = true;

		if (!worked)
			Sleep(1);
	}
}

static void ReceiverThread()
{
	std::vector<char> data(8192);

	while (g_run.load())
	{
		try
		{
			asio::ip::udp::endpoint senderEndpoint;
			std::size_t len = theSocket.receive_from(asio::buffer(data), senderEndpoint);
			std::string msg(data.data(), len);
			HandleServerPacket(msg);
		}
		catch (const std::exception &e)
		{
			if (g_run.load() && wo_native::IsConnected())
				std::cout << "Receive error: " << e.what() << "\n";

			if (g_run.load())
				Sleep(500);
		}
	}
}

static void connectServer()
{
	if (g_shutdown.load())
		return;

	try
	{
		g_run.store(true);
		theSocket.open(asio::ip::udp::v4());
		theSocket.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
		serverEndpoint = *resolver.resolve(asio::ip::udp::v4(), ip, port).begin();
		theSocket.connect(serverEndpoint);
		wo_native::SetConnected(true);

		// These workers are process-lifetime. Detaching avoids std::thread global
		// destruction/join work during DLL_PROCESS_DETACH. The module is pinned, so
		// their code cannot be unloaded while they are running.
		std::thread(SenderThread).detach();
		std::thread(ReceiverThread).detach();
	}
	catch (const std::exception &e)
	{
		wo_native::SetConnected(false);
		std::cout << "Connection error: " << e.what() << "\n";
	}
}

static void initScript()
{
	fs::path baseDir = getExecutablePath();
	fs::path woDir = baseDir / "WitcherOnline";

	fs::create_directories(woDir);

	wo_native::InitLog((woDir / "WitcherOnline.log").string());

	wo_native::DebugLog("startup");

	fs::path fullPath = woDir / "config.xml";

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(fullPath.c_str());

	if (result)
	{
		pugi::xml_node xml = doc.child("Config");
		if (xml)
		{
			std::string user = xml.child("Username").text().as_string();
			username = std::regex_replace(user, std::regex("[^A-Za-z0-9_]"), "");

			if (username.length() > 16)
				username.resize(16);

			ip = xml.child("ServerIP").text().as_string();
			port = xml.child("Port").text().as_string();

			if (username.length() < 2 || username == "none")
				username = "Player";
		}
	}

	wo_native::SetUsername(username);

	if (!wo_native::RegisterNatives())
	{
		wo_native::DebugLog("failed to register WitcherOnline native functions");
		return;
	}

	connectServer();
}

static DWORD WINAPI InitThreadProc(LPVOID)
{
	if (g_shutdown.load())
		return 0;

	// WitcherOnline owns long-lived networking workers. Keep the ASI loaded for
	// the process lifetime so an ASI loader cannot FreeLibrary it underneath them.
	PinClientModule();

	initScript();
	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hModule);
		g_initThread = CreateThread(nullptr, 0, InitThreadProc, nullptr, 0, nullptr);
		if (g_initThread)
		{
			CloseHandle(g_initThread);
			g_initThread = NULL;
		}
		break;

	case DLL_PROCESS_DETACH:
		break;
	}

	return TRUE;
}