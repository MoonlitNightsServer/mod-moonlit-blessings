// Moonlit Blessings: C++ spell scripts for the Combat Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/combat.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_combat_barbed_throw        Boon: a Throw or Deadly Throw that hits opens a bleed for a share of its damage (what is left of the old bleed is added)
//   spell_blessings_combat_blade_flurry_strike Boon (Wider Whirl) and Favor (Whirling Blades): bound to the STOCK extra strike of Blade Flurry (22482). When the rogue has
//                                              either, the strike is repeated (as a clone with its own id, so it cannot chain) on more enemies near him
//   spell_blessings_combat_rain_of_blades      Boon: a Sinister Strike that hits while Blade Flurry lasts also hits every other enemy near the target for a share of its damage
//   spell_blessings_combat_death_from_afar     Boon: a critical Deadly Throw gives back half of the energy it cost
//   spell_blessings_combat_perfect_duel        Boon: while the rogue fights one enemy at most, two helper auras are on him (no dodge / parry / block for Sinister Strike and
//                                              Eviscerate; critical strikes 15% stronger); checked once a second
//   spell_blessings_combat_duellist            Favor: a critical Sinister Strike adds a combo point (the proc row limits it to once every 2 sec)
//
// Everything else of the Combat recipes is spell data (modifiers on Throw, Deadly Throw, Sinister Strike and Blade Flurry). Spell ids come from the Combat registry block 1,720,000;
// tools/talents/recipes/combat.py asserts every unit named below. The amounts, ranges and helper spells are read from the effects of the Boon's or Favor's own passive, so the spell
// data stays the one place a number is written. A script on a passive only runs while the Boon or Favor is there; the script bound to the stock strike first looks for them.
// ~Moonlit Nights Team

#include "CombatManager.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "Player.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"

#include <algorithm>
#include <list>
#include <vector>

namespace
{
    enum CombatSpells : uint32
    {
        SPELL_COMBAT_BLADE_FLURRY        = 13877,        // stock: the Blade Flurry aura (the spell the rogue casts)
        SPELL_COMBAT_BLADE_FLURRY_STRIKE = 22482,        // stock: the extra strike the core's Blade Flurry script casts at one nearby enemy

        SPELL_COMBAT_WIDER_WHIRL         = 1720030,      // effect 1: amount = how many more enemies Blade Flurry strikes
        SPELL_COMBAT_FLURRY_STRIKE_CLONE = 1720120,      // clone of 22482 under our own id (the extra strikes of the two lines above)
        SPELL_COMBAT_FAVOR_WHIRLING      = 1720110       // effect 1: amount = how many more enemies Blade Flurry strikes
    };

    constexpr uint32 FLAGS0_THROW = 0x00000001;          // SpellFamilyFlags word 0 of Throw (the only Rogue spell with it)
    constexpr uint32 FLAGS1_DEADLY_THROW = 0x00000001;   // word 1 of Deadly Throw (the only Rogue spell with it)
    constexpr uint32 FLAGS0_SINISTER_STRIKE = 0x00000002;
    constexpr float FLURRY_RANGE = 5.0f;                 // yards from the rogue: the range the core's Blade Flurry script searches (NOMINAL_MELEE_RANGE)
    constexpr uint32 MAX_EXTRA_TARGETS = 10;             // the most enemies Rain of Blades hits besides the target
    constexpr int32 BARBED_TICKS = 6;                    // the bleed lasts 6 sec with a tick every second (asserted in the recipe)

    // Every attackable enemy of `seeker` within `range` yards of `center` that `center` can see (`center` itself included when it qualifies).
    std::list<Unit*> CombatEnemiesAround(Unit* seeker, Unit* center, float range)
    {
        std::list<Unit*> targets;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, seeker, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, targets, check);
        Cell::VisitObjects(center, searcher, range);
        targets.remove_if([seeker, center](Unit* unit) { return !seeker->IsValidAttackTarget(unit) || !center->IsWithinLOSInMap(unit); });
        return targets;
    }

    // Hit `target` as `caster` with a flat-hit helper spell (instant, no spell power, cannot miss or critically hit): the damage is exactly `amount` before armour.
    void CombatCastFlat(Unit* caster, Unit* target, uint32 spellId, int32 amount)
    {
        if (!caster || !target || amount <= 0 || !target->IsAlive())
            return;

        caster->CastCustomSpell(spellId, SPELLVALUE_BASE_POINT0, amount - 1, target, true);     // the spell adds 1 to its base points
    }

    // `percent` percent of `damage` as a flat hit on every enemy within `range` yards of `center` except `center` (up to `limit` of them).
    void CombatHitAround(Unit* caster, Unit* center, uint32 damage, int32 percent, float range, uint32 hitSpell, uint32 limit)
    {
        int32 const amount = int32(std::min<uint64>(uint64(damage) * uint64(std::max(percent, 0)) / 100, 2000000000));
        if (amount <= 0)
            return;

        uint32 count = 0;
        for (Unit* unit : CombatEnemiesAround(caster, center, range))
        {
            if (unit == center || !unit->IsAlive())
                continue;

            CombatCastFlat(caster, unit, hitSpell, amount);
            if (++count >= limit)
                break;
        }
    }

    // The proc row of Throw and Deadly Throw is exact (the recipe seeds the mask); these tests only make the script safe if the row is ever widened.
    bool IsThrowOrDeadlyThrow(SpellInfo const* spell)
    {
        return spell && spell->SpellFamilyName == SPELLFAMILY_ROGUE && ((spell->SpellFamilyFlags[0] & FLAGS0_THROW) || (spell->SpellFamilyFlags[1] & FLAGS1_DEADLY_THROW));
    }

    bool IsSinisterStrike(SpellInfo const* spell)
    {
        return spell && spell->SpellFamilyName == SPELLFAMILY_ROGUE && (spell->SpellFamilyFlags[0] & FLAGS0_SINISTER_STRIKE);
    }

    // How many more enemies than the one the core picks Blade Flurry strikes: Wider Whirl (effect 1 of its passive) plus the Favor of the Whirling Blades (effect 1 of its passive).
    int32 ExtraFlurryStrikes(Unit const* caster)
    {
        int32 extra = 0;
        if (AuraEffect const* wider = caster->GetAuraEffect(SPELL_COMBAT_WIDER_WHIRL, EFFECT_1))
            extra += std::max(wider->GetAmount(), 0);
        if (AuraEffect const* favor = caster->GetAuraEffect(SPELL_COMBAT_FAVOR_WHIRLING, EFFECT_1))
            extra += std::max(favor->GetAmount(), 0);
        return extra;
    }

    // Does `unit` fight at most one enemy? Counts its own combat references (enemies in combat with it that are alive and not suppressed); no grid search.
    bool FightsAtMostOneEnemy(Unit const* unit)
    {
        uint32 count = 0;
        CombatManager const& combat = unit->GetCombatManager();
        for (auto const& entry : combat.GetPvECombatRefs())
        {
            Unit* other = entry.second->GetOther(unit);
            if (other && other->IsAlive() && !entry.second->IsSuppressedFor(unit))
                ++count;
        }

        for (auto const& entry : combat.GetPvPCombatRefs())
        {
            Unit* other = entry.second->GetOther(unit);
            if (other && other->IsAlive() && !entry.second->IsSuppressedFor(unit))
                ++count;
        }

        return count <= 1;
    }
}

// Barbed Throw. Effect 0: amount = percent of the damage of the throw that the bleed deals in all, trigger spell = the bleed (its effect 0 takes the damage of ONE tick as base points).
// The bleed is re-cast on every throw that hits; what is left of the old bleed (ticks still to come) is added to the new one, so quick throwing never throws damage away.
class spell_blessings_combat_barbed_throw : public AuraScript
{
    PrepareAuraScript(spell_blessings_combat_barbed_throw);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return IsThrowOrDeadlyThrow(eventInfo.GetSpellInfo());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        if (!victim || !victim->IsAlive() || !damageInfo || !damageInfo->GetDamage())
            return;

        uint32 const bleed = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        int64 total = int64(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        if (AuraEffect const* old = victim->GetAuraEffect(bleed, EFFECT_0, caster->GetGUID()))
        {
            int32 const ticksLeft = std::max<int32>(old->GetTotalTicks() - int32(old->GetTickNumber()), 0);
            total += int64(old->GetAmount()) * ticksLeft;
        }

        int32 const perTick = int32(std::min<int64>(total / BARBED_TICKS, 2000000000));
        if (perTick <= 0)
            return;

        caster->CastCustomSpell(bleed, SPELLVALUE_BASE_POINT0, perTick - 1, victim, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_combat_barbed_throw::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_combat_barbed_throw::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// The extra strike of Blade Flurry (stock 22482, bound by the recipe; the core's script casts it with the unmitigated damage of the hit as its base points and the critical result of
// the hit forced). When the rogue has Wider Whirl or the Favor of the Whirling Blades it is repeated on that many more enemies within 5 yards of the rogue, never the rogue's own victim,
// never the enemy the core already chose, never a totem. The repeats are casts of a clone with another id (it carries no script and cannot proc Blade Flurry), with the same damage and
// the same critical result. Spell damage of the stock strike is already the whole damage of the hit, so there is nothing to add for "full damage".
class spell_blessings_combat_blade_flurry_strike : public SpellScript
{
    PrepareSpellScript(spell_blessings_combat_blade_flurry_strike);

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* struck = GetHitUnit();
        Spell* spell = GetSpell();
        if (!caster || !struck || !spell || !caster->IsPlayer())
            return;

        int32 const extra = ExtraFlurryStrikes(caster);
        if (extra <= 0)
            return;

        SpellValue const* value = spell->GetSpellValue();
        std::vector<Unit*> pool;
        for (Unit* unit : CombatEnemiesAround(caster, caster, FLURRY_RANGE))
            if (unit != struck && unit != caster->GetVictim() && unit->IsAlive() && !unit->IsTotem())
                pool.push_back(unit);

        for (int32 i = 0; i < extra && !pool.empty(); ++i)
        {
            size_t const pick = urand(0, uint32(pool.size() - 1));
            Unit* target = pool[pick];
            pool.erase(pool.begin() + pick);

            CustomSpellValues values;
            values.AddSpellMod(SPELLVALUE_BASE_POINT0, value->EffectBasePoints[EFFECT_0]);
            values.AddSpellMod(SPELLVALUE_FORCED_CRIT_RESULT, int32(value->ForcedCritResult));
            caster->CastCustomSpell(SPELL_COMBAT_FLURRY_STRIKE_CLONE, values, target, TRIGGERED_FULL_MASK);
        }
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_combat_blade_flurry_strike::HandleHit, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Rain of Blades. Effect 0: amount = percent of the Sinister Strike damage, misc = yards, trigger spell = the flat hit. Only while Blade Flurry (13877) is on the rogue. The damage is the
// unmitigated damage of the strike (armour reduces each flat hit again, by the armour of the enemy it hits).
class spell_blessings_combat_rain_of_blades : public AuraScript
{
    PrepareAuraScript(spell_blessings_combat_rain_of_blades);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return IsSinisterStrike(eventInfo.GetSpellInfo()) && GetTarget()->HasAura(SPELL_COMBAT_BLADE_FLURRY);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!victim || !victim->IsAlive() || !damageInfo)
            return;

        SpellInfo const* info = GetSpellInfo();
        CombatHitAround(caster, victim, damageInfo->GetUnmitigatedDamage(), aurEff->GetAmount(), float(info->Effects[EFFECT_0].MiscValue), info->Effects[EFFECT_0].TriggerSpell,
                        MAX_EXTRA_TARGETS);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_combat_rain_of_blades::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_combat_rain_of_blades::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Death from Afar. Effect 1 (a dummy): proc on a critical Deadly Throw. The rogue gets back half of the energy that throw cost (the real cost, so Quick Draw is counted).
class spell_blessings_combat_death_from_afar : public AuraScript
{
    PrepareAuraScript(spell_blessings_combat_death_from_afar);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        return spell && spell->SpellFamilyName == SPELLFAMILY_ROGUE && (spell->SpellFamilyFlags[1] & FLAGS1_DEADLY_THROW);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Spell const* procSpell = eventInfo.GetProcSpell();
        int32 cost = procSpell ? procSpell->GetPowerCost() : 0;
        if (cost <= 0)
            cost = eventInfo.GetSpellInfo()->ManaCost;

        int32 const back = CalculatePct(cost, aurEff->GetAmount());
        if (back > 0 && caster->IsAlive())
            caster->EnergizeBySpell(caster, GetId(), uint32(back), POWER_ENERGY);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_combat_death_from_afar::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_combat_death_from_afar::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Perfect Duel. Effect 0 is a periodic dummy every second (trigger spell = the helper that takes dodge, parry and block out of the roll for Sinister Strike and Eviscerate), effect 1
// holds the second helper (critical strikes 15% stronger). Both helpers are on the rogue while he fights at most one enemy and are taken off when a second enemy joins the fight or the
// Boon is lost. Checked when the Boon starts and then once a second, so a pull of several enemies takes up to a second to switch the helpers off.
class spell_blessings_combat_perfect_duel : public AuraScript
{
    PrepareAuraScript(spell_blessings_combat_perfect_duel);

    void Update(Unit* target)
    {
        bool const duel = target->IsAlive() && FightsAtMostOneEnemy(target);
        SpellInfo const* info = GetSpellInfo();
        uint32 const helpers[] = { info->Effects[EFFECT_0].TriggerSpell, info->Effects[EFFECT_1].TriggerSpell };
        for (uint32 helper : helpers)
        {
            if (!helper)
                continue;

            if (!duel)
                target->RemoveAurasDueToSpell(helper);
            else if (!target->HasAura(helper))
                target->CastSpell(target, helper, true);
        }
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Update(GetTarget());
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Update(GetTarget());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        SpellInfo const* info = GetSpellInfo();
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(info->Effects[EFFECT_0].TriggerSpell);
        target->RemoveAurasDueToSpell(info->Effects[EFFECT_1].TriggerSpell);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_combat_perfect_duel::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_combat_perfect_duel::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_combat_perfect_duel::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Duellist. Effect 1 (a dummy): proc on a critical Sinister Strike (the proc row is exact for Sinister Strike and has a 2 sec cooldown). It adds one combo point on the
// enemy that was struck; the core caps the points at five.
class spell_blessings_combat_duellist : public AuraScript
{
    PrepareAuraScript(spell_blessings_combat_duellist);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        return IsSinisterStrike(eventInfo.GetSpellInfo());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (victim && victim->IsAlive() && caster->IsPlayer())
            caster->AddComboPoints(victim, 1);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_combat_duellist::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_combat_duellist::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

void AddMoonlitBlessingsCombatScripts()
{
    RegisterSpellScript(spell_blessings_combat_barbed_throw);
    RegisterSpellScript(spell_blessings_combat_blade_flurry_strike);
    RegisterSpellScript(spell_blessings_combat_rain_of_blades);
    RegisterSpellScript(spell_blessings_combat_death_from_afar);
    RegisterSpellScript(spell_blessings_combat_perfect_duel);
    RegisterSpellScript(spell_blessings_combat_duellist);
}
