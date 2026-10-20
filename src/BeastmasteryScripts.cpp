// Moonlit Blessings: C++ spell scripts for the Beast Mastery Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/beastmastery.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_bm_pack_bond        the hidden aura on the pet: forwards each damaging blow of the pet to the hunter's Boons (Brawler's Pact haste, Howl of the Pack,
//                                       Hunt's Crescendo, Fang and Claw)
//   spell_blessings_bm_bond_keeper      bound to every Boon/Favor that needs the bond: puts the Pack Bond on the pet (every 2 sec, cheap)
//   spell_blessings_bm_brawlers_pact    Raptor Strike / Mongoose Bite give the pet an attack power buff (the pet-to-hunter half is in the bond)
//   spell_blessings_bm_same_enemy       Shared Wounds and the Favor of the Melee Companion: a buff while hunter and pet fight the same enemy
//   spell_blessings_bm_fang_dance       a dodge or parry readies the next Raptor Strike / Mongoose Bite and the pet's next special attack
//   spell_blessings_bm_fang_and_claw    the hunter's strikes make the pet strike again (the pet's strikes are in the bond)
//   spell_blessings_bm_summon_hawk      the kit ability Summon Hawk: adds ranged attack power to the instant damage, calls the Hawks, keeps the Hawk cap (the oldest leaves)
//   spell_blessings_bm_wild_hunt        Favor of the Wild Hunt: free Focus for the pet's special attacks while it is under Bestial Wrath
//   MoonlitBlessingsHawkScript          (UnitScript) sets the melee blows of the Hawks to a share of the pet's damage per second
//
// The Hawks: Summon Hawk (1310130) shares the category cooldown of Arcane Shot (data only). Each Hawk is the stock Fjord Hawk (creature 24747) summoned by 1310131 as an ally guardian
// that lasts 18 sec (Pack Master's duration modifier lengthens the summon); the hidden mark 1310132 on every Hawk carries its Ferocity critical bonus (effect 0) and its damage share
// of the pet (effect 1). The Pack Master Boon (1310050) adds damage (effect 1); the Favor of the Pack Alpha (1310100) raises the cap (effect 0) and the Hawks per cast (effect 1).
//
// The spell ids below come from the Beast Mastery registry block 1,310,000 (units are listed at the top of tools/talents/recipes/beastmastery.py, which asserts them);
// the helper spells a script casts are read from the TriggerSpell column of the passive's effects wherever that is simpler. Numbers (chances, percents) are read
// from the amounts of the passive's effects, so the recipe is the only place they are written.
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "TemporarySummon.h"
#include "UnitScript.h"
#include <algorithm>
#include <vector>

namespace
{
    enum BeastmasterySpells : uint32
    {
        SPELL_BRAWLERS_PACT      = 1310000,   // effect 0: trigger = pet attack power buff, effect 1: trigger = hunter melee haste buff
        SPELL_SHARED_WOUNDS      = 1310010,
        SPELL_FANG_DANCE         = 1310020,   // effect 0: trigger = Raptor/Mongoose buff, effect 1: trigger = pet critical buff
        SPELL_FANG_AND_CLAW      = 1310030,   // effect 0: chance, trigger = pet strike; effect 1: percent of the damage, trigger = hunter strike
        SPELL_HOWL_OF_THE_PACK   = 1310040,   // effect 1: trigger = the damage buff
        SPELL_PACK_MASTER        = 1310050,   // effect 0: duration modifier (+%, applied by the core), effect 1: Hawk damage bonus %
        SPELL_HUNTS_CRESCENDO    = 1310070,   // effect 1: trigger = the stacking buff
        SPELL_PACK_BOND          = 1310080,
        SPELL_FAVOR_PACK_ALPHA   = 1310100,   // effect 0: the most Hawks at once, effect 1: Hawks per cast
        SPELL_SUMMON_HAWK        = 1310130,   // effect 1: the most Hawks at once, effect 2: percent of ranged attack power added to the damage
        SPELL_HAWK_SUMMON        = 1310131,   // summons one Fjord Hawk per unit of its effect amount
        SPELL_HAWK_MARK          = 1310132,   // effect 0: critical chance bonus of the Hawk, effect 1: its damage in percent of the pet's per second

        SPELL_BESTIAL_WRATH      = 19574,
        SPELL_UNLEASHED_FURY     = 19620,     // rank 5 of the stock talent that the Mastery teaches: effect 0 = pet damage bonus %
        SPELL_FEROCITY           = 19602      // rank 5: effect 0 = pet critical chance bonus %
    };

    constexpr uint32 NPC_HAWK = 24747;                        // the stock Fjord Hawk creature (no AI, no script); only the ones summoned by SPELL_HAWK_SUMMON are ours
    constexpr uint32 HAWKS_PER_CAST = 1;                      // Hawks one Summon Hawk calls without the Favor of the Pack Alpha
    constexpr uint32 PET_SPECIAL_FLAG = 0x10000000;           // SpellFamilyFlags[1] of the Hunter pet special attacks (Bite, Claw, Smack, Gore, the breaths ...)
    constexpr float SHARED_STRIKE_RANGE = 30.0f;              // how far the hunter and the pet may be from the enemy for Fang and Claw

    int32 Param(Unit const* unit, uint32 spellId, uint8 effect = 0)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
    }

    // The helper spell named in the TriggerSpell column of effect `effect` of the aura `spellId` on `unit` (0 when the unit does not have it).
    uint32 HelperOf(Unit const* unit, uint32 spellId, uint8 effect)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetSpellInfo()->Effects[effect].TriggerSpell : 0;
    }

    bool IsPetSpecial(SpellInfo const* info)
    {
        return info && info->SpellFamilyName == SPELLFAMILY_HUNTER && (info->SpellFamilyFlags[1] & PET_SPECIAL_FLAG);
    }

    bool IsHawk(Unit const* unit)
    {
        return unit && unit->IsCreature() && unit->GetEntry() == NPC_HAWK && unit->GetUInt32Value(UNIT_CREATED_BY_SPELL) == SPELL_HAWK_SUMMON;
    }

    Player* OwnerPlayer(Unit const* unit)
    {
        Unit* owner = unit ? unit->GetOwner() : nullptr;
        return owner ? owner->ToPlayer() : nullptr;
    }

    // The pet and every other pet of the hunter that is alive (the Hawks included), copied so that casting on them cannot disturb the set.
    std::vector<Unit*> PetsOf(Player* owner)
    {
        std::vector<Unit*> pets;
        for (Unit* controlled : owner->m_Controlled)
            if (controlled && controlled->IsAlive() && (controlled->IsPet() || IsHawk(controlled)))
                pets.push_back(controlled);

        return pets;
    }

    // The live Hawks of the hunter, the oldest first (a creature's guid counter only grows, so the smallest is the oldest).
    std::vector<Unit*> HawksOf(Player* owner)
    {
        std::vector<Unit*> hawks;
        for (Unit* controlled : owner->m_Controlled)
            if (controlled && controlled->IsAlive() && IsHawk(controlled))
                hawks.push_back(controlled);

        std::sort(hawks.begin(), hawks.end(), [](Unit const* a, Unit const* b) { return a->GetGUID().GetCounter() < b->GetGUID().GetCounter(); });
        return hawks;
    }

    void EnsureBond(Player* player)
    {
        Pet* pet = player ? player->GetPet() : nullptr;
        if (pet && pet->IsAlive() && !pet->HasAura(SPELL_PACK_BOND))
            pet->CastSpell(pet, SPELL_PACK_BOND, true);
    }

    // The hunter's target (or the enemy the hunter attacks) is `enemy`.
    bool HunterFights(Player const* player, Unit const* enemy)
    {
        return enemy && (player->GetVictim() == enemy || player->GetTarget() == enemy->GetGUID());
    }

    // Summon Hawk: call the Hawks, drop the oldest beyond the cap, then mark the new ones and send them at `target`.
    void CallHawks(Player* owner, Unit* target)
    {
        SpellInfo const* castable = sSpellMgr->GetSpellInfo(SPELL_SUMMON_HAWK);
        if (!castable || !sSpellMgr->GetSpellInfo(SPELL_HAWK_SUMMON) || !sSpellMgr->GetSpellInfo(SPELL_HAWK_MARK))
            return;

        int32 cap = castable->Effects[EFFECT_1].CalcValue();
        int32 count = int32(HAWKS_PER_CAST);
        if (owner->HasAura(SPELL_FAVOR_PACK_ALPHA))
        {
            cap = std::max(cap, Param(owner, SPELL_FAVOR_PACK_ALPHA, EFFECT_0));
            count = std::max(count, Param(owner, SPELL_FAVOR_PACK_ALPHA, EFFECT_1));
        }

        owner->CastCustomSpell(SPELL_HAWK_SUMMON, SPELLVALUE_BASE_POINT0, count - 1, owner, true);

        std::vector<Unit*> hawks = HawksOf(owner);
        size_t const leaving = hawks.size() > size_t(cap) ? hawks.size() - size_t(cap) : 0;
        for (size_t i = 0; i < hawks.size(); ++i)
        {
            Unit* hawk = hawks[i];
            if (i < leaving)
            {
                if (TempSummon* summon = hawk->ToTempSummon())
                    summon->UnSummon();

                continue;
            }

            if (hawk->HasAura(SPELL_HAWK_MARK))
                continue;

            // Ferocity: the hunter's pet critical bonus (a spell modifier on the pet's abilities in the stock game) is given to the Hawk as a plain critical chance bonus
            int32 const crit = Param(owner, SPELL_FEROCITY, EFFECT_0);
            if (crit > 0)
                hawk->CastCustomSpell(SPELL_HAWK_MARK, SPELLVALUE_BASE_POINT0, crit - 1, hawk, true);
            else
                hawk->CastSpell(hawk, SPELL_HAWK_MARK, true);

            if (target && target->IsAlive())
                if (CreatureAI* ai = hawk->ToCreature() ? hawk->ToCreature()->AI() : nullptr)
                    ai->AttackStart(target);
        }
    }
}

// The hidden aura on the pet (a passive with a dummy effect that procs on the pet's damaging blows). It tells the hunter's Boons what the pet did.
class spell_blessings_bm_pack_bond : public AuraScript
{
    PrepareAuraScript(spell_blessings_bm_pack_bond);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* pet = GetTarget();
        Player* owner = OwnerPlayer(pet);
        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!owner || !owner->IsAlive() || !victim || !damageInfo || !damageInfo->GetDamage())
            return;

        // only the pet's own auto attacks and its special attacks count; the extra blows and echoes of the scripts do not
        SpellInfo const* info = eventInfo.GetSpellInfo();
        bool const special = IsPetSpecial(info);
        if (info && !special)
            return;

        uint32 const damage = damageInfo->GetDamage();
        bool const crit = (eventInfo.GetHitMask() & PROC_HIT_CRITICAL) != 0;

        // Brawler's Pact: a special attack gives the hunter melee haste
        if (special && owner->HasAura(SPELL_BRAWLERS_PACT))
            if (uint32 haste = HelperOf(owner, SPELL_BRAWLERS_PACT, EFFECT_1))
                owner->CastSpell(owner, haste, true);

        // Howl of the Pack: a critical hit of the pet raises the damage of the hunter and every pet of his
        if (crit && owner->HasAura(SPELL_HOWL_OF_THE_PACK))
            if (uint32 buff = HelperOf(owner, SPELL_HOWL_OF_THE_PACK, EFFECT_1))
            {
                owner->CastSpell(owner, buff, true);
                for (Unit* other : PetsOf(owner))
                    other->CastSpell(other, buff, true);
            }

        // Hunt's Crescendo: a special attack during Bestial Wrath adds a stack to the hunter and the pet, lasting as long as the rage
        if (special && owner->HasAura(SPELL_HUNTS_CRESCENDO))
            if (Aura const* wrath = pet->GetAura(SPELL_BESTIAL_WRATH))
                if (uint32 buff = HelperOf(owner, SPELL_HUNTS_CRESCENDO, EFFECT_1))
                {
                    int32 const left = wrath->GetDuration();
                    for (Unit* target : { static_cast<Unit*>(owner), pet })
                    {
                        target->CastSpell(target, buff, true);
                        if (Aura* stacks = target->GetAura(buff))
                            if (left > 0)
                            {
                                stacks->SetMaxDuration(left);
                                stacks->SetDuration(left);
                            }
                    }
                }

        // Fang and Claw: the pet's blow may make the hunter strike again at once
        if (AuraEffect const* chance = owner->GetAuraEffect(SPELL_FANG_AND_CLAW, EFFECT_0))
            if (roll_chance_i(chance->GetAmount()) && HunterFights(owner, victim) && victim->IsAlive() && owner->IsWithinDist(victim, SHARED_STRIKE_RANGE))
                if (uint32 strike = HelperOf(owner, SPELL_FANG_AND_CLAW, EFFECT_1))
                {
                    int32 const amount = std::max<int32>(1, int32(CalculatePct(damage, Param(owner, SPELL_FANG_AND_CLAW, EFFECT_1))));
                    owner->CastCustomSpell(strike, SPELLVALUE_BASE_POINT0, amount - 1, victim, true);
                }

    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_bm_pack_bond::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Puts the Pack Bond on the pet and keeps it there. Bound to every passive that has a periodic dummy effect for it.
class spell_blessings_bm_bond_keeper : public AuraScript
{
    PrepareAuraScript(spell_blessings_bm_bond_keeper);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        EnsureBond(GetTarget()->ToPlayer());
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_bm_bond_keeper::HandlePeriodic, EFFECT_ALL, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Brawler's Pact, the hunter half: Raptor Strike and Mongoose Bite (the proc names them) give the pet the stacking attack power buff.
class spell_blessings_bm_brawlers_pact : public AuraScript
{
    PrepareAuraScript(spell_blessings_bm_brawlers_pact);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        Pet* pet = player ? player->GetPet() : nullptr;
        uint32 const buff = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (pet && pet->IsAlive() && buff)
            pet->CastSpell(pet, buff, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_bm_brawlers_pact::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Shared Wounds (hunter and pet) and the Favor of the Melee Companion (pet only): the periodic effect (its TriggerSpell is the buff, its misc value is 1 for "both")
// keeps a short buff on while the pet's enemy is the hunter's target or the enemy the hunter attacks.
class spell_blessings_bm_same_enemy : public AuraScript
{
    PrepareAuraScript(spell_blessings_bm_same_enemy);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        uint32 const buff = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (!player || !buff)
            return;

        bool const both = aurEff->GetMiscValue() == 1;
        Pet* pet = player->GetPet();
        bool const together = player->IsAlive() && pet && pet->IsAlive() && HunterFights(player, pet->GetVictim());
        if (together)
        {
            pet->CastSpell(pet, buff, true);
            if (both)
                player->CastSpell(player, buff, true);
            return;
        }

        if (pet)
            pet->RemoveAurasDueToSpell(buff);

        player->RemoveAurasDueToSpell(buff);
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        uint32 const buff = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (Player* player = target->ToPlayer())
            if (Pet* pet = player->GetPet())
                pet->RemoveAurasDueToSpell(buff);

        target->RemoveAurasDueToSpell(buff);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_bm_same_enemy::HandlePeriodic, EFFECT_ALL, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_bm_same_enemy::HandleRemove, EFFECT_ALL, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Fang Dance: a dodge or parry (the proc) readies the two one-use buffs, both on the hunter (the pet's critical buff is a modifier the pet's spells read from its owner).
class spell_blessings_bm_fang_dance : public AuraScript
{
    PrepareAuraScript(spell_blessings_bm_fang_dance);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* hunter = GetTarget();
        for (uint8 effect : { EFFECT_0, EFFECT_1 })
            if (uint32 buff = GetSpellInfo()->Effects[effect].TriggerSpell)
                hunter->CastSpell(hunter, buff, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_bm_fang_dance::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Fang and Claw, the hunter half: a melee strike (auto attack, Raptor Strike, Mongoose Bite) may make the pet strike again, with a share of the damage.
class spell_blessings_bm_fang_and_claw : public AuraScript
{
    PrepareAuraScript(spell_blessings_bm_fang_and_claw);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        Unit* victim = eventInfo.GetActionTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Pet* pet = player ? player->GetPet() : nullptr;
        if (!pet || !pet->IsAlive() || !victim || !victim->IsAlive() || !damageInfo || !damageInfo->GetDamage())
            return;

        if (!roll_chance_i(aurEff->GetAmount()) || pet->GetVictim() != victim || !pet->IsWithinDist(victim, SHARED_STRIKE_RANGE))
            return;

        uint32 const strike = GetSpellInfo()->Effects[EFFECT_0].TriggerSpell;
        int32 const percent = Param(player, GetId(), EFFECT_1);
        int32 const amount = std::max<int32>(1, int32(CalculatePct(damageInfo->GetDamage(), percent)));
        if (strike)
            pet->CastCustomSpell(strike, SPELLVALUE_BASE_POINT0, amount - 1, victim, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_bm_fang_and_claw::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Summon Hawk (the kit ability, bound to its castable): effect 2 holds the percent of ranged attack power added to the instant damage (the rest of the damage is the spell's own).
// Calling the Hawks does not depend on the shot hitting.
class spell_blessings_bm_summon_hawk : public SpellScript
{
    PrepareSpellScript(spell_blessings_bm_summon_hawk);

    void HandleDamage(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (!caster || !GetHitUnit())
            return;

        int32 const percent = GetSpellInfo()->Effects[EFFECT_2].CalcValue(caster);
        SetHitDamage(GetHitDamage() + int32(CalculatePct(caster->GetTotalAttackPowerValue(RANGED_ATTACK), percent)));
    }

    void HandleAfterCast()
    {
        Player* owner = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (owner && owner->IsAlive())
            CallHawks(owner, GetExplTargetUnit());
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_blessings_bm_summon_hawk::HandleDamage, EFFECT_0, SPELL_EFFECT_SCHOOL_DAMAGE);
        AfterCast += SpellCastFn(spell_blessings_bm_summon_hawk::HandleAfterCast);
    }
};

// Favor of the Wild Hunt, the free-Focus half: a hunter buff (the pet's special attacks read it from their owner) is on exactly while the pet is under Bestial Wrath.
class spell_blessings_bm_wild_hunt : public AuraScript
{
    PrepareAuraScript(spell_blessings_bm_wild_hunt);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        uint32 const buff = GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (!player || !buff)
            return;

        Pet* pet = player->GetPet();
        bool const raging = player->IsAlive() && pet && pet->IsAlive() && pet->HasAura(SPELL_BESTIAL_WRATH);
        if (raging && !player->HasAura(buff))
            player->CastSpell(player, buff, true);
        else if (!raging && player->HasAura(buff))
            player->RemoveAurasDueToSpell(buff);
    }

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_bm_wild_hunt::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_bm_wild_hunt::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The Hawks: the incoming blow (already worked out from the Hawk's own range and its own auras, such as Howl of the Pack) is scaled so that the Hawk's average blow becomes a share
// of the pet's average blow (effect 1 of its mark, 20%), times 1.3 with Pack Master and times the Unleashed Fury bonus of the hunter, and again scaled by how much slower or faster
// the Hawk swings than the pet (so the share holds per second). Without a living pet the hunter's own range is the basis. The Ferocity critical bonus is on the mark aura.
class MoonlitBlessingsHawkScript : public UnitScript
{
public:
    MoonlitBlessingsHawkScript() : UnitScript("MoonlitBlessingsHawkScript", true, { UNITHOOK_MODIFY_MELEE_DAMAGE }) { }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage) override
    {
        if (!attacker || attacker->GetEntry() != NPC_HAWK || !IsHawk(attacker))
            return;

        AuraEffect const* mark = attacker->GetAuraEffect(SPELL_HAWK_MARK, EFFECT_1);
        Player* owner = OwnerPlayer(attacker);
        if (!mark || !owner)
            return;

        Unit* basis = owner->GetPet();
        if (!basis || !basis->IsAlive())
            basis = owner;

        float const basisAverage = (basis->GetFloatValue(UNIT_FIELD_MINDAMAGE) + basis->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        float const ownAverage = (attacker->GetFloatValue(UNIT_FIELD_MINDAMAGE) + attacker->GetFloatValue(UNIT_FIELD_MAXDAMAGE)) / 2.0f;
        if (ownAverage <= 0.0f)
            return;

        float const speed = float(attacker->GetAttackTime(BASE_ATTACK)) / float(std::max<uint32>(basis->GetAttackTime(BASE_ATTACK), 1));
        float const share = float(mark->GetAmount()) * float(100 + Param(owner, SPELL_PACK_MASTER, EFFECT_1)) / 100.0f * float(100 + Param(owner, SPELL_UNLEASHED_FURY, EFFECT_0)) / 100.0f;
        float const wanted = basisAverage * share / 100.0f * speed;
        damage = uint32(std::max(1.0f, float(damage) * wanted / ownAverage));
    }
};

void AddMoonlitBlessingsBeastmasteryScripts()
{
    RegisterSpellScript(spell_blessings_bm_pack_bond);
    RegisterSpellScript(spell_blessings_bm_bond_keeper);
    RegisterSpellScript(spell_blessings_bm_brawlers_pact);
    RegisterSpellScript(spell_blessings_bm_same_enemy);
    RegisterSpellScript(spell_blessings_bm_fang_dance);
    RegisterSpellScript(spell_blessings_bm_fang_and_claw);
    RegisterSpellScript(spell_blessings_bm_summon_hawk);
    RegisterSpellScript(spell_blessings_bm_wild_hunt);
    new MoonlitBlessingsHawkScript();
}
