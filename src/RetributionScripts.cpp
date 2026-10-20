// Moonlit Blessings: C++ spell scripts for the Retribution Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/retribution.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_retribution_twin_judgement        Boon: a Judgement also strikes the nearest other enemy near its target for a share of the damage
//   spell_blessings_retribution_judgement_echo        Boon: a critical Judgement repeats a share of its damage on the same enemy a few seconds later
//   spell_blessings_retribution_rallying_strikes      Boon: Crusader Strike and Divine Storm give you and the nearest group members an attack power buff
//   spell_blessings_retribution_echoing_strike        Boon: a critical Crusader Strike sometimes strikes again at once for a share of its damage
//   spell_blessings_retribution_sealbound_strikes     Boon: while a Seal is on you, a damage buff is kept up and Crusader Strike and Divine Storm return mana
//   spell_blessings_retribution_judgement_day         Boon: a critical Judgement makes Crusader Strike ready at once (the free, cooldown-free buff is spell data)
//   spell_blessings_retribution_sealfire              Boon: every 6 sec the active Seal unleashes the damage of a Judgement at 60%
//   spell_blessings_retribution_standard_of_the_templar   Boon: a raid area aura that reaches only allies that are under one of the Paladin's auras
//   spell_blessings_retribution_twin_seals            Favor of the Twin Seals: teaches Dual Wield and clears the Twin Seal when the Favor is dropped
//   spell_blessings_retribution_twin_seal             the Twin Seal marker: makes the Seal that a new Seal pushed off keep triggering on the Paladin's hits
//   MoonlitBlessingsTwinSealsScript                   the hook that notices a Seal being pushed off by another Seal (sets the marker) and a Seal being cast again (drops a marker that holds it)
//
// Spell ids come from the Retribution registry block 1,530,000 (tools/talents/recipes/retribution.py asserts every unit named here and the stock ids).
// ~Moonlit Nights Team

#include "Pet.h"   // BlessingsHelpers.h needs the full Pet type (it is not included there)
#include "BlessingsHelpers.h"
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
#include "WorldSession.h"
#include <algorithm>
#include <list>
#include <utility>
#include <vector>

namespace
{
    enum RetributionSpells : uint32
    {
        SPELL_TWIN_JUDGEMENT_STRIKE   = 1530041,   // single target holy hit for a fixed amount (the second Judgement)
        SPELL_JUDGEMENT_ECHO_HIT      = 1530051,   // single target holy hit for a fixed amount (the echo)
        SPELL_RALLYING_STRIKES_BUFF   = 1530061,   // buff: attack power +5%
        SPELL_ECHOING_STRIKE_HIT      = 1530071,   // single target holy hit for a fixed amount (the second Crusader Strike)
        SPELL_SEALBOUND_STRIKES_BUFF  = 1530081,   // buff kept up while a Seal is on the Paladin: Crusader Strike and Divine Storm deal more damage
        SPELL_SEALFIRE_CUT            = 1530101,   // 2 sec spell modifier: Judgement and Seal damage -40% (set around the Sealfire cast only)
        SPELL_TWIN_SEALS              = 1530140,   // Favor of the Twin Seals (the hidden passive)
        SPELL_TWIN_SEAL               = 1530141    // the Twin Seal marker: effect 0 holds the spell id of the second Seal
    };

    enum StockSpells : uint32
    {
        SPELL_DUAL_WIELD              = 674,       // Dual Wield Passive: SPELL_EFFECT_DUAL_WIELD (the core drops dual wield from a player who does not know it)
        SPELL_CRUSADER_STRIKE         = 35395,
        SPELL_JUDGEMENT_DAMAGE        = 54158,     // the Judgement damage of a Seal that has no Judgement of its own (Light, Wisdom, Justice)
        SPELL_SEAL_OF_RIGHTEOUSNESS_DAMAGE = 25742,
        SPELL_SEAL_OF_VENGEANCE       = 31801,
        SPELL_SEAL_OF_CORRUPTION      = 53736,
        SPELL_HOLY_VENGEANCE          = 31803,     // the burn of Seal of Vengeance
        SPELL_BLOOD_CORRUPTION        = 53742,     // the burn of Seal of Corruption
        SPELL_SEAL_OF_VENGEANCE_HIT   = 42463,
        SPELL_SEAL_OF_CORRUPTION_HIT  = 53739,
        SPELL_DEVOTION_AURA_R1        = 465,       // first ranks of the seven Paladin auras (the same list the stock Sanctified Retribution uses)
        SPELL_RETRIBUTION_AURA_R1     = 7294,
        SPELL_CONCENTRATION_AURA      = 19746,
        SPELL_SHADOW_RESISTANCE_AURA_R1 = 19876,
        SPELL_FROST_RESISTANCE_AURA_R1  = 19888,
        SPELL_FIRE_RESISTANCE_AURA_R1   = 19891,
        SPELL_CRUSADER_AURA           = 32223
    };

    // SpellFamilyFlags of the Seals (word 0, word 1); the core tests the same bits in SpellInfo::LoadSpellSpecific
    constexpr uint32 FLAGS1_SEAL_OF_RIGHTEOUSNESS = 0x20000000;
    constexpr uint32 FLAGS0_SEAL_OF_COMMAND       = 0x02000000;
    constexpr uint32 FLAGS1_SEAL_OF_VENGEANCE     = 0x00000800;     // Vengeance and Corruption (they are the same seal for the two factions)
    constexpr uint32 FLAGS1_DIVINE_STORM_HAMMER   = 0x00060000;     // Divine Storm and Hammer of the Righteous: Seal of Command does not cleave from them
    constexpr uint32 FLAGS1_SEAL_BURN             = 0x00000800;     // the burn of Vengeance / Corruption (class mask word 1)

    constexpr float SEALFIRE_RANGE = 10.0f;                         // yards within which Sealfire looks for an enemy
    constexpr uint32 RALLYING_ALLIES = 4;                           // group members besides the Paladin that Rallying Strikes reaches

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

    // The nearest enemy to `center`, other than `center` itself.
    Unit* NearestOtherEnemy(Unit* seeker, Unit* center, float range)
    {
        Unit* best = nullptr;
        float bestDistance = range + 1.0f;
        for (Unit* unit : EnemiesAround(seeker, center, range))
        {
            if (unit == center)
                continue;

            float const distance = center->GetDistance(unit);
            if (distance < bestDistance)
            {
                best = unit;
                bestDistance = distance;
            }
        }

        return best;
    }

    // Cast the fixed-damage strike `spellId` on `target` for `percent` of `damage`. The amount of a one-value effect is its base points plus one, hence the minus one.
    void CastShare(Unit* caster, uint32 spellId, Unit* target, uint32 damage, int32 percent, AuraEffect const* triggeredBy)
    {
        int32 const amount = int32(CalculatePct(damage, percent));
        if (amount <= 0 || !target || !target->IsAlive())
            return;

        caster->CastCustomSpell(spellId, SPELLVALUE_BASE_POINT0, amount - 1, target, true, nullptr, triggeredBy);
    }

    // The spell a real Judgement casts for damage: the Seal's own (the value of its third effect, a dummy), else the generic Judgement damage. Same search as spell_pal_judgement.
    uint32 JudgementDamageSpell(Unit const* caster)
    {
        for (AuraEffect const* aurEff : caster->GetAuraEffectsByType(SPELL_AURA_DUMMY))
            if (aurEff->GetEffIndex() == EFFECT_2 && aurEff->GetSpellInfo()->GetSpellSpecific() == SPELL_SPECIFIC_SEAL && sSpellMgr->GetSpellInfo(aurEff->GetAmount()))
                return uint32(aurEff->GetAmount());

        return SPELL_JUDGEMENT_DAMAGE;
    }

    // Two Seals are the same Seal when their class masks are equal (ranks of one Seal, or Vengeance and Corruption).
    bool SameSeal(SpellInfo const* a, SpellInfo const* b)
    {
        return a->SpellFamilyFlags[0] == b->SpellFamilyFlags[0] && a->SpellFamilyFlags[1] == b->SpellFamilyFlags[1] && a->SpellFamilyFlags[2] == b->SpellFamilyFlags[2];
    }
}

// Twin Judgement: effect 0 holds the percent (amount) and the radius in yards (misc value). The proc fires for every Judgement spell; only the damage spells have damage.
class spell_blessings_retribution_twin_judgement : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_twin_judgement);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        Unit* caster = GetTarget();
        if (Unit* other = NearestOtherEnemy(caster, victim, float(GetSpellInfo()->Effects[EFFECT_0].MiscValue)))
            CastShare(caster, SPELL_TWIN_JUDGEMENT_STRIKE, other, damageInfo->GetDamage(), aurEff->GetAmount(), aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_retribution_twin_judgement::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Judgement Echo: effect 0 holds the percent (amount) and the delay in seconds (misc value). Only critical Judgements reach here (spell_proc hit mask). Guids, not pointers, travel to the event.
class spell_blessings_retribution_judgement_echo : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_judgement_echo);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        int32 const amount = int32(CalculatePct(damageInfo->GetDamage(), aurEff->GetAmount()));
        int32 const delay = std::max<int32>(GetSpellInfo()->Effects[EFFECT_0].MiscValue, 0) * IN_MILLISECONDS;
        if (amount <= 0)
            return;

        Unit* caster = GetTarget();
        ObjectGuid const casterGuid = caster->GetGUID();
        ObjectGuid const victimGuid = victim->GetGUID();
        caster->m_Events.AddEventAtOffset([casterGuid, victimGuid, amount]()
        {
            Player* player = ObjectAccessor::FindPlayer(casterGuid);
            if (!player || !player->IsAlive())
                return;

            Unit* target = ObjectAccessor::GetUnit(*player, victimGuid);
            if (!target || !target->IsAlive())
                return;

            int32 const baseDamage = amount - 1;     // the amount of a one-value effect is its base points plus one
            player->CastCustomSpell(target, SPELL_JUDGEMENT_ECHO_HIT, &baseDamage, nullptr, nullptr, true);
        }, Milliseconds(delay));
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_retribution_judgement_echo::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Rallying Strikes: effect 0's misc value is the radius in yards. The buff goes on the Paladin and on up to four other members of his party or raid within that radius, nearest first.
class spell_blessings_retribution_rallying_strikes : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_rallying_strikes);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->IsAlive())
            return;

        player->CastSpell(player, SPELL_RALLYING_STRIKES_BUFF, true);

        Group* group = player->GetGroup();
        if (!group)
            return;

        float const range = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        std::vector<std::pair<float, Player*>> nearby;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == player || !member->IsAlive() || !member->IsInWorld() || member->GetMap() != player->GetMap())
                continue;

            float const distance = player->GetDistance(member);
            if (distance <= range)
                nearby.emplace_back(distance, member);
        }

        std::sort(nearby.begin(), nearby.end(), [](std::pair<float, Player*> const& a, std::pair<float, Player*> const& b) { return a.first < b.first; });
        if (nearby.size() > RALLYING_ALLIES)
            nearby.resize(RALLYING_ALLIES);

        for (auto const& entry : nearby)
            player->CastSpell(entry.second, SPELL_RALLYING_STRIKES_BUFF, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_retribution_rallying_strikes::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Echoing Strike: effect 0 holds the percent (amount). The 25% chance and the critical hit are in the spell_proc row.
class spell_blessings_retribution_echoing_strike : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_echoing_strike);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        Unit* victim = eventInfo.GetActionTarget();
        if (!damageInfo || !damageInfo->GetDamage() || !victim)
            return;

        CastShare(GetTarget(), SPELL_ECHOING_STRIKE_HIT, victim, damageInfo->GetDamage(), aurEff->GetAmount(), aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_retribution_echoing_strike::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Sealbound Strikes: effect 0 (a half second tick) keeps the damage buff on exactly while a Seal is on the Paladin (the core sets the Judgement aura state for any Seal);
// effect 1 holds the percent of maximum mana that a Crusader Strike or Divine Storm returns while a Seal is on him.
class spell_blessings_retribution_sealbound_strikes : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_sealbound_strikes);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* target = GetTarget();
        bool const sealed = target->HasAuraState(AURA_STATE_JUDGEMENT);
        bool const buffed = target->HasAura(SPELL_SEALBOUND_STRIKES_BUFF);
        if (sealed && !buffed)
            target->CastSpell(target, SPELL_SEALBOUND_STRIKES_BUFF, true);
        else if (!sealed && buffed)
            target->RemoveAurasDueToSpell(SPELL_SEALBOUND_STRIKES_BUFF);
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& /*eventInfo*/)
    {
        PreventDefaultAction();
        Unit* target = GetTarget();
        if (!target->HasAuraState(AURA_STATE_JUDGEMENT))
            return;

        uint32 const mana = CalculatePct(target->GetMaxPower(POWER_MANA), aurEff->GetAmount());
        if (mana)
            target->EnergizeBySpell(target, GetId(), mana, POWER_MANA);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        GetTarget()->RemoveAurasDueToSpell(SPELL_SEALBOUND_STRIKES_BUFF);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_retribution_sealbound_strikes::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
        OnEffectProc += AuraEffectProcFn(spell_blessings_retribution_sealbound_strikes::HandleProc, EFFECT_1, SPELL_AURA_DUMMY);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_retribution_sealbound_strikes::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Judgement Day: the buff (free, no cooldown, +30% damage, one charge) is cast by the proc trigger itself; this handler only makes Crusader Strike ready at once, and the default
// action goes on to cast the buff. Only critical Judgements reach here (spell_proc hit mask).
class spell_blessings_retribution_judgement_day : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_judgement_day);

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& /*eventInfo*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            player->RemoveSpellCooldown(SPELL_CRUSADER_STRIKE, true);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_retribution_judgement_day::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Sealfire: a 6 second tick. With a Seal on him and in combat, the Paladin casts the damage spell of his Judgement on his current enemy (or the nearest enemy that attacks him) while the 40% damage cut
// (a spell modifier on Judgement and Seal damage) is on him. Everything is synchronous: the instant spells have landed before the cut is taken away again.
class spell_blessings_retribution_sealfire : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_sealfire);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetTarget();
        if (!caster->IsAlive() || !caster->IsInCombat() || !caster->HasAuraState(AURA_STATE_JUDGEMENT))
            return;

        // his current enemy, else the nearest enemy that is attacking him (never an uninvolved creature that merely stands nearby)
        Unit* target = caster->GetVictim();
        if (!target || !target->IsAlive() || !caster->IsValidAttackTarget(target) || !caster->IsWithinDistInMap(target, SEALFIRE_RANGE))
        {
            target = nullptr;
            float bestDistance = SEALFIRE_RANGE + 1.0f;
            for (Unit* attacker : caster->getAttackers())
            {
                if (!attacker || !attacker->IsAlive() || !caster->IsValidAttackTarget(attacker) || !caster->IsWithinDistInMap(attacker, SEALFIRE_RANGE))
                    continue;

                float const distance = caster->GetDistance(attacker);
                if (distance < bestDistance)
                {
                    target = attacker;
                    bestDistance = distance;
                }
            }
        }

        if (!target)
            return;

        uint32 const damageSpell = JudgementDamageSpell(caster);
        caster->CastSpell(caster, SPELL_SEALFIRE_CUT, true);
        caster->CastSpell(target, damageSpell, true);
        caster->RemoveAurasDueToSpell(SPELL_SEALFIRE_CUT);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_retribution_sealfire::HandlePeriodic, EFFECT_0, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// Standard of the Templar: the spell is a raid area aura. This is the same test the stock Sanctified Retribution aura uses ("targets affected by any of your auras").
class spell_blessings_retribution_standard_of_the_templar : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_standard_of_the_templar);

    bool CheckAreaTarget(Unit* target)
    {
        for (uint32 const auraId : { SPELL_DEVOTION_AURA_R1, SPELL_RETRIBUTION_AURA_R1, SPELL_CONCENTRATION_AURA, SPELL_SHADOW_RESISTANCE_AURA_R1, SPELL_FROST_RESISTANCE_AURA_R1,
                                     SPELL_FIRE_RESISTANCE_AURA_R1, SPELL_CRUSADER_AURA })
            if (target->GetAuraOfRankedSpell(auraId, GetCasterGUID()))
                return true;

        return false;
    }

    void Register() override
    {
        DoCheckAreaTarget += AuraCheckAreaTargetFn(spell_blessings_retribution_standard_of_the_templar::CheckAreaTarget);
    }
};

// Favor of the Twin Seals: the Favor aura teaches Dual Wield (a Paladin has none) and takes it away again when the Favor is dropped, not when the player merely logs out.
// Weapon skill: a Paladin already uses one-handed swords, maces and axes; nothing else is taught here.
class spell_blessings_retribution_twin_seals : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_twin_seals);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (player && player->getClass() == CLASS_PALADIN && !player->HasSpell(SPELL_DUAL_WIELD))
            player->learnSpell(SPELL_DUAL_WIELD);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout() || player->getClass() != CLASS_PALADIN)
            return;

        player->RemoveAurasDueToSpell(SPELL_TWIN_SEAL);
        player->removeSpell(SPELL_DUAL_WIELD, SPEC_MASK_ALL, false);
        player->SetCanDualWield(false);
        player->AutoUnequipOffhandIfNeed();
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_retribution_twin_seals::HandleApply, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_retribution_twin_seals::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// The Twin Seal marker. Its first effect holds the spell id of the second Seal. The core allows only one Seal per Paladin at a time, so the second Seal is not on the Paladin as an
// aura: this marker repeats what that Seal does when it procs. The Seal's own spell_proc row decides when (flags, spell family, hit mask, chance or procs per minute), and the same
// spells the real Seals cast are cast here, with the marker as the triggering aura (the real Seal's own checks then ignore them).
class spell_blessings_retribution_twin_seal : public AuraScript
{
    PrepareAuraScript(spell_blessings_retribution_twin_seal);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();
        Unit* caster = GetTarget();
        Unit* target = eventInfo.GetProcTarget();
        if (!target || target == caster || !target->IsAlive() || eventInfo.GetTriggerAuraSpell() || !caster->HasAura(SPELL_TWIN_SEALS))
            return;

        SpellInfo const* seal = sSpellMgr->GetSpellInfo(uint32(aurEff->GetAmount()));
        if (!seal || seal->GetSpellSpecific() != SPELL_SPECIFIC_SEAL)
            return;

        // the Seal's own proc conditions
        if (SpellProcEntry const* entry = sSpellMgr->GetSpellProcEntry(seal->Id))
        {
            if (!sSpellMgr->CanSpellTriggerProcOnEvent(*entry, eventInfo))
                return;

            float chance = entry->Chance;
            if (entry->ProcsPerMinute != 0.0f)
            {
                WeaponAttackType attackType = BASE_ATTACK;
                if (DamageInfo const* damageInfo = eventInfo.GetDamageInfo())
                    attackType = damageInfo->GetAttackType();

                chance = caster->GetPPMProcChance(caster->GetAttackTime(attackType), entry->ProcsPerMinute, seal);
            }

            if (chance > 0.0f && !roll_chance_f(chance))
                return;
        }

        SpellInfo const* procSpell = eventInfo.GetSpellInfo();
        if (seal->SpellFamilyFlags[1] & FLAGS1_SEAL_OF_RIGHTEOUSNESS)
            ProcRighteousness(aurEff, caster, target, eventInfo);
        else if (seal->SpellFamilyFlags[0] & FLAGS0_SEAL_OF_COMMAND)
        {
            // Seal of Command strikes up to 3 enemies; Divine Storm and Hammer of the Righteous strike only the target (as the stock Seal does)
            int32 targets = 3;
            if (procSpell && procSpell->SpellFamilyName == SPELLFAMILY_PALADIN && (procSpell->SpellFamilyFlags[1] & FLAGS1_DIVINE_STORM_HAMMER))
                targets = 1;

            caster->CastCustomSpell(seal->Effects[EFFECT_0].TriggerSpell, SPELLVALUE_MAX_TARGETS, targets, target, true, nullptr, aurEff);
        }
        else if (seal->SpellFamilyFlags[1] & FLAGS1_SEAL_OF_VENGEANCE)
            ProcVengeance(aurEff, caster, target, eventInfo, seal->Id == SPELL_SEAL_OF_VENGEANCE);
        else if (seal->Effects[EFFECT_0].ApplyAuraName == SPELL_AURA_PROC_TRIGGER_SPELL && seal->Effects[EFFECT_0].TriggerSpell)
        {
            // Light, Wisdom, Justice: the spell the Seal triggers (it picks its own target: the Paladin or the enemy)
            if (target != caster)
                caster->CastSpell(target, seal->Effects[EFFECT_0].TriggerSpell, true, nullptr, aurEff);
        }
    }

    // Seal of Righteousness: the same damage formula as the stock aura script (spell_pal_seal_of_righteousness), 25742 with that damage as its value.
    void ProcRighteousness(AuraEffect const* aurEff, Unit* caster, Unit* target, ProcEventInfo& eventInfo)
    {
        DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        float const attackPower = caster->GetTotalAttackPowerValue(BASE_ATTACK);
        int32 holy = caster->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_HOLY);
        holy += target->SpellBaseDamageBonusTaken(SPELL_SCHOOL_MASK_HOLY);
        int32 const damage = std::max<int32>(0, int32((attackPower * 0.022f + 0.044f * holy) * caster->GetAttackTime(BASE_ATTACK) / 1000));
        caster->CastCustomSpell(SPELL_SEAL_OF_RIGHTEOUSNESS_DAMAGE, SPELLVALUE_BASE_POINT0, damage, target, true, nullptr, aurEff);
    }

    // Seal of Vengeance / Corruption: as the stock aura script, the strike scales with the stacks of the burn and is read before the hit adds a stack; the burn is added by auto attacks.
    void ProcVengeance(AuraEffect const* aurEff, Unit* caster, Unit* target, ProcEventInfo& eventInfo, bool vengeance)
    {
        uint32 const burnSpell = vengeance ? SPELL_HOLY_VENGEANCE : SPELL_BLOOD_CORRUPTION;
        uint32 const hitSpell = vengeance ? SPELL_SEAL_OF_VENGEANCE_HIT : SPELL_SEAL_OF_CORRUPTION_HIT;
        if (AuraEffect const* burn = target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_PALADIN, 0x00000000, FLAGS1_SEAL_BURN, 0x00000000, caster->GetGUID()))
        {
            uint8 const stacks = burn->GetBase()->GetStackAmount();
            uint8 const maxStacks = burn->GetSpellInfo()->StackAmount;
            if (SpellInfo const* hitInfo = sSpellMgr->GetSpellInfo(hitSpell))
                if (stacks && maxStacks)
                {
                    int32 const amount = (hitInfo->Effects[EFFECT_0].CalcValue() * stacks) / maxStacks;
                    caster->CastCustomSpell(hitSpell, SPELLVALUE_BASE_POINT0, amount, target, true, nullptr, aurEff);
                }
        }

        if (eventInfo.GetTypeMask() & PROC_FLAG_DONE_MELEE_AUTO_ATTACK)
            caster->CastSpell(target, burnSpell, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_retribution_twin_seal::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Sets the Twin Seal marker. When a Paladin who has chosen the Favor of the Twin Seals loses a Seal because another Seal pushed it off (mode "default", and the other Seal is already
// owned by him: the core adds the new aura to his list before it removes the old ones), the pushed-off Seal becomes the Twin Seal for the time it had left. A one millisecond event does it,
// because the aura lists are still being changed inside this hook. Only guids travel to the event.
class MoonlitBlessingsTwinSealsScript : public UnitScript
{
public:
    MoonlitBlessingsTwinSealsScript() : UnitScript("MoonlitBlessingsTwinSealsScript", true, { UNITHOOK_ON_AURA_APPLY, UNITHOOK_ON_AURA_REMOVE }) { }

    // A Seal is cast or refreshed: if it is the very Seal that the Twin Seal marker holds (a cancelled or expired Seal was cast again), the marker would count it twice. Drop the marker.
    void OnAuraApply(Unit* unit, Aura* aura) override
    {
        if (!unit->IsPlayer())
            return;

        SpellInfo const* info = aura->GetSpellInfo();
        if (info->GetSpellSpecific() != SPELL_SPECIFIC_SEAL)
            return;

        Player* player = unit->ToPlayer();
        if (aura->GetCasterGUID() != player->GetGUID() || !player->HasAura(SPELL_TWIN_SEALS))
            return;

        ObjectGuid const guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([guid]()
        {
            Player* owner = ObjectAccessor::FindPlayer(guid);
            if (!owner || !owner->IsInWorld())
                return;

            AuraEffect const* marker = owner->GetAuraEffect(SPELL_TWIN_SEAL, EFFECT_0);
            SpellInfo const* twin = marker ? sSpellMgr->GetSpellInfo(uint32(marker->GetAmount())) : nullptr;
            if (!twin)
                return;

            for (auto const& entry : owner->GetOwnedAuras())
            {
                Aura const* other = entry.second;
                if (other->IsRemoved() || other->GetCasterGUID() != guid || other->GetSpellInfo()->GetSpellSpecific() != SPELL_SPECIFIC_SEAL)
                    continue;

                if (SameSeal(other->GetSpellInfo(), twin))
                {
                    owner->RemoveAurasDueToSpell(SPELL_TWIN_SEAL);
                    break;
                }
            }
        }, Milliseconds(1));
    }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (mode != AURA_REMOVE_BY_DEFAULT || !unit->IsPlayer())
            return;

        Aura const* aura = aurApp->GetBase();
        SpellInfo const* info = aura->GetSpellInfo();
        if (info->GetSpellSpecific() != SPELL_SPECIFIC_SEAL)
            return;

        Player* player = unit->ToPlayer();
        if (aura->GetCasterGUID() != player->GetGUID() || !player->HasAura(SPELL_TWIN_SEALS))
            return;

        bool pushedOff = false;
        for (auto const& entry : player->GetOwnedAuras())
        {
            Aura const* other = entry.second;
            if (other == aura || other->IsRemoved() || other->GetCasterGUID() != player->GetGUID())
                continue;

            SpellInfo const* otherInfo = other->GetSpellInfo();
            if (otherInfo->GetSpellSpecific() == SPELL_SPECIFIC_SEAL && !SameSeal(otherInfo, info))
            {
                pushedOff = true;
                break;
            }
        }

        if (!pushedOff)
            return;

        ObjectGuid const guid = player->GetGUID();
        uint32 const sealId = info->Id;
        int32 const duration = aura->GetDuration();
        player->m_Events.AddEventAtOffset([guid, sealId, duration]()
        {
            Player* owner = ObjectAccessor::FindPlayer(guid);
            if (!owner || !owner->IsInWorld() || !owner->IsAlive() || !owner->HasAura(SPELL_TWIN_SEALS) || !owner->HasAuraState(AURA_STATE_JUDGEMENT))
                return;

            owner->RemoveAurasDueToSpell(SPELL_TWIN_SEAL);
            int32 const baseAmount = int32(sealId) - 1;       // the amount of a one-value effect is its base points plus one
            owner->CastCustomSpell(owner, SPELL_TWIN_SEAL, &baseAmount, nullptr, nullptr, true);
            if (Aura* marker = owner->GetAura(SPELL_TWIN_SEAL))
                if (duration > 0)
                {
                    marker->SetMaxDuration(duration);
                    marker->SetDuration(duration);
                }
        }, Milliseconds(1));
    }
};

void AddMoonlitBlessingsRetributionScripts()
{
    RegisterSpellScript(spell_blessings_retribution_twin_judgement);
    RegisterSpellScript(spell_blessings_retribution_judgement_echo);
    RegisterSpellScript(spell_blessings_retribution_rallying_strikes);
    RegisterSpellScript(spell_blessings_retribution_echoing_strike);
    RegisterSpellScript(spell_blessings_retribution_sealbound_strikes);
    RegisterSpellScript(spell_blessings_retribution_judgement_day);
    RegisterSpellScript(spell_blessings_retribution_sealfire);
    RegisterSpellScript(spell_blessings_retribution_standard_of_the_templar);
    RegisterSpellScript(spell_blessings_retribution_twin_seals);
    RegisterSpellScript(spell_blessings_retribution_twin_seal);
    new MoonlitBlessingsTwinSealsScript();
}
