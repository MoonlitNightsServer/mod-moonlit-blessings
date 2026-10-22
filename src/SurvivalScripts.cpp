// Moonlit Blessings: C++ spell scripts for the Survival Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/survival.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_survival_skewer              RETIRED Boon (replaced by Survivalist's Discipline; the script stays registered so that no id moves): Mongoose Bite opens a bleed (two-handed weapon)
//   spell_blessings_survival_skewer_barbs        RETIRED Boon: critical strikes extend the hunter's bleeds on the target (two-handed weapon)
//   spell_blessings_survival_trappers_gambit     Boon: a trap going off resets the other traps and marks the enemies it catches
//   spell_blessings_survival_greatspear          Favor of the Greatspear: keeps the two-handed stance buff up while a two-handed weapon is wielded
//   spell_blessings_survival_twin_talons         Favor of the Twin Talons: teaches Dual Wield and keeps the dual wield stance buff up
//   spell_blessings_survival_lacerating_strikes  Mastery: Mongoose Bite opens a 21 sec bleed that deals the whole Bite again (Wow Forever Survival)
//   spell_blessings_survival_expose_prey         Mastery: attacks on the target of the hunter's Hunter's Mark sometimes ready Mongoose Bite
//   spell_blessings_survival_predators_edge      Mastery: keeps the dual wield or the two-handed stance (critical damage, off-hand damage, Raptor Strike / Mongoose Bite damage) up
//   spell_blessings_survival_lone_wolf           Boon of Survival AND Marksmanship: +20% damage while the hunter has no living pet (a stance kept up by a 1 sec check)
//   spell_blessings_survival_flanking_strike     Boon (tier 3, two-handed option): the castable; needs a two-handed weapon and a living pet (CheckCast); the hunter's hit is spell data, this adds the stacking buff and the pet's strike
//   spell_blessings_survival_flanking_reset      Boon passive (learned with Flanking Strike): a Raptor Strike hit may reset the cooldown of Flanking Strike
//   spell_blessings_survival_twin_rhythm         Boon (tier 3, dual wield option): the hunter's melee crits give the pet a stacking crit chance buff; keeps the listener on the pet
//   spell_blessings_survival_twin_rhythm_listener  the listener on the pet: the pet's crits give the dual wielding hunter a stacking crit chance buff
//   spell_blessings_survival_hunters_cadence     Boon: Mongoose Bite makes the next Raptor Strike hit harder, Raptor Strike may ready Mongoose Bite
//   spell_blessings_survival_stalkers_stride     Boon: a Strider Kick hit slows the target (the cooldown cut is spell data)
//   (Bloodtrail and Hunter's Quarry keystones have no script of their own: the Lacerating Strikes script and MoonlitSurvivalDamageScript read their auras)
//   spell_blessings_survival_cobra_slayer        Boon (Season of Discovery rune): melee attacks may cast a free Mongoose Bite with 45% attack power added
//   MoonlitSurvivalDamageScript                  Greatspear (auto attack damage), Snaremaster (melee strikes against enemies the hunter slowed or rooted) and Hunter's Quarry (melee damage of hunter and pet against the Hunter's Mark target)
//
// Spell ids come from the Survival registry block 1,330,000 (tools/talents/recipes/survival.py asserts every unit and helper named here and the stock ids).
// ~Moonlit Nights Team

#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <cmath>
#include <list>
#include <string_view>

namespace
{
    enum SurvivalSpells : uint32
    {
        SPELL_SKEWER_BLEED            = 1330031,   // 6 sec bleed, one tick per second; effect 0 holds the damage of one tick
        SPELL_GAMBIT_MARK             = 1330071,   // 10 sec, 10% more damage taken from the hunter
        SPELL_FAVOR_GREATSPEAR        = 1330080,   // Favor of the Greatspear (the hidden aura)
        SPELL_GREATSPEAR_STANCE       = 1330081,   // the stance buff: effect 0 haste, effect 1 the strike damage percent (also used for auto attacks)
        SPELL_FAVOR_SNAREMASTER       = 1330090,   // Favor of the Snaremaster: effect 1 holds the percent of extra melee damage against slowed or rooted enemies
        SPELL_FAVOR_TWIN_TALONS       = 1330100,   // Favor of the Twin Talons
        SPELL_TWIN_TALONS_STANCE      = 1330101,   // the stance buff
        SPELL_LACERATING_BLEED        = 1330111,   // 21 sec bleed, one tick every 3 sec; effect 0 holds the damage of one tick
        SPELL_PREDATORS_DUAL          = 1330141,   // Predator's Edge, dual wield stance
        SPELL_PREDATORS_TWO_HANDED    = 1330142,   // Predator's Edge, two-handed stance
        SPELL_LONE_WOLF_STANCE        = 1330151,   // Lone Wolf: +20% damage done
        SPELL_FLANKING_STRIKE         = 1330180,   // the castable (class mask bit "Flanking Strike")
        SPELL_FLANKING_BUFF           = 1330181,   // Flanked: Mongoose Bite and Raptor Strike +8% damage per stack, 3 stacks, 10 sec
        SPELL_FLANKING_PET_HIT        = 1330182,   // the pet's 100% weapon hit
        SPELL_FLANKING_RESET          = 1330183,   // the hidden passive that resets the cooldown
        SPELL_COBRA_SLAYER            = 1330190,   // Cobra Slayer: effect 0 base chance, 1 step per miss, 2 attack power percent
        SPELL_TWIN_RHYTHM             = 1330200,   // Twin Rhythm (the Boon aura on the hunter)
        SPELL_TWIN_RHYTHM_PET         = 1330201,   // the stacking crit chance buff on the pet (cast by the pet on itself)
        SPELL_TWIN_RHYTHM_HUNTER      = 1330202,   // the stacking crit chance buff on the hunter (cast by the hunter on himself)
        SPELL_TWIN_RHYTHM_LISTENER    = 1330203,   // the hidden proc aura kept on the pet: the pet's melee crits
        SPELL_CADENCE_BUFF            = 1330211,   // Hunter's Cadence: the next Raptor Strike deals 30% more damage
        SPELL_STRIDE_SLOW             = 1330221,   // Stalker's Stride: 40% slow for 5 sec
        SPELL_BLOODTRAIL              = 1330230,   // Bloodtrail (keystone): effect 0 holds the percent added to the Lacerating Strikes bleed
        SPELL_HUNTERS_QUARRY          = 1330240    // Hunter's Quarry (keystone): effect 0 holds the percent of extra damage against the Hunter's Mark target
    };

    enum StockSpells : uint32
    {
        SPELL_DUAL_WIELD              = 674,       // Dual Wield Passive: SPELL_EFFECT_DUAL_WIELD (the core drops dual wield from a player who does not know it)
        SPELL_MONGOOSE_BITE           = 14271      // top rank at level 60; Expose Prey clears the cooldown category of Mongoose Bite (the recipe asserts category 65 and the ranks)
    };

    // SpellFamilyFlags of the Hunter spells the scripts tell apart (checked against the spell store by the recipe)
    constexpr uint32 FLAGS0_RAPTOR_OR_MONGOOSE = 0x00000002;     // Raptor Strike and Mongoose Bite
    constexpr uint32 FLAGS2_RAPTOR_STRIKE      = 0x00010000;     // Raptor Strike only
    constexpr uint32 FLAGS1_COUNTERATTACK      = 0x00080000;     // Counterattack
    constexpr uint32 FLAGS0_HUNTERS_MARK       = 0x00000400;     // Hunter's Mark, all ranks (asserted by the recipe)

    constexpr float FLANKING_PET_RANGE = 10.0f;                    // the pet joins a Flanking Strike only within this many yards of the target
    constexpr float GAMBIT_RANGE     = 10.0f;                      // yards around the enemy that set an area trap off
    constexpr int32 SKEWER_TICKS     = 6;                          // the bleed lasts 6 sec in one second ticks (asserted by the recipe)
    constexpr int32 LACERATING_TICKS = 7;                          // the bleed lasts 21 sec in ticks of 3 sec (asserted by the recipe)
    constexpr int32 BLEED_EXTEND_CAP = 2;                          // a bleed never lasts more than this many times its normal length

    // A two-handed melee weapon in the main hand.
    bool IsTwoHanded(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        if (!player)
            return false;

        Item const* item = player->GetWeaponForAttack(BASE_ATTACK, true);
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        return proto && proto->InventoryType == INVTYPE_2HWEAPON;
    }

    // A weapon in each hand.
    bool IsDualWielding(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        return player && player->GetWeaponForAttack(BASE_ATTACK, true) && player->GetWeaponForAttack(OFF_ATTACK, true);
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

    // Does the unit have a living pet with him? (Player::GetPet is the one active pet; a dead pet that is still summoned does not count.)
    bool HasLivingPet(Unit const* unit)
    {
        Player const* player = unit ? unit->ToPlayer() : nullptr;
        Pet const* pet = player ? player->GetPet() : nullptr;
        return pet && pet->IsAlive();
    }

    // Keep a stance buff on exactly while its condition holds.
    void SyncStance(Unit* unit, uint32 stanceId, bool wanted)
    {
        bool const has = unit->HasAura(stanceId);
        if (wanted && !has)
            unit->CastSpell(unit, stanceId, true);
        else if (!wanted && has)
            unit->RemoveAurasDueToSpell(stanceId);
    }

    // Is `target` slowed or rooted by `attacker` himself?
    bool IsSnaredBy(Unit const* target, Unit const* attacker)
    {
        ObjectGuid const guid = attacker->GetGUID();
        for (AuraEffect const* aurEff : target->GetAuraEffectsByType(SPELL_AURA_MOD_DECREASE_SPEED))
            if (aurEff->GetCasterGUID() == guid && aurEff->GetAmount() < 0)
                return true;

        for (AuraEffect const* aurEff : target->GetAuraEffectsByType(SPELL_AURA_MOD_ROOT))
            if (aurEff->GetCasterGUID() == guid)
                return true;

        return false;
    }

    // The five trap kinds, told apart by the English name of the spell (the casts are "Immolation Trap", "Explosive Trap", "Freezing Trap", "Frost Trap" and "Snake Trap"; the spells a
    // trap sets off are "Immolation Trap", "Explosive Trap Effect", "Freezing Trap Effect" (and "Freezing Arrow Effect"), "Frost Trap Aura" and "Snake Trap Effect").
    enum TrapKind : uint8
    {
        TRAP_NONE,
        TRAP_IMMOLATION,
        TRAP_EXPLOSIVE,
        TRAP_FREEZING,
        TRAP_FROST,
        TRAP_SNAKE
    };

    TrapKind TrapKindOf(SpellInfo const* info)
    {
        char const* raw = info ? info->SpellName[LOCALE_enUS] : nullptr;
        if (!raw)
            return TRAP_NONE;

        std::string_view const name(raw);
        if (name.find("Immolation") != std::string_view::npos)
            return TRAP_IMMOLATION;
        if (name.find("Explosive") != std::string_view::npos)
            return TRAP_EXPLOSIVE;
        if (name.find("Freezing") != std::string_view::npos)
            return TRAP_FREEZING;
        if (name.find("Frost") != std::string_view::npos)
            return TRAP_FROST;
        if (name.find("Snake") != std::string_view::npos)
            return TRAP_SNAKE;

        return TRAP_NONE;
    }

    // A hunter spell that places a trap: its first effect summons a game object into one of the four object slots.
    bool IsTrapCast(SpellInfo const* info)
    {
        if (!info || info->SpellFamilyName != SPELLFAMILY_HUNTER)
            return false;

        uint32 const effect = info->Effects[EFFECT_0].Effect;
        return effect >= SPELL_EFFECT_SUMMON_OBJECT_SLOT1 && effect <= SPELL_EFFECT_SUMMON_OBJECT_SLOT4;
    }
}

// Skewer: effect 0 holds the percent of the Mongoose Bite damage that the bleed deals in all. The spell_proc mask also matches Raptor Strike, so the script lets only
// Mongoose Bite through (Raptor Strike carries bit 0x10000 of the third class mask word). The bleed spell takes the damage of ONE tick, as its first base points.
class spell_blessings_survival_skewer : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_skewer);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        return spell && (spell->SpellFamilyFlags[0] & FLAGS0_RAPTOR_OR_MONGOOSE) && !(spell->SpellFamilyFlags[2] & FLAGS2_RAPTOR_STRIKE) && IsTwoHanded(GetTarget());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim || !victim->IsAlive())
            return;

        int32 const total = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        int32 const perTick = total / SKEWER_TICKS;
        if (perTick <= 0)
            return;

        GetTarget()->CastCustomSpell(SPELL_SKEWER_BLEED, SPELLVALUE_BASE_POINT0, perTick - 1, victim, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_survival_skewer::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_skewer::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Skewer, the critical strikes: effect 0 holds the extension in seconds. Every bleed (an aura whose effects have the bleed mechanic) that the hunter owns on the struck enemy is
// lengthened, to at most BLEED_EXTEND_CAP times its normal length. Only critical hits reach here (spell_proc hit mask).
class spell_blessings_survival_skewer_barbs : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_skewer_barbs);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return IsTwoHanded(GetTarget());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* victim = eventInfo.GetActionTarget();
        if (!victim || !victim->IsAlive())
            return;

        int32 const extension = aurEff->GetAmount() * IN_MILLISECONDS;
        ObjectGuid const casterGuid = GetTarget()->GetGUID();
        for (auto const& entry : victim->GetAppliedAuras())
        {
            Aura* aura = entry.second->GetBase();
            if (aura->GetCasterGUID() != casterGuid || aura->IsRemoved())
                continue;

            SpellInfo const* info = aura->GetSpellInfo();
            if (!(info->GetAllEffectsMechanicMask() & (1ULL << MECHANIC_BLEED)))
                continue;

            int32 const normal = std::max(info->GetMaxDuration(), 0);
            int32 const duration = aura->GetDuration();
            if (normal <= 0 || duration <= 0)
                continue;

            int32 const longer = std::min(duration + extension, normal * BLEED_EXTEND_CAP);
            if (longer <= duration)
                continue;

            aura->SetMaxDuration(std::max(aura->GetMaxDuration(), longer));
            aura->SetDuration(longer);
        }
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_survival_skewer_barbs::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_skewer_barbs::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Trapper's Gambit: the core raises the trap activation proc (finish phase) for the owner of a trap that goes off; the victim of the event is the enemy that set it off.
// Once per 30 sec (the Cooldown of the spell_proc row). Every other trap the hunter knows has its cooldown cleared; Frost, Explosive and Immolation Traps (area traps) mark every
// enemy within GAMBIT_RANGE yards of the one that set them off, the others mark that one enemy.
class spell_blessings_survival_trappers_gambit : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_trappers_gambit);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        TrapKind const kind = TrapKindOf(eventInfo.GetSpellInfo());
        for (auto const& entry : player->GetSpellMap())
        {
            if (entry.second->State == PLAYERSPELL_REMOVED)
                continue;

            SpellInfo const* info = sSpellMgr->GetSpellInfo(entry.first);
            if (!IsTrapCast(info))
                continue;

            TrapKind const other = TrapKindOf(info);
            if (other != TRAP_NONE && other != kind)
                player->RemoveSpellCooldown(entry.first, true);
        }

        Unit* victim = eventInfo.GetActionTarget() ? eventInfo.GetActionTarget() : eventInfo.GetProcTarget();
        if (!victim || !victim->IsAlive())
            return;

        if (kind == TRAP_FROST || kind == TRAP_EXPLOSIVE || kind == TRAP_IMMOLATION)
        {
            for (Unit* enemy : EnemiesAround(player, victim, GAMBIT_RANGE))
                player->CastSpell(enemy, SPELL_GAMBIT_MARK, true);
        }
        else if (player->IsValidAttackTarget(victim))
            player->CastSpell(victim, SPELL_GAMBIT_MARK, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_trappers_gambit::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Favor of the Greatspear: once a second (and at once when the Favor is switched on) the stance buff is put on the hunter while the main-hand weapon is two-handed and taken off otherwise.
class spell_blessings_survival_greatspear : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_greatspear);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, SPELL_GREATSPEAR_STANCE, IsTwoHanded(target));
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, SPELL_GREATSPEAR_STANCE, target->IsAlive() && IsTwoHanded(target));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_GREATSPEAR_STANCE);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_survival_greatspear::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_survival_greatspear::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_survival_greatspear::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Twin Talons: teaches Dual Wield (a hunter without it; it is not taken away again, because the hunter trainer teaches the same spell) and keeps the stance buff on the hunter
// while a weapon is wielded in each hand.
class spell_blessings_survival_twin_talons : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_twin_talons);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player)
            return;

        if (player->getClass() == CLASS_HUNTER && !player->HasSpell(SPELL_DUAL_WIELD))
            player->learnSpell(SPELL_DUAL_WIELD);

        SyncStance(player, SPELL_TWIN_TALONS_STANCE, IsDualWielding(player));
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, SPELL_TWIN_TALONS_STANCE, target->IsAlive() && IsDualWielding(target));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_TWIN_TALONS_STANCE);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_survival_twin_talons::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_survival_twin_talons::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_survival_twin_talons::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Lacerating Strikes (Mastery): effect 0 holds the percent of the Mongoose Bite damage that the bleed deals in all. The spell_proc mask also matches Raptor Strike, so the script lets only
// Mongoose Bite through (Raptor Strike carries bit 0x10000 of the third class mask word) unless the hunter has the keystone Bloodtrail, which lets Raptor Strike open the bleed too and adds its
// effect 0 (50) to the percent. The bleed spell takes the damage of ONE tick, as its first base points; a new hit recasts it.
class spell_blessings_survival_lacerating_strikes : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_lacerating_strikes);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        if (!spell || !(spell->SpellFamilyFlags[0] & FLAGS0_RAPTOR_OR_MONGOOSE))
            return false;

        return !(spell->SpellFamilyFlags[2] & FLAGS2_RAPTOR_STRIKE) || GetTarget()->HasAura(SPELL_BLOODTRAIL);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim || !victim->IsAlive())
            return;

        int32 percent = aurEff->GetAmount();
        if (AuraEffect const* trail = GetTarget()->GetAuraEffect(SPELL_BLOODTRAIL, EFFECT_0))
            percent += trail->GetAmount();

        int32 const total = int32(CalculatePct(damageInfo->GetDamage(), percent));
        int32 const perTick = total / LACERATING_TICKS;
        if (perTick <= 0)
            return;

        GetTarget()->CastCustomSpell(SPELL_LACERATING_BLEED, SPELLVALUE_BASE_POINT0, perTick - 1, victim, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_survival_lacerating_strikes::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_lacerating_strikes::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Expose Prey (Mastery): the chance (10%) is in the spell_proc row. Only attacks on an enemy that carries the hunter's OWN Hunter's Mark reach the effect, and Mongoose Bite itself does not. The effect
// is what a dodge does to a hunter (the reactive "defense" state with its timer) plus the end of the Mongoose Bite cooldown category: the Bite of this build is usable at any time, and the cooldown
// is the only thing the proc can lift.
class spell_blessings_survival_expose_prey : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_expose_prey);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        if (spell && (spell->SpellFamilyFlags[0] & FLAGS0_RAPTOR_OR_MONGOOSE) && !(spell->SpellFamilyFlags[2] & FLAGS2_RAPTOR_STRIKE))
            return false;

        Unit* victim = eventInfo.GetActionTarget();
        return victim && victim->GetAuraEffect(SPELL_AURA_MOD_STALKED, SPELLFAMILY_HUNTER, FLAGS0_HUNTERS_MARK, 0, 0, GetTarget()->GetGUID());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        caster->ModifyAuraState(AURA_STATE_DEFENSE, true);
        caster->StartReactiveTimer(REACTIVE_DEFENSE);

        Player* player = caster->ToPlayer();
        SpellInfo const* bite = sSpellMgr->GetSpellInfo(SPELL_MONGOOSE_BITE);
        if (player && bite && bite->GetCategory())
            player->RemoveCategoryCooldown(bite->GetCategory());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_survival_expose_prey::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_expose_prey::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Predator's Edge (Mastery): effect 0 (on apply) and the 1 sec tick (effect 1) keep exactly one stance on the hunter: the dual wield one with a weapon in each hand, the two-handed one with a
// two-handed weapon. The numbers are the stances' spell data.
class spell_blessings_survival_predators_edge : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_predators_edge);

    static void SyncEdge(Unit* unit, bool alive)
    {
        SyncStance(unit, SPELL_PREDATORS_DUAL, alive && IsDualWielding(unit));
        SyncStance(unit, SPELL_PREDATORS_TWO_HANDED, alive && IsTwoHanded(unit));
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        SyncEdge(GetTarget(), true);
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        SyncEdge(target, target->IsAlive());
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_PREDATORS_DUAL);
        GetTarget()->RemoveAurasDueToSpell(SPELL_PREDATORS_TWO_HANDED);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_survival_predators_edge::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_survival_predators_edge::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_survival_predators_edge::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Lone Wolf (Boon of Survival and of Marksmanship, one spell): once a second (and at once when the Boon is learned) the +20% damage stance is put on the hunter while he has no living pet and taken
// off otherwise. Only the Boon's own aura runs this, so a hunter who did not choose it never gets the stance.
class spell_blessings_survival_lone_wolf : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_lone_wolf);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, SPELL_LONE_WOLF_STANCE, !HasLivingPet(target));
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        SyncStance(target, SPELL_LONE_WOLF_STANCE, target->IsAlive() && !HasLivingPet(target));
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_LONE_WOLF_STANCE);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_survival_lone_wolf::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_survival_lone_wolf::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_survival_lone_wolf::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Flanking Strike (the castable, learned with the two-handed Boon of the same name): it can only be cast with a two-handed weapon in the main hand and a living pet (CheckCast). The hunter's hit is the spell's own weapon damage effect. Each hit gives the hunter one stack of the Flanked buff; when the cast ends the hunter's pet, if it is
// alive and within FLANKING_PET_RANGE yards of the target, strikes it as well (the same moment: the cast is instant).
class spell_blessings_survival_flanking_strike : public SpellScript
{
    PrepareSpellScript(spell_blessings_survival_flanking_strike);

    SpellCastResult CheckCast()
    {
        Unit* caster = GetCaster();
        if (!IsTwoHanded(caster))
            return SPELL_FAILED_EQUIPPED_ITEM_CLASS_MAINHAND;

        if (!HasLivingPet(caster))
            return SPELL_FAILED_NO_PET;

        return SPELL_CAST_OK;
    }

    void HandleHit()
    {
        Unit* caster = GetCaster();
        if (caster && GetHitUnit())
            caster->CastSpell(caster, SPELL_FLANKING_BUFF, true);
    }

    void HandleAfterCast()
    {
        Player* player = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        Unit* target = GetExplTargetUnit();
        if (!player || !target || !target->IsAlive())
            return;

        Pet* pet = player->GetPet();
        if (pet && pet->IsAlive() && pet->IsWithinDist(target, FLANKING_PET_RANGE))
            pet->CastSpell(target, SPELL_FLANKING_PET_HIT, true);
    }

    void Register() override
    {
        OnCheckCast += SpellCheckCastFn(spell_blessings_survival_flanking_strike::CheckCast);
        OnHit += SpellHitFn(spell_blessings_survival_flanking_strike::HandleHit);
        AfterCast += SpellCastFn(spell_blessings_survival_flanking_strike::HandleAfterCast);
    }
};

// Flanking Strike, the reset: the Raptor Strike proc (chance 50% in the spell_proc row) clears the cooldown of the castable. It is learned with the Boon.
class spell_blessings_survival_flanking_reset : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_flanking_reset);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        if (Player* player = GetTarget()->ToPlayer())
            player->RemoveSpellCooldown(SPELL_FLANKING_STRIKE, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_flanking_reset::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Cobra Slayer: the spell_proc row lets every melee hit, critical hit and dodge through; the roll is here. Mongoose Bite itself is excluded (its class bit 0x2 without Raptor Strike's bit of word 2).
// The chance starts at effect 0 (10%) and rises by effect 1 (10%) with every attack that does not trigger it; a dodged attack always triggers. The count lives in this aura instance (it is
// lost when the aura is removed). The free Bite is the top rank of Mongoose Bite with its damage raised by effect 2 (45%) of the hunter's attack power; it is a triggered cast, so it starts no cooldown.
class spell_blessings_survival_cobra_slayer : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_cobra_slayer);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        if (spell && (spell->SpellFamilyFlags[0] & FLAGS0_RAPTOR_OR_MONGOOSE) && !(spell->SpellFamilyFlags[2] & FLAGS2_RAPTOR_STRIKE))
            return false;

        Unit* victim = eventInfo.GetActionTarget();
        return victim && victim->IsAlive();
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        AuraEffect const* step = GetEffect(EFFECT_1);
        AuraEffect const* bonus = GetEffect(EFFECT_2);
        SpellInfo const* bite = sSpellMgr->GetSpellInfo(SPELL_MONGOOSE_BITE);
        if (!victim || !step || !bonus || !bite)
            return;

        bool const dodged = (eventInfo.GetHitMask() & PROC_HIT_DODGE) != 0;
        int32 const chance = std::min<int32>(100, aurEff->GetAmount() + int32(_misses) * step->GetAmount());
        if (!dodged && !roll_chance_i(chance))
        {
            ++_misses;
            return;
        }

        _misses = 0;
        int32 const damage = bite->Effects[EFFECT_0].CalcValue(caster) + int32(CalculatePct(caster->GetTotalAttackPowerValue(BASE_ATTACK), bonus->GetAmount()));
        caster->CastCustomSpell(SPELL_MONGOOSE_BITE, SPELLVALUE_BASE_POINT0, damage - 1, victim, true, nullptr, aurEff);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_survival_cobra_slayer::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_cobra_slayer::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }

    uint32 _misses = 0;
};

// Twin Rhythm (Boon, dual wield option): effect 0 (dummy) is the proc of the hunter's own melee critical strikes and the apply hook, effect 1 a 1 sec tick. While a weapon is wielded in each hand a
// melee crit of the hunter gives the pet the stacking crit chance buff (the pet casts it on itself). The tick keeps the hidden listener aura (SPELL_TWIN_RHYTHM_LISTENER) on the living pet; the
// listener hears the pet's crits. Nothing is stored in the script: the pet is looked up from the player each time.
class spell_blessings_survival_twin_rhythm : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_twin_rhythm);

    static void SyncListener(Unit* hunter)
    {
        Player* player = hunter->ToPlayer();
        Pet* pet = player ? player->GetPet() : nullptr;
        if (pet && pet->IsAlive() && !pet->HasAura(SPELL_TWIN_RHYTHM_LISTENER))
            pet->AddAura(SPELL_TWIN_RHYTHM_LISTENER, pet);
    }

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        SyncListener(GetTarget());
    }

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        if (target->IsAlive())
            SyncListener(target);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_TWIN_RHYTHM_HUNTER);
        Player* player = target->ToPlayer();
        if (Pet* pet = player ? player->GetPet() : nullptr)
        {
            pet->RemoveAurasDueToSpell(SPELL_TWIN_RHYTHM_LISTENER);
            pet->RemoveAurasDueToSpell(SPELL_TWIN_RHYTHM_PET);
        }
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* hunter = GetTarget();
        Player* player = hunter->ToPlayer();
        Pet* pet = player ? player->GetPet() : nullptr;
        if (!pet || !pet->IsAlive() || !IsDualWielding(hunter))
            return;

        pet->CastSpell(pet, SPELL_TWIN_RHYTHM_PET, true);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_survival_twin_rhythm::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_survival_twin_rhythm::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_survival_twin_rhythm::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_twin_rhythm::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Twin Rhythm, the listener on the pet: only the pet's melee critical strikes reach here (spell_proc). The master must still have the Boon and wield a weapon in each hand; then he gains the stacking
// crit chance buff (he casts it on himself). The aura belongs to the pet, so it is inert for a pet whose master has dropped the Boon (the Boon script removes it, too).
class spell_blessings_survival_twin_rhythm_listener : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_twin_rhythm_listener);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* owner = GetTarget()->GetOwner();
        if (!owner || !owner->IsPlayer() || !owner->IsAlive() || !owner->HasAura(SPELL_TWIN_RHYTHM) || !IsDualWielding(owner))
            return;

        owner->CastSpell(owner, SPELL_TWIN_RHYTHM_HUNTER, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_twin_rhythm_listener::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Hunter's Cadence (Boon): effect 0 holds the chance (25%) that a Raptor Strike readies Mongoose Bite. The spell_proc mask matches both strikes (Raptor Strike carries bit 0x10000 of the third class mask
// word): a Mongoose Bite puts the buff on the hunter (a modifier of +30% damage on Raptor Strike alone), a Raptor Strike uses it up and may clear the Mongoose Bite cooldown category, as Expose Prey does.
class spell_blessings_survival_hunters_cadence : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_hunters_cadence);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        SpellInfo const* spell = eventInfo.GetSpellInfo();
        if (!spell)
            return;

        Unit* caster = GetTarget();
        if (!(spell->SpellFamilyFlags[2] & FLAGS2_RAPTOR_STRIKE))
        {
            caster->CastSpell(caster, SPELL_CADENCE_BUFF, true);
            return;
        }

        caster->RemoveAurasDueToSpell(SPELL_CADENCE_BUFF);
        if (!roll_chance_i(aurEff->GetAmount()))
            return;

        Player* player = caster->ToPlayer();
        SpellInfo const* bite = sSpellMgr->GetSpellInfo(SPELL_MONGOOSE_BITE);
        if (player && bite && bite->GetCategory())
            player->RemoveCategoryCooldown(bite->GetCategory());
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_hunters_cadence::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Stalker's Stride (Boon): effect 0 is the cooldown modifier of Strider Kick (spell data); effect 1 (dummy) is the proc of a Strider Kick hit, which slows the target (the hunter casts the snare on it,
// so it counts as the hunter's own slow).
class spell_blessings_survival_stalkers_stride : public AuraScript
{
    PrepareAuraScript(spell_blessings_survival_stalkers_stride);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* victim = eventInfo.GetActionTarget();
        if (victim && victim->IsAlive())
            GetTarget()->CastSpell(victim, SPELL_STRIDE_SLOW, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_survival_stalkers_stride::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
    }
};

// The damage hooks of two Favors and of the keystone Hunter's Quarry. All are cheap when the attacker is not a hunter (or hunter pet) with the effect: a type test and one aura lookup.
//  * Greatspear: the stance's strike percent (effect 1) is also added to the melee auto attacks (the spell modifiers of the stance cover the abilities).
//  * Snaremaster: Raptor Strike, Mongoose Bite, Counterattack, Flanking Strike and the melee auto attacks deal the Favor's percent (effect 1) more to an enemy the hunter has slowed or rooted.
//  * Hunter's Quarry: the same strikes of the hunter, and every melee attack of his pet, deal the Boon's percent (effect 0) more to the enemy that carries the hunter's OWN Hunter's Mark.
class MoonlitSurvivalDamageScript : public UnitScript
{
public:
    MoonlitSurvivalDamageScript() : UnitScript("MoonlitSurvivalDamageScript", true, { UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN }) { }

    // The extra percent of Hunter's Quarry that `attacker` (the hunter, or his pet) deals to `target`: 0 without the Boon or without the hunter's own Hunter's Mark on the target.
    static int32 QuarryBonus(Unit* target, Unit* attacker)
    {
        Unit* master = attacker->IsPlayer() ? attacker : (attacker->IsPet() ? attacker->GetOwner() : nullptr);
        if (!master || !master->IsPlayer())
            return 0;

        AuraEffect const* quarry = master->GetAuraEffect(SPELL_HUNTERS_QUARRY, EFFECT_0);
        if (!quarry || quarry->GetAmount() <= 0)
            return 0;

        return target->GetAuraEffect(SPELL_AURA_MOD_STALKED, SPELLFAMILY_HUNTER, FLAGS0_HUNTERS_MARK, 0, 0, master->GetGUID()) ? quarry->GetAmount() : 0;
    }

    void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& damage) override
    {
        if (!target || !attacker || !damage || (!attacker->IsPlayer() && !attacker->IsPet()))
            return;

        if (attacker->IsPlayer())
        {
            if (AuraEffect const* spear = attacker->GetAuraEffect(SPELL_GREATSPEAR_STANCE, EFFECT_1))
                if (spear->GetAmount() > 0)
                    AddPct(damage, spear->GetAmount());

            if (AuraEffect const* favor = attacker->GetAuraEffect(SPELL_FAVOR_SNAREMASTER, EFFECT_1))
                if (favor->GetAmount() > 0 && IsSnaredBy(target, attacker))
                    AddPct(damage, favor->GetAmount());
        }

        if (int32 const bonus = QuarryBonus(target, attacker))
            AddPct(damage, bonus);
    }

    void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!target || !attacker || damage <= 0 || !spellInfo)
            return;

        if (attacker->IsPet())
        {
            // the melee abilities of the pet (Claw, Bite, Smack and the like)
            if (spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MELEE)
                if (int32 const bonus = QuarryBonus(target, attacker))
                    AddPct(damage, bonus);

            return;
        }

        if (!attacker->IsPlayer() || spellInfo->SpellFamilyName != SPELLFAMILY_HUNTER)
            return;

        if (!(spellInfo->SpellFamilyFlags[0] & FLAGS0_RAPTOR_OR_MONGOOSE) && !(spellInfo->SpellFamilyFlags[1] & FLAGS1_COUNTERATTACK) && spellInfo->Id != SPELL_FLANKING_STRIKE)
            return;

        if (AuraEffect const* favor = attacker->GetAuraEffect(SPELL_FAVOR_SNAREMASTER, EFFECT_1))
            if (favor->GetAmount() > 0 && IsSnaredBy(target, attacker))
                AddPct(damage, favor->GetAmount());

        if (int32 const bonus = QuarryBonus(target, attacker))
            AddPct(damage, bonus);
    }
};

void AddMoonlitBlessingsSurvivalScripts()
{
    RegisterSpellScript(spell_blessings_survival_skewer);
    RegisterSpellScript(spell_blessings_survival_skewer_barbs);
    RegisterSpellScript(spell_blessings_survival_trappers_gambit);
    RegisterSpellScript(spell_blessings_survival_greatspear);
    RegisterSpellScript(spell_blessings_survival_twin_talons);
    RegisterSpellScript(spell_blessings_survival_lacerating_strikes);
    RegisterSpellScript(spell_blessings_survival_expose_prey);
    RegisterSpellScript(spell_blessings_survival_predators_edge);
    RegisterSpellScript(spell_blessings_survival_lone_wolf);
    RegisterSpellScript(spell_blessings_survival_flanking_strike);
    RegisterSpellScript(spell_blessings_survival_flanking_reset);
    RegisterSpellScript(spell_blessings_survival_cobra_slayer);
    RegisterSpellScript(spell_blessings_survival_twin_rhythm);
    RegisterSpellScript(spell_blessings_survival_twin_rhythm_listener);
    RegisterSpellScript(spell_blessings_survival_hunters_cadence);
    RegisterSpellScript(spell_blessings_survival_stalkers_stride);
    new MoonlitSurvivalDamageScript();
}
