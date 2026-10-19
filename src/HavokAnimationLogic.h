#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::HavokAnimationLogic
{
	struct AnimationInfo
	{
		std::string className;  // e.g. hkaSplineCompressedAnimation
		float       durationSeconds{ 0.0f };
		int         transformTracks{ 0 };
		int         floatTracks{ 0 };
		int         pointerSize{ 0 };  // 8 for the 64-bit packfiles Skyrim SE and VR ship, 4 for the older 32-bit ones
	};

	/// A clip generator of a behavior graph: the clip's own name and the animation file it plays.
	struct Clip
	{
		std::string name;
		std::string animation;  // as stored, e.g. Animations\Special_CiceroDance1.hkx
	};

	/// Reads the animation object out of a Havok binary packfile (a .hkx): its class, duration and track counts. Nullopt when
	/// the bytes are not a packfile this reader understands or hold no animation object; it never reads outside a_bytes.
	std::optional<AnimationInfo> ParseAnimation(std::span<const std::uint8_t> a_bytes);

	/// The clip generators of a behavior graph packfile. Empty when the bytes are not a packfile this reader understands.
	std::vector<Clip> ParseClips(std::span<const std::uint8_t> a_bytes);

	/// The behavior files a behavior graph packfile hands control to (its reference generators), as stored, e.g.
	/// Behaviors\MT_Behavior.hkx.
	std::vector<std::string> ParseBehaviorReferences(std::span<const std::uint8_t> a_bytes);

	/// How a clip was matched to an event, best first.
	enum class MatchKind
	{
		kExact,     // the clip is named like the event
		kNoPrefix,  // like the event without its Idle prefix (event IdleCiceroDance1, clip CiceroDance1)
		kSuffix,    // the clip name ends with that name after an underscore (clip Tor_LightSconce)
		kContains,  // the clip name has that name at its start or after an underscore (clip MT_Wounded_0100)
	};

	struct ClipMatch
	{
		Clip        clip;
		MatchKind   kind{ MatchKind::kExact };
		std::size_t candidates{ 1 };  // clips matched at the same kind; the shortest name is chosen
	};

	std::string_view MatchKindName(MatchKind a_kind);

	/// The clip an animation event plays, found by name: a graph names its clips after their events, often with a prefix
	/// (MT_, Tor_), and the graph's transitions are not followed, so this is a guess that a clip named for its event is the one
	/// the event plays. Nullopt when no clip name relates to the event.
	std::optional<ClipMatch> FindClipForEvent(const std::vector<Clip>& a_clips, std::string_view a_event);
}
