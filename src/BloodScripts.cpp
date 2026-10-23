// Moonlit Blessings: C++ spell scripts for the Death Knight Blood Boons and Favors (registered by AddMoonlitBlessingsBloodScripts, which the module loader calls).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/blood.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_blood_marked_heal          Boon Marked for Blood: bound to the Mark of Blood heal (61607): the heal is 2% of maximum health larger for the death knight who marked the enemy
//   spell_blessings_blood_sovereign            Boon (keystone) Blood Sovereign: twice a second, a damage and healing buff above 75% health, a cooldown buff below 35%
//   spell_blessings_blood_crimson_tempest      Boon (keystone) Crimson Tempest: every fourth Blood Boil refreshes the cooldown of Death and Decay
//   spell_blessings_blood_voracious            Boon Voracious: 4% of the damage you deal heals you
//   spell_blessings_blood_rapid_decomposition  Boon Rapid Decomposition: standing in your own Death and Decay heals 1% of maximum health every second
//   spell_blessings_blood_undying_thirst       Boon (keystone) Undying Thirst: a kill during Vampiric Blood heals 5% of maximum health
//   spell_blessings_blood_undying_thirst_keep  bound to Vampiric Blood: with Undying Thirst the health is not taken away when the extra maximum health ends
//   spell_blessings_blood_bladed_heart         Favor of the Bladed Heart: a Blood Strike hit also strikes the nearest other enemy for 50% of the damage
//   spell_blessings_blood_bloodcaller          Favor of the Bloodcaller: a weapon hit (9%) calls Blood Worms; a driver keeps one permanent Blood Beast
//   spell_blessings_blood_worms_summon         the Blood Worms summon spell: 2 to 4 worms, at most 8 at once, no healing
//   spell_blessings_blood_sanguine_mender      Favor of the Sanguine Mender: teaches Mending Coil while the Favor is chosen
//   spell_blessings_blood_mender_rune_tap      bound to Rune Tap: with the Mender's Improved Rune Tap, the group within 15 yards is healed too
//   spell_blessings_blood_mending_coil         the Mending Coil spell: a Death Coil that heals any friend for 150% of the damage
//   MoonlitBloodUnitScript                     Blood Worm / Blood Beast bites, Marked for Blood's extra physical damage, Sanguine Echoes' stronger rune weapon, the Mender's rune weapon heals
//   MoonlitBloodSpellScript                    Red Thirst: every 10 runic power spent takes a second off Vampiric Blood
//
// The Mender's conversion (1110140) uses the generic spell_blessings_shaman_hybrid_heal of ElementalScripts.cpp.
// A passive's helper spell (the buff, the heal, the strike) is read from the TriggerSpell column of one of its effects wherever that is simpler; the ids below are asserted by tools/talents/recipes/blood.py.
// ~Moonlit Nights Team

#include "AllSpellScript.h"
#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DynamicObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "UnitScript.h"
#include "WorldSession.h"
#include <algorithm>
#include <list>
#include <map>
#include <vector>

namespace
{
    enum BloodSpells : uint32
    {
        SPELL_MARKED_FOR_BLOOD   = 1110000,   // effect 1: extra percent of maximum health on the Mark of Blood heal, effect 2: extra percent of physical damage on the marked enemy
        SPELL_BLOOD_SOVEREIGN    = 1110010,   // effect 1: high health threshold (trigger = buff), effect 2: low health threshold (trigger = cooldown buff)
        SPELL_RED_THIRST         = 1110040,   // effect 0: runic power per step, effect 1: seconds taken off Vampiric Blood
        SPELL_CRIMSON_TEMPEST    = 1110050,   // effect 1: Blood Boils per refresh
        SPELL_SANGUINE_ECHOES    = 1110090,   // effect 1: percent less damage the rune weapon deals (stock: 50)
        SPELL_UNDYING_THIRST     = 1110100,
        SPELL_BLADED_HEART       = 1110110,   // effect 2: percent of the Blood Strike damage, trigger = the strike
        SPELL_BLOODCALLER        = 1110120,   // effect 0: most Blood Worms at once, trigger = the worm summon; effect 1: driver, trigger = the beast summon
        SPELL_BLOODCALLER_WORMS  = 1110121,
        SPELL_BLOODCALLER_BEAST  = 1110122,
        SPELL_BEAST_MARK         = 1110123,   // effect 0: bite in percent of attack power every 2 sec
        SPELL_WORM_MARK          = 1110124,   // effect 0: bite in percent of attack power every 2 sec
        SPELL_SANGUINE_MENDER    = 1110130,
        SPELL_MENDER_CONVERSION  = 1110140,   // trigger of effect 0 = the heal helper
        SPELL_MENDER_RUNE_TAP    = 1110150,   // effect 2: percent of maximum health the group heal gives
        SPELL_MENDING_COIL       = 1110160,

        // stock spells
        SPELL_MARK_OF_BLOOD      = 49005,
        SPELL_VAMPIRIC_BLOOD     = 55233,
        SPELL_RUNE_TAP_PARTY     = 59754,     // only used to credit the group heal
        SPELL_DEATH_COIL_DAMAGE  = 47632,
        SPELL_DEATH_COIL_HEAL    = 47633,
        SPELL_BLOODWORM_LEECH    = 50453,     // the creature addon aura that heals the master
        SPELL_SHADOWFIEND_LEECH  = 28305      // the Shadowfiend aura that hands mana back
    };

    constexpr uint32 NPC_BLOOD_WORM_BODY = 28017;                 // the stock Bloodworm: the body of the Blood Worms (the summon spell clones the stock one)
    constexpr uint32 NPC_BEAST_BODY = 19668;                // the stock Shadowfiend: scriptless ground melee body of the Blood Beast
    constexpr uint32 NPC_RUNE_WEAPON = 27893;               // the Dancing Rune Weapon
    constexpr uint32 BEAST_MODELS[] = { 31107, 31163, 31164 };   // the three Blood Beast models of Deathbringer Saurfang's adds (creature 38508)
    constexpr uint32 DEFAULT_MAX_WORMS = 8;
    constexpr float BLADED_HEART_RANGE = 10.0f;             // how far from its target the second Blood Strike may find an enemy
    constexpr float RUNE_TAP_GROUP_RANGE = 15.0f;
    constexpr float BITE_PERIOD_MS = 2000.0f;               // the Blood Worm and the Blood Beast bite "every 2 sec": the blow is scaled by their real swing time

    // the Death and Decay ranks (the cast; the ground effect is a dynamic object under the same id)
    constexpr uint32 DEATH_AND_DECAY_RANKS[] = { 43265, 49936, 49937, 49938 };
    // the Death Coil ranks, best first (47541 is rank 1, 49892 to 49895 are ranks 2 to 5)
    constexpr uint32 DEATH_COIL_RANKS[] = { 49895, 49894, 49893, 49892, 47541 };

    int32 Param(Unit const* unit, uint32 spellId, uint8 effect)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
    }

    // Put a helper buff on (once) or take it off.
    void SetHelper(Unit* unit, uint32 helper, bool wanted)
    {
        if (!helper)
            return;

        if (wanted)
        {
            if (!unit->HasAura(helper))
                unit->CastSpell(unit, helper, true);
        }
        else
            unit->RemoveAurasDueToSpell(helper);
    }

    bool IsWorm(Unit const* unit)
    {
        return unit && unit->IsCreature() && unit->GetEntry() == NPC_BLOOD_WORM_BODY && unit->GetUInt32Value(UNIT_CREATED_BY_SPELL) == SPELL_BLOODCALLER_WORMS;
    }

    bool IsBeast(Unit const* unit)
    {
        return unit && unit->IsCreature() && unit->GetEntry() == NPC_BEAST_BODY && unit->GetUInt32Value(UNIT_CREATED_BY_SPELL) == SPELL_BLOODCALLER_BEAST;
    }

    // The live Blood Worms of the death knight, the oldest first (a creature's guid counter only grows).
    std::vector<Unit*> WormsOf(Unit* owner)
    {
        std::vector<Unit*> worms;
        for (Unit* controlled : owner->m_Controlled)
            if (controlled && controlled->IsAlive() && IsWorm(controlled))
                worms.push_back(controlled);

        std::sort(worms.begin(), worms.end(), [](Unit const* a, Unit const* b) { return a->GetGUID().GetCounter() < b->GetGUID().GetCounter(); });
        return worms;
    }

    Unit* FindBeast(Unit* owner)
    {
        for (Unit* controlled : owner->m_Controlled)
            if (controlled && controlled->IsAlive() && IsBeast(controlled))          // a dead beast does not count: it is called again within 2 sec
                return controlled;

        return nullptr;
    }

    // A worm or the beast: put the mark on it (it carries the bite share and tells the UnitScript the unit is ours), take off the healing auras of the stock body.
    void Dress(Unit* minion, bool beast)
    {
        uint32 const mark = beast ? SPELL_BEAST_MARK : SPELL_WORM_MARK;
        if (!minion->HasAura(mark))
            minion->CastSpell(minion, mark, true);

        minion->RemoveAurasDueToSpell(beast ? uint32(SPELL_SHADOWFIEND_LEECH) : uint32(SPELL_BLOODWORM_LEECH));
        if (beast)
        {
            uint32 const display = BEAST_MODELS[urand(0, 2)];
            bool known = false;
            for (uint32 const model : BEAST_MODELS)
                known = known || minion->GetDisplayId() == model;

            if (!known)
            {
                minion->SetDisplayId(display);
                minion->SetNativeDisplayId(display);
            }
        }
    }

    // The best known Death Coil rank's damage value (the number the stock script works from).
    int32 DeathCoilValue(Player const* player)
    {
        for (uint32 const rank : DEATH_COIL_RANKS)
            if (player->HasSpell(rank))
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(rank))
                    return info->Effects[EFFECT_0].CalcValue(player);

        return 0;
    }
}

// Marked for Blood, the heal half. The Mark of Blood aura on the enemy casts 61607 (a heal of 4% of the target's maximum health) from the marked enemy; the mark's caster is the death knight.
class spell_blessings_blood_marked_heal : public SpellScript
{
    PrepareSpellScript(spell_blessings_blood_marked_heal);

    void HandleHeal(SpellEffIndex /*effIndex*/)
    {
        Unit* enemy = GetCaster();
        Aura const* mark = enemy ? enemy->GetAura(SPELL_MARK_OF_BLOOD) : nullptr;
        Unit* knight = mark ? mark->GetCaster() : GetOriginalCaster();
        if (!knight)
            return;

        if (int32 const extra = Param(knight, SPELL_MARKED_FOR_BLOOD, EFFECT_1))
            SetEffectValue(GetEffectValue() + extra);
    }

    void Register() override
    {
        OnEffectLaunchTarget += SpellEffectFn(spell_blessings_blood_marked_heal::HandleHeal, EFFECT_0, SPELL_EFFECT_HEAL_PCT);   // the heal percent is read in the launch phase
    }
};

// Blood Sovereign: effect 0 ticks twice a second; effect 1 holds the high threshold and names the buff, effect 2 the low threshold and the cooldown buff.
class spell_blessings_blood_sovereign : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_sovereign);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* knight = GetTarget();
        AuraEffect const* high = GetAura()->GetEffect(EFFECT_1);
        AuraEffect const* low = GetAura()->GetEffect(EFFECT_2);
        if (!high || !low)
            return;

        bool const alive = knight->IsAlive();
        SetHelper(knight, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, alive && knight->HealthAbovePct(high->GetAmount()));
        SetHelper(knight, GetSpellInfo()->Effects[EFFECT_2].TriggerSpell, alive && knight->HealthBelowPct(low->GetAmount()));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[EFFECT_2].TriggerSpell);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_blood_sovereign::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_blood_sovereign::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Crimson Tempest: a cast-phase proc on Blood Boil (once per cast); effect 1 holds how many Blood Boils make one refresh.
class spell_blessings_blood_crimson_tempest : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_crimson_tempest);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        if (++_boils < std::max<int32>(aurEff->GetAmount(), 1))
            return;

        _boils = 0;
        for (uint32 const rank : DEATH_AND_DECAY_RANKS)
            player->RemoveSpellCooldown(rank, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_blood_crimson_tempest::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }

private:
    int32 _boils = 0;
};

// Voracious: effect 1 holds the percent of the damage dealt that heals the death knight, its trigger names the heal.
class spell_blessings_blood_voracious : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_voracious);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* knight = GetTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !knight->IsAlive())
            return;

        int32 const heal = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (heal > 0)
            if (uint32 const helper = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell)
                knight->CastCustomSpell(helper, SPELLVALUE_BASE_POINT0, heal, knight, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_blood_voracious::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Rapid Decomposition: effect 1 ticks once a second; effect 2 names the 1% heal. It is given while the death knight stands inside the ground effect of his own Death and Decay.
class spell_blessings_blood_rapid_decomposition : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_rapid_decomposition);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* knight = GetTarget();
        if (!knight->IsAlive())
            return;

        for (uint32 const rank : DEATH_AND_DECAY_RANKS)
        {
            DynamicObject* ground = knight->GetDynObject(rank);
            if (ground && knight->IsWithinDist(ground, ground->GetRadius()))
            {
                if (uint32 const helper = GetSpellInfo()->Effects[EFFECT_2].TriggerSpell)
                    knight->CastSpell(knight, helper, true);

                return;
            }
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_blood_rapid_decomposition::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Undying Thirst, the kill half: a kill proc; the heal only counts while Vampiric Blood is on. Effect 1 names the 5% heal.
class spell_blessings_blood_undying_thirst : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_undying_thirst);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return GetTarget()->HasAura(SPELL_VAMPIRIC_BLOOD);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        Unit* knight = GetTarget();
        if (knight->IsAlive())
            if (uint32 const helper = GetSpellInfo()->Effects[EFFECT_1].TriggerSpell)
                knight->CastSpell(knight, helper, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_blood_undying_thirst::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_blood_undying_thirst::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Undying Thirst, the health half: bound to Vampiric Blood. The stock aura takes its extra maximum health away from the current health when it ends; with Undying Thirst the health the
// death knight had when it ended stays (never more than his normal maximum).
class spell_blessings_blood_undying_thirst_keep : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_undying_thirst_keep);

    void HandleBeforeRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* knight = GetTarget();
        _health = (knight->IsAlive() && knight->HasAura(SPELL_UNDYING_THIRST)) ? knight->GetHealth() : 0;
    }

    void HandleAfterRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* knight = GetTarget();
        if (_health && knight->IsAlive())
            knight->SetHealth(std::min<uint32>(_health, knight->GetMaxHealth()));
    }

    void Register() override
    {
        OnEffectRemove += AuraEffectRemoveFn(spell_blessings_blood_undying_thirst_keep::HandleBeforeRemove, EFFECT_1, SPELL_AURA_MOD_INCREASE_HEALTH, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_blood_undying_thirst_keep::HandleAfterRemove, EFFECT_1, SPELL_AURA_MOD_INCREASE_HEALTH, AURA_EFFECT_HANDLE_REAL);
    }

private:
    uint32 _health = 0;
};

// Favor of the Bladed Heart: effect 2 holds the percent of the Blood Strike damage, its trigger names the strike (a physical bolt whose exact amount is set here).
class spell_blessings_blood_bladed_heart : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_bladed_heart);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        Unit* knight = GetTarget();
        // the enemy nearest to the struck one (within range of it), not under crowd control
        std::list<Unit*> found;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(victim, knight, BLADED_HEART_RANGE);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(victim, found, check);
        Cell::VisitObjects(victim, searcher, BLADED_HEART_RANGE);
        Unit* other = nullptr;
        float best = 1e9f;
        for (Unit* candidate : found)
        {
            if (candidate == victim || !knight->IsValidAttackTarget(candidate) || candidate->HasBreakableByDamageCrowdControlAura() || !victim->IsWithinLOSInMap(candidate))
                continue;

            float const distance = victim->GetExactDist(candidate);
            if (distance < best)
            {
                best = distance;
                other = candidate;
            }
        }

        if (!other)
            return;

        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (damage > 0)
            if (uint32 const strike = GetSpellInfo()->Effects[EFFECT_2].TriggerSpell)
                knight->CastCustomSpell(strike, SPELLVALUE_BASE_POINT0, damage, other, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_blood_bladed_heart::HandleProc, EFFECT_2, SPELL_AURA_DUMMY);
    }
};

// Favor of the Bloodcaller. Effect 0 (a 9% proc on weapon hits): calls the Blood Worms at the target (its amount is the most worms at once). Effect 1: a driver that keeps the permanent
// Blood Beast beside the death knight; it calls the beast again within 2 sec if it died or was dismissed.
class spell_blessings_blood_bloodcaller : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_bloodcaller);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        Unit* knight = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!knight->IsAlive() || !victim || !victim->IsAlive() || !knight->IsHostileTo(victim))
            return;

        if (uint32 const worms = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell)
            knight->CastSpell(victim, worms, true);
    }

    static void EnsureBeast(Unit* knight, uint32 summonSpell)
    {
        if (!knight || !knight->IsInWorld() || !knight->IsAlive() || !summonSpell)
            return;

        Unit* beast = FindBeast(knight);
        if (!beast)
        {
            knight->CastSpell(knight, summonSpell, true);
            beast = FindBeast(knight);
        }

        if (beast && beast->IsAlive())
            Dress(beast, true);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        EnsureBeast(GetTarget(), GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        std::vector<Unit*> beasts;
        for (Unit* controlled : GetTarget()->m_Controlled)
            if (controlled && IsBeast(controlled))
                beasts.push_back(controlled);

        for (Unit* beast : beasts)
            if (TempSummon* summon = beast->ToTempSummon())
                summon->UnSummon();
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        EnsureBeast(GetTarget(), GetSpellInfo()->Effects[EFFECT_1].TriggerSpell);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_blood_bloodcaller::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_blood_bloodcaller::HandleApply, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_blood_bloodcaller::HandleRemove, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_blood_bloodcaller::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The Blood Worms summon spell (a clone of the stock Bloodworm summon, which has its own script bound to the stock id only): 2 to 4 worms per cast, then the oldest beyond the cap leave and
// every worm is dressed (the bite mark; the heal leech of the stock body is taken off).
class spell_blessings_blood_worms_summon : public SpellScript
{
    PrepareSpellScript(spell_blessings_blood_worms_summon);

    void HandleSummon(SpellEffIndex /*effIndex*/)
    {
        SetEffectValue(irand(2, 4));
    }

    void HandleAfterCast()
    {
        Unit* knight = GetCaster();
        if (!knight)
            return;

        int32 const cap = Param(knight, SPELL_BLOODCALLER, EFFECT_0);
        size_t const most = cap > 0 ? size_t(cap) : size_t(DEFAULT_MAX_WORMS);
        std::vector<Unit*> worms = WormsOf(knight);
        size_t const leaving = worms.size() > most ? worms.size() - most : 0;
        for (size_t i = 0; i < worms.size(); ++i)
        {
            if (i < leaving)
            {
                if (TempSummon* summon = worms[i]->ToTempSummon())
                    summon->UnSummon();
            }
            else
                Dress(worms[i], false);
        }
    }

    void Register() override
    {
        OnEffectHit += SpellEffectFn(spell_blessings_blood_worms_summon::HandleSummon, EFFECT_0, SPELL_EFFECT_SUMMON);
        AfterCast += SpellCastFn(spell_blessings_blood_worms_summon::HandleAfterCast);
    }
};

// Favor of the Sanguine Mender: effect 1 is the apply/remove hook. Mending Coil is learned with the Favor and unlearned when it ends.
class spell_blessings_blood_sanguine_mender : public AuraScript
{
    PrepareAuraScript(spell_blessings_blood_sanguine_mender);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->learnSpell(SPELL_MENDING_COIL);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->removeSpell(SPELL_MENDING_COIL, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_blood_sanguine_mender::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_blood_sanguine_mender::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The Glyph of Rune Tap half of the Mender's Improved Rune Tap: bound to Rune Tap itself. Everyone in the group within 15 yards (not the caster, who has the Rune Tap heal itself) is healed
// for the percent of his maximum health that effect 2 of Improved Rune Tap holds.
class spell_blessings_blood_mender_rune_tap : public SpellScript
{
    PrepareSpellScript(spell_blessings_blood_mender_rune_tap);

    void HandleAfterCast()
    {
        Player* knight = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!knight)
            return;

        int32 const percent = Param(knight, SPELL_MENDER_RUNE_TAP, EFFECT_2);
        Group* group = knight->GetGroup();
        if (percent <= 0 || !group)
            return;

        std::vector<Player*> members;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member != knight && member->IsAlive() && member->IsWithinDist(knight, RUNE_TAP_GROUP_RANGE))
                    members.push_back(member);

        for (Player* member : members)
            BlessingsHelpers::HealUnit(knight, member, member->CountPctFromMaxHealth(percent), SPELL_RUNE_TAP_PARTY);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_blessings_blood_mender_rune_tap::HandleAfterCast);
    }
};

// Mending Coil (a clone of Death Coil rank 1 without the stock script that refuses living friends): the damage value is that of the best Death Coil rank the death knight knows; a friend is
// healed for 150% of it (the stock undead heal spell), an enemy is struck as by Death Coil.
class spell_blessings_blood_mending_coil : public SpellScript
{
    PrepareSpellScript(spell_blessings_blood_mending_coil);

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Player* knight = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Unit* target = GetHitUnit();
        if (!knight || !target)
            return;

        int32 value = DeathCoilValue(knight);
        if (value <= 0)
            value = GetEffectValue();

        if (knight->IsFriendlyTo(target))
        {
            int32 const heal = int32(float(value) * 1.5f);
            knight->CastCustomSpell(target, SPELL_DEATH_COIL_HEAL, &heal, nullptr, nullptr, true);
        }
        else
            knight->CastCustomSpell(target, SPELL_DEATH_COIL_DAMAGE, &value, nullptr, nullptr, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_blood_mending_coil::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// Blood Worm and Blood Beast bites, Marked for Blood's physical damage bonus, Sanguine Echoes' stronger rune weapon, and the Mender's rune weapon heals.
class MoonlitBloodUnitScript : public UnitScript
{
public:
    MoonlitBloodUnitScript() : UnitScript("MoonlitBloodUnitScript", true, { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_ON_DAMAGE }) { }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!attacker)
            return;

        // the worms and the beast bite for a share of the owner's attack power per 2 sec (scaled by the real swing time)
        bool const worm = IsWorm(attacker);
        if (worm || IsBeast(attacker))
        {
            Unit* owner = attacker->GetOwner();
            AuraEffect const* mark = attacker->GetAuraEffect(worm ? uint32(SPELL_WORM_MARK) : uint32(SPELL_BEAST_MARK), EFFECT_0);
            if (owner && mark)
            {
                float const swing = float(std::max<uint32>(attacker->GetAttackTime(BASE_ATTACK), 1));
                float const bite = owner->GetTotalAttackPowerValue(BASE_ATTACK) * float(mark->GetAmount()) / 100.0f * swing / BITE_PERIOD_MS;
                damage = uint32(std::max(1.0f, bite));
            }

            return;
        }

        if (attacker->GetEntry() == NPC_RUNE_WEAPON)
            damage = EchoesDamage(attacker, damage);
        else
            damage = MarkedDamage(target, attacker, damage, true);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!attacker || !spellInfo || damage <= 0)
            return;

        if (attacker->GetEntry() == NPC_RUNE_WEAPON)
            damage = int32(EchoesDamage(attacker, uint32(damage)));
        else
            damage = int32(MarkedDamage(target, attacker, uint32(damage), (spellInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL) != 0));
    }

    // The Mender: the rune weapon's damage (already reduced by the Mender's loss: the core takes the owner's bonuses) heals the most injured ally for the same amount.
    void OnDamage(Unit* attacker, Unit* /*victim*/, uint32& damage) override
    {
        if (!attacker || attacker->GetEntry() != NPC_RUNE_WEAPON || !damage)
            return;

        Unit* knight = attacker->GetOwner();
        if (!knight || !knight->IsAlive() || !knight->HasAura(SPELL_MENDER_CONVERSION))
            return;

        SpellInfo const* conversion = sSpellMgr->GetSpellInfo(SPELL_MENDER_CONVERSION);
        uint32 const healSpell = conversion ? conversion->Effects[EFFECT_0].TriggerSpell : 0;
        SpellInfo const* healInfo = healSpell ? sSpellMgr->GetSpellInfo(healSpell) : nullptr;
        if (!healInfo)
            return;

        Unit* ally = BlessingsHelpers::MostInjuredFriend(knight, knight, healInfo->GetMaxRange(true));
        if (ally)
            knight->CastCustomSpell(healSpell, SPELLVALUE_BASE_POINT0, int32(damage), ally, true);
    }

private:
    // Sanguine Echoes: the core halves the rune weapon's damage; the Boon makes it deal (100 - effect 1) percent instead.
    static uint32 EchoesDamage(Unit* weapon, uint32 damage)
    {
        Unit* knight = weapon->GetOwner();
        int32 const less = Param(knight, SPELL_SANGUINE_ECHOES, EFFECT_1);
        if (less <= 0 || less >= 50)
            return damage;

        return uint32(uint64(damage) * uint64(100 - less) / 50);
    }

    // Marked for Blood: an enemy marked by this death knight takes more physical damage from him.
    static uint32 MarkedDamage(Unit* target, Unit* attacker, uint32 damage, bool physical)
    {
        if (!physical || !target || !attacker->IsPlayer())
            return damage;

        int32 const more = Param(attacker, SPELL_MARKED_FOR_BLOOD, EFFECT_2);
        if (more <= 0 || !target->GetAura(SPELL_MARK_OF_BLOOD, attacker->GetGUID()))
            return damage;

        return damage + CalculatePct(damage, more);
    }
};

// Red Thirst: every cast that spends runic power adds to a running total (runic power is counted in tenths); each full step takes seconds off the Vampiric Blood cooldown.
class MoonlitBloodSpellScript : public AllSpellScript
{
public:
    MoonlitBloodSpellScript() : AllSpellScript("MoonlitBloodSpellScript", { ALLSPELLHOOK_ON_CAST }) { }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* spellInfo, bool /*skipCheck*/) override
    {
        Player* knight = caster ? caster->ToPlayer() : nullptr;
        if (!knight || !spell || !spellInfo || spellInfo->PowerType != POWER_RUNIC_POWER)
            return;

        AuraEffect const* step = knight->GetAuraEffect(SPELL_RED_THIRST, EFFECT_0);
        AuraEffect const* seconds = knight->GetAuraEffect(SPELL_RED_THIRST, EFFECT_1);
        int32 const cost = spell->GetPowerCost();
        if (!step || !seconds || step->GetAmount() <= 0 || cost <= 0)
            return;

        uint32& spent = _spent[knight->GetGUID().GetCounter()];
        spent += uint32(cost);
        uint32 const unit = uint32(step->GetAmount()) * 10;
        while (spent >= unit)
        {
            spent -= unit;
            knight->ModifySpellCooldown(SPELL_VAMPIRIC_BLOOD, -1000 * seconds->GetAmount());
        }
    }

private:
    std::map<uint32, uint32> _spent;
};

void AddMoonlitBlessingsBloodScripts()
{
    RegisterSpellScript(spell_blessings_blood_bladed_heart);
    RegisterSpellScript(spell_blessings_blood_bloodcaller);
    RegisterSpellScript(spell_blessings_blood_crimson_tempest);
    RegisterSpellScript(spell_blessings_blood_marked_heal);
    RegisterSpellScript(spell_blessings_blood_mender_rune_tap);
    RegisterSpellScript(spell_blessings_blood_mending_coil);
    RegisterSpellScript(spell_blessings_blood_rapid_decomposition);
    RegisterSpellScript(spell_blessings_blood_sanguine_mender);
    RegisterSpellScript(spell_blessings_blood_sovereign);
    RegisterSpellScript(spell_blessings_blood_undying_thirst);
    RegisterSpellScript(spell_blessings_blood_undying_thirst_keep);
    RegisterSpellScript(spell_blessings_blood_voracious);
    RegisterSpellScript(spell_blessings_blood_worms_summon);
    new MoonlitBloodUnitScript();
    new MoonlitBloodSpellScript();
}
