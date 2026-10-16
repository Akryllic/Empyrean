#pragma once

namespace Hooks
{
	class LocationalDamage final
	{
	public:
		static bool InstallHooks();
		static void LoadData();

	private:
		struct Rule
		{
			float              damageMult = 1.0f;
			bool               deflect = false;
			RE::BGSImpactData* impactData = nullptr;
			const char*        sound = nullptr;
			const char*        messageKey = nullptr;
			const char*        messageFallback = nullptr;
			RE::SpellItem*     spell = nullptr;
			bool               castFromTarget = false;
		};

		struct PendingHit
		{
			RE::ObjectRefHandle                   projectile;
			RE::ActorHandle                       target;
			RE::ActorHandle                       aggressor;
			const Rule*                           rule = nullptr;
			std::chrono::steady_clock::time_point queued;
			std::vector<RE::ActiveEffect*>        amplified;
		};

		enum Zone : std::size_t
		{
			kHead,
			kArm,
			kLeg,
			kZoneCount
		};

		enum Rank : std::size_t
		{
			kRank1,
			kRank2,
			kRankCount
		};

		static bool ArcheryLocationalDamageProvidesRules();
		static bool IsExpectedCall(std::uintptr_t a_site, std::uintptr_t a_callee);
		static void InstallImpactDataHook();

		static bool KillOnCollision(RE::Projectile* a_projectile);
		static void HandleHitTask(RE::Actor* a_target, RE::HitData* a_hitData);
		static RE::BGSImpactData* GetImpactData(RE::BGSImpactDataSet* a_impactDataSet, RE::BGSMaterialType* a_material);

		static void ProcessImpact(RE::Projectile* a_projectile);
		static const Rule* FindRule(RE::Projectile* a_projectile, RE::Actor* a_target, RE::Actor* a_shooter, std::string_view a_nodeName);
		static std::optional<Zone> GetZone(std::string_view a_nodeName);
		static RE::NiNode* FindClosestHitNode(RE::NiNode* a_root, const RE::NiPoint3& a_position, bool a_isPlayer, float& a_distance);
		static bool HasActiveEffectWithKeyword(RE::Actor* a_actor, const RE::BGSKeyword* a_keyword);
		static bool ShotHasKeywords(RE::Projectile* a_projectile, std::initializer_list<const RE::BGSKeyword*> a_keywords);
		static bool IsLowTierArrow(RE::Projectile* a_projectile);

		static std::optional<PendingHit> TakePendingHit(const RE::HitData& a_hitData);
		static void ApplyEffects(const PendingHit& a_hit);
		static std::vector<RE::ActiveEffect*> AmplifyNewEffects(RE::Actor* a_target, RE::ActorHandle a_caster, float a_mult, const std::vector<RE::ActiveEffect*>& a_skip);
		static std::string HitMessage(const Rule& a_rule);
		static bool CastsFromTarget(const RE::SpellItem* a_spell);

		static inline REL::Relocation<decltype(&KillOnCollision)> _KillOnCollision;
		static inline REL::Relocation<decltype(&HandleHitTask)> _HandleHitTask;
		static inline REL::Relocation<decltype(&GetImpactData)> _GetImpactData;

		static inline bool hooksInstalled = false;
		static inline bool active = false;
		static inline bool showMessages = true;
		static inline float headshotMultRank1 = 1.30f;
		static inline float headshotMultRank2 = 1.15f;

		static inline RE::BGSKeyword* keywordDeflectArrowsActive;
		static inline RE::BGSKeyword* keywordPhalanxActive;
		static inline RE::BGSKeyword* keywordWeapTypeCrossbow;
		static inline RE::BGSKeyword* keywordVendorItemArrow;
		static inline std::array<RE::BGSKeyword*, 4> keywordsLowTierMaterial;
		static inline std::array<RE::BGSPerk*, kRankCount> conditionPerks;

		static inline Rule deflectRule;
		static inline Rule phalanxRule;
		static inline std::array<std::array<Rule, kRankCount>, kZoneCount> crossbowRules;

		static inline std::mutex pendingLock;
		static inline std::vector<PendingHit> pendingHits;

		static constexpr auto PENDING_HIT_LIFETIME = std::chrono::seconds(2);
	};
}
