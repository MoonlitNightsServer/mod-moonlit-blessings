// Moonlit Blessings: C++ spell scripts for the Martyr Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/martyr.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_martyr_aegis_hooks               a second script on the Eternal Aegis shield (1640011): Martyr's Vigor, Thorned Shield, Mending Wounds,
//                                          the extra refill of Guarded Faith / Favor of the Bulwark, the Rooted in Faith switch
//   spell_martyr_sanctified_pact           the castable ability Sanctified Pact
//   spell_martyr_radiant_pulse             Radiant Pulse: the periodic trigger fires only with a shield up and in combat
//   spell_martyr_sanctified_blow_boons     a second script on Sanctified Blow: Wrath of the Aegis, Blessed Strikes
//   spell_martyr_burning_strike_boons      a second script on Burning Strike: Blessed Strikes
//   spell_martyr_righteous_cleave_boons    a second script on Righteous Cleave: Righteous Siphon, the spread of Burning Retribution
//   spell_martyr_burning_retribution       Burning Retribution: melee hits taken burn the attacker
//   spell_martyr_lights_bulwark            Light's Bulwark: a hit below 40% health refills the Aegis
//   spell_martyr_favor_redeemer            Favor of the Redeemer: every melee hit adds to the Aegis
//   MoonlitMartyrUnitScript                Unyielding Martyr (damage hook, after absorbs)
//
// The shared helpers (cap, refill, adding to the Aegis) are in BlessingsHelpers.h, namespace BlessingsHelpers::Martyr. The values are the amounts of the Boon spells' effects.
// ~Moonlit Nights Team

#include "Cell.h"
#include "CellImpl.h"
#include "BlessingsHelpers.h"
#include "GridNotifiers.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "UnitScript.h"

#include <list>
#include <vector>

namespace Martyr = BlessingsHelpers::Martyr;

namespace
{
    constexpr int32 RETRIBUTION_TICKS = 4;                 // the burn lasts 4 sec at one tick per second (see the duration of spell 1640311)

    // Glyph of the Martyr, Reflective Shield rewrite: the reflected damage is split evenly among the foes within 10 yards of the Martyr.
    void ReflectAround(Unit* martyr, int32 total)
    {
        std::list<Unit*> foes;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(martyr, martyr, 10.0f);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(martyr, foes, check);
        Cell::VisitObjects(martyr, searcher, 10.0f);
        foes.remove_if([martyr](Unit* unit) { return !martyr->IsValidAttackTarget(unit); });
        if (foes.empty())
            return;

        int32 const each = total / int32(foes.size());
        if (each <= 0)
            return;

        for (Unit* foe : foes)
            martyr->CastCustomSpell(foe, Martyr::SPELL_THORNED_DAMAGE, &each, nullptr, nullptr, true);
    }
}

// A second script on the Eternal Aegis shield. The owner's script (spell_blessings_eternal_aegis_shield) keeps the shield and its one-second refill; this one adds what the
// Boons need from the shield: damage it absorbed and the extra refill.
class spell_martyr_aegis_hooks : public AuraScript
{
    PrepareAuraScript(spell_martyr_aegis_hooks);

    void HandleAbsorb(AuraEffect* aurEff, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Unit* target = GetTarget();

        // Keep-alive: the core removes an absorb aura whose amount reaches 0 after this hook, and nothing would bring the Aegis back. Leave 1 point, so the shield (and its
        // refill tick) always stays; an Aegis holding only 1 point is an empty one.
        if (aurEff->GetAmount() - int32(absorbAmount) < Martyr::AEGIS_EMPTY_AMOUNT)
            aurEff->SetAmount(int32(absorbAmount) + Martyr::AEGIS_EMPTY_AMOUNT);

        if (absorbAmount)
        {
            // Martyr's Vigor: every step of maximum health absorbed restores a share of the mana.
            if (AuraEffect const* vigor = target->GetAuraEffect(Martyr::SPELL_MARTYRS_VIGOR, EFFECT_0))
            {
                _absorbedForVigor += absorbAmount;
                uint32 const step = target->CountPctFromMaxHealth(std::max(vigor->GetAmount(), 1));
                uint32 const steps = step ? _absorbedForVigor / step : 0;
                if (steps)
                {
                    _absorbedForVigor -= steps * step;
                    int32 const manaPct = Martyr::Param(target, Martyr::SPELL_MARTYRS_VIGOR, EFFECT_1);
                    target->EnergizeBySpell(target, Martyr::SPELL_MARTYRS_VIGOR, CalculatePct(target->GetMaxPower(POWER_MANA), manaPct * int32(steps)), POWER_MANA);
                }
            }

            // Thorned Shield: a share of the absorbed damage goes back to the attacker as holy damage (never from its own reflection).
            if (AuraEffect const* thorned = target->GetAuraEffect(Martyr::SPELL_THORNED_SHIELD, EFFECT_0))
            {
                Unit* attacker = dmgInfo.GetAttacker();
                SpellInfo const* source = dmgInfo.GetSpellInfo();
                if (attacker && attacker != target && attacker->IsAlive() && target->IsValidAttackTarget(attacker) && (!source || source->Id != Martyr::SPELL_THORNED_DAMAGE))
                {
                    int32 const damage = CalculatePct(int32(absorbAmount), thorned->GetAmount());
                    if (damage > 0)
                        target->CastCustomSpell(attacker, Martyr::SPELL_THORNED_DAMAGE, &damage, nullptr, nullptr, true);
                }
            }

            // Reflective Shield rewrite: 20% per rank of the absorbed damage hits every foe around the Martyr, split evenly (never from its own reflection).
            if (uint32 const reflective = Martyr::VariantRank(target, Martyr::SPELL_GLYPH_REFLECTIVE, 2))
            {
                SpellInfo const* source = dmgInfo.GetSpellInfo();
                if (dmgInfo.GetAttacker() && dmgInfo.GetAttacker() != target && (!source || source->Id != Martyr::SPELL_THORNED_DAMAGE))
                    ReflectAround(target, CalculatePct(int32(absorbAmount), 20 * int32(reflective)));
            }

            // Mending Wounds: a share of the absorbed damage heals the player.
            if (AuraEffect const* mending = target->GetAuraEffect(Martyr::SPELL_MENDING_WOUNDS, EFFECT_0))
                BlessingsHelpers::HealUnit(target, target, CalculatePct(absorbAmount, mending->GetAmount()), Martyr::SPELL_MENDING_WOUNDS);
        }

        // The core takes the absorbed damage off the shield after this hook, so tell the Rooted in Faith check about it.
        Martyr::RefreshRooted(target, int32(absorbAmount));
    }

    // Guarded Faith and Favor of the Bulwark: a share of the normal refill is added on top of it each second.
    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        int32 const bonus = Martyr::AegisRefillBonusPct(target);
        if (bonus > 0)
            if (AuraEffect* absorb = GetAura()->GetEffect(EFFECT_0))
            {
                int32 const cap = Martyr::AegisCap(target);
                if (absorb->GetAmount() < cap)
                    absorb->SetAmount(std::min(absorb->GetAmount() + CalculatePct(Martyr::AegisRefillPerSecond(target), bonus), cap));
            }

        Martyr::RefreshRooted(target);
    }

    void Register() override
    {
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_martyr_aegis_hooks::HandleAbsorb, EFFECT_0);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_martyr_aegis_hooks::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }

private:
    uint32 _absorbedForVigor = 0;
};

// Sanctified Pact: give up a share of the maximum health (taken directly, so the Aegis neither absorbs it nor reflects it) to refill the Aegis.
// Glyph of the Martyr, Divine Aegis rewrite: a melee critical strike adds a share (effect 0, 10% per rank) of its damage to the Aegis.
class spell_martyr_glyph_divine_aegis : public AuraScript
{
    PrepareAuraScript(spell_martyr_glyph_divine_aegis);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        if (damageInfo && damageInfo->GetDamage())
            Martyr::AddToAegis(GetTarget(), CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount()));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_martyr_glyph_divine_aegis::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

class spell_martyr_sanctified_pact : public SpellScript
{
    PrepareSpellScript(spell_martyr_sanctified_pact);

    SpellCastResult CheckCast()
    {
        Unit* caster = GetCaster();
        if (!caster->HasAura(Martyr::SPELL_ETERNAL_AEGIS))
            return SPELL_FAILED_CASTER_AURASTATE;

        if (caster->GetHealth() <= caster->CountPctFromMaxHealth(GetSpellInfo()->Effects[EFFECT_0].CalcValue(caster)))
            return SPELL_FAILED_NO_POWER;

        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        uint32 const cost = std::min<uint32>(caster->CountPctFromMaxHealth(GetEffectValue()), caster->GetHealth() - 1);
        caster->ModifyHealth(-int32(cost));
        Martyr::AddToAegis(caster, CalculatePct(Martyr::AegisCap(caster), GetSpellInfo()->Effects[EFFECT_1].CalcValue(caster)));
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_martyr_sanctified_pact::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_martyr_sanctified_pact::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Radiant Pulse: the periodic trigger only fires while the Aegis holds a shield and the player is in combat.
class spell_martyr_radiant_pulse : public AuraScript
{
    PrepareAuraScript(spell_martyr_radiant_pulse);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        if (!target->IsInCombat() || Martyr::AegisAmount(target) <= Martyr::AEGIS_EMPTY_AMOUNT)
            PreventDefaultAction();
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_martyr_radiant_pulse::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
    }
};

// Sanctified Blow, second script: Wrath of the Aegis (extra holy damage from the shield) and Blessed Strikes (the share rises from 10% to the Boon's value; the owner's
// script adds the first 10%).
class spell_martyr_sanctified_blow_boons : public SpellScript
{
    PrepareSpellScript(spell_martyr_sanctified_blow_boons);

    void HandleOnHit()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        if (AuraEffect const* wrath = caster->GetAuraEffect(Martyr::SPELL_WRATH_OF_THE_AEGIS, EFFECT_0))
            if (int32 const shield = Martyr::AegisAmount(caster))
                SetHitDamage(GetHitDamage() + CalculatePct(shield, wrath->GetAmount()));
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        int32 const extra = Martyr::Param(caster, Martyr::SPELL_BLESSED_STRIKES) - Martyr::BLOW_BASE_SHARE_PCT;
        if (extra > 0)
            Martyr::AddToAegis(caster, CalculatePct(GetHitDamage(), extra));
    }

    void Register() override
    {
        OnHit += SpellHitFn(spell_martyr_sanctified_blow_boons::HandleOnHit);
        AfterHit += SpellHitFn(spell_martyr_sanctified_blow_boons::HandleAfterHit);
    }
};

// Burning Strike, second script: Blessed Strikes adds a share of the direct hit (the burn is a separate aura effect and is not counted).
class spell_martyr_burning_strike_boons : public SpellScript
{
    PrepareSpellScript(spell_martyr_burning_strike_boons);

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        if (int32 const share = Martyr::Param(caster, Martyr::SPELL_BLESSED_STRIKES))
            Martyr::AddToAegis(caster, CalculatePct(GetHitDamage(), share));
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_martyr_burning_strike_boons::HandleAfterHit);
    }
};

// Righteous Cleave, second script: Righteous Siphon (a share of the damage to each enemy hit goes into the Aegis) and the spread of Burning Retribution (a burning enemy
// that the Cleave hits passes the burn to another enemy the Cleave hits that is not burning).
class spell_martyr_righteous_cleave_boons : public SpellScript
{
    PrepareSpellScript(spell_martyr_righteous_cleave_boons);

    void CollectTargets(std::list<WorldObject*>& targets)
    {
        _targets.clear();
        for (WorldObject* object : targets)
            if (Unit* unit = object->ToUnit())
                _targets.push_back(unit->GetGUID());
    }

    void HandleAfterHit()
    {
        Unit* caster = GetCaster();
        if (!caster)
            return;

        if (int32 const share = Martyr::Param(caster, Martyr::SPELL_RIGHTEOUS_SIPHON))
            Martyr::AddToAegis(caster, CalculatePct(GetHitDamage(), share));
    }

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->HasAura(Martyr::SPELL_BURNING_RETRIBUTION))
            return;

        std::vector<Unit*> burning, clean;
        for (ObjectGuid const& guid : _targets)
            if (Unit* unit = ObjectAccessor::GetUnit(*caster, guid))
                if (unit->IsAlive())
                    (unit->HasAura(Martyr::SPELL_RETRIBUTION_DOT, caster->GetGUID()) ? burning : clean).push_back(unit);

        for (Unit* source : burning)
        {
            if (clean.empty())
                break;

            AuraEffect const* burn = source->GetAuraEffect(Martyr::SPELL_RETRIBUTION_DOT, EFFECT_0, caster->GetGUID());
            if (!burn)
                continue;

            int32 const tick = burn->GetAmount();
            Unit* destination = clean.back();
            clean.pop_back();
            caster->CastCustomSpell(destination, Martyr::SPELL_RETRIBUTION_DOT, &tick, nullptr, nullptr, true);
        }
    }

    void Register() override
    {
        OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_martyr_righteous_cleave_boons::CollectTargets, EFFECT_0, TARGET_UNIT_CONE_ENEMY_24);
        AfterHit += SpellHitFn(spell_martyr_righteous_cleave_boons::HandleAfterHit);
        AfterCast += SpellCastFn(spell_martyr_righteous_cleave_boons::HandleAfterCast);
    }

private:
    std::vector<ObjectGuid> _targets;
};

// Burning Retribution: a melee hit taken burns the attacker for a share of the hit (the damage before the Aegis absorbed any of it) over 4 sec.
class spell_martyr_burning_retribution : public AuraScript
{
    PrepareAuraScript(spell_martyr_burning_retribution);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        Unit* target = GetTarget();
        Unit* attacker = eventInfo.GetActor();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!attacker || attacker == target || !attacker->IsAlive() || !damageInfo)
            return;

        int32 const total = CalculatePct(int32(damageInfo->GetDamage() + damageInfo->GetAbsorb()), aurEff->GetAmount());
        int32 const tick = std::max(1, total / RETRIBUTION_TICKS);
        target->CastCustomSpell(attacker, Martyr::SPELL_RETRIBUTION_DOT, &tick, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_martyr_burning_retribution::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Light's Bulwark: only a hit that carries the player from 40% health or more to below it counts (the proc cooldown of 45 sec is in the spell_proc row; this check runs first,
// so a hit that does not qualify does not start it).
class spell_martyr_lights_bulwark : public AuraScript
{
    PrepareAuraScript(spell_martyr_lights_bulwark);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* target = GetTarget();
        AuraEffect const* threshold = GetAura()->GetEffect(EFFECT_0);
        if (!damageInfo || !damageInfo->GetDamage() || !threshold || !target->IsAlive())
            return false;

        uint32 const limit = target->CountPctFromMaxHealth(threshold->GetAmount());
        uint32 const health = target->GetHealth();
        return health < limit && health + damageInfo->GetDamage() >= limit;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        Unit* target = GetTarget();
        if (AuraEffect const* refill = GetAura()->GetEffect(EFFECT_1))
            Martyr::AddToAegis(target, CalculatePct(Martyr::AegisCap(target), refill->GetAmount()));
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_martyr_lights_bulwark::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_martyr_lights_bulwark::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Redeemer: every melee hit the player makes adds a share of its damage to the Aegis.
class spell_martyr_favor_redeemer : public AuraScript
{
    PrepareAuraScript(spell_martyr_favor_redeemer);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (damageInfo && damageInfo->GetDamage())
            Martyr::AddToAegis(GetTarget(), CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount()));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_martyr_favor_redeemer::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Unyielding Martyr. UnitScript::OnDamage runs inside Unit::DealDamage after every absorb has been taken, so `damage` is what will
// really come off the player's health: the order of the absorb auras does not matter.
class MoonlitMartyrUnitScript : public UnitScript
{
public:
    MoonlitMartyrUnitScript() : UnitScript("MoonlitMartyrUnitScript", true, { UNITHOOK_ON_DAMAGE }) { }

    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!victim || !victim->IsPlayer() || !victim->IsAlive() || !victim->HasAura(Martyr::SPELL_ETERNAL_AEGIS))
            return;

        Player* player = victim->ToPlayer();

        if (!damage)
            return;

        AuraEffect const* unyielding = player->GetAuraEffect(Martyr::SPELL_UNYIELDING_MARTYR, EFFECT_0);
        uint32 const health = player->GetHealth();
        if (!unyielding || player->HasSpellCooldown(Martyr::SPELL_UNYIELDING_MARTYR) || damage < health)
            return;

        // A killing blow: leave the player at the passive's percent of maximum health and refill the Aegis.
        uint32 const left = player->CountPctFromMaxHealth(unyielding->GetAmount());
        if (health > left)
            damage = health - left;
        else
        {
            damage = 0;
            player->SetHealth(left);
        }

        player->AddSpellCooldown(Martyr::SPELL_UNYIELDING_MARTYR, 0, uint32(Martyr::Param(player, Martyr::SPELL_UNYIELDING_MARTYR, EFFECT_1)) * IN_MILLISECONDS);
        Martyr::FillAegis(player);
    }
};

void AddMoonlitBlessingsMartyrScripts()
{
    RegisterSpellScript(spell_martyr_aegis_hooks);
    RegisterSpellScript(spell_martyr_sanctified_pact);
    RegisterSpellScript(spell_martyr_glyph_divine_aegis);
    RegisterSpellScript(spell_martyr_radiant_pulse);
    RegisterSpellScript(spell_martyr_sanctified_blow_boons);
    RegisterSpellScript(spell_martyr_burning_strike_boons);
    RegisterSpellScript(spell_martyr_righteous_cleave_boons);
    RegisterSpellScript(spell_martyr_burning_retribution);
    RegisterSpellScript(spell_martyr_lights_bulwark);
    RegisterSpellScript(spell_martyr_favor_redeemer);
    new MoonlitMartyrUnitScript();
}
