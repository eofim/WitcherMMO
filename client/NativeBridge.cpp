#include "NativeBridge.h"
#include "GameModule.h"
#include "InlineHook.h"
#include "SignatureScanner.h"
#include "pch.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wo_native
{
	struct ScriptApi
	{
		void *alloc = nullptr;
		void *memset = nullptr;
		void *namePool = nullptr;
		void *addName = nullptr;
		void *functionCtor = nullptr;
		void *scriptSystem = nullptr;
		void *registerGlobal = nullptr;
		void *opcodeTable = nullptr;
		void *bufferAlloc = nullptr;
		void *bufferCopy = nullptr;
		const wchar_t **emptyString = nullptr;
		const int *emptyStringLength = nullptr;
		size_t matches = 0;
		size_t consistent = 0;

		bool IsComplete() const
		{
			return alloc && memset && namePool && addName && functionCtor && scriptSystem && registerGlobal;
		}

		bool CanMarshalStrings() const
		{
			return opcodeTable && bufferAlloc && bufferCopy && emptyString && emptyStringLength;
		}
	};

	using AllocFn = void *(*)(size_t size, size_t alignment);
	using MemsetFn = void *(*)(void *destination, int value, size_t size);
	using NamePoolFn = void *(*)();
	using AddNameFn = int (*)(void *pool, const wchar_t *name);
	using FunctionCtorFn = void *(*)(void *self, int *nameIndex, void *implementation);
	using ScriptSystemFn = void *(*)();
	using RegisterGlobalFn = void (*)(void *system, void *function);
	using OpcodeHandlerFn = void (*)(void *context, void *frame, void *destination);
	using BufferAllocFn = void *(*)(size_t zero, size_t bytes, size_t kind, size_t tag);
	using BufferCopyFn = void *(*)(void *destination, const void *source, size_t bytes);

	struct RedString
	{
		wchar_t *data;
		uint32_t size;
		uint32_t padding;
	};

	static const char *kRegistrationSignature = "BA 10 00 00 00 B9 C0 00 00 00 E8 ?? ?? ?? ?? 48 8B F8 48 85 C0 74 ?? "
	                                            "33 D2 41 B8 C0 00 00 00 48 8B C8 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? "
	                                            "48 8D 15 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? "
	                                            "89 45 10 48 8D 55 10 48 8B CF E8 ?? ?? ?? ?? "
	                                            "48 8B F8 EB ?? 48 8B FB E8 ?? ?? ?? ?? 48 8B C8 48 8B D7 E8 ?? ?? ?? ??";

	static const int kOffsetName = 44;
	static const int kOffsetImpl = 59;
	static const int kOffsetAlloc = 10;
	static const int kOffsetMemset = 34;
	static const int kOffsetNamePool = 39;
	static const int kOffsetAddName = 54;
	static const int kOffsetFunctionCtor = 76;
	static const int kOffsetScriptSystem = 89;
	static const int kOffsetRegisterGlobal = 100;
	static const size_t kMinimumMatches = 32;

	static ScriptApi g_api;
	static bool g_bindingAttempted = false;
	static bool g_bindingResolved = false;
	static std::map<std::string, void *> g_natives;
	static InlineHook g_registerHook;
	static std::atomic<bool> g_registrationDone{false};
	static std::atomic<int> g_registeredCount{0};
	static std::string g_registrationError;

	static std::mutex g_logMutex;
	static std::ofstream g_logFile;

	void InitLog(const std::string &path)
	{
		std::lock_guard<std::mutex> lock(g_logMutex);

		g_logFile.open(path, std::ios::out | std::ios::trunc);
	}

	void ShutdownLog()
	{
		std::lock_guard<std::mutex> lock(g_logMutex);

		if (g_logFile.is_open())
		{
			g_logFile.flush();
			g_logFile.close();
		}
	}

	void DebugLog(const std::string &text)
	{
		SYSTEMTIME now{};
		GetLocalTime(&now);

		char time[32]{};

		sprintf_s(time, sizeof(time), "%02u:%02u:%02u.%03u", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

		const std::string message = std::string(time) + " WitcherOnline debug: " + text;

		OutputDebugStringA((message + "\n").c_str());

		std::lock_guard<std::mutex> lock(g_logMutex);

		if (g_logFile.is_open())
		{
			g_logFile << message << "\n";
			g_logFile.flush();
		}
	}

	static void *ResolveCall(uint8_t *site, int offset)
	{
		return SignatureScanner::ResolveRelative(site + offset, 1, 5);
	}

	static void *ResolveLea(uint8_t *site, int offset)
	{
		return SignatureScanner::ResolveRelative(site + offset, 3, 7);
	}

	static std::string NarrowUtf16(const wchar_t *text, size_t limit = 96)
	{
		std::string out;
		if (!text)
			return out;

		for (size_t i = 0; i < limit && text[i]; ++i)
		{
			const wchar_t c = text[i];
			if (c < 32 || c > 126)
				return std::string();
			out += static_cast<char>(c);
		}

		return out;
	}

	static bool IsInsideImage(void *candidate)
	{
		const ModuleRegion &image = GameModule::Image();
		if (!candidate || !image.IsValid())
			return false;

		uint8_t *address = static_cast<uint8_t *>(candidate);
		return address >= image.base && address < image.base + image.size;
	}

	static bool IsPopulatedCodeTable(void *candidate, size_t required = 32)
	{
		if (!IsInsideImage(candidate))
			return false;

		const ModuleRegion &text = GameModule::Text();
		uint8_t **entries = static_cast<uint8_t **>(candidate);
		size_t valid = 0;

		for (size_t i = 0; i < required; ++i)
		{
			uint8_t *entry = entries[i];
			if (!entry)
				continue;
			if (entry < text.base || entry >= text.base + text.size)
				return false;
			valid++;
		}

		return valid * 2 >= required;
	}

	static void *FindNative(const std::string &name)
	{
		auto it = g_natives.find(name);
		return it == g_natives.end() ? nullptr : it->second;
	}

	static void ResolveStringMarshalling(ScriptApi &api)
	{
		void *logChannel = FindNative("LogChannel");
		if (!logChannel)
			return;

		uint8_t *code = static_cast<uint8_t *>(logChannel);

		for (int i = 0; i < 0x40; ++i)
		{
			if (code[i] != 0x48 && code[i] != 0x4C)
				continue;
			if (code[i + 1] != 0x8D)
				continue;
			if ((code[i + 2] & 0xC7) != 0x05)
				continue;

			void *candidate = SignatureScanner::ResolveRelative(code + i, 3, 7);
			if (IsInsideImage(candidate))
			{
				api.opcodeTable = candidate;
				break;
			}
		}

		SignaturePattern allocPattern = SignaturePattern::Parse("44 8D 49 0E 44 8D 41 02 E8 ?? ?? ?? ??");

		for (int i = 0; i < 0xC0; ++i)
		{
			if (allocPattern.MatchesAt(code + i))
			{
				api.bufferAlloc = SignatureScanner::ResolveRelative(code + i + 8, 1, 5);
				break;
			}
		}

		for (int i = 0; i < 0xC0; ++i)
		{
			if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x15)
			{
				void *slot = SignatureScanner::ResolveRelative(code + i, 3, 7);
				api.emptyString = static_cast<const wchar_t **>(slot);
				break;
			}
		}

		for (int i = 0; i < 0x40; ++i)
		{
			if (code[i] == 0x8B && code[i + 1] == 0x05)
			{
				api.emptyStringLength = reinterpret_cast<const int *>(SignatureScanner::ResolveRelative(code + i, 2, 6));
				break;
			}
		}

		if (api.bufferAlloc)
		{
			for (int i = 0; i < 0xC0; ++i)
			{
				if (code[i] != 0xE8)
					continue;

				void *target = SignatureScanner::ResolveRelative(code + i, 1, 5);
				if (target != api.bufferAlloc)
					continue;

				for (int j = i + 5; j < i + 0x30; ++j)
				{
					if (code[j] == 0xE8)
					{
						api.bufferCopy = SignatureScanner::ResolveRelative(code + j, 1, 5);
						break;
					}
				}
				break;
			}
		}
	}

	static void *Consensus(const std::vector<void *> &values, size_t &agreeing)
	{
		std::map<void *, size_t> tally;
		for (void *value : values)
		{
			if (value)
				tally[value]++;
		}

		void *best = nullptr;
		agreeing = 0;
		for (const auto &entry : tally)
		{
			if (entry.second > agreeing)
			{
				agreeing = entry.second;
				best = entry.first;
			}
		}
		return best;
	}

	bool ResolveScriptApi()
	{
		if (g_bindingAttempted)
			return g_bindingResolved;

		g_bindingAttempted = true;

		if (!GameModule::Resolve() || GameModule::IsSelfHosted())
			return false;

		SignaturePattern pattern = SignaturePattern::Parse(kRegistrationSignature);
		if (!pattern.IsValid())
			return false;

		std::vector<uint8_t *> sites = SignatureScanner::FindAll(GameModule::Text(), pattern);
		g_api.matches = sites.size();

		if (sites.size() < kMinimumMatches)
			return false;

		std::vector<void *> allocs, memsets, pools, addNames, ctors, systems, registrars;
		allocs.reserve(sites.size());
		memsets.reserve(sites.size());
		pools.reserve(sites.size());
		addNames.reserve(sites.size());
		ctors.reserve(sites.size());
		systems.reserve(sites.size());
		registrars.reserve(sites.size());

		g_natives.clear();

		for (uint8_t *site : sites)
		{
			const wchar_t *nameText = static_cast<const wchar_t *>(ResolveLea(site, kOffsetName));
			void *impl = ResolveLea(site, kOffsetImpl);

			if (nameText && impl)
			{
				const std::string decoded = NarrowUtf16(nameText);
				if (!decoded.empty())
					g_natives[decoded] = impl;
			}

			allocs.push_back(ResolveCall(site, kOffsetAlloc));
			memsets.push_back(ResolveCall(site, kOffsetMemset));
			pools.push_back(ResolveCall(site, kOffsetNamePool));
			addNames.push_back(ResolveCall(site, kOffsetAddName));
			ctors.push_back(ResolveCall(site, kOffsetFunctionCtor));
			systems.push_back(ResolveCall(site, kOffsetScriptSystem));
			registrars.push_back(ResolveCall(site, kOffsetRegisterGlobal));
		}

		size_t agree = 0;
		size_t worst = sites.size();

		g_api.alloc = Consensus(allocs, agree);
		worst = (std::min)(worst, agree);
		g_api.memset = Consensus(memsets, agree);
		worst = (std::min)(worst, agree);
		g_api.namePool = Consensus(pools, agree);
		worst = (std::min)(worst, agree);
		g_api.addName = Consensus(addNames, agree);
		worst = (std::min)(worst, agree);
		g_api.functionCtor = Consensus(ctors, agree);
		worst = (std::min)(worst, agree);
		g_api.scriptSystem = Consensus(systems, agree);
		worst = (std::min)(worst, agree);
		g_api.registerGlobal = Consensus(registrars, agree);
		worst = (std::min)(worst, agree);

		g_api.consistent = worst;
		g_bindingResolved = g_api.IsComplete() && worst == sites.size();

		if (g_bindingResolved)
			ResolveStringMarshalling(g_api);

		return g_bindingResolved;
	}

	static constexpr size_t kFunctionSize = 0xC0;
	static constexpr size_t kFunctionAlignment = 0x10;
	static constexpr size_t kMaxQueueDepth = 4096;

	static std::mutex g_queueMutex;
	static std::deque<std::string> g_outbound;
	static std::array<std::string, 5> g_outboundRealtime;
	static std::array<bool, 5> g_outboundRealtimePending{};
	static size_t g_outboundRealtimeCursor = 0;
	static std::list<InboundMessage> g_inbound;
	static std::unordered_map<unsigned long long, std::list<InboundMessage>::iterator> g_inboundReplaceable;
	static InboundMessage g_current;

	static std::mutex g_stateMutex;
	static int g_localId = 0;
	static std::string g_username;
	static std::atomic<bool> g_connected{false};

	static bool IsReplaceableInbound(InboundOpcode opcode)
	{
		return opcode == InboundOpcode::Move || opcode == InboundOpcode::Update1 || opcode == InboundOpcode::Update2 || opcode == InboundOpcode::Update3 || opcode == InboundOpcode::Update4;
	}

	static unsigned long long InboundKey(const InboundMessage &message)
	{
		return (static_cast<unsigned long long>(static_cast<unsigned int>(message.playerId)) << 8) | static_cast<unsigned int>(message.opcode);
	}

	static int OutboundRealtimeSlot(const std::string &payload)
	{
		if (payload.rfind("move ", 0) == 0)
			return 0;
		if (payload.rfind("wo ", 0) == 0)
			return 1;
		if (payload.rfind("wo2 ", 0) == 0)
			return 2;
		if (payload.rfind("wo3 ", 0) == 0)
			return 3;
		if (payload.rfind("wo4 ", 0) == 0)
			return 4;
		return -1;
	}

	static size_t OutboundDepthLocked()
	{
		size_t depth = g_outbound.size();
		for (bool pending : g_outboundRealtimePending)
		{
			if (pending)
				depth++;
		}
		return depth;
	}

	static void AdvanceFrame(void *frame)
	{
		uint8_t **code = reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(frame) + 0x30);
		if (*code)
			*code += 1;
	}

	static void ReadParameter(void *frame, void *destination)
	{
		uint8_t **code = reinterpret_cast<uint8_t **>(static_cast<uint8_t *>(frame) + 0x30);
		if (!*code)
			return;

		const uint8_t opcode = **code;
		*code += 1;

		void *context = *reinterpret_cast<void **>(frame);
		void *table = g_api.opcodeTable;

		if (!IsPopulatedCodeTable(table))
			return;

		OpcodeHandlerFn handler = static_cast<OpcodeHandlerFn *>(table)[opcode];
		if (!handler)
			return;

		handler(context, frame, destination);
	}

	static bool MakeEmptyString(RedString &value)
	{
		if (!g_api.CanMarshalStrings())
			return false;

		BufferAllocFn allocate = reinterpret_cast<BufferAllocFn>(g_api.bufferAlloc);
		BufferCopyFn copy = reinterpret_cast<BufferCopyFn>(g_api.bufferCopy);

		const int length = *g_api.emptyStringLength;
		const wchar_t *source = *g_api.emptyString;

		if (length < 0 || length > 64)
			return false;

		value.data = nullptr;
		value.size = static_cast<uint32_t>(length);
		value.padding = 0;

		if (length == 0)
			return true;
		if (!source)
			return false;

		void *buffer = allocate(0, static_cast<size_t>(length) * 2, 2, 14);
		if (!buffer)
			return false;

		copy(buffer, source, static_cast<size_t>(length) * 2);
		value.data = static_cast<wchar_t *>(buffer);
		return true;
	}

	static int ReadStringParameter(void *frame, RedString &text)
	{
		__try
		{
			if (!MakeEmptyString(text))
				return -2;

			ReadParameter(frame, &text);
			return static_cast<int>(text.size);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return -3;
		}
	}

	static int ReadIntParameter(void *frame)
	{
		int value = 0;
		__try
		{
			ReadParameter(frame, &value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
		return value;
	}

	static uint32_t LogicalLength(const RedString &text)
	{
		return (text.data && text.size > 0) ? text.size - 1 : 0;
	}

	static std::string NarrowPayload(const RedString &text)
	{
		std::string out;
		if (!text.data)
			return out;

		const uint32_t length = LogicalLength(text);
		out.reserve(length);

		for (uint32_t i = 0; i < length; ++i)
		{
			const wchar_t c = text.data[i];
			out += (c < 256) ? static_cast<char>(c) : '?';
		}
		return out;
	}

	static bool WriteStringResultUnguarded(void *result, const wchar_t *text, size_t length)
	{
		if (!result || !g_api.CanMarshalStrings())
			return false;

		BufferAllocFn allocate = reinterpret_cast<BufferAllocFn>(g_api.bufferAlloc);
		BufferCopyFn copy = reinterpret_cast<BufferCopyFn>(g_api.bufferCopy);
		const size_t characters = length + 1;

		void *buffer = allocate(0, characters * sizeof(wchar_t), 2, 14);
		if (!buffer)
			return false;

		copy(buffer, text, characters * sizeof(wchar_t));

		RedString *destination = static_cast<RedString *>(result);
		destination->data = static_cast<wchar_t *>(buffer);
		destination->size = static_cast<uint32_t>(characters);
		destination->padding = 0;
		return true;
	}

	static bool WriteStringResultGuarded(void *result, const wchar_t *text, size_t length)
	{
		__try
		{
			return WriteStringResultUnguarded(result, text, length);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static bool WriteStringResult(void *result, const std::string &text)
	{
		std::wstring wide(text.begin(), text.end());
		return WriteStringResultGuarded(result, wide.c_str(), wide.size());
	}

	static int InternNameUnguarded(const wchar_t *text)
	{
		NamePoolFn pool = reinterpret_cast<NamePoolFn>(g_api.namePool);
		AddNameFn addName = reinterpret_cast<AddNameFn>(g_api.addName);

		void *namePool = pool();
		if (!namePool)
			return 0;

		return addName(namePool, text);
	}

	static int InternNameGuarded(const wchar_t *text)
	{
		__try
		{
			return InternNameUnguarded(text);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return 0;
		}
	}

	static void WriteNameResult(void *result, const std::string &text)
	{
		if (!result)
			return;

		if (text.empty() || text == "None")
		{
			*static_cast<int *>(result) = 0;
			return;
		}

		std::wstring wide(text.begin(), text.end());
		*static_cast<int *>(result) = InternNameGuarded(wide.c_str());
	}

	static const std::string &CurrentField(int index)
	{
		static const std::string empty;
		if (index < 0 || static_cast<size_t>(index) >= g_current.fields.size())
			return empty;
		return g_current.fields[static_cast<size_t>(index)];
	}

	bool PopOutbound(std::string &payload)
	{
		std::lock_guard<std::mutex> lock(g_queueMutex);

		if (g_outboundRealtimePending[0])
		{
			payload = std::move(g_outboundRealtime[0]);
			g_outboundRealtime[0].clear();
			g_outboundRealtimePending[0] = false;
			return true;
		}

		for (size_t checked = 0; checked < 4; ++checked)
		{
			const size_t slot = 1 + ((g_outboundRealtimeCursor + checked) % 4);
			if (!g_outboundRealtimePending[slot])
				continue;

			payload = std::move(g_outboundRealtime[slot]);
			g_outboundRealtime[slot].clear();
			g_outboundRealtimePending[slot] = false;
			g_outboundRealtimeCursor = (slot - 1 + 1) % 4;
			return true;
		}

		if (!g_outbound.empty())
		{
			payload = std::move(g_outbound.front());
			g_outbound.pop_front();
			return true;
		}

		return false;
	}

	void PushInbound(InboundMessage &&message)
	{
		std::lock_guard<std::mutex> lock(g_queueMutex);
		const bool replaceable = IsReplaceableInbound(message.opcode);
		const unsigned long long key = replaceable ? InboundKey(message) : 0;

		if (replaceable)
		{
			auto found = g_inboundReplaceable.find(key);
			if (found != g_inboundReplaceable.end())
			{
				*(found->second) = std::move(message);
				return;
			}
		}

		if (g_inbound.size() >= kMaxQueueDepth)
		{
			if (IsReplaceableInbound(g_inbound.front().opcode))
				g_inboundReplaceable.erase(InboundKey(g_inbound.front()));
			g_inbound.pop_front();
		}

		g_inbound.push_back(std::move(message));
		if (replaceable)
		{
			auto inserted = g_inbound.end();
			--inserted;
			g_inboundReplaceable[key] = inserted;
		}
	}

	void SetLocalId(int id)
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_localId = id;
	}

	void SetUsername(const std::string &name)
	{
		std::lock_guard<std::mutex> lock(g_stateMutex);
		g_username = name;
	}

	void SetConnected(bool connected)
	{
		g_connected.store(connected);
	}

	void PushControl(InboundOpcode opcode)
	{
		InboundMessage message;
		message.opcode = opcode;
		PushInbound(std::move(message));
	}

	static void WO_Send(void *, void *frame, void *result)
	{
		RedString text{};
		const int size = ReadStringParameter(frame, text);
		AdvanceFrame(frame);

		bool queued = false;

		if (!g_connected.load())
		{
			if (result)
				*static_cast<bool *>(result) = false;
			return;
		}

		if (size > 0)
		{
			std::string payload = NarrowPayload(text);
			if (!payload.empty())
			{
				std::lock_guard<std::mutex> lock(g_queueMutex);
				const int slot = OutboundRealtimeSlot(payload);

				if (slot >= 0)
				{
					g_outboundRealtime[slot] = std::move(payload);
					g_outboundRealtimePending[slot] = true;
				}
				else
				{
					if (g_outbound.size() >= kMaxQueueDepth)
						g_outbound.pop_front();
					g_outbound.push_back(std::move(payload));
				}

				queued = true;
			}
		}

		if (result)
			*static_cast<bool *>(result) = queued;
	}

	static constexpr ULONGLONG kWoTransportIntervalMs = 20;
	static constexpr ULONGLONG kWoGather1IntervalMs = 50;
	static constexpr ULONGLONG kWoGather2IntervalMs = 500;
	static constexpr ULONGLONG kWoGather3IntervalMs = 100;
	static constexpr ULONGLONG kWoGather4IntervalMs = 50;

	static ULONGLONG g_woNextTransportMs = 0;
	static ULONGLONG g_woNextGather1Ms = 0;
	static ULONGLONG g_woNextGather2Ms = 0;
	static ULONGLONG g_woNextGather3Ms = 0;
	static ULONGLONG g_woNextGather4Ms = 0;
	static bool g_woTickWasConnected = false;

	static void ResetWoTickSchedule()
	{
		g_woNextTransportMs = 0;
		g_woNextGather1Ms = 0;
		g_woNextGather2Ms = 0;
		g_woNextGather3Ms = 0;
		g_woNextGather4Ms = 0;
	}

	static bool WoTickDue(ULONGLONG now, ULONGLONG &next, ULONGLONG interval)
	{
		if (next == 0)
		{
			next = now + interval;
			return true;
		}

		if (now < next)
			return false;

		if ((now - next) > interval * 8ULL)
		{
			next = now + interval;
		}
		else
		{
			do
			{
				next += interval;
			} while (next <= now);
		}

		return true;
	}

	static void WO_Tick(void *, void *frame, void *result)
	{
		AdvanceFrame(frame);

		int mask = 0;
		const bool connected = g_connected.load();

		if (!connected)
		{
			ResetWoTickSchedule();
			g_woTickWasConnected = false;
		}
		else
		{
			if (!g_woTickWasConnected)
			{
				ResetWoTickSchedule();
				g_woTickWasConnected = true;
			}

			const ULONGLONG now = GetTickCount64();

			if (WoTickDue(now, g_woNextTransportMs, kWoTransportIntervalMs))
				mask += 1;
			if (WoTickDue(now, g_woNextGather1Ms, kWoGather1IntervalMs))
				mask += 2;
			if (WoTickDue(now, g_woNextGather2Ms, kWoGather2IntervalMs))
				mask += 4;
			if (WoTickDue(now, g_woNextGather3Ms, kWoGather3IntervalMs))
				mask += 8;
			if (WoTickDue(now, g_woNextGather4Ms, kWoGather4IntervalMs))
				mask += 16;
		}

		if (result)
			*static_cast<int *>(result) = mask;
	}

	static void WO_Poll(void *, void *frame, void *result)
	{
		AdvanceFrame(frame);
		int opcode = -1;

		{
			std::lock_guard<std::mutex> lock(g_queueMutex);
			if (!g_inbound.empty())
			{
				g_current = std::move(g_inbound.front());
				if (IsReplaceableInbound(g_current.opcode))
					g_inboundReplaceable.erase(InboundKey(g_current));
				g_inbound.pop_front();
				opcode = static_cast<int>(g_current.opcode);
			}
			else
			{
				g_current = InboundMessage();
			}
		}

		if (result)
			*static_cast<int *>(result) = opcode;
	}

	static void WO_Str(void *, void *frame, void *result)
	{
		const int index = ReadIntParameter(frame);
		AdvanceFrame(frame);

		if (index == -1)
		{
			WriteStringResult(result, g_current.sender);
			return;
		}
		if (index == -2)
		{
			WriteStringResult(result, std::to_string(g_current.playerId));
			return;
		}
		if (index == -3)
		{
			WriteStringResult(result, std::to_string(g_current.fields.size()));
			return;
		}
		if (index == -4)
		{
			std::string localName;
			{
				std::lock_guard<std::mutex> lock(g_stateMutex);
				localName = g_username;
			}
			WriteStringResult(result, localName);
			return;
		}
		if (index == -5)
		{
			int localId = 0;
			{
				std::lock_guard<std::mutex> lock(g_stateMutex);
				localId = g_localId;
			}
			WriteStringResult(result, std::to_string(localId));
			return;
		}

		WriteStringResult(result, CurrentField(index));
	}

	static void WO_NameAt(void *, void *frame, void *result)
	{
		const int index = ReadIntParameter(frame);
		AdvanceFrame(frame);

		if (index == -1)
		{
			WriteNameResult(result, g_current.sender);
			return;
		}

		WriteNameResult(result, CurrentField(index));
	}

	static void *RegisterNativeUnguarded(const wchar_t *name, void *implementation)
	{
		AllocFn alloc = reinterpret_cast<AllocFn>(g_api.alloc);
		MemsetFn zero = reinterpret_cast<MemsetFn>(g_api.memset);
		NamePoolFn pool = reinterpret_cast<NamePoolFn>(g_api.namePool);
		AddNameFn addName = reinterpret_cast<AddNameFn>(g_api.addName);
		FunctionCtorFn construct = reinterpret_cast<FunctionCtorFn>(g_api.functionCtor);
		ScriptSystemFn system = reinterpret_cast<ScriptSystemFn>(g_api.scriptSystem);
		RegisterGlobalFn registerGlobal = reinterpret_cast<RegisterGlobalFn>(g_registerHook.IsInstalled() ? g_registerHook.Trampoline() : g_api.registerGlobal);

		void *storage = alloc(kFunctionSize, kFunctionAlignment);
		if (!storage)
			return nullptr;

		zero(storage, 0, kFunctionSize);

		void *namePool = pool();
		if (!namePool)
			return nullptr;

		int nameIndex = addName(namePool, name);
		void *function = construct(storage, &nameIndex, implementation);
		if (!function)
			return nullptr;

		void *scriptSystem = system();
		if (!scriptSystem)
			return nullptr;

		registerGlobal(scriptSystem, function);
		return function;
	}

	static bool RegisterNativeGuarded(const wchar_t *name, void *implementation)
	{
		__try
		{
			return RegisterNativeUnguarded(name, implementation) != nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	static void RegisterOne(const wchar_t *name, void *implementation, const char *label)
	{
		if (RegisterNativeGuarded(name, implementation))
			g_registeredCount.fetch_add(1);
		else
			g_registrationError += std::string(" ") + label;
	}

	static void RegisterOurNatives()
	{
		if (g_registrationDone.exchange(true))
			return;

		if (!g_api.CanMarshalStrings())
		{
			g_registrationError = "string marshalling unavailable";
			DebugLog(g_registrationError);
			return;
		}

		RegisterOne(L"WO_Send", reinterpret_cast<void *>(&WO_Send), "WO_Send");
		RegisterOne(L"WO_Poll", reinterpret_cast<void *>(&WO_Poll), "WO_Poll");
		RegisterOne(L"WO_Tick", reinterpret_cast<void *>(&WO_Tick), "WO_Tick");
		RegisterOne(L"WO_Str", reinterpret_cast<void *>(&WO_Str), "WO_Str");
		RegisterOne(L"WO_NameAt", reinterpret_cast<void *>(&WO_NameAt), "WO_NameAt");

		DebugLog("registered native functions=" + std::to_string(g_registeredCount.load()) + (g_registrationError.empty() ? "" : " failed:" + g_registrationError));
	}

	static void RegisterGlobalDetour(void *system, void *function)
	{
		RegisterGlobalFn original = reinterpret_cast<RegisterGlobalFn>(g_registerHook.Trampoline());
		original(system, function);

		if (!g_registrationDone.load())
			RegisterOurNatives();
	}

	bool InstallRegistrationHook()
	{
		if (!g_bindingResolved)
			return false;

		if (g_registerHook.IsInstalled())
			return true;

		const std::vector<uint8_t> prologue = {0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20};

		if (!g_registerHook.Install(g_api.registerGlobal, reinterpret_cast<void *>(&RegisterGlobalDetour), prologue))
		{
			g_registrationError = g_registerHook.Error();
			return false;
		}

		return true;
	}

	void RemoveRegistrationHook()
	{
		g_registerHook.Remove();
	}

	bool CanMarshalStrings()
	{
		return g_api.CanMarshalStrings();
	}

	const std::string &RegistrationError()
	{
		return g_registrationError;
	}

	bool IsConnected()
	{
		return g_connected.load();
	}
} // namespace wo_native
