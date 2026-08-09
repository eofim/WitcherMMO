#include "SignatureScanner.h"
#include "pch.h"

#include <cstdint>

namespace wo_native
{
	static bool HexValue(char c, uint8_t &value)
	{
		if (c >= '0' && c <= '9')
		{
			value = static_cast<uint8_t>(c - '0');
			return true;
		}

		if (c >= 'a' && c <= 'f')
		{
			value = static_cast<uint8_t>(c - 'a' + 10);
			return true;
		}

		if (c >= 'A' && c <= 'F')
		{
			value = static_cast<uint8_t>(c - 'A' + 10);
			return true;
		}

		return false;
	}

	SignaturePattern SignaturePattern::Parse(const std::string &pattern)
	{
		SignaturePattern result;
		size_t index = 0;

		while (index < pattern.size())
		{
			const char c = pattern[index];

			if (c == ' ')
			{
				++index;
				continue;
			}

			if (c == '?')
			{
				result.bytes_.push_back(0);
				result.mask_.push_back(false);
				++index;
				if (index < pattern.size() && pattern[index] == '?')
					++index;
				continue;
			}

			uint8_t high = 0;
			uint8_t low = 0;

			if (index + 1 >= pattern.size() || !HexValue(pattern[index], high) || !HexValue(pattern[index + 1], low))
			{
				result.bytes_.clear();
				result.mask_.clear();
				return result;
			}

			result.bytes_.push_back(static_cast<uint8_t>((high << 4) | low));
			result.mask_.push_back(true);
			index += 2;
		}

		return result;
	}

	bool SignaturePattern::MatchesAt(const uint8_t *address) const
	{
		for (size_t i = 0; i < bytes_.size(); ++i)
		{
			if (mask_[i] && address[i] != bytes_[i])
				return false;
		}

		return true;
	}

	std::vector<uint8_t *> SignatureScanner::FindAll(const ModuleRegion &region, const SignaturePattern &pattern, size_t limit)
	{
		std::vector<uint8_t *> matches;

		if (!region.IsValid() || !pattern.IsValid() || pattern.Size() > region.size)
			return matches;

		const size_t end = region.size - pattern.Size();

		for (size_t offset = 0; offset <= end; ++offset)
		{
			uint8_t *candidate = region.base + offset;

			if (!pattern.MatchesAt(candidate))
				continue;

			matches.push_back(candidate);

			if (limit != 0 && matches.size() >= limit)
				break;
		}

		return matches;
	}

	uint8_t *SignatureScanner::ResolveRelative(uint8_t *instruction, int operandOffset, int instructionLength)
	{
		if (!instruction)
			return nullptr;

		const int32_t displacement = *reinterpret_cast<int32_t *>(instruction + operandOffset);
		return instruction + instructionLength + displacement;
	}
} // namespace wo_native
