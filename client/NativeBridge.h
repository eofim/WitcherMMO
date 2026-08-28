#pragma once

#include <string>
#include <vector>

namespace wo_native
{
	enum class InboundOpcode
	{
		None = 0,
		Move = 1,
		Update1 = 2,
		Update2 = 3,
		Update3 = 4,
		Update4 = 5,
		UsernameTaken = 6,
		Kicked = 7,
		Banned = 8,
		NotWhitelisted = 9
	};

	struct InboundMessage
	{
		InboundOpcode opcode = InboundOpcode::None;
		int playerId = 0;
		std::string sender;
		std::vector<std::string> fields;
	};

	void InitLog(const std::string &path);
	void ShutdownLog();
	void DebugLog(const std::string &text);

	bool RegisterNatives();

	bool IsConnected();
	void SetConnected(bool connected);
	void SetLocalId(int id);
	void SetUsername(const std::string &name);

	bool PopOutbound(std::string &payload);
	void PushInbound(InboundMessage &&message);
	void PushControl(InboundOpcode opcode);
} // namespace wo_native
