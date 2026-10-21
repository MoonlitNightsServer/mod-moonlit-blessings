// Moonlit Blessings: C++ spell scripts for the Arms Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/arms.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_arms_wide_sweep   Boon (Wide Sweep): bound to the two stock hits of Sweeping Strikes (12723, 26654). When one lands and the warrior has the Boon, a third
//                                     enemy near him is struck by a clone of that hit.
//   spell_blessings_arms_bladestorm   Boons (Wide Sweep, Storm Sovereign) and Favor (Bladestorm Master): bound to the stock Bladestorm aura (46924). While it runs, the helper
//                                     auras of the Boons and the Favor the warrior has are on him; the helpers are spell modifiers on the Whirlwind class mask (which the
//                                     player's own Whirlwind shares, hence the helpers exist only while Bladestorm runs).
//
// Everything else of the Arms recipes is spell data (modifiers, ignore armour, ignore combat result, a proc and a one-charge buff).
// Spell ids come from the Arms registry block 2,010,000 (tools/talents/recipes/arms.py asserts every unit and stock id named here).
// ~Moonlit Nights Team

#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Unit.h"

namespace
{
    enum ArmsSpells : uint32
    {
        SPELL_SWEEPING_STRIKES_HIT          = 12723,        // stock: the extra strike of Sweeping Strikes (a damage hit carrying the first hit's damage as its value)
        SPELL_SWEEPING_STRIKES_EXECUTE_HIT  = 26654,        // stock: the extra strike when Execute started it (normalized weapon damage)

        SPELL_WIDE_SWEEP                    = 2010000,      // the Boon's passive
        SPELL_WIDE_SWEEP_BLADESTORM         = 2010001,      // helper: Whirlwind radius +2 yards while Bladestorm runs
        SPELL_WIDE_SWEEP_STRIKE             = 2010002,      // clone of 12723 for the third enemy
        SPELL_WIDE_SWEEP_EXECUTE_STRIKE     = 2010003,      // clone of 26654 for the third enemy

        SPELL_STORM_SOVEREIGN               = 2010020,
        SPELL_STORM_SOVEREIGN_BLADESTORM    = 2010021,      // helper: the Whirlwind tick is not capped at four targets

        SPELL_BLADESTORM_MASTER             = 2010030,      // the Favor's passive
        SPELL_BLADESTORM_MASTER_BLADESTORM  = 2010031       // helper: Whirlwind +30% damage and +4 yards
    };

    struct BladestormHelper
    {
        uint32 source;      // the passive of the Boon or Favor
        uint32 helper;      // the buff cast on the warrior while Bladestorm runs
    };

    constexpr BladestormHelper BLADESTORM_HELPERS[] =
    {
        { SPELL_WIDE_SWEEP,       SPELL_WIDE_SWEEP_BLADESTORM },
        { SPELL_STORM_SOVEREIGN,  SPELL_STORM_SOVEREIGN_BLADESTORM },
        { SPELL_BLADESTORM_MASTER, SPELL_BLADESTORM_MASTER_BLADESTORM }
    };
}

// Wide Sweep: stock Sweeping Strikes casts 12723 (or 26654) at a second enemy. This script is bound to both stock spells, so it runs for every caster of them: it does
// nothing unless the caster has the Boon. Unit::SelectNearbyNoTotemTarget leaves out the caster's current victim and the enemy that was just struck, so the third strike
// goes to a different enemy within melee reach. The clones have their own ids, so the script does not run for them (no chain).
class spell_blessings_arms_wide_sweep : public SpellScript
{
    PrepareSpellScript(spell_blessings_arms_wide_sweep);

    void HandleHit(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* struck = GetHitUnit();
        if (!caster || !struck || !caster->HasAura(SPELL_WIDE_SWEEP))
            return;

        Unit* third = caster->SelectNearbyNoTotemTarget(struck);
        if (!third)
            return;

        if (GetSpellInfo()->Id == SPELL_SWEEPING_STRIKES_EXECUTE_HIT)
        {
            caster->CastSpell(third, SPELL_WIDE_SWEEP_EXECUTE_STRIKE, true);
            return;
        }

        int32 const damage = GetEffectValue();
        if (damage > 0)
            caster->CastCustomSpell(SPELL_WIDE_SWEEP_STRIKE, SPELLVALUE_BASE_POINT0, damage, third, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_arms_wide_sweep::HandleHit, EFFECT_0, SPELL_EFFECT_ANY);
    }
};

// Bladestorm: when the stock aura starts (its first Whirlwind tick comes on the next aura update, after this handler) the helpers of the Boons and the Favor the warrior
// has are cast on him; when it ends they are removed (the helpers also run out by themselves after 10 sec, longer than any Bladestorm).
class spell_blessings_arms_bladestorm : public AuraScript
{
    PrepareAuraScript(spell_blessings_arms_bladestorm);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        for (BladestormHelper const& entry : BLADESTORM_HELPERS)
            if (target->HasAura(entry.source))
                target->CastSpell(target, entry.helper, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        for (BladestormHelper const& entry : BLADESTORM_HELPERS)
            target->RemoveAurasDueToSpell(entry.helper);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_arms_bladestorm::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_arms_bladestorm::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }
};

void AddMoonlitBlessingsArmsScripts()
{
    RegisterSpellScript(spell_blessings_arms_wide_sweep);
    RegisterSpellScript(spell_blessings_arms_bladestorm);
}
