#pragma once
#include <chrono>

class FTimeout
{
public:
	FTimeout(std::chrono::system_clock::duration duration)
	{
		Start = std::chrono::system_clock::now();
		Timeout = duration;
	}

	std::chrono::system_clock::duration GetElapsedTime() const
	{
		auto ElapseTime = (std::chrono::system_clock::now() - Start);
		return ElapseTime;
	}

	std::chrono::system_clock::duration GetRemainingTime() const
	{
		if (Timeout == std::chrono::system_clock::duration::max())
		{
			return Timeout;
		}
		return Timeout - GetElapsedTime();
	}

	bool IsExpired() const
	{
		return GetRemainingTime() <= std::chrono::system_clock::duration::zero();
	}

	static FTimeout Never()
	{
		return FTimeout{ std::chrono::system_clock::duration::max() };
	}

	std::chrono::system_clock::duration GetTimeout() const
	{
		return Timeout;
	}

	uint32_t GetRemainingRoundedUpMilliseconds() const
	{
		if (*this == Never())
		{
			return std::numeric_limits<uint32_t>::max();
		}

		int64_t RemainingMsecs = std::chrono::duration_cast<std::chrono::milliseconds>(GetRemainingTime()).count();
		int64_t RemainingMsecsClamped = std::max(0ll, std::min(RemainingMsecs, (int64_t)std::numeric_limits<uint32_t>::max()));
		return (uint32_t)RemainingMsecsClamped;
	}

	friend bool operator==(FTimeout L, FTimeout R)
	{
		return L.Timeout == R.Timeout && (L.Timeout == std::chrono::system_clock::duration::max() || L.Start == R.Start);
	}

	friend bool operator!=(FTimeout Left, FTimeout Right)
	{
		return !operator==(Left, Right);
	}
private:
	std::chrono::time_point<std::chrono::system_clock> Start;
	std::chrono::system_clock::duration Timeout;
};