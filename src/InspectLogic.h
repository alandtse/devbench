#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dvb::InspectLogic
{
	enum class LightsScope
	{
		kRef,
		kScene,
	};

	/// "" and "ref" read one reference's 3D, "scene" reads the renderer's light lists; anything else is nullopt.
	std::optional<LightsScope> ParseLightsScope(std::string_view a_scope);

	/// One budget for a whole lights read: graph nodes visited, parent links walked, and lights emitted.
	/// Once a limit is hit that kind of work stops and the read reports itself truncated.
	class TraversalBudget
	{
	public:
		TraversalBudget(std::size_t a_maxNodes, std::size_t a_maxParentSteps, std::size_t a_maxOutput) :
			m_maxNodes(a_maxNodes), m_maxParentSteps(a_maxParentSteps), m_maxOutput(a_maxOutput)
		{}

		bool VisitNode() { return Spend(m_nodes, m_maxNodes, m_nodesExhausted); }
		bool StepParent() { return Spend(m_parentSteps, m_maxParentSteps, m_parentsExhausted); }
		bool Emit() { return Spend(m_output, m_maxOutput, m_outputExhausted); }

		std::size_t Nodes() const { return m_nodes; }
		std::size_t ParentSteps() const { return m_parentSteps; }
		std::size_t Output() const { return m_output; }
		std::size_t MaxNodes() const { return m_maxNodes; }
		std::size_t MaxParentSteps() const { return m_maxParentSteps; }
		std::size_t MaxOutput() const { return m_maxOutput; }

		bool                     Truncated() const { return m_nodesExhausted || m_parentsExhausted || m_outputExhausted; }
		std::vector<std::string> TruncatedBy() const;

	private:
		static bool Spend(std::size_t& a_used, std::size_t a_max, bool& a_exhausted)
		{
			if (a_used >= a_max) {
				a_exhausted = true;
				return false;
			}
			++a_used;
			return true;
		}

		std::size_t m_maxNodes;
		std::size_t m_maxParentSteps;
		std::size_t m_maxOutput;
		std::size_t m_nodes = 0;
		std::size_t m_parentSteps = 0;
		std::size_t m_output = 0;
		bool        m_nodesExhausted = false;
		bool        m_parentsExhausted = false;
		bool        m_outputExhausted = false;
	};

	struct SceneEntry
	{
		std::optional<float> distance;  // nullopt when the player's position is unknown
		std::string          name;
		std::string          path;
		float                x = 0.0f;
		float                y = 0.0f;
		float                z = 0.0f;
		std::size_t          index = 0;  // the caller's own slot for this entry
	};

	/// Nearest first, unknown distances last; equal distances by name, path, then position, so the
	/// same scene always yields the same order before a limit is applied.
	void OrderScene(std::vector<SceneEntry>& a_entries);

	struct HandObservation
	{
		bool                holdsSpell = false;
		bool                casterPresent = false;
		bool                artAttached = false;     // the caster's kCastingArtAttached flag
		bool                artLoading = false;      // the caster still has a clone task for its art
		bool                artPending = false;      // the caster names an art still to attach (castingArt set)
		bool                artNodePresent = false;  // an attached art node is recorded (castingArtData.attachedArt)
		std::optional<bool> pendingMatchesEquipped;  // the pending art is one of the equipped spell's; nullopt when none is pending
	};

	struct HandsObservation
	{
		bool            settledDrawn = false;  // weapon state is exactly kDrawn
		HandObservation left;
		HandObservation right;
	};

	/// The castingArtReady field: a hand holding no spell, or one whose caster flags its art attached.
	bool ArtAttachedOrNoSpell(const HandObservation& a_hand);

	/// handsReady: weapon state exactly kDrawn, and every hand holding a spell has a caster that has
	/// finished its attach sequence: no art pending, no clone task, the attached flag set and an
	/// attached art node recorded. Equipping a spell clears the flag and names the new art as pending,
	/// so a finished sequence follows that equip. It does not compare the attached model with the
	/// spell, and it is not proof that a rendered frame shows the art.
	bool HandsReady(const HandsObservation& a_hands);
}
