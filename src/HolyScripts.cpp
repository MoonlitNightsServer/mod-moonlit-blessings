// Moonlit Blessings: C++ spell scripts for the Holy Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/holy.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_holy_frontline_faith   Boon: once a second, while an enemy is in melee range of the paladin, a 2 sec buff (healing +8%, melee damage taken -5%) is kept up
//   spell_blessings_holy_aura_of_mending   Boon: every 3 sec heals the allies under one of the paladin's auras for 1.5% of their maximum health
//   spell_blessings_holy_wings_of_mercy    Boon: while Avenging Wrath is on the paladin, keeps a buff (melee haste, faster Holy Light and Flash of Light) for exactly its remaining time
//   spell_blessings_holy_beacon            Favor: the direct heals also heal the Beacon of Light (the group member that carries the paladin's Beacon aura); teaches the Beacon cast
//   spell_blessings_holy_lightbringer      Favor: the direct heals also heal up to 2 other injured allies near the target
//   spell_blessings_holy_radiant_blade     Favor: every melee hit is followed by a holy strike (spell 1510061)
//   spell_blessings_holy_radiant_strike    Favor: the strike heals the most injured ally within 30 yards for the damage it dealt
//
// Spell ids come from the registry block 1,510,000 (Holy): unit n of tools/talents/recipes/holy.py is 1,510,000 + 10 n; holy.py asserts the units named here.
// Everything that heals goes through BlessingsHelpers::HealUnit with the spell id of a helper spell of this Blessing (a heal without a spell id does nothing).
// ~Moonlit Nights Team

#include "BlessingsHelpers.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "WorldSession.h"
#include <algorithm>
#include <vector>

namespace
{
    enum HolySpells : uint32
    {
        SPELL_FRONTLINE_FAITH_BUFF   = 1510001,
        SPELL_AURA_OF_MENDING_HEAL   = 1510011,
        SPELL_WINGS_OF_MERCY_BUFF    = 1510031,
        SPELL_BEACON_OF_LIGHT        = 1510041,     // the castable Beacon: effect 0 holds the percent of the heals the Beacon gets
        SPELL_BEACON_HEAL            = 1510042,
        SPELL_LIGHTBRINGER_HEAL      = 1510051,
        SPELL_FAVOR_RADIANT_BLADE    = 1510060,
        SPELL_RADIANT_STRIKE         = 1510061,
        SPELL_RADIANT_BLADE_HEAL     = 1510062,

        // stock spells (holy.py asserts the name of each)
        SPELL_AVENGING_WRATH         = 31884
    };

    constexpr float BEACON_RANGE = 60.0f;               // yards from the paladin within which the Beacon is healed
    constexpr float LIGHTBRINGER_RANGE = 15.0f;         // yards from the healed target within which the spread heals land
    constexpr uint32 LIGHTBRINGER_MAX_TARGETS = 2;
    constexpr float MENDING_RANGE = 40.0f;              // yards from the paladin
    constexpr float RADIANT_BLADE_HEAL_RANGE = 30.0f;   // yards from the paladin
    constexpr uint32 LAY_ON_HANDS_FLAG = 0x8000;        // SpellFamilyFlags[0] of Lay on Hands (every rank, nothing else of the Paladin family)

    // The caster, the other members of the caster's group and their pets.
    void CollectFriends(Unit* caster, std::vector<Unit*>& out)
    {
        out.push_back(caster);
        if (Player* player = caster->ToPlayer())
        {
            if (Pet* pet = player->GetPet())
                out.push_back(pet);

            if (Group* group = player->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (!member || member == player)
                        continue;

                    out.push_back(member);
                    if (Pet* pet = member->GetPet())
                        out.push_back(pet);
                }
            }
        }
    }

    // Alive, in the world and on the same map as `reference`.
    bool IsUsable(Unit const* unit, Unit const* reference)
    {
        return unit && unit->IsInWorld() && unit->IsAlive() && unit->IsInMap(reference);
    }

    int64 MissingHealth(Unit const* unit)
    {
        return int64(unit->GetMaxHealth()) - int64(unit->GetHealth());
    }

    // The friend (caster, group member or a pet) within `range` yards of `center` that is missing the most health. Null if nobody is hurt.
    Unit* MostInjured(Unit* caster, Unit* center, float range)
    {
        std::vector<Unit*> friends;
        CollectFriends(caster, friends);

        Unit* best = nullptr;
        int64 bestMissing = 0;
        for (Unit* unit : friends)
        {
            if (!IsUsable(unit, center) || !unit->IsWithinDist(center, range))
                continue;

            int64 const missing = MissingHealth(unit);
            if (missing > bestMissing)
            {
                bestMissing = missing;
                best = unit;
            }
        }

        return best;
    }

    // What a Beacon or Lightbringer copy is a share of: the heal before the target's own healing-taken modifiers (critical strikes included), the same number the stock Light's Beacon
    // uses. Lay on Hands is applied before those are known, so the paladin's maximum health stands for it (again like the stock Light's Beacon).
    uint32 HealBase(Unit const* caster, SpellInfo const* spell, HealInfo const* heal)
    {
        if (spell->SpellFamilyName == SPELLFAMILY_PALADIN && (spell->SpellFamilyFlags[0] & LAY_ON_HANDS_FLAG))
            return caster->GetMaxHealth();

        return heal->GetHealBeforeTakenMods() ? heal->GetHealBeforeTakenMods() : heal->GetHeal();
    }

    // An enemy that is attacking the paladin, or that the paladin is attacking, within melee range.
    bool HasEnemyInMeleeRange(Unit* unit)
    {
        if (Unit* victim = unit->GetVictim())
            if (victim->IsInWorld() && victim->IsAlive() && unit->IsHostileTo(victim) && unit->IsWithinMeleeRange(victim))
                return true;

        for (Unit* attacker : unit->getAttackers())
            if (attacker && attacker->IsInWorld() && attacker->IsAlive() && unit->IsWithinMeleeRange(attacker))
                return true;

        return false;
    }
}

// Frontline Faith: the passive ticks once a second; while an enemy is within melee range a 2 sec buff is cast on the paladin (so it ends by itself a moment after the enemy is gone).
class spell_blessings_holy_frontline_faith : public AuraScript
{
    PrepareAuraScript(spell_blessings_holy_frontline_faith);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_FRONTLINE_FAITH_BUFF });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        if (target->IsAlive() && HasEnemyInMeleeRange(target))
            target->CastSpell(target, SPELL_FRONTLINE_FAITH_BUFF, true);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_holy_frontline_faith::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Aura of Mending: effect 0 (every 3 sec) heals the paladin and the group members (and pets) within 40 yards that carry one of the paladin's own auras (a non-passive spell of the
// Paladin family with the aura class bit that the paladin cast himself) for effect-0-value tenths of a percent of their maximum health.
class spell_blessings_holy_aura_of_mending : public AuraScript
{
    PrepareAuraScript(spell_blessings_holy_aura_of_mending);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_AURA_OF_MENDING_HEAL });
    }

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* paladin = GetTarget();
        if (!paladin->IsAlive() || aurEff->GetAmount() <= 0)
            return;

        // the paladin auras he has up (usually one)
        std::vector<uint32> auraIds;
        for (auto const& pair : paladin->GetAppliedAuras())
        {
            Aura const* aura = pair.second->GetBase();
            SpellInfo const* info = aura->GetSpellInfo();
            if (aura->GetCasterGUID() == paladin->GetGUID() && info->SpellFamilyName == SPELLFAMILY_PALADIN && info->GetSpellSpecific() == SPELL_SPECIFIC_AURA && !info->IsPassive())
                auraIds.push_back(pair.first);
        }

        if (auraIds.empty())
            return;

        std::vector<Unit*> friends;
        CollectFriends(paladin, friends);
        for (Unit* unit : friends)
        {
            if (!IsUsable(unit, paladin) || !unit->IsWithinDist(paladin, MENDING_RANGE) || MissingHealth(unit) <= 0)
                continue;

            bool underAura = false;
            for (uint32 id : auraIds)
            {
                if (unit->HasAura(id, paladin->GetGUID()))
                {
                    underAura = true;
                    break;
                }
            }

            if (underAura)
                BlessingsHelpers::HealUnit(paladin, unit, uint32(int64(unit->GetMaxHealth()) * aurEff->GetAmount() / 1000), SPELL_AURA_OF_MENDING_HEAL);
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_holy_aura_of_mending::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Wings of Mercy: effect 0 ticks once a second. While the paladin has Avenging Wrath, the buff (melee haste, faster Holy Light and Flash of Light) is on him and has exactly as much time
// left as Avenging Wrath; when Avenging Wrath is gone (ended or cancelled) the buff is removed. Effects 1 and 2 are the plain cooldown and duration modifiers.
class spell_blessings_holy_wings_of_mercy : public AuraScript
{
    PrepareAuraScript(spell_blessings_holy_wings_of_mercy);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_AVENGING_WRATH, SPELL_WINGS_OF_MERCY_BUFF });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* paladin = GetTarget();
        Aura* wrath = paladin->GetAura(SPELL_AVENGING_WRATH, paladin->GetGUID());
        if (!wrath || wrath->GetDuration() <= 0)
        {
            paladin->RemoveAurasDueToSpell(SPELL_WINGS_OF_MERCY_BUFF);
            return;
        }

        Aura* buff = paladin->GetAura(SPELL_WINGS_OF_MERCY_BUFF, paladin->GetGUID());
        if (!buff)
        {
            paladin->CastSpell(paladin, SPELL_WINGS_OF_MERCY_BUFF, true);
            buff = paladin->GetAura(SPELL_WINGS_OF_MERCY_BUFF, paladin->GetGUID());
        }

        if (buff)
        {
            buff->SetMaxDuration(wrath->GetDuration());
            buff->SetDuration(wrath->GetDuration());
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_holy_wings_of_mercy::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Favor of the Beacon: the Favor teaches the Beacon cast (SPELL_BEACON_OF_LIGHT, an aura on one group member whose effect 0 holds the percent) and forgets it when the Favor is dropped
// (not when the player merely logs out). Every direct heal the paladin casts on someone other than himself heals the group member that carries his Beacon aura for that percent.
class spell_blessings_holy_beacon : public AuraScript
{
    PrepareAuraScript(spell_blessings_holy_beacon);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_BEACON_OF_LIGHT, SPELL_BEACON_HEAL });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->learnSpell(SPELL_BEACON_OF_LIGHT);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->removeSpell(SPELL_BEACON_OF_LIGHT, SPEC_MASK_ALL, false);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        HealInfo const* heal = eventInfo.GetHealInfo();
        if (!spell || !heal || !heal->GetHeal())
            return;

        Unit* paladin = GetTarget();
        Unit* healed = heal->GetTarget();
        if (!healed || healed == paladin)
            return;

        // find the Beacon: the paladin or a group member carrying the Beacon aura the paladin cast
        std::vector<Unit*> friends;
        CollectFriends(paladin, friends);
        for (Unit* unit : friends)
        {
            if (unit == healed || !IsUsable(unit, paladin) || !unit->IsWithinDist(paladin, BEACON_RANGE))
                continue;

            AuraEffect const* beacon = unit->GetAuraEffect(SPELL_BEACON_OF_LIGHT, EFFECT_0, paladin->GetGUID());
            if (!beacon)
                continue;

            uint32 const amount = CalculatePct(HealBase(paladin, spell, heal), std::max(beacon->GetAmount(), 0));
            BlessingsHelpers::HealUnit(paladin, unit, amount, SPELL_BEACON_HEAL);
            return;     // one Beacon at a time
        }
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_holy_beacon::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_holy_beacon::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectProc += AuraEffectProcFn(spell_blessings_holy_beacon::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Lightbringer: every direct heal also heals up to 2 other injured friends within 15 yards of the healed target for effect-0-value percent of the amount.
class spell_blessings_holy_lightbringer : public AuraScript
{
    PrepareAuraScript(spell_blessings_holy_lightbringer);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_LIGHTBRINGER_HEAL });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        HealInfo const* heal = eventInfo.GetHealInfo();
        if (!spell || !heal || !heal->GetHeal())
            return;

        Unit* paladin = GetTarget();
        Unit* healed = heal->GetTarget();
        if (!healed || !IsUsable(healed, paladin))
            return;

        uint32 const amount = CalculatePct(HealBase(paladin, spell, heal), std::max(aurEff->GetAmount(), 0));
        if (!amount)
            return;

        std::vector<Unit*> friends;
        CollectFriends(paladin, friends);

        std::vector<Unit*> hurt;
        for (Unit* unit : friends)
            if (unit != healed && IsUsable(unit, healed) && unit->IsWithinDist(healed, LIGHTBRINGER_RANGE) && MissingHealth(unit) > 0)
                hurt.push_back(unit);

        std::sort(hurt.begin(), hurt.end(), [](Unit const* a, Unit const* b) { return MissingHealth(a) > MissingHealth(b); });
        if (hurt.size() > LIGHTBRINGER_MAX_TARGETS)
            hurt.resize(LIGHTBRINGER_MAX_TARGETS);

        for (Unit* unit : hurt)
            BlessingsHelpers::HealUnit(paladin, unit, amount, SPELL_LIGHTBRINGER_HEAL);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_holy_lightbringer::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Radiant Blade, part 1: a melee hit casts the holy strike on the target, with the hit's damage (effect-0-value percent of it) as base damage. The strike adds its own
// share of spell power (set on the spell). It is a triggered cast of the Favor's own aura, so it cannot start another strike.
class spell_blessings_holy_radiant_blade : public AuraScript
{
    PrepareAuraScript(spell_blessings_holy_radiant_blade);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_RADIANT_STRIKE, SPELL_RADIANT_BLADE_HEAL });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim || !victim->IsAlive())
            return;

        Unit* paladin = GetTarget();
        if (!paladin->IsAlive() || !paladin->IsValidAttackTarget(victim))
            return;

        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), std::max(aurEff->GetAmount(), 0));
        if (damage > 0)
            paladin->CastCustomSpell(SPELL_RADIANT_STRIKE, SPELLVALUE_BASE_POINT0, damage, victim, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_holy_radiant_blade::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Radiant Blade, part 2: the strike, once it has hit, heals the most injured friend within 30 yards for the damage it dealt (as dealt: after critical strikes and resistance).
class spell_blessings_holy_radiant_strike : public SpellScript
{
    PrepareSpellScript(spell_blessings_holy_radiant_strike);

    void HandleAfterHit()
    {
        Unit* paladin = GetCaster();
        if (!paladin || !paladin->HasAura(SPELL_FAVOR_RADIANT_BLADE))
            return;

        int32 const damage = GetHitDamage();
        if (damage <= 0)
            return;

        if (Unit* ally = MostInjured(paladin, paladin, RADIANT_BLADE_HEAL_RANGE))
            BlessingsHelpers::HealUnit(paladin, ally, uint32(damage), SPELL_RADIANT_BLADE_HEAL);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_blessings_holy_radiant_strike::HandleAfterHit);
    }
};

void AddMoonlitBlessingsHolyScripts()
{
    RegisterSpellScript(spell_blessings_holy_frontline_faith);
    RegisterSpellScript(spell_blessings_holy_aura_of_mending);
    RegisterSpellScript(spell_blessings_holy_wings_of_mercy);
    RegisterSpellScript(spell_blessings_holy_beacon);
    RegisterSpellScript(spell_blessings_holy_lightbringer);
    RegisterSpellScript(spell_blessings_holy_radiant_blade);
    RegisterSpellScript(spell_blessings_holy_radiant_strike);
}
