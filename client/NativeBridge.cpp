#include "NativeBridge.h"
#include "pch.h"
#include <WitcherNativeBridgeApi.h>

#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <fstream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace wo_native
{
	static std::mutex g_logMutex;
	static std::ofstream g_logFile;
	static std::atomic<bool> g_registrationAttempted{false};
	static std::atomic<bool> g_registrationSucceeded{false};

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

	static uint32_t LogicalLength(const WNB_String &text)
	{
		return (text.data && text.size > 0) ? text.size - 1 : 0;
	}

	static std::string NarrowPayload(const WNB_String &text)
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

	static bool ReturnString(void *frame, void *result, const std::string &text)
	{
		const std::wstring wide(text.begin(), text.end());
		return WNB_ReturnString(frame, result, wide.c_str(), static_cast<uint32_t>(wide.size()));
	}

	static void ReturnName(void *frame, void *result, const std::string &text)
	{
		const std::wstring wide(text.begin(), text.end());
		WNB_ReturnNameFromString(frame, result, wide.c_str());
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
		WNB_String text{};
		const int size = WNB_ReadStringParameter(frame, &text);

		bool queued = false;

		if (!g_connected.load())
		{
			WNB_ReturnBool(frame, result, false);
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

		WNB_ReturnBool(frame, result, queued);
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

		WNB_ReturnInt(frame, result, mask);
	}

	static void WO_Poll(void *, void *frame, void *result)
	{
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

		WNB_ReturnInt(frame, result, opcode);
	}

	static void WO_Str(void *, void *frame, void *result)
	{
		const int index = WNB_ReadIntParameter(frame);

		if (index == -1)
		{
			ReturnString(frame, result, g_current.sender);
			return;
		}
		if (index == -2)
		{
			ReturnString(frame, result, std::to_string(g_current.playerId));
			return;
		}
		if (index == -3)
		{
			ReturnString(frame, result, std::to_string(g_current.fields.size()));
			return;
		}
		if (index == -4)
		{
			std::string localName;
			{
				std::lock_guard<std::mutex> lock(g_stateMutex);
				localName = g_username;
			}
			ReturnString(frame, result, localName);
			return;
		}
		if (index == -5)
		{
			int localId = 0;
			{
				std::lock_guard<std::mutex> lock(g_stateMutex);
				localId = g_localId;
			}
			ReturnString(frame, result, std::to_string(localId));
			return;
		}

		ReturnString(frame, result, CurrentField(index));
	}

	static void WO_NameAt(void *, void *frame, void *result)
	{
		const int index = WNB_ReadIntParameter(frame);

		if (index == -1)
		{
			ReturnName(frame, result, g_current.sender);
			return;
		}

		ReturnName(frame, result, CurrentField(index));
	}

	bool RegisterNatives()
	{
		if (g_registrationAttempted.exchange(true))
			return g_registrationSucceeded.load();

		const uint32_t apiVersion = WNB_GetApiVersion();
		if (apiVersion < WNB_API_VERSION)
		{
			DebugLog("WitcherNativeBridge API is too old: found " + std::to_string(apiVersion) + ", need " + std::to_string(WNB_API_VERSION));
			return false;
		}

		struct NativeRegistration
		{
			const char *name;
			WNB_NativeImplementation implementation;
		};

		const NativeRegistration registrations[] = {
		    {"WO_Send", &WO_Send}, {"WO_Poll", &WO_Poll}, {"WO_Tick", &WO_Tick}, {"WO_Str", &WO_Str}, {"WO_NameAt", &WO_NameAt},
		};

		int registeredCount = 0;
		bool success = true;

		for (const NativeRegistration &registration : registrations)
		{
			if (WNB_RegisterNative(registration.name, registration.implementation))
			{
				++registeredCount;
				continue;
			}

			DebugLog(std::string("failed to register native through WitcherNativeBridge: ") + registration.name);
			success = false;
		}

		g_registrationSucceeded.store(success);

		DebugLog("WitcherNativeBridge API version=" + std::to_string(apiVersion) + " registered WitcherOnline natives=" + std::to_string(registeredCount) + "/" + std::to_string(sizeof(registrations) / sizeof(registrations[0])));

		return success;
	}

	bool IsConnected()
	{
		return g_connected.load();
	}
} // namespace wo_native
