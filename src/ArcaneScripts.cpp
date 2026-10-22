// Moonlit Blessings: C++ spell scripts for the Arcane Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/arcane.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_arcane_overcharge      Favor of the Overcharge: builds the Overcharge stacks, unloads them into the next damaging spell, and carries the parameters of
//                                          Charged Rounds, Discharge Echo and Arcane Cascade (hidden passives that only hold numbers)
//   spell_blessings_arcane_unloaded        the "Overcharged" bonus (a plain spell modifier): tells the Favor's script when it has ended
//   spell_blessings_arcane_mana_overflow   Boon: mana from the Overcharge stacks every 3 sec
//   spell_blessings_arcane_volley          Boon: each Arcane Missiles missile may refund the mana cost of the cast
//   spell_blessings_arcane_tempest         Boon: every third missile also hits a second enemy
//   spell_blessings_arcane_singularity     Boon: critical strikes extend Arcane Power
//   spell_blessings_arcane_power_cost      bound to Arcane Power (12042): Mana Singularity removes its extra mana cost
//   spell_blessings_arcane_missile_storm   Favor of the Missile Storm: teaches the copy of Arcane Missiles (no move interrupt) of the player's rank, swaps it on level-up, removes it on drop
//   MoonlitArcaneStormPlayerScript         the level-up half of the Favor of the Missile Storm
//
// Spell ids come from the Arcane registry block 1,410,000; tools/talents/recipes/arcane.py asserts every unit named here (IDS). ~Moonlit Nights Team

#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include <algorithm>
#include <list>

namespace
{
    enum ArcaneSpells : uint32
    {
        SPELL_DISCHARGE_ECHO       = 1410040,   // Boon: effect 0 = percent of the unloading spell's damage repeated, effect 1 = seconds of delay
        SPELL_DISCHARGE_ECHO_HIT   = 1410041,   // the echo (a fixed amount, set per cast)
        SPELL_ARCANE_CASCADE       = 1410050,   // Boon: effect 0 = the stack cap and unload point, effect 1 = percent of maximum mana per stack unloaded
        SPELL_MANA_SINGULARITY     = 1410060,
        SPELL_TEMPEST_STRIKE       = 1410071,   // the second hit of Missile Tempest (a fixed amount, set per cast)
        SPELL_CHARGED_ROUNDS       = 1410000,   // Boon: effect 0 = chance in percent, effect 1 = stacks on a charged cast, effect 2 = extra seconds of duration
        SPELL_FAVOR_OVERCHARGE     = 1410100,
        SPELL_OVERCHARGE           = 1410101,   // the stack buff
        SPELL_OVERCHARGED          = 1410102,   // the unload bonus: a direct-damage spell modifier on every Mage spell (effect 0 = damage percent), ended by the Favor's script

        SPELL_FAVOR_MISSILE_STORM  = 1410090,   // Favor of the Missile Storm; 1410091 .. 1410098 are its copies of Arcane Missiles rank 1 .. 8 (no move interrupt)

        // stock spell
        SPELL_ARCANE_POWER         = 12042
    };

    constexpr uint32 STORM_RANKS[8] = { 1410091, 1410092, 1410093, 1410094, 1410095, 1410096, 1410097, 1410098 };    // Arcane Missiles rank 1 .. 8; their spell level is the stock rank's
    // every rank of the spell book's Arcane Missiles (the Lua system removes them while the Favor is chosen); the action bars are moved from them to the copy
    constexpr uint32 STOCK_MISSILES[13] = { 5143, 5144, 5145, 8416, 8417, 10211, 10212, 25345, 27075, 38699, 38704, 42843, 42846 };

    constexpr int32 OVERCHARGE_BASE_CAP = 10;                 // the Favor unloads at this many stacks (Arcane Cascade raises it)
    constexpr int32 OVERCHARGE_BASE_SECONDS = 10;             // how long the stacks last, refreshed by every stack gained
    constexpr int32 UNLOAD_PERCENT_PER_STACK = 3;             // damage added to the unloading spell per stack
    constexpr uint32 FLAG_ARCANE_MISSILES = 0x00000800;       // SpellFamilyFlags[0] of the Arcane Missiles channel (every rank)
    constexpr int32 CHANNEL_GRACE_MS = 400;                   // the bonus outlives a channel by this much so that the last missile is still raised

    // The amount of effect `effect` of the passive `spellId` on `unit`; 0 when the unit does not have it.
    int32 Param(Unit const* unit, uint32 spellId, uint8 effect = 0)
    {
        AuraEffect const* aurEff = unit ? unit->GetAuraEffect(spellId, effect) : nullptr;
        return aurEff ? aurEff->GetAmount() : 0;
    }

    // A Mage spell that deals damage: a direct hit, a damage-over-time aura, or a channel whose pulses hit (Arcane Missiles).
    bool IsDamagingMageSpell(SpellInfo const* info)
    {
        if (!info || info->SpellFamilyName != SPELLFAMILY_MAGE)
            return false;

        for (SpellEffectInfo const& effect : info->Effects)
        {
            if (effect.Effect == SPELL_EFFECT_SCHOOL_DAMAGE || effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE)
                return true;

            if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_TRIGGER_SPELL)
            {
                SpellInfo const* pulse = sSpellMgr->GetSpellInfo(effect.TriggerSpell);
                if (pulse && pulse->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE))
                    return true;
            }
        }

        return false;
    }

    // Favor of the Missile Storm: the player keeps exactly one copy of Arcane Missiles, the highest rank whose spell level the player has reached (the lowest below rank 1).
    // Action bar buttons that held the spell book's ranks or another copy are moved to it, so the bar survives the swap. No-op when nothing changes.
    void SyncMissileStormRank(Player* player)
    {
        uint32 wanted = STORM_RANKS[0];
        for (uint32 const rank : STORM_RANKS)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(rank);
            if (info && info->SpellLevel <= player->GetLevel())
                wanted = rank;
        }

        bool changed = false;
        if (!player->HasSpell(wanted))
        {
            player->learnSpell(wanted);
            changed = true;
        }

        for (uint8 slot = 0; slot < MAX_ACTION_BUTTONS; ++slot)
        {
            ActionButton const* button = player->GetActionButton(slot);
            if (!button || button->GetType() != ACTION_BUTTON_SPELL || button->GetAction() == wanted)
                continue;

            uint32 const action = button->GetAction();
            bool old = std::find(std::begin(STORM_RANKS), std::end(STORM_RANKS), action) != std::end(STORM_RANKS)
                || std::find(std::begin(STOCK_MISSILES), std::end(STOCK_MISSILES), action) != std::end(STOCK_MISSILES);
            if (old && player->addActionButton(slot, wanted, ACTION_BUTTON_SPELL))
                changed = true;
        }

        for (uint32 const rank : STORM_RANKS)
            if (rank != wanted && player->HasSpell(rank))
                player->removeSpell(rank, SPEC_MASK_ALL, false);

        if (changed)
            player->SendInitialActionButtons();
    }

    // The nearest enemy of `seeker` within `range` yards of `center` other than `center` itself that `center` can see; null when there is none.
    Unit* NearestOtherEnemy(Unit* seeker, Unit* center, float range)
    {
        std::list<Unit*> targets;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(center, seeker, range);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(center, targets, check);
        Cell::VisitObjects(center, searcher, range);

        Unit* best = nullptr;
        float bestDistance = range + 1.0f;
        for (Unit* unit : targets)
        {
            if (unit == center || !seeker->IsValidAttackTarget(unit) || !center->IsWithinLOSInMap(unit))
                continue;

            float const distance = center->GetDistance(unit);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = unit;
            }
        }

        return best;
    }
}

// Favor of the Overcharge. The passive procs on the CAST of every damaging spell the player starts (once per cast), on every damage HIT (also the missile ticks, which are triggered
// spells: the passive carries SPELL_ATTR3_CAN_PROC_FROM_PROCS) and on the FINISH of each spell (once per spell, after every target was hit; for a channel that is when it begins).
// The cast half builds and unloads the stacks; the hit half adds up the damage of the unloading spell for Discharge Echo; the finish half ends the Overcharged bonus.
// The core's own "charge" of a spell modifier is not used for the bonus: every missile of a channel is a spell of its own and would use the charge up on the first one.
class spell_blessings_arcane_overcharge : public AuraScript
{
    PrepareAuraScript(spell_blessings_arcane_overcharge);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_OVERCHARGE, SPELL_OVERCHARGED, SPELL_DISCHARGE_ECHO_HIT });
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        Spell const* spell = eventInfo.GetProcSpell();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!player || !spell || !info)
            return;

        if (eventInfo.GetSpellPhaseMask() & PROC_SPELL_PHASE_HIT)
        {
            AddUnloadDamage(eventInfo, spell, info);
            return;
        }

        if (eventInfo.GetSpellPhaseMask() & PROC_SPELL_PHASE_FINISH)
        {
            FinishUnload(player, spell, info);
            return;
        }

        // the cast of a damaging Mage spell that the player started (not a triggered pulse, helper or proc)
        if (spell->IsTriggered() || !IsDamagingMageSpell(info))
            return;

        if (player->HasAura(SPELL_OVERCHARGED))
        {
            // the unloading spell is still being cast (or flying): nothing builds or unloads until it has ended
            if (info->Id == _unloadSpellId || !_unloadChannel)
                return;

            // another spell after an interrupted channel: what is left of the bonus ends here, then this cast is an ordinary one
            player->RemoveAurasDueToSpell(SPELL_OVERCHARGED);
        }

        Aura* stacks = player->GetAura(SPELL_OVERCHARGE);
        int32 const cap = StackCap(player);
        if (stacks && int32(stacks->GetStackAmount()) >= cap)
        {
            ReleaseStacks(player, int32(stacks->GetStackAmount()), info);
            return;
        }

        AddStacks(player, stacks, cap);
    }

public:
    // The Overcharged bonus has ended (its charge was used by the spell, or it ran out): the unloading spell is over. Repeat part of its damage for Discharge Echo.
    void UnloadEnded()
    {
        uint32 const damage = _unloadDamage;
        ObjectGuid const victim = _unloadVictim;
        _unloadSpellId = 0;
        _unloadChannel = false;
        _unloadDamage = 0;
        _unloadVictim.Clear();

        Unit* caster = GetTarget();
        int32 const percent = Param(caster, SPELL_DISCHARGE_ECHO, EFFECT_0);
        if (!caster || !damage || victim.IsEmpty() || percent <= 0)
            return;

        int32 const amount = int32(CalculatePct(damage, percent));
        int32 const delay = std::max<int32>(Param(caster, SPELL_DISCHARGE_ECHO, EFFECT_1), 0) * IN_MILLISECONDS;
        if (amount <= 0)
            return;

        ObjectGuid const casterGuid = caster->GetGUID();
        caster->m_Events.AddEventAtOffset([casterGuid, victim, amount]()
        {
            Player* player = ObjectAccessor::FindPlayer(casterGuid);
            if (!player || !player->IsAlive())
                return;

            Unit* target = ObjectAccessor::GetUnit(*player, victim);
            if (!target || !target->IsAlive())
                return;

            int32 const baseDamage = amount - 1;     // the amount of a one-value effect is its base points plus one
            player->CastCustomSpell(target, SPELL_DISCHARGE_ECHO_HIT, &baseDamage, nullptr, nullptr, true);
        }, Milliseconds(delay));
    }

private:
    // The Favor is switched off (Lua) or lost: the stacks and a bonus that was waiting go with it.
    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Unit* target = GetTarget();
        target->RemoveAurasDueToSpell(SPELL_OVERCHARGE);
        target->RemoveAurasDueToSpell(SPELL_OVERCHARGED);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_arcane_overcharge::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_arcane_overcharge::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }

    // The stack count at which the stacks unload: 10, or Arcane Cascade's value (15) when the player has it.
    static int32 StackCap(Unit const* player)
    {
        return std::max(OVERCHARGE_BASE_CAP, Param(player, SPELL_ARCANE_CASCADE, EFFECT_0));
    }

    // One stack, or the number Charged Rounds gives on its chance; every stack refreshes the duration (Charged Rounds lengthens it).
    void AddStacks(Player* player, Aura* stacks, int32 cap)
    {
        int32 add = 1;
        if (int32 const chance = Param(player, SPELL_CHARGED_ROUNDS, EFFECT_0))
            if (roll_chance_i(chance))
                add = std::max(add, Param(player, SPELL_CHARGED_ROUNDS, EFFECT_1));

        if (!stacks)
        {
            player->CastSpell(player, SPELL_OVERCHARGE, true);
            stacks = player->GetAura(SPELL_OVERCHARGE);
            --add;
        }

        if (!stacks)
            return;

        if (add > 0)
            stacks->ModStackAmount(add);

        if (int32(stacks->GetStackAmount()) > cap)
            stacks->SetStackAmount(uint8(cap));

        int32 const seconds = OVERCHARGE_BASE_SECONDS + std::max(Param(player, SPELL_CHARGED_ROUNDS, EFFECT_2), 0);
        stacks->SetMaxDuration(seconds * IN_MILLISECONDS);
        stacks->SetDuration(seconds * IN_MILLISECONDS);
    }

    // All the stacks are spent: this cast (and, for a channel, all of it) deals 3% per stack more damage. Arcane Cascade restores mana per stack.
    void ReleaseStacks(Player* player, int32 stackCount, SpellInfo const* info)
    {
        int32 const baseBonus = stackCount * UNLOAD_PERCENT_PER_STACK - 1;      // the amount of a one-value effect is its base points plus one
        player->CastCustomSpell(player, SPELL_OVERCHARGED, &baseBonus, nullptr, nullptr, true);
        player->RemoveAurasDueToSpell(SPELL_OVERCHARGE);

        if (int32 const percentPerStack = Param(player, SPELL_ARCANE_CASCADE, EFFECT_1))
        {
            int32 const mana = CalculatePct(int32(player->GetMaxPower(POWER_MANA)), percentPerStack * stackCount);
            if (mana > 0)
                player->EnergizeBySpell(player, SPELL_ARCANE_CASCADE, uint32(mana), POWER_MANA);
        }

        // this spell is the unloading one until its FINISH ends the bonus (Discharge Echo adds up its damage, and that of its pulses, meanwhile)
        _unloadSpellId = info->Id;
        _unloadChannel = false;
        _unloadDamage = 0;
        _unloadVictim.Clear();
    }

    // The unloading spell has finished: a normal spell ends the bonus now (every target was hit); a channel (its finish is its start) keeps it until the channel is over.
    void FinishUnload(Player* player, Spell const* spell, SpellInfo const* info)
    {
        if (!_unloadSpellId || spell->IsTriggered() || info->Id != _unloadSpellId || !player->HasAura(SPELL_OVERCHARGED))
            return;

        if (!info->IsChanneled())
        {
            player->RemoveAurasDueToSpell(SPELL_OVERCHARGED);
            return;
        }

        Spell* channel = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);     // the proc hands out a const Spell; the running channel is the same spell
        int32 const remaining = channel && channel->GetSpellInfo() == info ? std::max(channel->GetCastTimeRemaining(), 0) : 0;
        if (Aura* bonus = player->GetAura(SPELL_OVERCHARGED))
        {
            _unloadChannel = true;
            bonus->SetMaxDuration(remaining + CHANNEL_GRACE_MS);
            bonus->SetDuration(remaining + CHANNEL_GRACE_MS);
        }
    }

    // A damage hit of the unloading spell itself, or of a pulse of it: add it up for Discharge Echo.
    void AddUnloadDamage(ProcEventInfo& eventInfo, Spell const* spell, SpellInfo const* info)
    {
        if (!_unloadSpellId || Param(GetTarget(), SPELL_DISCHARGE_ECHO, EFFECT_0) <= 0)
            return;

        SpellInfo const* parent = spell->GetTriggeredByAuraSpellInfo();
        if (info->Id != _unloadSpellId && (!parent || parent->Id != _unloadSpellId))
            return;

        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        _unloadDamage += damageInfo->GetDamage();
        if (_unloadVictim.IsEmpty())
            if (Unit* victim = eventInfo.GetActionTarget())
                _unloadVictim = victim->GetGUID();
    }

    uint32 _unloadSpellId = 0;
    bool _unloadChannel = false;
    uint32 _unloadDamage = 0;
    ObjectGuid _unloadVictim;
};

// Overcharged: the spell modifier. Whatever removes it (the Favor's script at the end of the unloading spell, its short timer after a channel, a later cast, or the Favor itself)
// is the end of the unload: the Favor's script is told so that Discharge Echo can fire. Nothing else happens here; the Favor's script owns the numbers.
class spell_blessings_arcane_unloaded : public AuraScript
{
    PrepareAuraScript(spell_blessings_arcane_unloaded);

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Aura* favor = GetTarget()->GetAura(SPELL_FAVOR_OVERCHARGE);
        if (!favor)
            return;

        if (spell_blessings_arcane_overcharge* script = favor->GetScript<spell_blessings_arcane_overcharge>("spell_blessings_arcane_overcharge"))
            script->UnloadEnded();
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_arcane_unloaded::HandleRemove, EFFECT_0, SPELL_AURA_ADD_PCT_MODIFIER, AURA_EFFECT_HANDLE_REAL);
    }
};

// Mana Overflow: effect 0 is a periodic dummy (every 3 sec, amount = percent of maximum mana), effect 1 holds the stack count that is needed. The Overcharge buff only exists
// while the player has Favor of the Overcharge.
class spell_blessings_arcane_mana_overflow : public AuraScript
{
    PrepareAuraScript(spell_blessings_arcane_mana_overflow);

    void HandlePeriodic(AuraEffect const* aurEff)
    {
        Unit* target = GetTarget();
        AuraEffect const* needed = GetEffect(EFFECT_1);
        if (!target->IsAlive() || !needed)
            return;

        Aura const* stacks = target->GetAura(SPELL_OVERCHARGE);
        if (!stacks || int32(stacks->GetStackAmount()) < needed->GetAmount())
            return;

        int32 const mana = CalculatePct(int32(target->GetMaxPower(POWER_MANA)), aurEff->GetAmount());
        if (mana > 0)
            target->EnergizeBySpell(target, GetId(), uint32(mana), POWER_MANA);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_arcane_mana_overflow::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Arcane Volley: the proc row selects the Arcane Missiles spells by class mask (the pulse and the channel share the family name); only the pulse (it deals the damage) may refund,
// and the chance (4%) is the spell_proc chance. The refund is what the channel being cast cost: the channel's own paid cost, so a free cast (Clearcasting) refunds nothing.
class spell_blessings_arcane_volley : public AuraScript
{
    PrepareAuraScript(spell_blessings_arcane_volley);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        return info && info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        Player* player = GetTarget()->ToPlayer();
        Spell const* channel = player ? player->GetCurrentSpell(CURRENT_CHANNELED_SPELL) : nullptr;
        if (!channel)
            return;

        SpellInfo const* info = channel->GetSpellInfo();
        if (info->SpellFamilyName != SPELLFAMILY_MAGE || !(info->SpellFamilyFlags[0] & FLAG_ARCANE_MISSILES))
            return;

        int32 const cost = channel->GetPowerCost();
        if (cost > 0 && info->PowerType == POWER_MANA)
            player->EnergizeBySpell(player, GetId(), uint32(cost), POWER_MANA);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_arcane_volley::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_arcane_volley::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Missile Tempest: effect 0 holds "every Nth missile" (amount) and the search radius in yards (misc value). The pulses of the channel carry their tick number; every Nth pulse
// repeats its damage on the nearest other enemy around the target. The second hit is a helper that ignores the caster's modifiers (the damage already contains them) and cannot crit.
class spell_blessings_arcane_tempest : public AuraScript
{
    PrepareAuraScript(spell_blessings_arcane_tempest);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        SpellInfo const* info = eventInfo.GetSpellInfo();
        Spell const* spell = eventInfo.GetProcSpell();
        AuraEffect const* every = GetEffect(EFFECT_0);
        if (!info || !spell || !every || every->GetAmount() <= 0 || !info->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE))
            return false;

        uint32 const tick = spell->GetTriggeredByAuraTickNumber();
        return tick && tick % uint32(every->GetAmount()) == 0;
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        Unit* second = NearestOtherEnemy(caster, victim, float(aurEff->GetMiscValue()));
        if (!second)
            return;

        int32 const baseDamage = int32(damageInfo->GetDamage()) - 1;       // the amount of a one-value effect is its base points plus one
        caster->CastCustomSpell(second, SPELL_TEMPEST_STRIKE, &baseDamage, nullptr, nullptr, true);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_arcane_tempest::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_arcane_tempest::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Mana Singularity, the extension half: effect 0 = seconds added per critical strike, effect 1 = the most seconds it may add to one Arcane Power. What has been added is the
// aura's maximum duration minus the maximum duration a fresh Arcane Power would have now (which already includes Favor of the Mana Surge).
class spell_blessings_arcane_singularity : public AuraScript
{
    PrepareAuraScript(spell_blessings_arcane_singularity);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        return GetTarget()->HasAura(SPELL_ARCANE_POWER, GetTarget()->GetGUID());
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();

        Unit* caster = GetTarget();
        AuraEffect const* cap = GetEffect(EFFECT_1);
        Aura* power = caster->GetAura(SPELL_ARCANE_POWER, caster->GetGUID());
        if (!power || !cap || power->IsPermanent())
            return;

        int32 const extended = power->GetMaxDuration() - power->CalcMaxDuration(caster);
        int32 const add = std::min(aurEff->GetAmount() * int32(IN_MILLISECONDS), cap->GetAmount() * int32(IN_MILLISECONDS) - extended);
        if (add <= 0)
            return;

        power->SetMaxDuration(power->GetMaxDuration() + add);
        power->SetDuration(power->GetDuration() + add);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_arcane_singularity::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_blessings_arcane_singularity::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Mana Singularity, the cost half: bound to Arcane Power itself. Its second effect is the "spells cost 20% more mana" modifier; with the Boon its amount is 0. Done as a script
// (not as a spell modifier) so that it cannot add up with Power Reserves into a negative amount.
class spell_blessings_arcane_power_cost : public AuraScript
{
    PrepareAuraScript(spell_blessings_arcane_power_cost);

    void CalcCost(AuraEffect const* aurEff, int32& amount, bool& /*canBeRecalculated*/)
    {
        if (aurEff->GetMiscValue() != SPELLMOD_COST)
            return;

        Unit* caster = GetCaster();
        if (caster && caster->HasAura(SPELL_MANA_SINGULARITY))
            amount = 0;
    }

    void Register() override
    {
        DoEffectCalcAmount += AuraEffectCalcAmountFn(spell_blessings_arcane_power_cost::CalcCost, EFFECT_1, SPELL_AURA_ADD_PCT_MODIFIER);
    }
};

// Favor of the Missile Storm: the copies of Arcane Missiles have no move interrupt (recipes/arcane.py). The Favor teaches the copy of the player's rank when it is applied (login included:
// the learned spell is saved with the character, so this is a no-op then) and takes them away when it is dropped, but not when the player merely logs out. The level-up half is
// MoonlitArcaneStormPlayerScript. The Lua system removes the spell book's ranks (blessing_spells.FAVOR_REMOVES).
class spell_blessings_arcane_missile_storm : public AuraScript
{
    PrepareAuraScript(spell_blessings_arcane_missile_storm);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            SyncMissileStormRank(player);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        for (uint32 const rank : STORM_RANKS)
            player->removeSpell(rank, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_arcane_missile_storm::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_arcane_missile_storm::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Level-up half of the Favor of the Missile Storm: a new rank of Arcane Missiles replaces the copy the player holds.
class MoonlitArcaneStormPlayerScript : public PlayerScript
{
public:
    MoonlitArcaneStormPlayerScript() : PlayerScript("MoonlitArcaneStormPlayerScript", { PLAYERHOOK_ON_LEVEL_CHANGED }) { }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (player && player->getClass() == CLASS_MAGE && player->HasAura(SPELL_FAVOR_MISSILE_STORM))
            SyncMissileStormRank(player);
    }
};

void AddMoonlitBlessingsArcaneScripts()
{
    RegisterSpellScript(spell_blessings_arcane_overcharge);
    RegisterSpellScript(spell_blessings_arcane_unloaded);
    RegisterSpellScript(spell_blessings_arcane_mana_overflow);
    RegisterSpellScript(spell_blessings_arcane_volley);
    RegisterSpellScript(spell_blessings_arcane_tempest);
    RegisterSpellScript(spell_blessings_arcane_singularity);
    RegisterSpellScript(spell_blessings_arcane_power_cost);
    RegisterSpellScript(spell_blessings_arcane_missile_storm);
    new MoonlitArcaneStormPlayerScript();
}
