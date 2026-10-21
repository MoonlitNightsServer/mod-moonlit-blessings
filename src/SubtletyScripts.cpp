// Moonlit Blessings: C++ spell scripts for the Subtlety Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/subtlety.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_subtlety_stealth            bound to the four stock Stealth ranks and the three Vanish stealth auras: Shadowed Guard (damage reduction while stealthed and
//                                               for 6 sec after) and Night Walker (speed for 6 sec after Stealth ends). Both are gated on the Boon's own passive.
//   spell_blessings_subtlety_shadow_dance       bound to the stock Shadow Dance (51713): Extended Repertoire's Hemorrhage buff lives while the Dance does; Grand Finale starts
//                                               when it ends.
//   spell_blessings_subtlety_shadowstep         bound to the stock Shadowstep bonus aura (36563): Umbral Blow and the Favor of the Shade add their own charged buffs while the
//                                               stock bonus lasts.
//   spell_blessings_subtlety_grand_finale_cast  bound to every player rank of Eviscerate, Envenom and Rupture: with the Grand Finale buff on, the cast counts as 5 combo points.
//   spell_blessings_subtlety_guard_stale        on the Shadowed Guard / Extended Repertoire helpers themselves (saved with the character): one loaded or created while its
//   spell_blessings_subtlety_repertoire_stale   condition is false fades at once, so a relog or a crash cannot leave it permanent.
//   spell_blessings_subtlety_helper_cleanup     on the passives of Shadowed Guard and Extended Repertoire: when the Boon is lost, its unlimited helper (always main id + 1) goes too.
//   spell_blessings_subtlety_dancer_echo        Favor of the Shadow Dancer: a hit of Backstab or Hemorrhage during Shadow Dance is echoed for a share of its damage.
//
// Everything else of the Subtlety recipes is spell data (modifiers, a proc that casts an energy helper, a speed aura).
// Stealth is shapeshift form 30 and Shadow Dance is form 13; for a rogue the core does NOT make one remove the other (HandleAuraModShapeshift skips the rogue family), so they
// coexist, and Shadow Dance never counts as Stealth here.
// Spell ids come from the Subtlety registry block 1,730,000 (tools/talents/recipes/subtlety.py asserts every unit and stock id named here).
// ~Moonlit Nights Team

#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Unit.h"

namespace
{
    enum SubtletySpells : uint32
    {
        SPELL_SHADOWED_GUARD        = 1730000,      // Boon passive; effect 0 value = seconds the guard lasts after Stealth ends
        SPELL_SHADOWED_GUARD_BUFF   = 1730001,      // unlimited -15% damage taken (main id + 1: the cleanup script relies on it)

        SPELL_EXTENDED_REPERTOIRE   = 1730030,      // Boon passive
        SPELL_REPERTOIRE_BUFF       = 1730031,      // unlimited: Hemorrhage costs 15 less energy

        SPELL_GRAND_FINALE          = 1730040,      // Boon passive
        SPELL_GRAND_FINALE_BUFF     = 1730041,      // 15 sec, one charge: +50% finisher damage

        SPELL_NIGHT_WALKER          = 1730050,      // Boon passive
        SPELL_NIGHT_WALKER_BUFF     = 1730051,      // 6 sec: +30% speed

        SPELL_UMBRAL_BLOW           = 1730060,      // Boon passive
        SPELL_UMBRAL_BLOW_BUFF      = 1730061,      // 10 sec, one charge: Ambush +30% damage, +100% crit

        SPELL_FAVOR_SHADE           = 1730080,      // Favor passive
        SPELL_SHADES_EDGE           = 1730081,      // 10 sec, one charge: the next ability +30% damage, +20% crit

        SPELL_FAVOR_DANCER          = 1730090,      // Favor passive; effect 2 value = percent of the hit echoed
        SPELL_DANCER_ECHO           = 1730091,      // the shadow echo (damage set per cast)

        SPELL_SHADOW_DANCE          = 51713         // stock: asserted in the recipe
    };

    // The helper buff is on you for as long as the condition holds: a leftover timed copy (from the last fade) is replaced by a fresh unlimited one.
    void HoldBuff(Unit* unit, uint32 buffId)
    {
        if (Aura const* aura = unit->GetAura(buffId))
        {
            if (aura->IsPermanent())
                return;

            unit->RemoveAurasDueToSpell(buffId);
        }

        unit->CastSpell(unit, buffId, true);
    }

    // The condition ended: the unlimited helper runs out `milliseconds` from now.
    void FadeBuff(Unit* unit, uint32 buffId, int32 milliseconds)
    {
        if (Aura* aura = unit->GetAura(buffId))
        {
            if (!aura->IsPermanent())
                return;

            aura->SetMaxDuration(milliseconds);
            aura->SetDuration(milliseconds);
        }
    }

    // Is there a stealth aura other than `skip` (the one that is going away)? Stealth and the Vanish aura overlap for a moment after a Vanish.
    bool HasOtherStealth(Unit const* unit, AuraEffect const* skip)
    {
        for (AuraEffect const* effect : unit->GetAuraEffectsByType(SPELL_AURA_MOD_STEALTH))
            if (effect != skip)
                return true;

        return false;
    }
}

// Shadowed Guard and Night Walker. Bound to the stock Stealth ranks and the Vanish stealth auras, both of which carry the stealth aura as their second effect, so it runs for every
// rogue that ever hides: it does nothing unless the Boon's passive is on the unit.
class spell_blessings_subtlety_stealth : public AuraScript
{
    PrepareAuraScript(spell_blessings_subtlety_stealth);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (target->HasAura(SPELL_SHADOWED_GUARD))
            HoldBuff(target, SPELL_SHADOWED_GUARD_BUFF);

        if (target->HasAura(SPELL_NIGHT_WALKER))
            target->RemoveAurasDueToSpell(SPELL_NIGHT_WALKER_BUFF);
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (HasOtherStealth(target, aurEff))
            return;

        if (AuraEffect const* guard = target->GetAuraEffect(SPELL_SHADOWED_GUARD, EFFECT_0))
            FadeBuff(target, SPELL_SHADOWED_GUARD_BUFF, std::max<int32>(guard->GetAmount(), 1) * IN_MILLISECONDS);

        if (target->IsAlive() && target->HasAura(SPELL_NIGHT_WALKER))
            target->CastSpell(target, SPELL_NIGHT_WALKER_BUFF, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_subtlety_stealth::HandleApply, EFFECT_1, SPELL_AURA_MOD_STEALTH, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_subtlety_stealth::HandleRemove, EFFECT_1, SPELL_AURA_MOD_STEALTH, AURA_EFFECT_HANDLE_REAL);
    }
};

// Extended Repertoire and Grand Finale. Bound to the stock Shadow Dance, whose third effect is the shapeshift form: it starts when the Dance starts and ends when it ends,
// however long Encore or the Favor of the Shadow Dancer made it.
class spell_blessings_subtlety_shadow_dance : public AuraScript
{
    PrepareAuraScript(spell_blessings_subtlety_shadow_dance);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (target->HasAura(SPELL_EXTENDED_REPERTOIRE))
            HoldBuff(target, SPELL_REPERTOIRE_BUFF);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_REPERTOIRE_BUFF);
        if (target->IsAlive() && target->HasAura(SPELL_GRAND_FINALE))
            target->CastSpell(target, SPELL_GRAND_FINALE_BUFF, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_subtlety_shadow_dance::HandleApply, EFFECT_2, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_subtlety_shadow_dance::HandleRemove, EFFECT_2, SPELL_AURA_MOD_SHAPESHIFT, AURA_EFFECT_HANDLE_REAL);
    }
};

// Umbral Blow and the Favor of the Shade. Bound to the stock Shadowstep bonus aura (36563: +20% damage on the next ability, one charge, 10 sec; its second effect is the
// damage modifier). The charged buffs of the Boon and the Favor are on you while it is, so they last exactly as long as the stock bonus: until your next ability, or 10 sec.
class spell_blessings_subtlety_shadowstep : public AuraScript
{
    PrepareAuraScript(spell_blessings_subtlety_shadowstep);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (target->HasAura(SPELL_UMBRAL_BLOW))
            target->CastSpell(target, SPELL_UMBRAL_BLOW_BUFF, true);

        if (target->HasAura(SPELL_FAVOR_SHADE))
            target->CastSpell(target, SPELL_SHADES_EDGE, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_UMBRAL_BLOW_BUFF);
        target->RemoveAurasDueToSpell(SPELL_SHADES_EDGE);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_subtlety_shadowstep::HandleApply, EFFECT_1, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_subtlety_shadowstep::HandleRemove, EFFECT_1, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// Grand Finale: runs before the cast is carried out (the core has ALREADY checked once that you have a combo point on the target, so at least 1 is needed). While the Grand Finale buff is on the caster, the combo points on the target are
// raised to 5 (Unit::AddComboPoints keeps points already on that target, caps at 5, and starts again at 5 on a new target), so the finisher counts as a 5 point one.
// If the cast then fails (no energy, out of range) the points stay; they are spent by the next finisher as usual.
class spell_blessings_subtlety_grand_finale_cast : public SpellScript
{
    PrepareSpellScript(spell_blessings_subtlety_grand_finale_cast);

    void HandleBeforeCast()
    {
        Unit* caster = GetCaster();
        Unit* target = GetExplTargetUnit();
        if (!caster || !target || !caster->HasAura(SPELL_GRAND_FINALE_BUFF))
            return;

        caster->AddComboPoints(target, 5);
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_blessings_subtlety_grand_finale_cast::HandleBeforeCast);
    }
};

// The unlimited helpers are saved with the character. If one is loaded (after a relog or a crash) while its condition is false it must not stay: it fades after 1 ms.
// Shadowed Guard needs a stealth aura, Extended Repertoire needs the Dance.
class spell_blessings_subtlety_guard_stale : public AuraScript
{
    PrepareAuraScript(spell_blessings_subtlety_guard_stale);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target->HasStealthAura())
            FadeBuff(target, GetId(), 1);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_subtlety_guard_stale::HandleApply, EFFECT_0, SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN, AURA_EFFECT_HANDLE_REAL);
    }
};

class spell_blessings_subtlety_repertoire_stale : public AuraScript
{
    PrepareAuraScript(spell_blessings_subtlety_repertoire_stale);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (!target->HasAura(SPELL_SHADOW_DANCE))
            FadeBuff(target, GetId(), 1);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_subtlety_repertoire_stale::HandleApply, EFFECT_0, SPELL_AURA_ADD_FLAT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// Passives of Shadowed Guard and Extended Repertoire: the unlimited helper is the passive's id + 1; it must not outlive the Boon (a respec while hidden or Dancing).
class spell_blessings_subtlety_helper_cleanup : public AuraScript
{
    PrepareAuraScript(spell_blessings_subtlety_helper_cleanup);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetId() + 1);
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_subtlety_helper_cleanup::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Shadow Dancer: the proc (spell_proc row) is the hit of Backstab or Hemorrhage. Only while the Dance is on you does the hit echo, for the third effect's percent of
// the damage it did. The percent is taken from the hit actually dealt, so a critical hit echoes a critical's worth.
class spell_blessings_subtlety_dancer_echo : public AuraScript
{
    PrepareAuraScript(spell_blessings_subtlety_dancer_echo);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim || !victim->IsAlive() || !caster->HasAura(SPELL_SHADOW_DANCE))
            return;

        int32 const amount = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        if (amount <= 0)
            return;

        int32 const baseDamage = amount - 1;     // the amount of a one-value effect is its base points plus one
        caster->CastCustomSpell(victim, SPELL_DANCER_ECHO, &baseDamage, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_subtlety_dancer_echo::HandleProc, EFFECT_2, SPELL_AURA_DUMMY);
    }
};

void AddMoonlitBlessingsSubtletyScripts()
{
    RegisterSpellScript(spell_blessings_subtlety_stealth);
    RegisterSpellScript(spell_blessings_subtlety_shadow_dance);
    RegisterSpellScript(spell_blessings_subtlety_shadowstep);
    RegisterSpellScript(spell_blessings_subtlety_grand_finale_cast);
    RegisterSpellScript(spell_blessings_subtlety_guard_stale);
    RegisterSpellScript(spell_blessings_subtlety_repertoire_stale);
    RegisterSpellScript(spell_blessings_subtlety_helper_cleanup);
    RegisterSpellScript(spell_blessings_subtlety_dancer_echo);
}
