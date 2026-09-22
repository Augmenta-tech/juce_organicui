/*
 ==============================================================================

 AppVersion.cpp
 Created: 29 Jul 2026 03:36:00am
 Author:  Leon Victor

 ==============================================================================
 */

 using namespace juce;

 AppVersion::AppVersion(const String& versionStr)
	{
		StringArray split;
		split.addTokens(versionStr, ".", "\"");

		jassert(split.size() == 3);

		major = split[0].getIntValue();
		minor = split[1].getIntValue();

		const int channelStartIdx = split[2].indexOfAnyOf("abcdefghijklmnopqrstuvwxyz", 0, true);
		if (channelStartIdx != -1)
		{
			const String patchStr = split[2].substring(0, channelStartIdx);
			patch = patchStr.getIntValue();

			const int channelEndIdx = split[2].lastIndexOfAnyOf("abcdefghijklmnopqrstuvwxyz", true);
			channelName = split[2].substring(channelStartIdx, channelEndIdx + 1);

			const String channelVersionStr = split[2].substring(channelEndIdx + 1);
			channelVersion = channelVersionStr.getIntValue();
		}
		else
		{
			patch = split[2].getIntValue();
		}
	}

	bool AppVersion::operator==(const AppVersion& other) const 
	{ 
		return major == other.major && minor == other.minor && patch == other.patch && channelName == other.channelName && channelVersion == other.channelVersion;
	}

	bool AppVersion::operator<(const AppVersion& other) const
	{
		if (major != other.major) return major < other.major;
		if (minor != other.minor) return minor < other.minor;
		if (patch != other.patch) return patch < other.patch;

		auto channelRank = [](const String& channel)
			{
				if (channel.isEmpty()) return 3; // release
				if (channel == "b") return 2;
				if (channel == "a") return 1;
				return 0; // other prerelease/custom suffixes
			};

		const int rank = channelRank(channelName);
		const int otherRank = channelRank(other.channelName);
		if (rank != otherRank) return rank < otherRank;

		if (channelName != other.channelName)
			return channelName < other.channelName;

		return channelVersion < other.channelVersion;
	}

	bool AppVersion::operator<=(const AppVersion& other) const
	{
		return *this == other || *this < other;
	}

	String AppVersion::toString() const 
	{ 
		String res = String(major) + "." + String(minor) + "." + String(patch);
		if (!isRelease())
		{
			res = res + channelName + String(channelVersion);
		}
		return res;
	}