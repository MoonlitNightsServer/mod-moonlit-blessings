// Moonlit Blessings: C++ spell scripts for the Voidbinder Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/voidbinder.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   The kit
//   spell_voidbinder_void_bulwark         Void Bulwark (1940000): the absorb is a percent of the maximum health; Heart of the Void heals what is left when it runs out
//   spell_voidbinder_consume_shadows      Consume Shadows (1940040): the heal over time is a share of the maximum health; Drinking Shadows removes a poison or a disease
//
//   Boons
//   spell_voidbinder_dark_cradle          unlimited absorb: a hit that leaves you below 35% health starts a 20% damage reduction (once per 45 sec)
//   spell_voidbinder_mark_on_hit          Alpha of the Hunt and Thorned Veil: a proc puts the debuff named by the passive on the enemy that was hit
//   spell_voidbinder_alpha_mark           the mark itself: when the marked enemy dies the mark jumps to the nearest enemy you are fighting
//   spell_voidbinder_sirens_reign_threat  Siren's Reign, part 1: the pulse of an area ability gives threat to enemies it did not reach
//   spell_voidbinder_sirens_reign_reflect Siren's Reign, part 2: enemies that hit you take Shadow damage equal to a share of the damage
//   spell_voidbinder_devour_essence       Devour Essence: heals you when it removes an effect (Devouring Hunger adds to the heal)
//   spell_voidbinder_barbed_lash          Barbed Lash: Cruel Lash strikes one more enemy near your target
//   MoonlitVoidbinderUnitScript           Boss Hunter (more damage to bosses and elites) and Thorned Veil (less damage from the veiled enemies)
//
//   Favors (the three demon forms; see the recipe for the whole picture)
//   spell_voidbinder_favor                the hidden Favor aura: teaches the form spell and the form's abilities, takes them away again when the Favor is dropped
//   spell_voidbinder_form_toggle          the form spell when it is CAST: casting it while the form is up ends the form (cast again = leave)
//   spell_voidbinder_form                 the form aura: keeps the model and the bonus helpers on you, dismisses any demon, ends the other forms (a 1 second tick, so a login is repaired)
//   spell_voidbinder_no_summon_in_form    bound to the stock demon summons and Enslave Demon: they fail while a form is up
//
// Spell ids come from the Voidbinder kit block 1,940,000 and recipe block 1,940,100; tools/talents/recipes/voidbinder.py asserts every unit named here.
// A helper spell's id is read from the TriggerSpell column of the passive's dummy effect where there is one, otherwise it is the unit's id + n as the constants say.
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "Timer.h"
#include "UnitScript.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <vector>

namespace
{
    enum VoidbinderSpells : uint32
    {
        SPELL_CRUEL_LASH        = 1940150,   // effect 1 (dummy) names the single-target Barbed Lash
        SPELL_DRINKING_SHADOWS  = 1940220,   // marker
        SPELL_BOSS_HUNTER       = 1940200,   // effect 0 = percent more damage
        SPELL_THORNED_VEIL      = 1940210,   // the passive; the debuff on the enemy is +1 (effect 0 = percent less damage)
        SPELL_ALPHA_HUNT        = 1940230,   // the passive; the mark on the enemy is +1
        SPELL_HEART_OF_THE_VOID = 1940250,   // effect 2 (dummy) names the attribution heal
        SPELL_DEVOURING_HUNGER  = 1940170,   // effect 0 = percent of the maximum health healed
        SPELL_FAVOR_FELHOUND    = 1940260,   // the Favor aura (hidden, teaches); +1 model, +2 Void Bite, +3 Devour Essence, +4 Spell Seal, +7 the FORM (castable and aura), +8 bonus helper
        SPELL_FAVOR_SUCCUBUS    = 1940270,   // +1 Succubus model, +2 male stand-in model, +3 Barbed Lash, +4 Allure, +5 Gloaming Veil, +6 the FORM, +7 bonus helper
        SPELL_FAVOR_VOIDWALKER  = 1940280    // +1 model, +3 the FORM, +4 and +5 bonus helpers (+2 is retired)
    };

    constexpr int32 DARK_CRADLE_HEALTH_PCT = 35;
    constexpr uint32 DARK_CRADLE_COOLDOWN_MS = 45 * IN_MILLISECONDS;
    constexpr float LASH_REACH_FALLBACK = 8.0f;                 // yards: the reach of Barbed Lash, Hellfire and Rain of Fire when the spell does not say
    constexpr float CRUEL_LASH_NEAR_TARGET = 8.0f;              // yards from the target that Cruel Lash looks for the extra enemy
    constexpr float SIREN_REACH = 15.0f;                        // yards from the target that Siren's Reign gives threat
    constexpr float ALPHA_JUMP_RANGE = 15.0f;                   // yards from the dead enemy that the mark jumps
    constexpr uint32 SAME_PULSE_MS = 50;                        // procs of one pulse that come within this time count as one pulse
    constexpr uint32 MARK_REFRESH_NUM = 1;                      // a veil or mark is not cast again while more than 1/2 of its duration is left
    constexpr uint32 MARK_REFRESH_DEN = 2;
    constexpr uint32 MARK_JUMP_DELAY_MS = 1;

    // Every attackable enemy that is in combat with `unit` (the Wildguard PickTarget precedent: the combat references, no grid search).
    std::vector<Unit*> CombatEnemies(Unit* unit)
    {
        std::vector<Unit*> enemies;
        for (auto const& pair : unit->GetCombatManager().GetPvECombatRefs())
        {
            Unit* enemy = pair.second->GetOther(unit);
            if (enemy && enemy->IsAlive() && unit->IsValidAttackTarget(enemy))
                enemies.push_back(enemy);
        }

        return enemies;
    }

    // Removes one harmful poison or disease from `unit` (the first it finds).
    void RemoveOnePoisonOrDisease(Unit* unit)
    {
        for (auto const& pair : unit->GetAppliedAuras())
        {
            AuraApplication const* application = pair.second;
            Aura const* aura = application->GetBase();
            if (application->IsPositive() || aura->IsPassive())
                continue;

            uint32 const dispel = aura->GetSpellInfo()->Dispel;
            if (dispel != DISPEL_POISON && dispel != DISPEL_DISEASE)
                continue;

            unit->RemoveAura(aura->GetId(), aura->GetCasterGUID());
            return;
        }
    }

    // How many beneficial magic effects `unit` carries that a dispel could take.
    uint32 CountBeneficialMagic(Unit const* unit)
    {
        uint32 count = 0;
        for (auto const& pair : unit->GetAppliedAuras())
        {
            AuraApplication const* application = pair.second;
            Aura const* aura = application->GetBase();
            if (application->IsPositive() && !aura->IsPassive() && aura->GetSpellInfo()->Dispel == DISPEL_MAGIC)
                ++count;
        }

        return count;
    }

    // The reach of an area ability in yards (the largest radius of its effects).
    float AreaReach(SpellInfo const* spellInfo, Unit* caster)
    {
        float reach = 0.0f;
        if (spellInfo)
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                reach = std::max(reach, spellInfo->Effects[i].CalcRadius(caster));

        return reach > 0.0f ? reach : LASH_REACH_FALLBACK;
    }

    // Takes a demon away from a warlock who is in a form: the one that is out, and the one that was only put away for a mount (so that dismounting does not bring it back).
    void DismissDemon(Player* player)
    {
        if (!player->IsInWorld())
            return;

        if (Pet* pet = player->GetPet())
            player->RemovePet(pet, PET_SAVE_NOT_IN_SLOT);
        else if (player->GetTemporaryUnsummonedPetNumber())
            player->RemovePet(nullptr, PET_SAVE_NOT_IN_SLOT);
    }

    // One demon form: the Favor aura that teaches it, the form itself (the castable AND the visible aura), the model helper (and the one a male warlock wears; 0: the same), up to two helpers that
    // carry the form's own bonuses, and the abilities the Favor teaches (castable only in the form: their CasterAuraSpell is the form).
    struct FormDef
    {
        uint32 favor;
        uint32 form;
        uint32 model;
        uint32 maleModel;
        uint32 helpers[2];
        uint32 abilities[3];
    };

    constexpr FormDef FORMS[] =
    {
        { SPELL_FAVOR_FELHOUND,   SPELL_FAVOR_FELHOUND + 7,   SPELL_FAVOR_FELHOUND + 1,   0,                        { SPELL_FAVOR_FELHOUND + 8, 0 },                       { SPELL_FAVOR_FELHOUND + 2, SPELL_FAVOR_FELHOUND + 3, SPELL_FAVOR_FELHOUND + 4 } },
        { SPELL_FAVOR_SUCCUBUS,   SPELL_FAVOR_SUCCUBUS + 6,   SPELL_FAVOR_SUCCUBUS + 1,   SPELL_FAVOR_SUCCUBUS + 2, { SPELL_FAVOR_SUCCUBUS + 7, 0 },                       { SPELL_FAVOR_SUCCUBUS + 3, SPELL_FAVOR_SUCCUBUS + 4, SPELL_FAVOR_SUCCUBUS + 5 } },
        { SPELL_FAVOR_VOIDWALKER, SPELL_FAVOR_VOIDWALKER + 3, SPELL_FAVOR_VOIDWALKER + 1, 0,                        { SPELL_FAVOR_VOIDWALKER + 4, SPELL_FAVOR_VOIDWALKER + 5 }, { 0, 0, 0 } }
    };

    FormDef const* FormOfFavor(uint32 favorId)
    {
        for (FormDef const& def : FORMS)
            if (def.favor == favorId)
                return &def;

        return nullptr;
    }

    FormDef const* FormOfAura(uint32 formId)
    {
        for (FormDef const& def : FORMS)
            if (def.form == formId)
                return &def;

        return nullptr;
    }

    bool InAnyForm(Unit const* unit)
    {
        if (unit)
            for (FormDef const& def : FORMS)
                if (unit->HasAura(def.form))
                    return true;

        return false;
    }

    // A demon the warlock controls by Enslave Demon is released (GetPet() does not return it).
    void ReleaseEnslavedDemon(Player* player)
    {
        Unit* charm = player->GetCharm();
        if (charm && charm->GetCreatureType() == CREATURE_TYPE_DEMON)
            player->StopCastingCharm();
    }
}

// Void Bulwark: effect 0 is the absorb; its amount is a percent of the maximum health (40, and the modifiers of Demonic Brutality and Heart of the Void are already in it), turned into health here.
// When the shield runs out by itself, Heart of the Void heals what it did not absorb (effect 2 of that passive names the heal spell the healing is attributed to).
class spell_voidbinder_void_bulwark : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_void_bulwark);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        if (Unit* caster = GetCaster())
            amount = int32(std::min<uint32>(caster->CountPctFromMaxHealth(std::max(amount, 0)), uint32(std::numeric_limits<int32>::max())));
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_EXPIRE)
            return;

        Unit* target = GetTarget();
        AuraEffect const* heart = target->GetAuraEffect(SPELL_HEART_OF_THE_VOID, EFFECT_2);
        int32 const left = aurEff->GetAmount();
        if (!heart || left <= 0 || !target->IsAlive())
            return;

        BlessingsHelpers::HealUnit(target, target, uint32(left), heart->GetSpellInfo()->Effects[EFFECT_2].TriggerSpell);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_voidbinder_void_bulwark::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        AfterEffectRemove += AuraEffectRemoveFn(spell_voidbinder_void_bulwark::HandleRemove, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB, AURA_EFFECT_HANDLE_REAL);
    }
};

// Consume Shadows: effect 0 is the heal over time; its value is in tenths of a percent of the maximum health a second (20 = 2%), turned into health here with the modifiers of Demonic Brutality,
// Drinking Shadows and the Voidwalker Form. Drinking Shadows also removes one poison or disease when the spell is cast.
class spell_voidbinder_consume_shadows : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_consume_shadows);

    // The amount the core hands in has had healing power and the healing-done percents added; none of that is wanted for a share of the maximum health, so it is worked out again from the spell data:
    // the value of effect 0 (with the all-effects modifiers) through the damage-over-time modifiers (Demonic Brutality, Drinking Shadows, Voidwalker Form).
    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        canBeRecalculated = false;
        Unit* caster = GetCaster();
        if (!caster)
            return;

        float tenths = float(GetSpellInfo()->Effects[EFFECT_0].CalcValue(caster));
        if (Player* modOwner = caster->GetSpellModOwner())
            modOwner->ApplySpellMod(GetId(), SPELLMOD_DOT, tenths);

        amount = int32(float(caster->GetMaxHealth()) * std::max(tenths, 0.0f) / 1000.0f);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        if (target->HasAura(SPELL_DRINKING_SHADOWS))
            RemoveOnePoisonOrDisease(target);
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_voidbinder_consume_shadows::CalculateAmount, EFFECT_0, SPELL_AURA_PERIODIC_HEAL);
        AfterEffectApply += AuraEffectApplyFn(spell_voidbinder_consume_shadows::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_HEAL, AURA_EFFECT_HANDLE_REAL);
    }
};

// Dark Cradle: effect 0 is an unlimited absorb (amount -1: the core hands the decision to the script), effect 1 a dummy that names the buff. A hit that leaves the warlock below 35% health
// starts the 20% reduction of the buff and is itself reduced by the same share, once per 45 sec (the passive's own cooldown).
class spell_voidbinder_dark_cradle : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_dark_cradle);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& canBeRecalculated)
    {
        amount = -1;
        canBeRecalculated = false;
    }

    void Absorb(AuraEffect* /*aurEff*/, DamageInfo& dmgInfo, uint32& absorbAmount)
    {
        Player* player = GetTarget()->ToPlayer();
        uint32 const damage = dmgInfo.GetDamage();
        uint32 const buff = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell;
        if (!player || !damage || player->HasAura(buff) || player->HasSpellCooldown(GetSpellInfo()->Id))
            return;

        if (!player->HealthBelowPctDamaged(DARK_CRADLE_HEALTH_PCT, damage))
            return;

        SpellInfo const* buffInfo = sSpellMgr->GetSpellInfo(buff);
        if (!buffInfo)
            return;

        int32 const reduction = std::abs(buffInfo->Effects[EFFECT_0].CalcValue(player));
        player->AddSpellCooldown(GetSpellInfo()->Id, 0, DARK_CRADLE_COOLDOWN_MS);
        player->CastSpell(player, buff, true);
        absorbAmount = CalculatePct(damage, reduction);     // the buff is not in force yet for this hit, so take the share off by hand
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_voidbinder_dark_cradle::CalculateAmount, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB);
        OnEffectAbsorb += AuraEffectAbsorbFn(spell_voidbinder_dark_cradle::Absorb, EFFECT_0);
    }
};

// Alpha of the Hunt and Thorned Veil: effect 0 is a dummy that names (TriggerSpell) the debuff put on the enemy that was hit; the proc row decides which hits. An enemy that already has the
// debuff from this warlock with more than half of its time left is not given it again.
class spell_voidbinder_mark_on_hit : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_mark_on_hit);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* warlock = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim || victim == warlock || !victim->IsAlive() || !warlock->IsValidAttackTarget(victim))
            return;

        uint32 const debuff = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        if (Aura const* current = victim->GetAura(debuff, warlock->GetGUID()))
            if (uint64(current->GetDuration()) * MARK_REFRESH_DEN > uint64(current->GetMaxDuration()) * MARK_REFRESH_NUM)
                return;

        warlock->CastSpell(victim, debuff, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_voidbinder_mark_on_hit::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Alpha's Mark: Mod Damage Taken From Caster in the data does the work; this script moves the mark when the marked enemy dies. The nearest enemy the warlock is fighting within 15 yards of the
// corpse gets it, one millisecond later (the aura map of the dead enemy is being emptied now). Everything is passed by guid.
class spell_voidbinder_alpha_mark : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_alpha_mark);

    static void Jump(ObjectGuid casterGuid, Position const origin, uint32 markId)
    {
        Player* player = ObjectAccessor::FindPlayer(casterGuid);
        if (!player || !player->IsInWorld() || !player->IsAlive() || !player->HasAura(SPELL_ALPHA_HUNT))
            return;

        Unit* best = nullptr;
        float bestDistance = ALPHA_JUMP_RANGE;
        for (Unit* enemy : CombatEnemies(player))
        {
            float const distance = enemy->GetExactDist(&origin);
            if (distance > bestDistance || enemy->HasAura(markId, casterGuid))
                continue;

            best = enemy;
            bestDistance = distance;
        }

        if (best)
            player->CastSpell(best, markId, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEATH)
            return;

        Unit* caster = GetCaster();
        Unit* dead = GetTarget();
        if (!caster || !caster->IsPlayer() || !caster->IsAlive() || !caster->HasAura(SPELL_ALPHA_HUNT))
            return;

        ObjectGuid const casterGuid = caster->GetGUID();
        Position const origin = dead->GetPosition();
        uint32 const markId = GetId();
        caster->m_Events.AddEventAtOffset([casterGuid, origin, markId]()
        {
            Jump(casterGuid, origin, markId);
        }, std::chrono::milliseconds(MARK_JUMP_DELAY_MS));
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_voidbinder_alpha_mark::HandleRemove, EFFECT_0, SPELL_AURA_MOD_DAMAGE_FROM_CASTER, AURA_EFFECT_HANDLE_REAL);
    }
};

// Siren's Reign, part 1: effect 0 is a dummy whose amount is the percent (100) of the damage of the pulse that becomes threat. The proc row (area abilities, hit phase) fires once for every enemy
// the pulse hits; the first of them within 50 ms counts as the pulse. Every enemy the warlock is fighting within 15 yards of the target he has selected (or the enemy hit), and further from him than
// the ability reaches, is given the threat. Enemies that are not yet in combat with him are not reached.
class spell_voidbinder_sirens_reign_threat : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_sirens_reign_threat);

    uint32 _lastPulse = 0;

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Player* player = GetTarget()->ToPlayer();
        Unit* hit = eventInfo.GetActionTarget();
        if (!player || !hit || !damageInfo || !damageInfo->GetDamage())
            return;

        uint32 const now = getMSTime();
        if (_lastPulse && getMSTimeDiff(_lastPulse, now) < SAME_PULSE_MS)
            return;

        _lastPulse = now;

        Unit* center = player->GetSelectedUnit();
        if (!center || !center->IsAlive() || !player->IsValidAttackTarget(center))
            center = hit;

        float const threat = float(damageInfo->GetDamage()) * float(aurEff->GetAmount()) / 100.0f;
        float const reach = AreaReach(eventInfo.GetSpellInfo(), player);
        if (threat <= 0.0f)
            return;

        for (Unit* enemy : CombatEnemies(player))
            if (enemy->GetExactDist(center) <= SIREN_REACH && player->GetExactDist(enemy) > reach)
                enemy->AddThreat(player, threat);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_voidbinder_sirens_reign_threat::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Siren's Reign, part 2: effect 0 is a dummy whose amount is the percent (10) and whose TriggerSpell is the Shadow bolt. Every hit the warlock takes sends that share back at the attacker.
class spell_voidbinder_sirens_reign_reflect : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_sirens_reign_reflect);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* warlock = GetTarget();
        Unit* attacker = eventInfo.GetActor();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!attacker || attacker == warlock || !attacker->IsAlive() || !damageInfo || !damageInfo->GetDamage())
            return;

        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (damage > 0)
            warlock->CastCustomSpell(attacker, GetSpellInfo()->Effects[EFFECT_0].TriggerSpell, &damage, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_voidbinder_sirens_reign_reflect::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Devour Essence: effect 0 dispels one beneficial magic effect from an enemy, effect 1 is a dummy: its amount is the percent of the maximum health that is healed (3) and its TriggerSpell the
// spell the healing is attributed to. The heal happens only if an effect was really removed (the enemy has fewer than before); Devouring Hunger adds its percent.
class spell_voidbinder_devour_essence : public SpellScript
{
    PrepareSpellScript(spell_voidbinder_devour_essence);

    uint32 _before = 0;

    void CountBefore(SpellEffIndex /*effIndex*/)
    {
        if (Unit* target = GetHitUnit())
            _before = CountBeneficialMagic(target);
    }

    void HealAfter()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !_before || CountBeneficialMagic(target) >= _before)
            return;

        int32 percent = GetSpellInfo()->Effects[EFFECT_1].CalcValue(caster);
        if (AuraEffect const* hunger = caster->GetAuraEffect(SPELL_DEVOURING_HUNGER, EFFECT_0))
            percent += hunger->GetAmount();

        BlessingsHelpers::HealUnit(caster, caster, caster->CountPctFromMaxHealth(percent), GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_voidbinder_devour_essence::CountBefore, EFFECT_0, SPELL_EFFECT_DISPEL);
        AfterHit += SpellHitFn(spell_voidbinder_devour_essence::HealAfter);
    }
};

// Barbed Lash (the area one): with Cruel Lash the warlock also strikes one more enemy: the one nearest to his target within 8 yards of it that the lash itself did not reach. The strike is the
// single-target Barbed Lash named by effect 1 of Cruel Lash.
class spell_voidbinder_barbed_lash : public SpellScript
{
    PrepareSpellScript(spell_voidbinder_barbed_lash);

    void HandleAfterCast()
    {
        Player* caster = GetCaster()->ToPlayer();
        AuraEffect const* cruel = caster ? caster->GetAuraEffect(SPELL_CRUEL_LASH, EFFECT_1) : nullptr;
        if (!cruel)
            return;

        Unit* target = GetExplTargetUnit();
        if (!target || !target->IsAlive() || !caster->IsValidAttackTarget(target))
            target = caster->GetVictim();

        if (!target)
            return;

        float const reach = AreaReach(GetSpellInfo(), caster);
        Unit* best = nullptr;
        float bestDistance = CRUEL_LASH_NEAR_TARGET;
        for (Unit* enemy : CombatEnemies(caster))
        {
            if (enemy == target || caster->GetExactDist(enemy) <= reach || !caster->IsWithinLOSInMap(enemy))
                continue;

            float const distance = enemy->GetExactDist(target);
            if (distance <= bestDistance)
            {
                best = enemy;
                bestDistance = distance;
            }
        }

        if (best)
            caster->CastSpell(best, cruel->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_voidbinder_barbed_lash::HandleAfterCast);
    }
};

// The Favor of a demon form: a hidden aura whose dummy effect only TEACHES. Applying it learns the form spell and the form's abilities; dropping the Favor ends the form and takes the spells away
// again (not when the player merely logs out). Nothing is switched on here: the form is a spell the warlock casts (the way Fire's Cauterizer teaches its spells).
class spell_voidbinder_favor : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_favor);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        FormDef const* def = FormOfFavor(GetId());
        if (!player || !def)
            return;

        player->learnSpell(def->form);
        for (uint32 const spellId : def->abilities)
            if (spellId)
                player->learnSpell(spellId);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        FormDef const* def = FormOfFavor(GetId());
        if (!player || !def || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->RemoveAurasDueToSpell(def->form);
        player->removeSpell(def->form, SPEC_MASK_ALL, false);
        for (uint32 const spellId : def->abilities)
            if (spellId)
                player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_voidbinder_favor::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_voidbinder_favor::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The form spell as a CAST: it is a toggle. Casting it while its aura is up ends the form and fails the cast without a message (no cost, no cooldown); casting it otherwise puts the aura on.
class spell_voidbinder_form_toggle : public SpellScript
{
    PrepareSpellScript(spell_voidbinder_form_toggle);

    SpellCastResult CheckToggle()
    {
        Unit* caster = GetCaster();
        uint32 const form = GetSpellInfo()->Id;
        if (!caster || !caster->HasAura(form))
            return SPELL_CAST_OK;

        caster->RemoveAurasDueToSpell(form);
        return SPELL_FAILED_DONT_REPORT;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_voidbinder_form_toggle::CheckToggle);
    }
};

// The form aura itself. Effect 0 is a one second periodic dummy (effect 1 is the melee critical immunity and effect 2 the +60% threat: the tank baseline lives in the data). The model helper (a
// Transform; the stand-in model for a male Succubus) and the bonus helpers are passive, so they are not saved at logout: this script puts them on at once when the form is cast and again every
// second, which also repairs a login (the form aura is saved, and is applied before the player is in the world). A demon that is out is dismissed (a pet restored at login goes within a second).
// Forms end each other. When the form ends the helpers go with it.
class spell_voidbinder_form : public AuraScript
{
    PrepareAuraScript(spell_voidbinder_form);

    static uint32 ModelSpell(FormDef const& def, Player const* player)
    {
        return def.maleModel && player->getGender() == GENDER_MALE ? def.maleModel : def.model;
    }

    void Refresh(Player* player, FormDef const& def)
    {
        if (!player->IsInWorld() || !player->IsAlive())
            return;

        uint32 const model = ModelSpell(def, player);
        if (!player->HasAura(model))
            player->CastSpell(player, model, true);

        for (uint32 const helper : def.helpers)
            if (helper && !player->HasAura(helper))
                player->CastSpell(player, helper, true);

        DismissDemon(player);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        FormDef const* def = FormOfAura(GetId());
        if (!player || !def)
            return;

        for (FormDef const& other : FORMS)
            if (other.form != def->form)
                player->RemoveAurasDueToSpell(other.form);

        if (player->IsInWorld())
            ReleaseEnslavedDemon(player);

        Refresh(player, *def);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        FormDef const* def = FormOfAura(GetId());
        if (!def)
            return;

        target->RemoveAurasDueToSpell(def->model);
        if (def->maleModel)
            target->RemoveAurasDueToSpell(def->maleModel);

        for (uint32 const helper : def->helpers)
            if (helper)
                target->RemoveAurasDueToSpell(helper);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        FormDef const* def = FormOfAura(GetId());
        if (player && def)
            Refresh(player, *def);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_voidbinder_form::HandleApply, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_voidbinder_form::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_voidbinder_form::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Bound to the stock Summon Imp, Felhunter, Voidwalker, Succubus, Felguard and the Enslave Demon ranks: a warlock in a form has no demon at his side and cannot call or enslave one.
class spell_voidbinder_no_summon_in_form : public SpellScript
{
    PrepareSpellScript(spell_voidbinder_no_summon_in_form);

    SpellCastResult CheckForm()
    {
        return InAnyForm(GetCaster()) ? SPELL_FAILED_NOT_SHAPESHIFT : SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_voidbinder_no_summon_in_form::CheckForm);
    }
};

// The damage hooks of two Boons. Both are cheap when no warlock with the Boon is involved: a player test, a class test and one aura lookup.
//  * Boss Hunter: the damage the warlock deals to a boss or an elite enemy (melee, spells and damage over time) is raised by the passive's percent. The threat that damage makes rises with it.
//  * Thorned Veil: the damage a veiled enemy (the debuff cast by this warlock) deals to the warlock is lowered by the debuff's percent.
class MoonlitVoidbinderUnitScript : public UnitScript
{
public:
    MoonlitVoidbinderUnitScript() : UnitScript("MoonlitVoidbinderUnitScript", true,
        { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        Adjust(target, attacker, damage);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (damage <= 0 || !spellInfo)
            return;

        uint32 amount = uint32(damage);
        Adjust(target, attacker, amount);
        damage = int32(std::min<uint32>(amount, uint32(std::numeric_limits<int32>::max())));
    }

    void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (spellInfo)
            Adjust(target, attacker, damage);
    }

private:
    static void Adjust(Unit* target, Unit* attacker, uint32& damage)
    {
        if (!damage || !target || !attacker || target == attacker)
            return;

        if (attacker->IsPlayer() && attacker->getClass() == CLASS_WARLOCK)
            if (AuraEffect const* hunter = attacker->GetAuraEffect(SPELL_BOSS_HUNTER, EFFECT_0))
                if (Creature const* creature = target->ToCreature())
                    if (creature->isElite() || creature->isWorldBoss() || creature->IsDungeonBoss())
                        damage += CalculatePct(damage, hunter->GetAmount());

        if (target->IsPlayer() && target->getClass() == CLASS_WARLOCK && target->HasAura(SPELL_THORNED_VEIL))
            if (Aura const* veil = attacker->GetAura(SPELL_THORNED_VEIL + 1, target->GetGUID()))
                if (AuraEffect const* veilEffect = veil->GetEffect(EFFECT_0))
                    damage -= std::min(damage, CalculatePct(damage, veilEffect->GetAmount()));
    }
};

void AddMoonlitBlessingsVoidbinderScripts()
{
    RegisterSpellScript(spell_voidbinder_void_bulwark);
    RegisterSpellScript(spell_voidbinder_consume_shadows);
    RegisterSpellScript(spell_voidbinder_dark_cradle);
    RegisterSpellScript(spell_voidbinder_mark_on_hit);
    RegisterSpellScript(spell_voidbinder_alpha_mark);
    RegisterSpellScript(spell_voidbinder_sirens_reign_threat);
    RegisterSpellScript(spell_voidbinder_sirens_reign_reflect);
    RegisterSpellScript(spell_voidbinder_devour_essence);
    RegisterSpellScript(spell_voidbinder_barbed_lash);
    RegisterSpellScript(spell_voidbinder_favor);
    RegisterSpellScript(spell_voidbinder_form_toggle);
    RegisterSpellScript(spell_voidbinder_form);
    RegisterSpellScript(spell_voidbinder_no_summon_in_form);
    new MoonlitVoidbinderUnitScript();
}
