// Moonlit Blessings: C++ spell scripts for the Penitent Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/penitent.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_penitent_damage_heal         Smite of Mercy and Penance Mend: a share of the damage heals the most injured ally
//   spell_blessings_penitent_holy_fire_mastery   each Smite refreshes the priest's Holy Fire burn on its target
//   spell_blessings_penitent_divine_judgment     a critical Holy Fire explodes on the enemies around its target
//   spell_blessings_penitent_fanatics_rage       Power Infusion also raises damage done, on the priest and on its target
//   spell_blessings_penitent_pillar_of_light     every sixth Smite calls a Pillar of Light on the enemies around its target
//   spell_blessings_penitent_inquisitor          Favor of the Inquisitor: Smite hits a second enemy for half and refunds mana on a critical
//   spell_blessings_penitent_zealot              Favor of the Zealot: Holy Fire's burn is copied to the nearest other enemy
//
// Spell ids come from the Penitent registry block 1,610,100 (units are listed at the top of tools/talents/recipes/penitent.py: the enum below must match them).
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <list>

namespace
{
    enum PenitentSpells : uint32
    {
        SPELL_DIVINE_JUDGMENT   = 1610151,   // single target holy hit for a fixed amount (the explosion)
        SPELL_FANATICS_RAGE     = 1610161,   // buff: damage done +20%
        SPELL_PILLAR_CHARGE     = 1610211,   // buff: counts Smites toward the next Pillar of Light (stacks)
        SPELL_PILLAR_DAMAGE     = 1610212,   // single target holy hit for a fixed amount (the pillar)
        SPELL_INQUISITOR_STRIKE = 1610221    // single target holy hit for a fixed amount (the second Smite)
    };

    constexpr uint32 FLAG_HOLY_FIRE = 0x100000;               // SpellFamilyFlags[0]
    constexpr uint8 PILLAR_EVERY = 6;                         // every sixth Smite calls the Pillar of Light
    constexpr int32 INQUISITOR_MANA_PCT = 3;                  // percent of maximum mana refunded by a critical Smite (Favor of the Inquisitor)

    // Every attackable enemy of `seeker` within `range` yards of `center` that `center` can see.
    std::list<Unit*> EnemiesAround(Unit* seeker, Unit* center, float range)
    {
        std::list<Unit*> targets;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, seeker, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, targets, check);
        Cell::VisitObjects(center, searcher, range);
        targets.remove_if([seeker, center](Unit* unit) { return !seeker->IsValidAttackTarget(unit) || !center->IsWithinLOSInMap(unit); });
        return targets;
    }

    // The nearest enemy to `center`, other than `center` itself, that does not carry one of the priest's own auras of `spellId` (any enemy when spellId is 0).
    Unit* NearestOtherEnemy(Unit* seeker, Unit* center, float range, uint32 spellId)
    {
        Unit* best = nullptr;
        float bestDistance = range + 1.0f;
        for (Unit* unit : EnemiesAround(seeker, center, range))
        {
            if (unit == center || (spellId && unit->GetAura(spellId, seeker->GetGUID())))
                continue;

            float const distance = center->GetDistance(unit);
            if (distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        }

        return best;
    }

    // The effects of a spell that are auras (what a copy of the spell applied with AddAura should carry).
    uint8 AuraEffectMask(SpellInfo const* info)
    {
        uint8 mask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (info->Effects[i].IsAura())
                mask |= uint8(1 << i);

        return mask;
    }
}

// Smite of Mercy and Penance Mend: the dummy effect holds the percent (amount) and the range in yards (misc value).
class spell_blessings_penitent_damage_heal : public AuraScript
{
    PrepareAuraScript(spell_blessings_penitent_damage_heal);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        Unit* caster = GetTarget();
        float const range = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        if (Unit* ally = BlessingsHelpers::MostInjuredFriend(caster, caster, range))
            BlessingsHelpers::HealUnit(caster, ally, CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()), GetSpellInfo()->Id);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_penitent_damage_heal::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Holy Fire Mastery: the proc is in the second effect (the first one is the burn modifier). Same refresh as the stock Pain and Suffering uses for Shadow Word: Pain.
class spell_blessings_penitent_holy_fire_mastery : public AuraScript
{
    PrepareAuraScript(spell_blessings_penitent_holy_fire_mastery);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim)
            return;

        if (AuraEffect* burn = victim->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_PRIEST, FLAG_HOLY_FIRE, 0, 0, caster->GetGUID()))
        {
            burn->GetBase()->RefreshTimersWithMods();
            burn->ChangeAmount(burn->CalculateAmount(burn->GetCaster()), false);
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_penitent_holy_fire_mastery::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Divine Judgment: the dummy effect holds the percent (amount) and the radius in yards (misc value). Only critical Holy Fire hits reach here (spell_proc hit mask).
class spell_blessings_penitent_divine_judgment : public AuraScript
{
    PrepareAuraScript(spell_blessings_penitent_divine_judgment);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !victim || !damageInfo->GetDamage())
            return;

        int32 damage = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        float const radius = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        for (Unit* unit : EnemiesAround(caster, victim, radius))
            if (unit != victim)
                caster->CastCustomSpell(unit, SPELL_DIVINE_JUDGMENT, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_penitent_divine_judgment::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Fanatic's Rage: the proc is in the second effect (the first one is the cooldown modifier). The buff is a friendly-target spell, so it can be cast on the Power Infusion target.
class spell_blessings_penitent_fanatics_rage : public AuraScript
{
    PrepareAuraScript(spell_blessings_penitent_fanatics_rage);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* target = eventInfo.GetActionTarget();
        caster->CastSpell(caster, SPELL_FANATICS_RAGE, true);
        if (target && target != caster && target->IsFriendlyTo(caster))
            caster->CastSpell(target, SPELL_FANATICS_RAGE, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_penitent_fanatics_rage::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Pillar of Light: the dummy effect's misc value is the radius in yards. Each Smite adds a charge; the Smite that finds five charges calls the pillar and empties them.
class spell_blessings_penitent_pillar_of_light : public AuraScript
{
    PrepareAuraScript(spell_blessings_penitent_pillar_of_light);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !victim)
            return;

        Aura const* charges = caster->GetAura(SPELL_PILLAR_CHARGE);
        if (!charges || charges->GetStackAmount() < PILLAR_EVERY - 1)
        {
            caster->CastSpell(caster, SPELL_PILLAR_CHARGE, true);
            return;
        }

        caster->RemoveAurasDueToSpell(SPELL_PILLAR_CHARGE);
        int32 damage = int32(damageInfo->GetDamage());
        if (!damage)
            return;

        float const radius = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        for (Unit* unit : EnemiesAround(caster, victim, radius))
            caster->CastCustomSpell(unit, SPELL_PILLAR_DAMAGE, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_penitent_pillar_of_light::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Inquisitor: the dummy effect holds the second hit's percent (amount) and its search range in yards (misc value).
class spell_blessings_penitent_inquisitor : public AuraScript
{
    PrepareAuraScript(spell_blessings_penitent_inquisitor);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !victim)
            return;

        if (eventInfo.GetHitMask() & PROC_HIT_CRITICAL)
            caster->EnergizeBySpell(caster, GetId(), CalculatePct(caster->GetMaxPower(POWER_MANA), INQUISITOR_MANA_PCT), POWER_MANA);

        int32 damage = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        if (damage <= 0)
            return;

        float const range = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        if (Unit* second = NearestOtherEnemy(caster, victim, range, 0))
            caster->CastCustomSpell(second, SPELL_INQUISITOR_STRIKE, &damage, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_penitent_inquisitor::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Zealot: the proc is in the second effect (the first one is the cooldown modifier), whose misc value is the spread range in yards.
// The copy is applied with AddAura: it carries the burn only (no second hit, no mana, nothing to proc), at the priest's current strength.
class spell_blessings_penitent_zealot : public AuraScript
{
    PrepareAuraScript(spell_blessings_penitent_zealot);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!victim || !info)
            return;

        if (Spell const* spell = eventInfo.GetProcSpell())
            if (spell->IsTriggered())
                return;

        float const range = float(GetSpellInfo()->Effects[EFFECT_1].MiscValue);
        if (Unit* other = NearestOtherEnemy(caster, victim, range, info->Id))
            caster->AddAura(info, AuraEffectMask(info), other);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_penitent_zealot::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

void AddMoonlitBlessingsPenitentScripts()
{
    RegisterSpellScript(spell_blessings_penitent_damage_heal);
    RegisterSpellScript(spell_blessings_penitent_holy_fire_mastery);
    RegisterSpellScript(spell_blessings_penitent_divine_judgment);
    RegisterSpellScript(spell_blessings_penitent_fanatics_rage);
    RegisterSpellScript(spell_blessings_penitent_pillar_of_light);
    RegisterSpellScript(spell_blessings_penitent_inquisitor);
    RegisterSpellScript(spell_blessings_penitent_zealot);
}
