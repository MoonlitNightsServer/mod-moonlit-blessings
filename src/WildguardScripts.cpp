// Moonlit Blessings: C++ spell scripts for the Wildguard Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/wildguard.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
// The Wildguard is the hunter whose PET is the tank. The scripts keep the tank stance and the Boon helpers on the pet, shape the damage and healing the pet takes, and drive the Beastrider.
//
//   MoonlitWildguardUnitScript                     UnitScript, three hooks:
//                                                    OnDamage             Blood Pact, Bulwark of the Beast, Warden's Vigil, Last Bond (the pet is hit), Mounted Charge (the pet hits)
//                                                    OnHeal               Heart of the Pack
//                                                    ModifyHealReceived   Shared Vigor (Mend Pet)
//   MoonlitWildguardPetScript                      PetScript: puts the helpers on a pet as it appears
//
//   The kit
//   spell_blessings_wildguard_bulwark_bond         the stance passive: every second keeps the tank stance and the Boon helpers on the pet (SyncPet)
//   spell_blessings_wildguard_growl                bound to the nine stock pet Growl ranks: 8 s cooldown, a real taunt, Focus cost and cooldown modifiers undone
//
//   Boons
//   spell_blessings_wildguard_blood_tether         ranged attacks heal the pet
//   spell_blessings_wildguard_dodging_guard        a dodge gives the pet a damage reduction
//   spell_blessings_wildguard_hounds_lure          ranged attacks on an enemy that attacks nobody give the pet threat
//   spell_blessings_wildguard_rider_sweep          Rider of the Wild: the pet's melee hits strike the enemies in front (the aura on the pet)
//
//   Favors
//   spell_blessings_wildguard_beastrider           teaches Ride Companion, stacks the pet's Momentum on every shot
//   spell_blessings_wildguard_ride_companion       the castable toggle
//   spell_blessings_wildguard_riding               the ride itself: the mount display, the hidden pet held beneath the hunter, the pet's target
//   spell_blessings_wildguard_lifebound            direct attacks heal the pet
//   spell_blessings_wildguard_lifebound_split      the damage share the hunter takes, never more than half of his current health
//
// Spell ids come from the Hunter kit block 1,340,000 and the Wildguard recipe block 1,340,100; tools/talents/recipes/wildguard.py asserts every unit named here.
// The amounts are read from the effects of the passives (Param), so the spell data stays the one place a number is written; a helper spell is the unit's id + 1 (+ 2 ...) as the recipe says.
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
#include "Cell.h"
#include "CellImpl.h"
#include "CombatManager.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GridNotifiers.h"
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
#include "ThreatManager.h"
#include "UnitScript.h"
#include "PetScript.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <list>

namespace
{
    enum WildguardSpells : uint32
    {
        SPELL_INTIMIDATION          = 19577,     // stock: its aura on the pet lasts while Intimidation lasts
        SPELL_PATHFINDING           = 19560,     // stock Pathfinding rank 2: raises the run speed while riding

        SPELL_BULWARK_BOND          = 1340010,   // kit: effect 0 periodic (1 s), effect 1 names the pet's tank stance
        SPELL_BULWARK_BUFF          = 1340011,   // the stance on the pet: effect 0 crit immunity, 1 threat, 2 armour (amount set by SyncPet)
        SPELL_GROWL_RULE            = 1340020,   // kit: effect 0 = cooldown of the pet's Growl in seconds (8)
        SPELL_GROWL_TAUNT           = 1340021,   // the real taunt the pet casts

        SPELL_BLOOD_TETHER          = 1340100,   // effect 0 = percent (3), TriggerSpell = the attribution heal
        SPELL_BARDED_COMPANION      = 1340110,   // helper +1 on the pet
        SPELL_DODGING_GUARD         = 1340120,   // effect 1 TriggerSpell = the buff for the pet
        SPELL_SHARED_VIGOR          = 1340130,   // effect 0 = percent (50), helper +1 = the attribution heal
        SPELL_HOUNDS_LURE           = 1340140,   // effect 0 = extra percent (100)
        SPELL_UNYIELDING_HIDE       = 1340150,   // helper +1 on the pet
        SPELL_BLOOD_PACT            = 1340160,   // effect 0 = percent (15)
        SPELL_MOUNTED_CHARGE        = 1340170,   // effect 0 = percent of attack power (150), effect 1 = percent of the damage as extra threat (200); +1 strike, +2 marker, +3 snare
        SPELL_WARDENS_VIGIL         = 1340180,   // effect 0 = percent less damage (30), 1 = percent healed (25), 2 = health threshold percent (35); +1 buff, +2 attribution heal; cooldown in RecoveryTime
        SPELL_STEADY_SEAT           = 1340200,   // helper +1 on the pet
        SPELL_LAST_BOND             = 1340220,   // effect 0 = percent of health left (20); cooldown in RecoveryTime
        SPELL_RIDER_OF_THE_WILD     = 1340230,   // effect 1 = percent of the pet's hit the sweep deals (50); +1 speed for the rider, +2 sweep aura on the pet, +3 sweep strike
        SPELL_BULWARK_OF_THE_BEAST  = 1340240,   // effect 0 = percent per attacker (3), effect 1 = cap (15); helper +1 on the pet
        SPELL_HEART_OF_THE_PACK     = 1340250,   // effect 0 = percent shared (30); +1 attribution heal

        SPELL_BEASTRIDER            = 1340260,   // the Favor: +1 Ride Companion, +2 the ride aura, +3 Momentum, +4 Rider's Pace
        SPELL_RIDE_COMPANION        = 1340261,
        SPELL_RIDING                = 1340262,
        SPELL_MOMENTUM              = 1340263,
        SPELL_RIDERS_PACE           = 1340264,
        SPELL_LIFEBOUND             = 1340280,   // effect 0 = percent healed (20), TriggerSpell = the attribution heal; +1 the split aura on the pet
        SPELL_LIFEBOUND_SPLIT       = 1340281
    };

    constexpr uint32 DISPLAY_INVISIBLE = 11686;            // "Invisible Stalker": the pet's model while its hunter rides it
    constexpr uint32 MEND_PET_FLAG = 0x00800000;           // SpellFamilyFlags[0] of every rank of Mend Pet
    constexpr int32 BULWARK_ARMOUR_SHARE_PCT = 65;         // on top of the 35% every hunter pet already inherits (spell_hun_generic_scaling): all of the hunter's armour
    constexpr float GLUE_DISTANCE = 1.5f;                  // the ridden pet is moved to the hunter when it is farther than this
    constexpr float RIDER_SWEEP_RANGE = 8.0f;              // yards around the pet that Rider of the Wild's sweep reaches
    constexpr uint32 RIDER_SWEEP_MAX = 5;                  // enemies hit besides the target
    constexpr uint32 PET_SYNC_DELAY_MS = 100;              // after a pet appears

    // The amount of effect `effect` of an aura of `spellId` on `unit` (0 without the aura).
    int32 Param(Unit const* unit, uint32 spellId, uint8 effect = EFFECT_0)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
    }

    // The hunter's pet when it is a living, placed hunter pet; otherwise null.
    Pet* LivePet(Player* player)
    {
        Pet* pet = player ? player->GetPet() : nullptr;
        return pet && pet->IsInWorld() && pet->IsAlive() && pet->getPetType() == HUNTER_PET ? pet : nullptr;
    }

    // Puts `spellId` on `target` (cast by `caster`) or takes it off.
    void EnsureAura(Unit* caster, Unit* target, uint32 spellId, bool wanted)
    {
        bool const has = target->HasAura(spellId);
        if (wanted && !has)
            caster->CastSpell(target, spellId, true);
        else if (!wanted && has)
            target->RemoveAurasDueToSpell(spellId);
    }

    // Everything the hunter's Blessing keeps on the pet: the tank stance (with the armour share), and the helper auras of the Boons and the Lifebound. Called every second by the stance passive
    // and when the pet appears. A helper is on exactly while its Boon is known (and, for Bulwark of the Beast, while Intimidation lasts).
    void SyncPet(Player* owner, Pet* pet)
    {
        if (!owner || !pet || !pet->IsInWorld() || !pet->IsAlive() || pet->getPetType() != HUNTER_PET)
            return;

        EnsureAura(pet, pet, SPELL_BULWARK_BUFF, true);
        if (AuraEffect* armour = pet->GetAuraEffect(SPELL_BULWARK_BUFF, EFFECT_2))
        {
            int32 const wanted = CalculatePct(std::max<int32>(0, int32(owner->GetArmor())), BULWARK_ARMOUR_SHARE_PCT);
            if (armour->GetAmount() != wanted)
                armour->ChangeAmount(wanted);
        }

        EnsureAura(pet, pet, SPELL_BARDED_COMPANION + 1, owner->HasAura(SPELL_BARDED_COMPANION));
        EnsureAura(pet, pet, SPELL_UNYIELDING_HIDE + 1, owner->HasAura(SPELL_UNYIELDING_HIDE));
        EnsureAura(pet, pet, SPELL_STEADY_SEAT + 1, owner->HasAura(SPELL_STEADY_SEAT));
        EnsureAura(pet, pet, SPELL_BULWARK_OF_THE_BEAST + 1, owner->HasAura(SPELL_BULWARK_OF_THE_BEAST) && pet->HasAura(SPELL_INTIMIDATION));

        // Lifebound: the split aura must be cast BY the hunter, so that the share lands on him
        if (owner->HasAura(SPELL_LIFEBOUND))
        {
            if (!pet->HasAura(SPELL_LIFEBOUND_SPLIT, owner->GetGUID()))
                owner->CastSpell(pet, SPELL_LIFEBOUND_SPLIT, true);
        }
        else if (pet->HasAura(SPELL_LIFEBOUND_SPLIT))
            pet->RemoveAurasDueToSpell(SPELL_LIFEBOUND_SPLIT);
    }

    // Takes every helper off the pet (the stance passive was lost: the Blessing changed).
    void StripPet(Pet* pet)
    {
        for (uint32 const spellId : { uint32(SPELL_BULWARK_BUFF), uint32(SPELL_BARDED_COMPANION + 1), uint32(SPELL_UNYIELDING_HIDE + 1), uint32(SPELL_STEADY_SEAT + 1),
                                      uint32(SPELL_BULWARK_OF_THE_BEAST + 1), uint32(SPELL_LIFEBOUND_SPLIT), uint32(SPELL_RIDER_OF_THE_WILD + 2) })
            pet->RemoveAurasDueToSpell(spellId);
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

    // Heals the pet for `pct` percent of the damage of the hit that triggered a proc (Blood Tether, Lifebound). `spellId` is the attribution heal.
    void HealPetFromHit(Unit* owner, ProcEventInfo& eventInfo, int32 pct, uint32 spellId)
    {
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        Pet* pet = LivePet(owner->ToPlayer());
        if (!pet || pet->IsFullHealth())
            return;

        BlessingsHelpers::HealUnit(owner, pet, CalculatePct(damageInfo->GetDamage(), pct), spellId);
    }

    thread_local bool sSharing = false;      // a heal the Heart of the Pack (or Shared Vigor) shares is not shared again

    // A moment later (never inside the damage code that decided on it): the Warden's Vigil buff on the pet and the heal.
    void ScheduleVigil(Pet* pet, ObjectGuid ownerGuid, int32 healPct)
    {
        pet->m_Events.AddEventAtOffset([pet, ownerGuid, healPct]()
        {
            Player* owner = ObjectAccessor::FindPlayer(ownerGuid);
            if (!owner || !pet->IsInWorld() || !pet->IsAlive())
                return;

            pet->CastSpell(pet, SPELL_WARDENS_VIGIL + 1, true);
            BlessingsHelpers::HealUnit(owner, pet, pet->CountPctFromMaxHealth(healPct), SPELL_WARDENS_VIGIL + 2);
        }, std::chrono::milliseconds(1));
    }

    // A hit that would have killed the pet left it with `left` health: the damage hook shortened the hit; when the pet had less than that already, it is lifted to it.
    void ScheduleRaise(Pet* pet, uint32 left)
    {
        pet->m_Events.AddEventAtOffset([pet, left]()
        {
            if (pet->IsInWorld() && pet->IsAlive() && pet->GetHealth() < left)
                pet->SetHealth(left);
        }, std::chrono::milliseconds(1));
    }

    // The hit of a pet: its first hit on an enemy is a charge (Mounted Charge).
    void PetDealsDamage(Pet* pet, Unit* victim)
    {
        Player* owner = pet->GetOwner();
        AuraEffect const* charge = owner ? owner->GetAuraEffect(SPELL_MOUNTED_CHARGE, EFFECT_0) : nullptr;
        if (!charge || pet->getPetType() != HUNTER_PET || !victim->IsAlive() || victim == owner || !pet->IsValidAttackTarget(victim))
            return;

        if (victim->HasAura(SPELL_MOUNTED_CHARGE + 2, pet->GetGUID()))
            return;

        ObjectGuid const petGuid = pet->GetGUID();
        int32 const attackPowerPct = charge->GetAmount();
        int32 const threatPct = Param(owner, SPELL_MOUNTED_CHARGE, EFFECT_1);
        victim->m_Events.AddEventAtOffset([victim, petGuid, attackPowerPct, threatPct]()
        {
            Creature* creature = ObjectAccessor::GetCreatureOrPetOrVehicle(*victim, petGuid);
            Pet* chargingPet = creature ? creature->ToPet() : nullptr;
            if (!chargingPet || !chargingPet->IsAlive() || !victim->IsAlive() || victim->HasAura(SPELL_MOUNTED_CHARGE + 2, petGuid))
                return;

            chargingPet->CastSpell(victim, SPELL_MOUNTED_CHARGE + 2, true);
            int32 const amount = std::max<int32>(1, CalculatePct(int32(chargingPet->GetTotalAttackPowerValue(BASE_ATTACK)), attackPowerPct));
            chargingPet->CastCustomSpell(SPELL_MOUNTED_CHARGE + 1, SPELLVALUE_BASE_POINT0, amount, victim, true);
            chargingPet->CastSpell(victim, SPELL_MOUNTED_CHARGE + 3, true);
            victim->GetThreatMgr().AddThreat(chargingPet, float(amount) * float(threatPct) / 100.0f);
        }, std::chrono::milliseconds(1));
    }

    // The hit the pet takes: Blood Pact, Bulwark of the Beast, Warden's Vigil and Last Bond reshape the damage that is about to land (after armour and absorbs).
    void PetTakesDamage(Pet* pet, uint32& damage)
    {
        Player* owner = pet->GetOwner();
        if (!owner || pet->getPetType() != HUNTER_PET || !pet->IsAlive() || !owner->HasAura(SPELL_BULWARK_BOND))
            return;

        uint32 const health = pet->GetHealth();
        uint32 const maxHealth = pet->GetMaxHealth();

        // Blood Pact: while the pet is below half health every hit is reduced
        if (AuraEffect const* pact = owner->GetAuraEffect(SPELL_BLOOD_PACT, EFFECT_0))
            if (uint64(health) * 2 < uint64(maxHealth))
                damage -= CalculatePct(damage, pact->GetAmount());

        // Bulwark of the Beast: a share per enemy that attacks the pet, up to the cap
        if (AuraEffect const* beast = owner->GetAuraEffect(SPELL_BULWARK_OF_THE_BEAST, EFFECT_0))
        {
            int32 const pct = std::min<int32>(beast->GetAmount() * int32(pet->getAttackers().size()), Param(owner, SPELL_BULWARK_OF_THE_BEAST, EFFECT_1));
            damage -= CalculatePct(damage, pct);
        }

        // Warden's Vigil: the hit that would take the pet below the threshold starts the reduction (for this hit too) and heals the pet
        if (AuraEffect const* vigil = owner->GetAuraEffect(SPELL_WARDENS_VIGIL, EFFECT_0))
        {
            if (!owner->HasSpellCooldown(SPELL_WARDENS_VIGIL))
            {
                uint32 const after = damage >= health ? 0 : health - damage;
                if (uint64(after) * 100 < uint64(maxHealth) * uint64(std::max<int32>(0, Param(owner, SPELL_WARDENS_VIGIL, EFFECT_2))))
                {
                    owner->AddSpellCooldown(SPELL_WARDENS_VIGIL, 0, vigil->GetSpellInfo()->RecoveryTime);
                    damage -= CalculatePct(damage, vigil->GetAmount());
                    ScheduleVigil(pet, owner->GetGUID(), Param(owner, SPELL_WARDENS_VIGIL, EFFECT_1));
                }
            }
        }

        // Last Bond: a killing blow leaves the pet at a fraction of its health instead
        if (AuraEffect const* bond = owner->GetAuraEffect(SPELL_LAST_BOND, EFFECT_0))
        {
            if (damage >= health && !owner->HasSpellCooldown(SPELL_LAST_BOND))
            {
                uint32 const left = std::max<uint32>(1, pet->CountPctFromMaxHealth(bond->GetAmount()));
                owner->AddSpellCooldown(SPELL_LAST_BOND, 0, bond->GetSpellInfo()->RecoveryTime);
                if (health > left)
                    damage = health - left;
                else
                {
                    damage = 0;
                    ScheduleRaise(pet, left);
                }
            }
        }
    }
}

class MoonlitWildguardUnitScript : public UnitScript
{
public:
    MoonlitWildguardUnitScript() : UnitScript("MoonlitWildguardUnitScript", true, { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_MODIFY_HEAL_RECEIVED }) { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!victim || !damage)
            return;

        if (Pet* pet = victim->ToPet())
            PetTakesDamage(pet, damage);

        if (attacker && attacker != victim && attacker->IsPet())
            if (Pet* pet = attacker->ToPet())
                PetDealsDamage(pet, victim);
    }

    // Heart of the Pack: a share of every heal that lands on the hunter or on his pet also lands on the other one.
    void OnHeal(Unit* /*healer*/, Unit* receiver, uint32& gain) override
    {
        if (!gain || !receiver || sSharing)
            return;

        Player* hunter = receiver->ToPlayer();
        Pet* pet = nullptr;
        if (hunter)
            pet = hunter->GetPet();
        else if (Pet* receiverPet = receiver->ToPet())
        {
            pet = receiverPet;
            hunter = receiverPet->GetOwner();
        }

        if (!hunter || !pet || pet->getPetType() != HUNTER_PET)
            return;

        AuraEffect const* heart = hunter->GetAuraEffect(SPELL_HEART_OF_THE_PACK, EFFECT_0);
        if (!heart)
            return;

        Unit* other = receiver == hunter ? static_cast<Unit*>(pet) : static_cast<Unit*>(hunter);
        if (!other->IsInWorld() || !other->IsAlive() || other->IsFullHealth())
            return;

        uint32 const share = CalculatePct(gain, heart->GetAmount());
        if (!share)
            return;

        sSharing = true;
        BlessingsHelpers::HealUnit(hunter, other, share, SPELL_HEART_OF_THE_PACK + 1);
        sSharing = false;
    }

    // Shared Vigor: every tick of Mend Pet on the pet also heals the hunter for a share of what the pet really gains. (The periodic path calls this with the pet first and the caster second.)
    void ModifyHealReceived(Unit* target, Unit* healer, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (!heal || sSharing || !spellInfo || spellInfo->SpellFamilyName != SPELLFAMILY_HUNTER || !(spellInfo->SpellFamilyFlags[0] & MEND_PET_FLAG))
            return;

        Pet* pet = target ? target->ToPet() : nullptr;
        Player* owner = pet ? pet->GetOwner() : nullptr;
        if (!owner || healer != owner || !owner->IsAlive())
            return;

        AuraEffect const* vigor = owner->GetAuraEffect(SPELL_SHARED_VIGOR, EFFECT_0);
        if (!vigor)
            return;

        uint32 const missing = pet->GetHealth() < pet->GetMaxHealth() ? pet->GetMaxHealth() - pet->GetHealth() : 0;
        uint32 const share = CalculatePct(std::min(heal, missing), vigor->GetAmount());
        if (!share || owner->IsFullHealth())
            return;

        sSharing = true;
        BlessingsHelpers::HealUnit(owner, owner, share, SPELL_SHARED_VIGOR + 1);
        sSharing = false;
    }
};

class MoonlitWildguardPetScript : public PetScript
{
public:
    MoonlitWildguardPetScript() : PetScript("MoonlitWildguardPetScript", { PETHOOK_ON_PET_ADD_TO_WORLD }) { }

    // A pet that appears (summoned, revived, after a zone change) gets its helpers at once instead of at the next second.
    void OnPetAddToWorld(Pet* pet) override
    {
        Player* owner = pet->GetOwner();
        if (!owner || pet->getPetType() != HUNTER_PET || !owner->HasAura(SPELL_BULWARK_BOND))
            return;

        ObjectGuid const ownerGuid = owner->GetGUID();
        pet->m_Events.AddEventAtOffset([pet, ownerGuid]()
        {
            if (Player* hunter = ObjectAccessor::FindPlayer(ownerGuid))
                SyncPet(hunter, pet);
        }, std::chrono::milliseconds(PET_SYNC_DELAY_MS));
    }
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ the kit
// Bulwark Bond: effect 0 is periodic (1 s): the pet's tank stance and the helpers of the Boons are kept on the pet.
class spell_blessings_wildguard_bulwark_bond : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_bulwark_bond);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_1].TriggerSpell });
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        Player* owner = GetTarget()->ToPlayer();
        if (owner)
            SyncPet(owner, owner->GetPet());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* owner = GetTarget()->ToPlayer();
        if (!owner || !owner->GetSession() || owner->GetSession()->PlayerLogout())
            return;

        if (Pet* pet = owner->GetPet())
            StripPet(pet);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_wildguard_bulwark_bond::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_wildguard_bulwark_bond::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Growl, all nine stock ranks: when the hunter has the Growl passive the cast leaves an 8 sec cooldown (whatever modifiers say), adds the real taunt, and Focus is paid in full.
// Everything that modifies "the pet's abilities" (Focused Resolve's cost, the Guardian's cooldown) is undone here, so no Boon or Favor can change the taunt.
class spell_blessings_wildguard_growl : public SpellScript
{
    PrepareSpellScript(spell_blessings_wildguard_growl);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_GROWL_TAUNT });
    }

    void HandleAfterCast()
    {
        Creature* pet = GetCaster() ? GetCaster()->ToCreature() : nullptr;
        Unit* owner = pet ? pet->GetOwner() : nullptr;
        AuraEffect const* rule = owner ? owner->GetAuraEffect(SPELL_GROWL_RULE, EFFECT_0) : nullptr;
        if (!rule)
            return;

        SpellInfo const* info = GetSpellInfo();
        if (info->PowerType == POWER_FOCUS)
        {
            int32 const paid = info->CalcPowerCost(pet, info->GetSchoolMask());
            int32 const full = int32(info->ManaCost);
            if (full > paid)
                pet->ModifyPower(POWER_FOCUS, -(full - paid));
        }

        uint32 const cooldown = uint32(std::max<int32>(1, rule->GetAmount())) * IN_MILLISECONDS;
        pet->AddSpellCooldown(info->Id, 0, cooldown);
        if (Player* player = owner->ToPlayer())
        {
            WorldPacket data;
            pet->BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_NONE, info->Id, cooldown);
            player->SendDirectMessage(&data);
        }

        if (Unit* target = GetExplTargetUnit())
            pet->CastSpell(target, SPELL_GROWL_TAUNT, true);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_blessings_wildguard_growl::HandleAfterCast);
    }
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ Boons
// Blood Tether: effect 0 is the percent of the damage of a ranged attack that heals the pet; its TriggerSpell is the attribution heal.
class spell_blessings_wildguard_blood_tether : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_blood_tether);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        HealPetFromHit(GetTarget(), eventInfo, aurEff->GetAmount(), GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_wildguard_blood_tether::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Dodging Guard: a dodged melee attack puts the buff named in the TriggerSpell column of effect 1 on the pet.
class spell_blessings_wildguard_dodging_guard : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_dodging_guard);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_1].TriggerSpell });
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        if (Pet* pet = LivePet(GetTarget()->ToPlayer()))
            pet->CastSpell(pet, GetSpellInfo()->Effects[EFFECT_1].TriggerSpell, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_wildguard_dodging_guard::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// Hound's Lure: effect 0 is the extra percent (100). A ranged attack on an enemy that is attacking nobody gives the pet threat equal to the damage plus that percent.
class spell_blessings_wildguard_hounds_lure : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_hounds_lure);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim || !victim->IsAlive() || victim->GetVictim() || !victim->CanHaveThreatList())
            return;

        Pet* pet = LivePet(GetTarget()->ToPlayer());
        if (!pet || !pet->IsValidAttackTarget(victim))
            return;

        victim->GetThreatMgr().AddThreat(pet, float(damageInfo->GetDamage()) * float(100 + aurEff->GetAmount()) / 100.0f);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_wildguard_hounds_lure::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Rider of the Wild, the aura on the pet while its hunter rides it: effect 0 is the percent (50) of a melee hit that also strikes the enemies in front of the pet.
class spell_blessings_wildguard_rider_sweep : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_rider_sweep);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* pet = GetTarget();
        Unit* main = eventInfo.GetActionTarget();
        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!pet || !main || !damageInfo || !damageInfo->GetDamage())
            return;

        if (SpellInfo const* procSpell = eventInfo.GetSpellInfo())
            if (procSpell->Id == SPELL_RIDER_OF_THE_WILD + 3)
                return;

        uint32 const share = CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount());
        if (!share)
            return;

        uint32 hit = 0;
        for (Unit* enemy : EnemiesAround(pet, pet, RIDER_SWEEP_RANGE))
        {
            if (enemy == main || !pet->HasInArc(float(M_PI), enemy))
                continue;

            pet->CastCustomSpell(SPELL_RIDER_OF_THE_WILD + 3, SPELLVALUE_BASE_POINT0, int32(share), enemy, true);
            if (++hit >= RIDER_SWEEP_MAX)
                break;
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_wildguard_rider_sweep::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------------------ Favors
// Favor of the Beastrider, the hidden aura: teaches Ride Companion while the Favor is chosen, and every ranged shot gives the pet a stack of Momentum.
class spell_blessings_wildguard_beastrider : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_beastrider);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->learnSpell(SPELL_RIDE_COMPANION);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        player->RemoveAurasDueToSpell(SPELL_RIDING);
        player->removeSpell(SPELL_RIDE_COMPANION, SPEC_MASK_ALL, false);
        if (Pet* pet = player->GetPet())
            pet->RemoveAurasDueToSpell(SPELL_MOMENTUM);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        if (Pet* pet = LivePet(GetTarget()->ToPlayer()))
            pet->CastSpell(pet, SPELL_MOMENTUM, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_wildguard_beastrider::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_wildguard_beastrider::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectProc += AuraEffectProcFn(spell_blessings_wildguard_beastrider::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Ride Companion, the castable: mounts the pet, or (cast again) dismounts.
class spell_blessings_wildguard_ride_companion : public SpellScript
{
    PrepareSpellScript(spell_blessings_wildguard_ride_companion);

    SpellCastResult CheckCast()
    {
        Player* player = GetCaster()->ToPlayer();
        if (!player)
            return SPELL_FAILED_BAD_TARGETS;

        if (player->HasAura(SPELL_RIDING))
            return SPELL_CAST_OK;

        if (!LivePet(player))
            return SPELL_FAILED_NO_PET;

        if (player->IsMounted() || player->GetVehicle() || player->IsInFlight())
            return SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW;

        return SPELL_CAST_OK;
    }

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Unit* caster = GetCaster();
        if (caster->HasAura(SPELL_RIDING))
            caster->RemoveAurasDueToSpell(SPELL_RIDING);
        else
            caster->CastSpell(caster, SPELL_RIDING, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_blessings_wildguard_ride_companion::CheckCast);
        OnEffectHitTarget += SpellEffectFn(spell_blessings_wildguard_ride_companion::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// The ride (the aura on the hunter; effect 0 is a marker, effect 1 is periodic every quarter second).
//   * The client draws the hunter on the pet's model: only the mount display field is set; the server-side mount flag is NOT, because the core refuses every shot and ability to a mounted player.
//   * The real pet gets an invisible model and a flag that stops it moving; every quarter second it is moved to the hunter. It fights the enemy in front of the hunter within melee reach:
//     the target is chosen here (the nearest enemy of the hunter or of the pet, in front, in reach); the pet's own AI attacks it and autocasts the pet's abilities at it.
//   * Rider of the Wild (if known) adds its speed and the sweep aura; Pathfinding adds its speed. Hard crowd control (stun, fear, confusion) ends the ride unless Rider of the Wild is known.
class spell_blessings_wildguard_riding : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_riding);

    float _petScale = 1.0f;
    ObjectGuid _petGuid;                   // the pet that is being ridden: a dismissed or re-summoned pet is another pet and ends the ride

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_RIDERS_PACE, SPELL_RIDER_OF_THE_WILD + 1, SPELL_RIDER_OF_THE_WILD + 2 });
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        Pet* pet = LivePet(player);
        if (!pet)
            return;                                  // the first tick ends the ride

        _petGuid = pet->GetGUID();
        _petScale = pet->GetObjectScale();
        player->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, pet->GetNativeDisplayId());
        pet->SetUnitFlag(UNIT_FLAG_DISABLE_MOVE);
        pet->StopMoving();
        pet->SetDisplayId(DISPLAY_INVISIBLE);
        MovePetToRider(player, pet);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        if (!player->IsMounted())
            player->SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);

        player->RemoveAurasDueToSpell(SPELL_RIDER_OF_THE_WILD + 1);
        player->RemoveAurasDueToSpell(SPELL_RIDERS_PACE);

        // only the pet that was ridden is given back: another pet was never hidden or held
        Pet* pet = _petGuid ? ObjectAccessor::GetPet(*player, _petGuid) : nullptr;
        if (!pet || !pet->IsInWorld())
            return;

        pet->RemoveUnitFlag(UNIT_FLAG_DISABLE_MOVE);
        pet->RemoveAurasDueToSpell(SPELL_RIDER_OF_THE_WILD + 2);
        pet->RestoreDisplayId();
        pet->SetObjectScale(_petScale);
        if (!pet->IsAlive())
            return;

        // it stood still beneath the rider: send it back to its master's side, or back at its enemy
        if (Unit* victim = pet->GetVictim())
        {
            if (CreatureAI* ai = pet->AI())
                ai->AttackStart(victim);
        }
        else
            pet->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, pet->GetFollowAngle());
    }

    static void MovePetToRider(Player* player, Pet* pet)
    {
        if (pet->GetMapId() != player->GetMapId() || pet->GetExactDist(player) > GLUE_DISTANCE)
            pet->UpdatePosition(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), player->GetOrientation(), true);
    }

    // The enemy in front of the hunter that the pet can hit from where it stands: the nearest of the enemies the hunter or the pet are in combat with.
    static Unit* PickTarget(Player* player, Pet* pet)
    {
        Unit* best = nullptr;
        float bestDistance = 0.0f;
        for (Unit* self : { static_cast<Unit*>(player), static_cast<Unit*>(pet) })
        {
            for (auto const& pair : self->GetCombatManager().GetPvECombatRefs())
            {
                Unit* enemy = pair.second->GetOther(self);
                if (!enemy || !enemy->IsAlive() || enemy->HasBreakableByDamageCrowdControlAura() || !pet->IsValidAttackTarget(enemy)
                    || !pet->IsWithinMeleeRange(enemy) || !player->HasInArc(float(M_PI), enemy))
                    continue;

                float const distance = pet->GetExactDist(enemy);
                if (!best || distance < bestDistance)
                {
                    best = enemy;
                    bestDistance = distance;
                }
            }
        }

        return best;
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        Pet* pet = LivePet(player);
        bool const rider = player && player->HasAura(SPELL_RIDER_OF_THE_WILD);
        if (!player || !pet || pet->GetGUID() != _petGuid || !player->IsAlive() || player->IsMounted() || player->GetVehicle() || player->IsInFlight() || pet->GetMapId() != player->GetMapId()
            || (!rider && (player->HasUnitState(UNIT_STATE_CONTROLLED) || player->IsCharmed())))
        {
            Remove();
            return;
        }

        MovePetToRider(player, pet);
        EnsureAura(player, player, SPELL_RIDER_OF_THE_WILD + 1, rider);
        EnsureAura(pet, pet, SPELL_RIDER_OF_THE_WILD + 2, rider);
        EnsureAura(player, player, SPELL_RIDERS_PACE, player->HasAura(SPELL_PATHFINDING));

        Unit* victim = pet->GetVictim();
        if (victim && victim->IsAlive() && pet->IsWithinMeleeRange(victim) && player->HasInArc(float(M_PI), victim))
            return;

        if (Unit* target = PickTarget(player, pet))
            if (CreatureAI* ai = pet->AI())
                ai->AttackStart(target);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_wildguard_riding::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_wildguard_riding::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_wildguard_riding::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Favor of the Lifebound, the hidden aura: effect 0 is the percent (20) of the damage of every direct attack that heals the pet; its TriggerSpell is the attribution heal.
class spell_blessings_wildguard_lifebound : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_lifebound);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell, SPELL_LIFEBOUND_SPLIT });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        HealPetFromHit(GetTarget(), eventInfo, aurEff->GetAmount(), GetSpellInfo()->Effects[EFFECT_0].TriggerSpell);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        if (Pet* pet = player->GetPet())
            pet->RemoveAurasDueToSpell(SPELL_LIFEBOUND_SPLIT);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_wildguard_lifebound::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_wildguard_lifebound::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The split-damage aura on the pet (cast by the hunter): the core moves the share of every hit to the hunter; here no share may be more than half of the hunter's current health.
class spell_blessings_wildguard_lifebound_split : public AuraScript
{
    PrepareAuraScript(spell_blessings_wildguard_lifebound_split);

    void HandleSplit(AuraEffect* /*aurEff*/, DamageInfo& /*dmgInfo*/, uint32& splitAmount)
    {
        Unit* hunter = GetCaster();
        if (!hunter || !hunter->IsAlive())
        {
            splitAmount = 0;
            return;
        }

        splitAmount = std::min(splitAmount, hunter->GetHealth() / 2);
    }

    void Register() override
    {
        OnEffectSplit += AuraEffectSplitFn(spell_blessings_wildguard_lifebound_split::HandleSplit, EFFECT_0);
    }
};

void AddMoonlitBlessingsWildguardScripts()
{
    RegisterSpellScript(spell_blessings_wildguard_bulwark_bond);
    RegisterSpellScript(spell_blessings_wildguard_growl);
    RegisterSpellScript(spell_blessings_wildguard_blood_tether);
    RegisterSpellScript(spell_blessings_wildguard_dodging_guard);
    RegisterSpellScript(spell_blessings_wildguard_hounds_lure);
    RegisterSpellScript(spell_blessings_wildguard_rider_sweep);
    RegisterSpellScript(spell_blessings_wildguard_beastrider);
    RegisterSpellScript(spell_blessings_wildguard_ride_companion);
    RegisterSpellScript(spell_blessings_wildguard_riding);
    RegisterSpellScript(spell_blessings_wildguard_lifebound);
    RegisterSpellScript(spell_blessings_wildguard_lifebound_split);
    new MoonlitWildguardUnitScript();
    new MoonlitWildguardPetScript();
}
