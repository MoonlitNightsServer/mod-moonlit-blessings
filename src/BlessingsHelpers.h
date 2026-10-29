// Moonlit Blessings: small helpers shared by the C++ spell scripts of the Blessings (one source file per Blessing, see src/*Scripts.cpp).
// ~Moonlit Nights Team
#ifndef MOONLIT_BLESSINGS_HELPERS_H
#define MOONLIT_BLESSINGS_HELPERS_H

#include "Group.h"
#include "Pet.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Unit.h"

#include <algorithm>

namespace BlessingsHelpers
{
    // The group member (or the caster, or a pet) within `range` yards of `center` that is missing the most health. Null if nobody is hurt.
    inline Unit* MostInjuredFriend(Unit* caster, Unit* center, float range)
    {
        if (!caster || !center)
            return nullptr;

        Unit* best = nullptr;
        uint32 bestMissing = 0;
        auto consider = [&](Unit* u)
        {
            if (!u || !u->IsAlive() || !u->IsWithinDist(center, range))
                return;

            uint32 const missing = u->GetMaxHealth() - u->GetHealth();
            if (missing > bestMissing)
            {
                bestMissing = missing;
                best = u;
            }
        };

        consider(caster);
        if (Player* player = caster->ToPlayer())
        {
            if (Group* group = player->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    consider(member);
                    if (member)
                        consider(member->GetPet());
                }
            }

            consider(player->GetPet());
        }

        return best;
    }

    // The group member (the caster, party and raid members and their pets) within `range` yards of the caster with the lowest health percent that is below 100%, leaving out the units
    // in `skip`. Null if nobody is hurt. (The Cauterizer and the Spirit Healer heal by health percent, not by missing health.)
    inline Unit* LowestHealthPercentFriend(Unit* caster, float range, std::vector<ObjectGuid> const& skip = {})
    {
        if (!caster)
            return nullptr;

        Unit* best = nullptr;
        float bestPct = 100.0f;
        auto consider = [&](Unit* u)
        {
            if (!u || !u->IsAlive() || !u->GetMaxHealth() || !u->IsWithinDist(caster, range) || std::find(skip.begin(), skip.end(), u->GetGUID()) != skip.end())
                return;

            float const pct = 100.0f * float(u->GetHealth()) / float(u->GetMaxHealth());
            if (pct < bestPct)
            {
                bestPct = pct;
                best = u;
            }
        };

        consider(caster);
        if (Player* player = caster->ToPlayer())
        {
            if (Group* group = player->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    consider(member);
                    if (member)
                        consider(member->GetPet());
                }
            }

            consider(player->GetPet());
        }

        return best;
    }

    // Heal `target` for `amount` as `caster` (no cast needed). `spellId` is the spell the heal is attributed to; it may be 0.
    inline void HealUnit(Unit* caster, Unit* target, uint32 amount, uint32 spellId = 0)
    {
        if (!caster || !target || !amount)
            return;

        SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
        if (!info)
            return;     // SendHealSpellLog needs a spell: an unattributed heal is not possible
        HealInfo healInfo(caster, target, amount, info, info ? info->GetSchoolMask() : SPELL_SCHOOL_MASK_HOLY);
        caster->HealBySpell(healInfo, false);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Martyr: the Eternal Aegis helpers (appended by the Martyr recipe, see MartyrScripts.cpp).
// A Boon or Favor that changes the Aegis is a hidden passive; the helpers read ONE number from a known spell id (the amount of one of its effects), so the spell
// data stays the only place a value is written. The ids below are asserted against the recipe by tools/talents/recipes/martyr.py.
namespace BlessingsHelpers
{
namespace Martyr
{
    constexpr uint32 SPELL_ETERNAL_AEGIS = 1640010;       // the stance (hand-written kit spell)
    constexpr uint32 SPELL_AEGIS_SHIELD = 1640011;        // the shield: effect 0 absorb, effect 1 the one-second refill tick

    constexpr uint32 SPELL_MARTYRS_VIGOR = 1640200;       // effect 0: percent of maximum health absorbed per step, effect 1: percent mana restored per step
    constexpr uint32 SPELL_SANCTIFIED_PACT = 1640210;     // the castable ability (effect 0: percent health given, effect 1: percent of the Aegis refilled)
    constexpr uint32 SPELL_THORNED_SHIELD = 1640220;      // effect 0: percent of the absorbed damage reflected
    constexpr uint32 SPELL_THORNED_DAMAGE = 1640221;      // the reflected holy damage (base points 0 is set per cast)
    constexpr uint32 SPELL_GUARDED_FAITH = 1640230;       // effect 0: percent faster Aegis refill
    constexpr uint32 SPELL_RADIANT_PULSE = 1640240;       // periodic trigger every 3 sec
    constexpr uint32 SPELL_RADIANT_PULSE_DAMAGE = 1640241;
    constexpr uint32 SPELL_MENDING_WOUNDS = 1640250;      // effect 0: percent of the absorbed damage healed
    constexpr uint32 SPELL_ROOTED_IN_FAITH = 1640260;     // effect 0: the Aegis must be more than this percent full
    constexpr uint32 SPELL_ROOTED_BUFF = 1640261;         // the damage reduction it switches on
    constexpr uint32 SPELL_WRATH_OF_THE_AEGIS = 1640270;  // effect 0: percent of the current Aegis added to Sanctified Blow
    constexpr uint32 SPELL_BLESSED_STRIKES = 1640280;     // effect 0: percent of Blow / Burning Strike damage added to the Aegis
    constexpr uint32 SPELL_LIGHTS_BULWARK = 1640290;      // effect 0: health percent threshold, effect 1: percent of the Aegis refilled
    constexpr uint32 SPELL_SANCTIFIED_CLEAVE = 1640300;
    constexpr uint32 SPELL_BURNING_RETRIBUTION = 1640310; // effect 0: percent of the hit taken, dealt back over 4 sec
    constexpr uint32 SPELL_RETRIBUTION_DOT = 1640311;
    constexpr uint32 SPELL_FINAL_VIGIL = 1640320;
    constexpr uint32 SPELL_RIGHTEOUS_SIPHON = 1640330;    // effect 0: percent of the damage per enemy hit added to the Aegis
    constexpr uint32 SPELL_ARCHONS_BLADE = 1640340;
    constexpr uint32 SPELL_SUNLIT_AEGIS = 1640350;        // effect 0: the Aegis cap, percent of maximum health
    constexpr uint32 SPELL_UNYIELDING_MARTYR = 1640360;   // effect 0: percent health left, effect 1: cooldown in seconds
    constexpr uint32 SPELL_FAVOR_BULWARK = 1640370;       // effect 1: percent faster Aegis refill
    constexpr uint32 SPELL_FAVOR_REDEEMER = 1640380;      // effect 0: percent of every melee hit added to the Aegis

    constexpr uint32 SPELL_GLYPH_IMPROVED_SHIELD = 1640460;   // Glyph of the Martyr: Improved Power Word: Shield rewrite (3 ranks): the Aegis is 5% larger and refills 5% faster per rank
    constexpr uint32 SPELL_GLYPH_REFLECTIVE = 1640470;        // Reflective Shield rewrite (2 ranks): 20% per rank of the absorbed damage as Holy damage split among the foes within 10 yards
    constexpr uint32 SPELL_GLYPH_WARDING = 1640490;           // Spell Warding rewrite (5 ranks): 2% less damage taken per rank while the Aegis holds a shield
    constexpr uint32 SPELL_GLYPH_WARDING_BUFF = 1640510;      // its helper (amount set per rank)

    constexpr int32 AEGIS_EMPTY_AMOUNT = 1;               // the shield never drops below 1 point (spell_martyr_aegis_hooks keeps it alive), so 1 means empty
    constexpr int32 AEGIS_BASE_CAP_PCT = 25;              // the Aegis size without Sunlit Aegis (MoonlitBlessingsSpells.cpp: AEGIS_CAP_PCT_OF_HEALTH)
    constexpr int32 BLOW_BASE_SHARE_PCT = 10;             // the share Sanctified Blow adds by itself (MoonlitBlessingsSpells.cpp: SANCTIFIED_BLOW_SHARE_PCT)

    // The amount of effect `effect` of the passive `spellId` on `unit`; 0 when the unit does not have it.
    inline int32 Param(Unit const* unit, uint32 spellId, uint8 effect = 0)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
    }

    // The rank (0 = none) of a rewritten talent the player has: the rewrite spells run on by one per rank.
    inline uint32 VariantRank(Unit const* unit, uint32 firstRank, uint32 ranks)
    {
        for (uint32 rank = ranks; rank > 0; --rank)
            if (unit->HasAura(firstRank + rank - 1))
                return rank;

        return 0;
    }

    // The Aegis size in percent of maximum health, and in health points. (Improved Power Word: Shield rewrite: 5% larger per rank.)
    inline int32 AegisCapPct(Unit const* unit)
    {
        int32 const pct = std::max(AEGIS_BASE_CAP_PCT, Param(unit, SPELL_SUNLIT_AEGIS));
        return pct + pct * 5 * int32(VariantRank(unit, SPELL_GLYPH_IMPROVED_SHIELD, 3)) / 100;
    }

    inline int32 AegisCap(Unit const* unit)
    {
        return int32(unit->CountPctFromMaxHealth(AegisCapPct(unit)));
    }

    // Extra refill speed in percent (Guarded Faith plus Favor of the Bulwark).
    inline int32 AegisRefillBonusPct(Unit const* unit)
    {
        return Param(unit, SPELL_GUARDED_FAITH) + Param(unit, SPELL_FAVOR_BULWARK, EFFECT_1) + 5 * int32(VariantRank(unit, SPELL_GLYPH_IMPROVED_SHIELD, 3));
    }

    // What the Aegis refills each second before any bonus. Same formula as the shield script in MoonlitBlessingsSpells.cpp (keep both in step).
    inline int32 AegisRefillPerSecond(Unit* unit)
    {
        float const level = float(std::max<uint8>(unit->GetLevel(), 1));
        float const base = 44.0f + (level - 1.0f) * (848.0f - 44.0f) / 59.0f;
        float const power = float(std::max<int32>(unit->SpellBaseHealingBonusDone(SPELL_SCHOOL_MASK_HOLY), 0)) * 0.8068f;
        return std::max<int32>(1, int32((base + power) / 4.0f));
    }

    inline AuraEffect* AegisEffect(Unit const* unit)
    {
        return unit->GetAuraEffect(SPELL_AEGIS_SHIELD, EFFECT_0);
    }

    // How much the shield holds now (0 without a shield).
    inline int32 AegisAmount(Unit const* unit)
    {
        AuraEffect const* aurEff = AegisEffect(unit);
        return aurEff ? std::max(aurEff->GetAmount(), 0) : 0;
    }

    // The shield's absorb effect. Normally the keep-alive in spell_martyr_aegis_hooks stops the core from removing a used-up shield; if it is missing anyway (not yet
    // merged, or removed by something else) it is cast again here, empty, while the stance is known.
    inline AuraEffect* EnsureAegis(Unit* unit)
    {
        if (!unit || !unit->IsAlive() || !unit->HasAura(SPELL_ETERNAL_AEGIS))
            return nullptr;

        if (!unit->HasAura(SPELL_AEGIS_SHIELD))
        {
            unit->CastSpell(unit, SPELL_AEGIS_SHIELD, true);
            if (AuraEffect* aurEff = AegisEffect(unit))
                aurEff->SetAmount(AEGIS_EMPTY_AMOUNT);
        }

        return AegisEffect(unit);
    }

    // Rooted in Faith: the damage reduction buff is on while the Aegis holds more than the passive's percent of its size.
    // `pendingAbsorb` is damage the shield has absorbed but the core has not yet taken off its amount (inside an absorb hook).
    inline void RefreshRooted(Unit* unit, int32 pendingAbsorb = 0)
    {
        bool active = false;
        if (int32 const threshold = Param(unit, SPELL_ROOTED_IN_FAITH))
        {
            int64 const cap = AegisCap(unit);
            active = cap > 0 && int64(AegisAmount(unit) - pendingAbsorb) * 100 > cap * threshold;
        }

        bool const has = unit->HasAura(SPELL_ROOTED_BUFF);
        if (active && !has)
            unit->CastSpell(unit, SPELL_ROOTED_BUFF, true);
        else if (!active && has)
            unit->RemoveAurasDueToSpell(SPELL_ROOTED_BUFF);

        // Spell Warding rewrite: 2% less damage from every source per rank, exactly while the Aegis holds more than its empty point
        uint32 const warding = VariantRank(unit, SPELL_GLYPH_WARDING, 5);
        bool const warded = warding && AegisAmount(unit) - pendingAbsorb > AEGIS_EMPTY_AMOUNT;
        if (warded)
        {
            if (!unit->HasAura(SPELL_GLYPH_WARDING_BUFF))
                unit->CastSpell(unit, SPELL_GLYPH_WARDING_BUFF, true);

            if (AuraEffect* effect = unit->GetAuraEffect(SPELL_GLYPH_WARDING_BUFF, EFFECT_0))
                effect->ChangeAmount(-2 * int32(warding));
        }
        else
            unit->RemoveAurasDueToSpell(SPELL_GLYPH_WARDING_BUFF);
    }

    // Add `amount` to the Aegis, never beyond its cap.
    inline void AddToAegis(Unit* unit, int32 amount)
    {
        if (amount <= 0)
            return;

        if (AuraEffect* aurEff = EnsureAegis(unit))
        {
            aurEff->SetAmount(std::min(std::max(aurEff->GetAmount(), 0) + amount, AegisCap(unit)));
            RefreshRooted(unit);
        }
    }

    inline void FillAegis(Unit* unit)
    {
        if (unit)
            AddToAegis(unit, AegisCap(unit));
    }
}
}

#endif
