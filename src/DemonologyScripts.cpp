// Moonlit Blessings: C++ spell scripts for the Demonology Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/demonology.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   Boons
//   spell_dem_form_gate               Demonic Frenzy: the stacking attack speed proc only works while the Felguard Form is up
//   spell_dem_fel_hunger              Fel Hunger: the mana refund of a melee hit, in the Felguard Form only, single-target hits only
//   spell_dem_felsteel_skin           Felsteel Skin: keeps its helper aura (armour, damage taken) up while the form and the Boon are both there
//   spell_dem_legion_cleave           Legion Cleave: a share of a single-target melee hit goes to up to 2 other enemies near the target
//   spell_dem_searing_aura_tick       bound to Immolation Aura's tick (50590): Searing Aura scales its damage and slows the enemies it burns
//   spell_dem_demonic_dread           Demonic Dread: a second, smaller bolt after Shadow Bolt, Incinerate and Soul Fire (Metamorphosis)
//   spell_dem_fel_charge              the Felguard Form's charge: Brutal Charge adds the stun
//
//   Favors
//   spell_dem_felguard_favor          Favor of the Felguard Form: teaches the form and its three attacks, keeps the no-pet rule, ends the form when the Favor is dropped
//   spell_dem_felguard_form           the form aura: keeps Felguard Plate and the Felsteel Skin helper in step
//   spell_dem_no_summon_in_form       bound to the stock demon summons and Enslave Demon: they cannot be cast in the Felguard Form
//   spell_dem_metamorphosis_master    Favor of the Metamorphosis Master: teaches the Metamorphosis melee kit
//   spell_dem_summoner                Favor of the Summoner: a temporary demon every 20 sec in combat
//   spell_dem_summoner_empowerment    bound to Demonic Empowerment: calls all four temporary demons for 10 sec
//   spell_dem_legion_demon            the mark aura of a temporary demon: sends it at the warlock's target and makes it cast its signature spell
//   spell_dem_legion_lord_infernal    bound to Inferno: Legion Lord applies to the Infernal
//
// Spell ids come from the Demonology block 1,920,000; tools/talents/recipes/demonology.py asserts every unit named here (helper = unit id + 1 ... as listed in its docstring).
// Gates on Metamorphosis are NOT scripted: those Boons are learned passives with a ShapeshiftMask, which the core switches with the form.
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Cell.h"
#include "CellImpl.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GridNotifiers.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include <algorithm>
#include <iterator>
#include <list>
#include <vector>

namespace
{
    enum DemonologySpells : uint32
    {
        SPELL_BRUTAL_CHARGE         = 1920000,   // passive: effect 1 trigger = the stun
        SPELL_BRUTAL_STUN           = 1920001,
        SPELL_SEARING_AURA          = 1920030,   // effect 0: percent more damage, effect 1 trigger = the slow
        SPELL_FELSTEEL_SKIN         = 1920050,   // passive; its helper is the next id
        SPELL_FELSTEEL_HELPER       = 1920051,
        SPELL_LEGION_CLEAVE         = 1920060,   // effect 0: percent of the hit (50), trigger = the bolt
        SPELL_DEMONIC_DREAD         = 1920080,   // effect 0: percent of the hit (30), trigger = the shadow bolt; the fire bolt is the next id after it
        SPELL_LEGION_LORD           = 1920090,   // effect 0: percent longer life (50)
        SPELL_LEGION_MIGHT          = 1920091,   // effect 0: spell damage, effect 1: attack power (set per demon)

        SPELL_FAVOR_FELGUARD        = 1920110,
        SPELL_FELGUARD_FORM         = 1920111,   // the visible form aura (also the castable)
        SPELL_FELGUARD_PLATE        = 1920112,
        SPELL_AXE_SWEEP             = 1920113,
        SPELL_FEL_CHARGE            = 1920114,
        SPELL_FEL_BRAND             = 1920115,

        SPELL_FAVOR_META_MASTER     = 1920120,
        SPELL_DEMON_STRIKE          = 1920121,
        SPELL_FEL_CLEAVE            = 1920122,
        SPELL_DEMONIC_LEAP          = 1920123,
        SPELL_DEMONIC_MIGHT         = 1920124,   // hidden stance passive (Metamorphosis): attack power from Intellect

        SPELL_FAVOR_SUMMONER        = 1920130,
        SPELL_LEGION_IMP            = 1920131,
        SPELL_LEGION_VOIDWALKER     = 1920132,
        SPELL_LEGION_SUCCUBUS       = 1920133,
        SPELL_LEGION_FELHUNTER      = 1920134,
        SPELL_LEGION_DEMON          = 1920135,   // the mark on each temporary demon: periodic dummy every 3 sec

        SPELL_FIREBOLT              = 47964,     // Firebolt rank 9
        SPELL_LASH_OF_PAIN          = 47992,     // Lash of Pain rank 9
        SPELL_SHADOW_BITE           = 54053      // Shadow Bite rank 5
    };

    constexpr uint32 NPC_FELGUARD = 17252;                    // the stock Felguard
    constexpr uint32 NPC_IMP = 416;                           // the stock demons of the Favor of the Summoner (no AI, no script)
    constexpr uint32 NPC_VOIDWALKER = 1860;
    constexpr uint32 NPC_SUCCUBUS = 1863;
    constexpr uint32 NPC_FELHUNTER = 417;
    constexpr uint32 BURST_LIFETIME_MS = 10000;               // the demons Demonic Empowerment calls
    constexpr float CLEAVE_RANGE = 8.0f;                      // Legion Cleave: other enemies within this many yards of the target
    constexpr uint32 CLEAVE_TARGETS = 2;
    constexpr float SIGNATURE_RANGE = 30.0f;                  // a temporary demon casts its signature spell at an enemy this near

    constexpr uint32 FELGUARD_FAVOR_SPELLS[] = { SPELL_FELGUARD_FORM, SPELL_AXE_SWEEP, SPELL_FEL_CHARGE, SPELL_FEL_BRAND };
    constexpr uint32 META_MASTER_SPELLS[] = { SPELL_DEMON_STRIKE, SPELL_FEL_CLEAVE, SPELL_DEMONIC_LEAP, SPELL_DEMONIC_MIGHT };

    // The four temporary demons: creature, summon spell, signature spell (0: none) and how many 3 sec ticks of the mark pass between two casts of it.
    struct LegionDemon
    {
        uint32 entry;
        uint32 summonSpell;
        uint32 signature;
        uint32 every;
    };

    constexpr LegionDemon LEGION_DEMONS[] =
    {
        { NPC_IMP,        SPELL_LEGION_IMP,        SPELL_FIREBOLT,     1 },    // Imp: Firebolt every 3 sec
        { NPC_VOIDWALKER, SPELL_LEGION_VOIDWALKER, 0,                  1 },    // Voidwalker: melee only (a taunt would steal threat from the tank)
        { NPC_SUCCUBUS,   SPELL_LEGION_SUCCUBUS,   SPELL_LASH_OF_PAIN, 2 },    // Succubus: Lash of Pain every 6 sec
        { NPC_FELHUNTER,  SPELL_LEGION_FELHUNTER,  SPELL_SHADOW_BITE,  2 }     // Felhunter: Shadow Bite every 6 sec
    };

    int32 Param(Unit const* unit, uint32 spellId, uint8 effect = 0)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
    }

    bool InFelguardForm(Unit const* unit)
    {
        return unit && unit->HasAura(SPELL_FELGUARD_FORM);
    }

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

    // The warlock's pet is dismissed: any pet while the form is up, only a Felguard otherwise (the Favor takes Summon Felguard away; a Felguard that was already out, or that a login
    // brought back, goes too). PET_SAVE_NOT_IN_SLOT: the demon is not restored at the next login.
    void DismissPet(Player* player, bool anyPet)
    {
        Pet* pet = player->GetPet();
        if (pet && (anyPet || pet->GetEntry() == NPC_FELGUARD))
            player->RemovePet(pet, PET_SAVE_NOT_IN_SLOT);
    }

    // Felsteel Skin: its helper is on while the form and the Boon are both there.
    void SyncFelsteelSkin(Unit* unit)
    {
        bool const wanted = unit->HasAura(SPELL_FELSTEEL_SKIN) && InFelguardForm(unit);
        if (wanted && !unit->HasAura(SPELL_FELSTEEL_HELPER))
            unit->CastSpell(unit, SPELL_FELSTEEL_HELPER, true);
        else if (!wanted)
            unit->RemoveAurasDueToSpell(SPELL_FELSTEEL_HELPER);
    }

    // The idempotent state of the Felguard Favor: called by the Favor's 2 sec tick and by the hooks of the form. In the form: no pet, Felguard Plate on, Felsteel helper by its Boon.
    // Out of the form: a Felguard is dismissed, the helpers are off.
    void ReconcileForm(Player* player)
    {
        if (!player || !player->IsInWorld() || !player->IsAlive())
            return;

        if (InFelguardForm(player))
        {
            DismissPet(player, true);
            if (!player->HasAura(SPELL_FELGUARD_PLATE))
                player->CastSpell(player, SPELL_FELGUARD_PLATE, true);
        }
        else
        {
            DismissPet(player, false);
            player->RemoveAurasDueToSpell(SPELL_FELGUARD_PLATE);
        }

        SyncFelsteelSkin(player);
    }

    bool IsLegionDemon(Unit const* unit)
    {
        if (!unit || !unit->IsCreature())
            return false;

        uint32 const created = unit->GetUInt32Value(UNIT_CREATED_BY_SPELL);
        for (LegionDemon const& demon : LEGION_DEMONS)
            if (demon.entry == unit->GetEntry() && demon.summonSpell == created)
                return true;

        return false;
    }

    // The live temporary demons of the warlock (copied: the caller may unsummon them while it loops).
    std::vector<Unit*> LegionDemonsOf(Player* owner)
    {
        std::vector<Unit*> demons;
        for (Unit* controlled : owner->m_Controlled)
            if (controlled && controlled->IsAlive() && controlled->IsInWorld() && IsLegionDemon(controlled))
                demons.push_back(controlled);

        return demons;
    }

    // The enemy the warlock is fighting: the selected one if it is attackable, otherwise the victim.
    Unit* FightTarget(Player* player)
    {
        Unit* target = player->GetSelectedUnit();
        if (target && target->IsAlive() && player->IsValidAttackTarget(target))
            return target;

        target = player->GetVictim();
        return target && target->IsAlive() ? target : nullptr;
    }

    // Legion Lord: the demon lives 50% longer and gets the warlock's spell power as spell damage and as attack power. (Master Demonologist is NOT shared: see the Boon's description.)
    void ApplyLegionLord(Player* owner, Unit* demon)
    {
        if (!owner->HasAura(SPELL_LEGION_LORD) || demon->HasAura(SPELL_LEGION_MIGHT))
            return;

        if (TempSummon* summon = demon->ToTempSummon())
            summon->SetTimer(summon->GetTimer() + CalculatePct(summon->GetTimer(), std::max(Param(owner, SPELL_LEGION_LORD), 0)));

        int32 const power = std::max(owner->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW), owner->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE));
        if (power > 0)
        {
            int32 const amount = power - 1;                    // base points: the aura shows base points + 1
            demon->CastCustomSpell(demon, SPELL_LEGION_MIGHT, &amount, &amount, nullptr, true);
        }
    }

    // Calls the temporary demon number `index` (the one of that kind that is out leaves first). `burst`: the 10 sec demons of Demonic Empowerment.
    void CallLegionDemon(Player* owner, uint32 index, bool burst)
    {
        LegionDemon const& kind = LEGION_DEMONS[index];
        if (!sSpellMgr->GetSpellInfo(kind.summonSpell) || !sSpellMgr->GetSpellInfo(SPELL_LEGION_DEMON))
            return;

        for (Unit* old : LegionDemonsOf(owner))
            if (old->GetEntry() == kind.entry && old->HasAura(SPELL_LEGION_DEMON))
                if (TempSummon* summon = old->ToTempSummon())
                    summon->UnSummon();

        owner->CastSpell(owner, kind.summonSpell, true);

        Unit* target = FightTarget(owner);
        for (Unit* demon : LegionDemonsOf(owner))
        {
            if (demon->GetEntry() != kind.entry || demon->HasAura(SPELL_LEGION_DEMON))
                continue;                                      // another kind, or one that is already marked (an older one)

            demon->CastSpell(demon, SPELL_LEGION_DEMON, true);
            if (burst)
                if (TempSummon* summon = demon->ToTempSummon())
                    summon->SetTimer(std::min<uint32>(summon->GetTimer(), BURST_LIFETIME_MS));

            ApplyLegionLord(owner, demon);

            if (target)
                if (CreatureAI* ai = demon->ToCreature() ? demon->ToCreature()->AI() : nullptr)
                    ai->AttackStart(target);
        }
    }
}

// Demonic Frenzy (and the form gate of the procs): the proc only counts while the Felguard Form is up.
class spell_dem_form_gate : public AuraScript
{
    PrepareAuraScript(spell_dem_form_gate);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return InFelguardForm(GetTarget());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_dem_form_gate::CheckProc);
    }
};

// Fel Hunger: the same gate, and only single-target hits give mana (an area ability would give it once per enemy hit).
class spell_dem_fel_hunger : public AuraScript
{
    PrepareAuraScript(spell_dem_fel_hunger);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        return InFelguardForm(GetTarget()) && !(spell && spell->IsTargetingArea());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_dem_fel_hunger::CheckProc);
    }
};

// Felsteel Skin: a dummy passive. Its helper (armour, damage taken) is cast and removed here and by the form; see ReconcileForm.
class spell_dem_felsteel_skin : public AuraScript
{
    PrepareAuraScript(spell_dem_felsteel_skin);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            if (player->IsInWorld())
                SyncFelsteelSkin(player);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_FELSTEEL_HELPER);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_dem_felsteel_skin::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_dem_felsteel_skin::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Legion Cleave: effect 0 is a proc trigger (value = percent, 50) naming the bolt; up to 2 other enemies within 8 yards of the target take it. Only in the form, and only for
// single-target hits (the hits of Axe Sweep and the Whirl would cleave again and again).
class spell_dem_legion_cleave : public AuraScript
{
    PrepareAuraScript(spell_dem_legion_cleave);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        return InFelguardForm(GetTarget()) && !(spell && spell->IsTargetingArea());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        Unit* caster = GetTarget();
        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (damage <= 0)
            return;

        uint32 const bolt = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        uint32 hit = 0;
        for (Unit* other : EnemiesAround(caster, victim, CLEAVE_RANGE))
        {
            if (other == victim)
                continue;

            caster->CastCustomSpell(other, bolt, &damage, nullptr, nullptr, true, nullptr, aurEff);
            if (++hit >= CLEAVE_TARGETS)
                break;
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_dem_legion_cleave::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_dem_legion_cleave::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Searing Aura, bound to the tick of the stock Immolation Aura of the demon form (50590, an area damage effect cast by the warlock): the damage of the tick rises by effect 0 percent
// (30) and every enemy it burns gets the slow. Only while the warlock has the Boon (a learned passive that exists only in Metamorphosis).
class spell_dem_searing_aura_tick : public SpellScript
{
    PrepareSpellScript(spell_dem_searing_aura_tick);

    void HandleDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        AuraEffect const* boon = caster ? caster->GetAuraEffect(SPELL_SEARING_AURA, EFFECT_0) : nullptr;
        if (!boon || !target)
            return;

        SetHitDamage(GetHitDamage() + CalculatePct(GetHitDamage(), boon->GetAmount()));
        caster->CastSpell(target, boon->GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_dem_searing_aura_tick::HandleDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
    }
};

// Demonic Dread: effect 0 is a proc trigger (value = percent, 30) naming the shadow bolt (the fire bolt is the next spell id). The proc row is name based and wider than the three spells, so
// the class bits are checked again here (Shadow Bolt: word 0 bit 0; Incinerate and Soul Fire: word 1 bits 6 and 7). Only in Metamorphosis.
class spell_dem_demonic_dread : public AuraScript
{
    PrepareAuraScript(spell_dem_demonic_dread);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        if (!spell || spell->SpellFamilyName != SPELLFAMILY_WARLOCK || GetTarget()->GetShapeshiftForm() != FORM_METAMORPHOSIS)
            return false;

        return (spell->SpellFamilyFlags[0] & 0x1) || (spell->SpellFamilyFlags[1] & 0xC0);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        SpellInfo const* spell = eventInfo.GetSpellInfo();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!spell || !damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        int32 const damage = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        uint32 const shadowBolt = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        uint32 const bolt = (spell->GetSchoolMask() & SPELL_SCHOOL_MASK_SHADOW) ? shadowBolt : shadowBolt + 1;
        if (damage > 0)
            GetTarget()->CastCustomSpell(victim, bolt, &damage, nullptr, nullptr, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_dem_demonic_dread::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_dem_demonic_dread::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Fel Charge (the form's charge, castable only in the form): with Brutal Charge the enemy it hits is stunned (the stun spell is the Boon's helper). The cooldown is a spell modifier.
class spell_dem_fel_charge : public SpellScript
{
    PrepareSpellScript(spell_dem_fel_charge);

    void HandleStun()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (caster && target && caster->HasAura(SPELL_BRUTAL_CHARGE))
            caster->CastSpell(target, SPELL_BRUTAL_STUN, true);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_dem_fel_charge::HandleStun);
    }
};

// Favor of the Felguard Form: effect 0 is a dummy (apply: teach the form and its attacks; remove: end the form and take them away again, not when the player merely logs out),
// effect 1 a 2 sec tick that keeps the state of the form (no pet, Felguard Plate, Felsteel Skin) in step. This covers a pet that a login or a resurrection brings back.
class spell_dem_felguard_favor : public AuraScript
{
    PrepareAuraScript(spell_dem_felguard_favor);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        for (uint32 const spellId : FELGUARD_FAVOR_SPELLS)
            player->learnSpell(spellId);

        ReconcileForm(player);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->RemoveAurasDueToSpell(SPELL_FELGUARD_FORM);
        player->RemoveAurasDueToSpell(SPELL_FELGUARD_PLATE);
        player->RemoveAurasDueToSpell(SPELL_FELSTEEL_HELPER);
        for (uint32 const spellId : FELGUARD_FAVOR_SPELLS)
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        ReconcileForm(GetTarget()->ToPlayer());
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_dem_felguard_favor::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_dem_felguard_favor::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_dem_felguard_favor::HandleTick, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The Felguard Form aura itself (a transform): at once on gaining it the pet goes and the helpers come; on losing it they go. (The Favor's tick catches anything this misses, for example
// a form that comes back with a login before the player is in the world.)
class spell_dem_felguard_form : public AuraScript
{
    PrepareAuraScript(spell_dem_felguard_form);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        ReconcileForm(GetTarget()->ToPlayer());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_FELGUARD_PLATE);
        target->RemoveAurasDueToSpell(SPELL_FELSTEEL_HELPER);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_dem_felguard_form::HandleApply, EFFECT_0, SPELL_AURA_TRANSFORM, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_dem_felguard_form::HandleRemove, EFFECT_0, SPELL_AURA_TRANSFORM, AURA_EFFECT_HANDLE_REAL);
    }
};

// Bound to the stock Summon Imp, Felhunter, Voidwalker, Succubus, Felguard and the Enslave Demon ranks: a warlock in the Felguard Form has no demon at his side and cannot call one.
class spell_dem_no_summon_in_form : public SpellScript
{
    PrepareSpellScript(spell_dem_no_summon_in_form);

    SpellCastResult CheckForm()
    {
        return InFelguardForm(GetCaster()) ? SPELL_FAILED_NOT_SHAPESHIFT : SPELL_CAST_OK;
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_dem_no_summon_in_form::CheckForm);
    }
};

// Favor of the Metamorphosis Master: teaches the melee kit of Metamorphosis (the three castables are stance spells, the fourth a hidden stance passive that the core switches on in the form).
class spell_dem_metamorphosis_master : public AuraScript
{
    PrepareAuraScript(spell_dem_metamorphosis_master);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            for (uint32 const spellId : META_MASTER_SPELLS)
                player->learnSpell(spellId);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        for (uint32 const spellId : META_MASTER_SPELLS)
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_dem_metamorphosis_master::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_dem_metamorphosis_master::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Summoner: effect 1 ticks every 20 sec; in combat a random kind of temporary demon appears.
class spell_dem_summoner : public AuraScript
{
    PrepareAuraScript(spell_dem_summoner);

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (player && player->IsAlive() && player->IsInCombat())
            CallLegionDemon(player, urand(0, uint32(std::size(LEGION_DEMONS)) - 1), false);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_dem_summoner::HandleTick, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Bound to Demonic Empowerment (a second script beside the stock one): with the Favor of the Summoner, all four temporary demons appear for 10 sec.
class spell_dem_summoner_empowerment : public SpellScript
{
    PrepareSpellScript(spell_dem_summoner_empowerment);

    void HandleAfterCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || !player->HasAura(SPELL_FAVOR_SUMMONER))
            return;

        for (uint32 i = 0; i < std::size(LEGION_DEMONS); ++i)
            CallLegionDemon(player, i, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_dem_summoner_empowerment::HandleAfterCast);
    }
};

// The mark of a temporary demon (periodic dummy, every 3 sec): it follows the warlock's fight, and casts its signature spell now and then. The demon leaves when its owner is gone.
class spell_dem_legion_demon : public AuraScript
{
    PrepareAuraScript(spell_dem_legion_demon);

    void HandleTick(AuraEffect const* aurEff)
    {
        Creature* demon = GetTarget()->ToCreature();
        if (!demon || !demon->IsAlive())
            return;

        Unit* owner = demon->GetOwner();
        Player* player = owner ? owner->ToPlayer() : nullptr;
        if (!player || !player->IsAlive())
        {
            if (TempSummon* summon = demon->ToTempSummon())
                summon->UnSummon();

            return;
        }

        Unit* target = FightTarget(player);
        if (!target)
            return;

        if (demon->GetVictim() != target)
            if (CreatureAI* ai = demon->AI())
                ai->AttackStart(target);

        for (LegionDemon const& kind : LEGION_DEMONS)
        {
            if (kind.entry != demon->GetEntry() || !kind.signature)
                continue;

            if (aurEff->GetTickNumber() % kind.every == 0 && demon->GetDistance(target) <= SIGNATURE_RANGE && demon->IsWithinLOSInMap(target))
                demon->CastSpell(target, kind.signature, true);

            break;
        }
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_dem_legion_demon::HandleTick, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Bound to Inferno (stock 1122): with Legion Lord the Infernal lives 50% longer and has the warlock's spell power (as Legion Lord gives the temporary demons).
class spell_dem_legion_lord_infernal : public SpellScript
{
    PrepareSpellScript(spell_dem_legion_lord_infernal);

    void HandleAfterCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!player || !player->HasAura(SPELL_LEGION_LORD))
            return;

        std::vector<Unit*> infernals;
        for (Unit* controlled : player->m_Controlled)
            if (controlled && controlled->IsAlive() && controlled->IsCreature() && controlled->GetUInt32Value(UNIT_CREATED_BY_SPELL) == GetSpellInfo()->Id)
                infernals.push_back(controlled);

        for (Unit* infernal : infernals)
            ApplyLegionLord(player, infernal);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_dem_legion_lord_infernal::HandleAfterCast);
    }
};

void AddMoonlitBlessingsDemonologyScripts()
{
    RegisterSpellScript(spell_dem_form_gate);
    RegisterSpellScript(spell_dem_fel_hunger);
    RegisterSpellScript(spell_dem_felsteel_skin);
    RegisterSpellScript(spell_dem_legion_cleave);
    RegisterSpellScript(spell_dem_searing_aura_tick);
    RegisterSpellScript(spell_dem_demonic_dread);
    RegisterSpellScript(spell_dem_fel_charge);
    RegisterSpellScript(spell_dem_felguard_favor);
    RegisterSpellScript(spell_dem_felguard_form);
    RegisterSpellScript(spell_dem_no_summon_in_form);
    RegisterSpellScript(spell_dem_metamorphosis_master);
    RegisterSpellScript(spell_dem_summoner);
    RegisterSpellScript(spell_dem_summoner_empowerment);
    RegisterSpellScript(spell_dem_legion_demon);
    RegisterSpellScript(spell_dem_legion_lord_infernal);
}
