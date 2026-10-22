// Moonlit Blessings: C++ spell scripts for the Feral Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/feral.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_feral_alpha_leader      Alpha of the Pack: a second script on the stock Leader of the Pack (24932) that lets the healing of Improved Leader of the Pack happen every 3 sec
//   spell_blessings_feral_pack_hunting      Favor of the Pack Hunting: teaches the pet spells, hands out the first companion, keeps the Pack Bond and the companion's attack power up
//   spell_blessings_feral_companion_might   the companion's attack power: a share of the druid's melee attack power (the stock pet scaling reads the RANGED attack power, which a cat has none of)
//   MoonlitFeralPlayerScript                answers the core's "is this player a hunter, for pet purposes?" for a druid who knows Call of the Pack (no core change)
//   MoonlitFeralUnitScript                  Hunter's Opening and Favor of the Ambusher (Pounce's bleed), Night Hunter and Ambusher (the window after Prowl), Bloodbath (the jump
//                                           of the strongest bleed)
//
// Spell ids come from the Feral registry block 1,220,000 (units are listed at the top of tools/talents/recipes/feral.py: the enum below must match them, and the recipe asserts every id
// and class bit named here) and the Druid kit ids 1,240,040 / 1,240,041. All state lives in auras (GUIDs and spell ids only): nothing is kept between hooks.
// ~Moonlit Nights Team

#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <list>
#include <vector>

namespace
{
    enum FeralSpells : uint32
    {
        SPELL_HUNTERS_OPENING      = 1220020,   // effect 2: percent more damage on every tick of Pounce's bleed
        SPELL_NIGHT_HUNTER         = 1220040,   // the Boon (its presence is all the script needs)
        SPELL_NIGHT_HUNTER_BUFF    = 1220041,   // +20% damage, +50% energy regeneration, 8 sec
        SPELL_BLOODBATH            = 1220050,   // effect 2: the range of the jump in yards
        SPELL_ALPHA_OF_THE_PACK    = 1220060,   // effect 2: the shortest time between two healings of Improved Leader of the Pack, in seconds
        SPELL_FAVOR_AMBUSHER       = 1220070,   // effect 1: percent more damage on every tick of Pounce's bleed
        SPELL_AMBUSHER_BUFF        = 1220071,   // +100% energy regeneration, 6 sec
        SPELL_FAVOR_PACK_HUNTING   = 1220090,   // effect 1: the range in yards of the Pack Bond
        SPELL_PACK_BOND            = 1220091,   // +10% damage done, on the druid and the companion
        SPELL_COMPANION_MIGHT      = 1220092,   // effect 0: attack power (worked out here), effect 1: percent of the druid's melee attack power
        SPELL_CALL_OF_THE_PACK     = 1240040,   // the Call Pet that works in Cat Form; knowing it makes a druid a pet owner (MoonlitFeralPlayerScript)
        SPELL_YOUNG_COMPANION      = 1240041,   // creates the first companion (a Young Nightsaber)

        // (the repair that casts the stock Leader of the Pack 24932 for a druid who was taught the talent spell 17007 lives in MoonlitBlessings.cpp, MoonlitDruidSharedScript: shared with the Guardian)
        SPELL_LEADER_HEAL               = 34299,    // the stock healing of Improved Leader of the Pack (the core gives it a 6 sec cooldown on the one who is healed)

        SPELL_TAME_BEAST           = 1515,
        SPELL_DISMISS_PET          = 2641,
        SPELL_REVIVE_PET           = 982,
        SPELL_FEED_PET             = 6991,
        SPELL_MEND_PET             = 13544
    };

    // The stock Prowl ranks (all carry the class bit 0x4000 of word 1) and the five bleeds the Pounce ranks trigger (SpellFamilyName 0, no class bit: asserted by the recipe).
    constexpr uint32 PROWL_RANKS[] = { 5215, 6783, 9913 };
    constexpr uint32 POUNCE_BLEEDS[] = { 9007, 9824, 9826, 27007, 49804 };

    // SpellFamilyFlags of the stock Druid spells (the recipe asserts them)
    constexpr uint32 FLAG_RAKE = 0x00001000;                // [0]
    constexpr uint32 FLAG_LACERATE = 0x00000100;            // [1]
    constexpr uint32 FLAG_RIP = 0x00200000;                 // [2]

    constexpr uint32 LEADER_HEAL_COOLDOWN_MS = 6000;        // the cooldown the core's Leader of the Pack script sets after a healing (spell_dru_leader_of_the_pack)
    constexpr uint32 EVENT_DELAY_MS = 1;                    // work that must not run inside the code that decided on it waits one millisecond of game time

    // The stock pet spells the Favor teaches. They belong to the Hunter's Beast Mastery skill line (50, class mask Hunter), and the core deletes a spell whose skill the character cannot have
    // when the character loads (Player::CheckSkillLearnedBySpell, ValidateSkillLearnedBySpells = 1), so they are learned as TEMPORARY spells: never saved, taught again by the Favor at every login.
    // Call of the Pack has no skill line; it is learned for good and is the switch that makes the druid a pet owner.
    constexpr uint32 TEMPORARY_PACK_SPELLS[] = { SPELL_TAME_BEAST, SPELL_DISMISS_PET, SPELL_REVIVE_PET, SPELL_FEED_PET, SPELL_MEND_PET };

    // The amount of effect `effect` of the passive `spellId` on `unit`; 0 when the unit does not have it.
    int32 Param(Unit const* unit, uint32 spellId, uint8 effect = 0)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
    }

    bool IsProwl(uint32 spellId)
    {
        return std::find(std::begin(PROWL_RANKS), std::end(PROWL_RANKS), spellId) != std::end(PROWL_RANKS);
    }

    bool IsPounceBleed(uint32 spellId)
    {
        return std::find(std::begin(POUNCE_BLEEDS), std::end(POUNCE_BLEEDS), spellId) != std::end(POUNCE_BLEEDS);
    }

    // Rake, Rip and Lacerate (any rank) by their class bits.
    bool IsBloodbathBleed(SpellInfo const* info)
    {
        return info && info->SpellFamilyName == SPELLFAMILY_DRUID
            && ((info->SpellFamilyFlags[0] & FLAG_RAKE) || (info->SpellFamilyFlags[1] & FLAG_LACERATE) || (info->SpellFamilyFlags[2] & FLAG_RIP));
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

    // Does the player own a pet anywhere: out, current, unslotted or in a stable slot?
    bool HasAnyPet(Player* player)
    {
        if (player->GetPet())
            return true;

        PetStable const* stable = player->GetPetStable();
        if (!stable)
            return false;

        if (stable->CurrentPet || !stable->UnslottedPets.empty())
            return true;

        for (auto const& slot : stable->StabledPets)
            if (slot)
                return true;

        return false;
    }

    // A bleed of the druid that is going to jump when its target dies: what is needed to rebuild it on the next enemy.
    struct BleedJump
    {
        ObjectGuid druid;
        uint32 spellId = 0;
        uint8 effectMask = 0;
        uint8 stacks = 1;
        int32 duration = 0;
        int32 amounts[MAX_SPELL_EFFECTS] = { };
        int64 score = 0;                                    // damage still to come: amount per tick (it already includes the stacks) x ticks left
    };

    // Bloodbath, one millisecond after the lethal blow: if the enemy really died, the saved bleed starts on the nearest other enemy within the Boon's range that does not carry it.
    // The copy has the damage per tick, stacks and time the original had left.
    void JumpBleed(Unit* corpse, BleedJump const& jump)
    {
        if (corpse->IsAlive() || !corpse->IsInWorld())
            return;

        Player* druid = ObjectAccessor::GetPlayer(*corpse, jump.druid);
        if (!druid || !druid->IsInWorld() || !druid->IsAlive())
            return;

        AuraEffect const* bloodbath = druid->GetAuraEffect(SPELL_BLOODBATH, EFFECT_2);
        SpellInfo const* info = sSpellMgr->GetSpellInfo(jump.spellId);
        if (!bloodbath || !info || bloodbath->GetAmount() <= 0)
            return;

        std::list<Unit*> enemies = EnemiesAround(druid, corpse, float(bloodbath->GetAmount()));
        ObjectGuid const druidGuid = druid->GetGUID();
        enemies.remove_if([corpse, info, druidGuid](Unit* unit) { return unit == corpse || !unit->IsAlive() || unit->GetAura(info->Id, druidGuid); });
        if (enemies.empty())
            return;

        Unit* best = enemies.front();
        for (Unit* unit : enemies)
            if (corpse->GetDistance(unit) < corpse->GetDistance(best))
                best = unit;

        Aura* copy = druid->AddAura(info, jump.effectMask, best);
        if (!copy)
            return;

        if (info->StackAmount > 1)
            copy->SetStackAmount(uint8(std::min<uint32>(jump.stacks, info->StackAmount)));

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect* effect = copy->GetEffect(i))
                effect->SetAmount(jump.amounts[i]);

        copy->SetDuration(jump.duration);
    }

    // Night Hunter and Favor of the Ambusher: a Prowl of the druid has ended (cancelled, broken by an attack, left with the form ...). A stealth breaking spell takes Prowl off when the cast starts,
    // before its hit is worked out, so the windows are already open for the opening Pounce or Ravage itself. (The core takes the aura out of its lists before it calls the script hook and is built
    // for spells to be cast from it.) The damage bonus of a bleed is fixed when the bleed starts, so bleeds that were already ticking keep the strength they had.
    void LeaveProwl(Player* player)
    {
        if (!player->IsInWorld() || !player->IsAlive())
            return;

        if (player->HasAura(SPELL_NIGHT_HUNTER))
            player->CastSpell(player, SPELL_NIGHT_HUNTER_BUFF, true);

        if (player->HasAura(SPELL_FAVOR_AMBUSHER))
            player->CastSpell(player, SPELL_AMBUSHER_BUFF, true);
    }
}

// Alpha of the Pack, the healing half: bound to the stock Leader of the Pack aura (24932), which the core's own script (spell_dru_leader_of_the_pack) handles too. The core heals the one whose
// critical strike set the aura off and then sets a 6 sec cooldown on the healing (spell 34299) on that player. This script runs BEFORE the effect handlers (OnProc), and when the druid who cast the
// aura has Alpha of the Pack and the cooldown has run for at least the Boon's number of seconds (3), it clears the cooldown so the core heals again. The cooldown exists only on players; the aura
// of a pet is left as the core has it. Without Improved Leader of the Pack the core heals for nothing, so there is nothing to speed up.
class spell_blessings_feral_alpha_leader : public AuraScript
{
    PrepareAuraScript(spell_blessings_feral_alpha_leader);

    void HandleProc(ProcEventInfo& /*eventInfo*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        Unit* druid = GetCaster();
        if (!player || !druid)
            return;

        int32 const seconds = Param(druid, SPELL_ALPHA_OF_THE_PACK, EFFECT_2);
        if (seconds <= 0 || uint32(seconds) * IN_MILLISECONDS >= LEADER_HEAL_COOLDOWN_MS)
            return;

        uint32 const left = player->GetSpellCooldownDelay(SPELL_LEADER_HEAL);
        if (left && left <= LEADER_HEAL_COOLDOWN_MS - uint32(seconds) * IN_MILLISECONDS)
            player->RemoveSpellCooldown(SPELL_LEADER_HEAL);
    }

    void Register() override
    {
        OnProc += AuraProcFn(spell_blessings_feral_alpha_leader::HandleProc);
    }
};

// Favor of the Pack Hunting. Effect 0 (applied) teaches the pet spells and, the first time, hands out a companion; removed (not on logout) the companion goes to the stable and the spells go.
// Effect 1 is the range of the Pack Bond in yards (10). Effect 2 is the one-second check: the companion's attack power aura is kept on the pet, and the Pack Bond (+10% damage) is kept on the
// druid and the pet while the pet is alive and within range, and taken off otherwise.
class spell_blessings_feral_pack_hunting : public AuraScript
{
    PrepareAuraScript(spell_blessings_feral_pack_hunting);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        bool const first = !player->HasSpell(SPELL_CALL_OF_THE_PACK);
        player->learnSpell(SPELL_CALL_OF_THE_PACK);
        for (uint32 const spellId : TEMPORARY_PACK_SPELLS)
            player->learnSpell(spellId, true);

        // the first companion: only the first time the Favor is taken, and only for a druid who has no pet anywhere
        if (first && !HasAnyPet(player))
            player->CastSpell(player, SPELL_YOUNG_COMPANION, true);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        // The pet leaves as "not in a slot", exactly as Dismiss Pet does it, so that nothing loads it at the next login (the pet slot "current" is loaded whatever the owner's class, and without Call of
        // the Pack the core would no longer treat the druid as a hunter): when the Favor comes back, Call of the Pack finds it again. RemovePet with no pet in the world also moves a
        // temporarily unsummoned pet (a mounted or flying player) from the current slot to the unslotted list. The spells that make the druid a pet owner are taken away last.
        Pet* pet = player->GetPet();
        if (pet)
        {
            pet->RemoveAurasDueToSpell(SPELL_PACK_BOND);
            pet->RemoveAurasDueToSpell(SPELL_COMPANION_MIGHT);
        }

        player->RemovePet(pet, PET_SAVE_NOT_IN_SLOT);

        player->RemoveAurasDueToSpell(SPELL_PACK_BOND);
        for (uint32 const spellId : TEMPORARY_PACK_SPELLS)
            player->removeSpell(spellId, SPEC_MASK_ALL, false);

        player->removeSpell(SPELL_CALL_OF_THE_PACK, SPEC_MASK_ALL, false);
    }

    void HandleTick(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        Pet* pet = player->GetPet();
        if (pet && (!pet->IsHunterPet() || !pet->IsAlive() || !pet->IsInWorld()))
            pet = nullptr;

        if (pet && !pet->HasAura(SPELL_COMPANION_MIGHT))
            pet->AddAura(SPELL_COMPANION_MIGHT, pet);

        float const range = float(std::max(Param(player, SPELL_FAVOR_PACK_HUNTING, EFFECT_1), 0));
        if (pet && player->IsAlive() && player->GetExactDist(pet) <= range)
        {
            if (!player->HasAura(SPELL_PACK_BOND))
                player->CastSpell(player, SPELL_PACK_BOND, true);

            if (!pet->HasAura(SPELL_PACK_BOND))
                player->AddAura(SPELL_PACK_BOND, pet);                      // CastSpell would put the aura on the druid again (the spell targets its caster)
        }
        else
        {
            player->RemoveAurasDueToSpell(SPELL_PACK_BOND);
            if (Pet* away = player->GetPet())
                away->RemoveAurasDueToSpell(SPELL_PACK_BOND);
        }

    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_feral_pack_hunting::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_feral_pack_hunting::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_feral_pack_hunting::HandleTick, EFFECT_2, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The companion's attack power: a hidden aura on the pet (kept there by the Favor's one-second check). The stock hunter pet scaling (spell_hun_generic_scaling on spell 34902) gives a hunter
// pet 22% of its owner's RANGED attack power; a druid in Cat Form or Bear Form has none (StatSystem.cpp), so the companion would hit like a kitten. This aura adds a share (effect 1: 20) of the
// druid's MELEE attack power, worked out again every two seconds like the stock script. The stock scaling still gives the pet its stamina, armour and resistances from the druid, and its share of
// the druid's ranged attack power when the druid is not in a feral form.
class spell_blessings_feral_companion_might : public AuraScript
{
    PrepareAuraScript(spell_blessings_feral_companion_might);

    void CalculateAmount(AuraEffect const* /*aurEff*/, int32& amount, bool& /*canBeRecalculated*/)
    {
        amount = 0;
        Unit* owner = GetUnitOwner() ? GetUnitOwner()->GetOwner() : nullptr;
        if (!owner || !owner->IsPlayer())
            return;

        int32 const percent = GetSpellInfo()->Effects[EFFECT_1].CalcValue();
        amount = int32(CalculatePct(std::max<float>(owner->GetTotalAttackPowerValue(BASE_ATTACK), 0.0f), percent));
    }

    void CalcPeriodic(AuraEffect const* /*aurEff*/, bool& isPeriodic, int32& amplitude)
    {
        isPeriodic = true;
        amplitude = 2 * IN_MILLISECONDS;
    }

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        PreventDefaultAction();
        GetEffect(aurEff->GetEffIndex())->RecalculateAmount();
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_feral_companion_might::CalculateAmount, EFFECT_0, SPELL_AURA_MOD_ATTACK_POWER);
        DoEffectCalcPeriodic += AuraEffectCalcPeriodicFn(spell_blessings_feral_companion_might::CalcPeriodic, EFFECT_0, SPELL_AURA_MOD_ATTACK_POWER);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_feral_companion_might::HandlePeriodic, EFFECT_0, SPELL_AURA_MOD_ATTACK_POWER);
    }
};

// Favor of the Pack Hunting: the core asks Player::IsClass(CLASS_HUNTER, CLASS_CONTEXT_PET) before it lets anyone tame a beast (Spell::EffectTameCreature), create or call a hunter pet
// (EffectCreateTamedPet, EffectSummonPet), choose the pet type when a pet is set up (Pet::InitStatsForLevel), use a stable master or resummon a pet after a mount or a flight. This hook answers
// "yes" for a Druid who knows Call of the Pack, and says nothing (the core's own answer stands) for everyone and everything else. A learned spell is the switch, not the Favor's aura: the pet
// is loaded at login before the Lua system puts the Favor's aura back, and the spell is saved with the character. The hook is asked very often, so everything is rejected on the first mismatch.
class MoonlitFeralPlayerScript : public PlayerScript
{
public:
    MoonlitFeralPlayerScript() : PlayerScript("MoonlitFeralPlayerScript", { PLAYERHOOK_ON_PLAYER_IS_CLASS }) { }

    Optional<bool> OnPlayerIsClass(Player const* player, Classes playerClass, ClassContext context) override
    {
        if (playerClass != CLASS_HUNTER || context != CLASS_CONTEXT_PET || !player || player->getClass() != CLASS_DRUID)
            return std::nullopt;

        if (!player->HasSpell(SPELL_CALL_OF_THE_PACK))
            return std::nullopt;

        return true;
    }
};

// One script object for everything that is not a proc or an aura of its own.
class MoonlitFeralUnitScript : public UnitScript
{
public:
    MoonlitFeralUnitScript() : UnitScript("MoonlitFeralUnitScript", true,
        { UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_ON_AURA_REMOVE, UNITHOOK_ON_DAMAGE }) { }

    // Hunter's Opening and Favor of the Ambusher: Pounce's damage is its bleed (Pounce Bleed, family 0 without a class bit), which no spell modifier can reach, so every tick of it is raised here
    // by the two percentages (30 and 60, added). Runs on every damage over time tick, so everything is rejected on the first mismatch.
    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!damage || !attacker || !spellInfo || !IsPounceBleed(spellInfo->Id) || !attacker->IsPlayer())
            return;

        int32 const percent = Param(attacker, SPELL_HUNTERS_OPENING, EFFECT_2) + Param(attacker, SPELL_FAVOR_AMBUSHER, EFFECT_1);
        if (percent > 0)
            damage += CalculatePct(damage, percent);
    }

    // Night Hunter and Favor of the Ambusher: the druid's Prowl ended. Both ids are checked first: this hook runs for every aura on every unit.
    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode /*mode*/) override
    {
        if (!unit || !aurApp || !unit->IsPlayer() || !IsProwl(aurApp->GetBase()->GetId()))
            return;

        Player* player = unit->ToPlayer();
        if (!player->IsInWorld() || !player->IsAlive() || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        if (!player->HasAura(SPELL_NIGHT_HUNTER) && !player->HasAura(SPELL_FAVOR_AMBUSHER))
            return;

        LeaveProwl(player);
    }

    // Bloodbath: this runs inside Unit::DealDamage before the death takes the auras away (OnUnitDeath comes after they are gone). Every druid with the Boon who has a Rake, Rip or Lacerate on the
    // victim gets his strongest one (the most damage still to come) written down; one millisecond later, if the victim really died, it starts on the nearest other enemy (JumpBleed).
    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!victim || !damage || damage < victim->GetHealth() || !victim->HasAuraType(SPELL_AURA_PERIODIC_DAMAGE))
            return;

        std::vector<BleedJump> best;                                        // one entry per druid
        for (AuraEffect const* effect : victim->GetAuraEffectsByType(SPELL_AURA_PERIODIC_DAMAGE))
        {
            if (!IsBloodbathBleed(effect->GetSpellInfo()))
                continue;

            Unit* caster = effect->GetCaster();
            if (!caster || !caster->IsPlayer() || caster == victim || !caster->IsInWorld() || !caster->GetAuraEffect(SPELL_BLOODBATH, EFFECT_2))
                continue;

            Aura const* aura = effect->GetBase();
            int32 const ticksLeft = std::max(effect->GetTotalTicks() - int32(effect->GetTickNumber()), 0);
            int64 const score = int64(std::max(effect->GetAmount(), 0)) * int64(ticksLeft);        // AuraEffect::CalculateAmount has already multiplied the amount by the stacks
            if (score <= 0)
                continue;

            auto it = std::find_if(best.begin(), best.end(), [caster](BleedJump const& jump) { return jump.druid == caster->GetGUID(); });
            if (it != best.end() && it->score >= score)
                continue;

            BleedJump jump;
            jump.druid = caster->GetGUID();
            jump.spellId = aura->GetId();
            jump.stacks = aura->GetStackAmount();
            jump.duration = aura->GetDuration();
            jump.score = score;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (AuraEffect const* each = aura->GetEffect(i))
                {
                    jump.effectMask |= uint8(1 << i);
                    jump.amounts[i] = each->GetAmount();
                }

            if (it != best.end())
                *it = jump;
            else
                best.push_back(jump);
        }

        for (BleedJump const& jump : best)
        {
            Unit* corpse = victim;
            corpse->m_Events.AddEventAtOffset([corpse, jump]() { JumpBleed(corpse, jump); }, std::chrono::milliseconds(EVENT_DELAY_MS));
        }
    }
};

void AddMoonlitBlessingsFeralScripts()
{
    RegisterSpellScript(spell_blessings_feral_alpha_leader);
    RegisterSpellScript(spell_blessings_feral_pack_hunting);
    RegisterSpellScript(spell_blessings_feral_companion_might);
    new MoonlitFeralPlayerScript();
    new MoonlitFeralUnitScript();
}
