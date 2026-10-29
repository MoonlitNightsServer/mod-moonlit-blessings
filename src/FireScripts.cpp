// Moonlit Blessings: C++ spell scripts for the Fire Boons and Favors (registered from AddSC_moonlit_blessings).
// Bind a script with k.script(spell_id, "class_name") in tools/talents/recipes/fire.py, then regenerate with tools/phase2/gen_blessings_spells.py.
//
//   spell_blessings_fire_searing_resolve   Boon: while a Fire spell is being cast, a short 15% damage reduction is kept up (checked twice a second)
//   spell_blessings_fire_streak            Inferno Streak (Boon) and Favor of the Hot Streak: counts two critical Fire hits in a row, then casts the passive's buff
//   spell_blessings_fire_hot_streak_buff   Favor of the Hot Streak: only an instant cast may use up the buff's charge
//   spell_blessings_fire_cauterizer        Favor of the Cauterizer: teaches Cauterize, Phoenix Flame, Hearth Fire and Ember Ward while the Favor is chosen
//   spell_blessings_fire_pyromancer        Favor of the Pyromancer: a burn set by Fireball, Pyroblast or Frostfire Bolt is copied to nearby enemies at half strength
//   MoonlitFireSpreadScript                Spreading Embers (a burn passes on when its carrier dies) and Wildfire Sovereign (a burn spreads when it would expire)
//   MoonlitFirePhoenixScript               Phoenix Ember (a killing blow leaves 25% health and burns the enemies around the mage)
//   spell_blessings_fire_cauterizer_conversion   Cauterizer's Conversion: Fire damage heals the group member with the lowest health percent; overheal becomes a heal over time
//   spell_blessings_fire_phoenix_streak    Glyph of the Cauterizer, Hot Streak rewrite: two crits in a row can make the next Phoenix Flame instant, free and stronger
//   MoonlitFireCauterizerScript            Glyph of the Cauterizer, Fiery Payback rewrite: the healing is stronger on allies below 35% health
//
// The Ignite and Master of Elements rewrites are clones of the stock talents carrying the stock scripts (recipes/fire.py clone_behavior).
// Spell ids come from the Fire registry block 1,420,000 (tools/talents/recipes/fire.py asserts the units that are named here).
// Living Bomb belongs to mod-living-bomb (spell_mage_living_bomb_mop): nothing here binds a script to it.
// ~Moonlit Nights Team

#include "Cell.h"
#include "CellImpl.h"
#include "Group.h"
#include "Pet.h"
#include "GroupReference.h"
#include "GridNotifiers.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "UnitScript.h"
#include "WorldSession.h"
#include <algorithm>
#include <list>

namespace
{
    enum FireSpells : uint32
    {
        SPELL_SPREADING_EMBERS      = 1420000,   // passive: effect 0 = how many enemies, misc value = range in yards
        SPELL_SEARING_RESOLVE_BUFF  = 1420011,   // buff: 15% less damage taken
        SPELL_PHOENIX_EMBER         = 1420030,   // passive: effect 0 = health percent left, effect 1 = cooldown in seconds, effect 2 = burst percent of maximum health
        SPELL_PHOENIX_EMBER_BURST   = 1420031,   // area damage around the caster (base points 0 is set per cast)
        SPELL_WILDFIRE_SOVEREIGN    = 1420040,   // passive: effect 0 = how many enemies, misc value = range in yards
        SPELL_SPREAD_MARKER         = 1420050,   // hidden debuff on every enemy that received a spread copy
        SPELL_CAUTERIZE             = 1420080,
        SPELL_PHOENIX_FLAME         = 1420090,
        SPELL_HEARTH_FIRE           = 1420100,
        SPELL_EMBER_WARD            = 1420110,

        SPELL_CONVERSION_HEAL       = 1420071,   // the heal helper of the conversion (base points set per cast)
        SPELL_CAUTERIZER_EMBERS     = 1420150,   // heal over time for the part of a heal an ally cannot take (base points = one tick)
        SPELL_GLYPH_IGNITE          = 1420160,   // Ignite rewrite, 5 ranks: a critical heal also gains a heal over time worth 10% per rank
        SPELL_GLYPH_PHOENIX_STREAK  = 1420190,   // buff: the next Phoenix Flame is instant, free and 50% stronger
        SPELL_GLYPH_PAYBACK         = 1420210    // Fiery Payback rewrite, 2 ranks: effect 0 holds the percent
    };

    constexpr float CONVERSION_RANGE = 40.0f;
    constexpr int32 EMBERS_TICKS = 4;                                        // the heal over time lasts 8 sec in 2 sec ticks
    constexpr uint32 PAYBACK_BELOW_PCT = 35;                                 // Fiery Payback: allies below this health percent

    constexpr uint32 CAUTERIZER_SPELLS[] = { SPELL_CAUTERIZE, SPELL_PHOENIX_FLAME, SPELL_HEARTH_FIRE, SPELL_EMBER_WARD };
    constexpr uint32 LIVING_BOMB_RANKS[] = { 44457, 55359, 55360 };         // the Living Bomb burn (rank 1 to 3)
    constexpr uint32 STREAK_CRITS = 2;                                       // critical hits in a row
    constexpr uint32 PYROMANCER_MAX_COPIES = 5;                              // enemies one hit may set ablaze
    constexpr int32 MARKER_EXTRA_MS = 2000;                                  // the spread marker is kept this long after the copy it marks
    constexpr int32 MIN_COPY_DURATION_MS = 1500;                             // a burn with less time left than this is not worth passing on

    // The rank (0 = none) of a rewritten talent the player has: the variant spells run on by one per rank.
    uint32 VariantRank(Unit const* owner, uint32 firstRank, uint32 ranks)
    {
        for (uint32 rank = ranks; rank > 0; --rank)
            if (owner->HasAura(firstRank + rank - 1))
                return rank;

        return 0;
    }

    // The group member (the caster, party and raid members and their pets) within `range` yards of the caster with the lowest health percent. Null if nobody is hurt.
    Unit* LowestHealthPercentFriend(Unit* caster, float range)
    {
        Unit* best = nullptr;
        float bestPct = 100.0f;
        auto consider = [&](Unit* u)
        {
            if (!u || !u->IsAlive() || !u->GetMaxHealth() || !u->IsWithinDist(caster, range))
                return;

            float const pct = 100.0f * float(u->GetHealth()) / float(u->GetMaxHealth());
            if (pct < bestPct)
            {
                bestPct = pct;
                best = u;
            }
        };

        consider(caster);
        if (Player* player = caster->ToPlayer())
        {
            if (Group* group = player->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    consider(member);
                    if (member)
                        consider(member->GetPet());
                }
            }

            consider(player->GetPet());
        }

        return best;
    }

    bool IsLivingBomb(SpellInfo const* info)
    {
        for (uint32 const rank : LIVING_BOMB_RANKS)
            if (info->Id == rank)
                return true;

        return false;
    }

    // A burn of the mage: a Mage spell with a Fire school and a periodic damage effect.
    bool IsMageBurn(SpellInfo const* info)
    {
        return info && info->SpellFamilyName == SPELLFAMILY_MAGE && (info->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE) && info->HasAura(SPELL_AURA_PERIODIC_DAMAGE);
    }

    bool IsFireSpell(Spell const* spell)
    {
        SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
        return info && info->SpellFamilyName == SPELLFAMILY_MAGE && (info->GetSchoolMask() & SPELL_SCHOOL_MASK_FIRE);
    }

    // Is the unit in the middle of casting (or channeling) a Mage Fire spell?
    bool IsCastingFire(Unit const* unit)
    {
        if (Spell const* spell = unit->GetCurrentSpell(CURRENT_GENERIC_SPELL))
            if (spell->getState() == SPELL_STATE_PREPARING && IsFireSpell(spell))
                return true;

        if (Spell const* spell = unit->GetCurrentSpell(CURRENT_CHANNELED_SPELL))
            if (spell->getState() == SPELL_STATE_CASTING && IsFireSpell(spell))
                return true;

        return false;
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

    // Copy the burn `source` (an aura of `caster` on `from`) to up to `count` of the nearest other enemies within `range` yards of `from` that do not carry that burn from the caster yet.
    // The copy keeps the source's damage per tick (`percent` of it) and gets `duration` ms (full duration when it is 0 or less). Every receiver is marked with the Spread Embers
    // debuff (as long as the copy lasts) so that Wildfire Sovereign does not spread a copy again when it expires.
    void SpreadBurn(Player* caster, Unit* from, Aura const* source, uint32 count, float range, int32 percent, int32 duration)
    {
        SpellInfo const* info = source->GetSpellInfo();
        ObjectGuid const casterGuid = caster->GetGUID();

        std::list<Unit*> enemies = EnemiesAround(caster, from, range);
        enemies.remove_if([from, info, casterGuid](Unit* unit) { return unit == from || unit->GetAura(info->Id, casterGuid); });
        enemies.sort([from](Unit* a, Unit* b) { return from->GetDistance(a) < from->GetDistance(b); });

        uint8 effectMask = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (source->HasEffect(i))
                effectMask |= uint8(1 << i);

        uint32 done = 0;
        for (Unit* other : enemies)
        {
            if (done >= count)
                break;

            Aura* copy = caster->AddAura(info, effectMask, other);
            if (!copy)
                continue;

            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                AuraEffect* effect = copy->GetEffect(i);
                AuraEffect const* original = source->GetEffect(i);
                if (effect && original && info->Effects[i].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE)
                    effect->SetAmount(CalculatePct(original->GetAmount(), percent));   // SetAmount also stops the aura from being recalculated back to the base value
            }

            if (duration > 0)
                copy->SetDuration(duration);

            if (Aura* marker = caster->AddAura(SPELL_SPREAD_MARKER, other))
                marker->SetDuration(std::max(copy->GetDuration(), MIN_COPY_DURATION_MS) + MARKER_EXTRA_MS);   // outlives the copy, whatever order they are removed in

            ++done;
        }
    }
}

// Searing Resolve: twice a second, while the mage is casting a Fire spell, the 15% damage reduction (2 sec buff) is renewed; when no Fire spell is being cast it is taken off.
class spell_blessings_fire_searing_resolve : public AuraScript
{
    PrepareAuraScript(spell_blessings_fire_searing_resolve);

    void HandlePeriodic(AuraEffect const* /*aurEff*/)
    {
        Unit* caster = GetTarget();
        if (IsCastingFire(caster))
        {
            if (Aura* buff = caster->GetAura(SPELL_SEARING_RESOLVE_BUFF))
                buff->RefreshDuration();
            else
                caster->CastSpell(caster, SPELL_SEARING_RESOLVE_BUFF, true);
        }
        else
            caster->RemoveAurasDueToSpell(SPELL_SEARING_RESOLVE_BUFF);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_blessings_fire_searing_resolve::HandlePeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DUMMY);
    }
};

// The streak counter of Inferno Streak and Favor of the Hot Streak (the same rule as the stock Hot Streak talent: two critical hits in a row, a hit that does not crit starts again).
// The spell_proc row of the passive selects Fireball, Fire Blast, Scorch, Frostfire Bolt and the Living Bomb explosion. Effect 0 of the passive: its amount is the percent of maximum
// mana returned when the streak completes (0 for the Favor), its trigger spell is the buff cast on the mage. The count lives in this aura (it is lost with the aura).
class spell_blessings_fire_streak : public AuraScript
{
    PrepareAuraScript(spell_blessings_fire_streak);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        if (!(eventInfo.GetHitMask() & PROC_HIT_CRITICAL))
        {
            _crits = 0;
            return;
        }

        if (++_crits < STREAK_CRITS)
            return;

        _crits = 0;
        Unit* caster = GetTarget();
        caster->CastSpell(caster, GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell, true);
        if (aurEff->GetAmount() > 0)
            caster->EnergizeBySpell(caster, GetId(), CalculatePct(caster->GetMaxPower(POWER_MANA), aurEff->GetAmount()), POWER_MANA);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_fire_streak::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }

private:
    uint32 _crits = 0;
};

// Favor of the Hot Streak, the buff (1420121): only an instant cast may use its charge up. The streak completes on a hit, often while the next Fireball is already being cast;
// that Fireball cannot be made instant any more, so it must not eat the charge (it still gets the +25% from the damage modifier at launch).
class spell_blessings_fire_hot_streak_buff : public AuraScript
{
    PrepareAuraScript(spell_blessings_fire_hot_streak_buff);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Spell const* spell = eventInfo.GetProcSpell();
        return spell && spell->GetCastTime() == 0;
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_blessings_fire_hot_streak_buff::CheckProc);
    }
};

// Favor of the Cauterizer: the hidden Favor aura teaches the four healing spells when it is applied and takes them away when it ends (the way Earth Slam is taught).
// The aura's other effects are the 60% damage loss (effect 0) and the +150% on Phoenix Flame (effect 2); this script owns the dummy effect 1.
class spell_blessings_fire_cauterizer : public AuraScript
{
    PrepareAuraScript(spell_blessings_fire_cauterizer);

    void HandleApply(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        if (Player* player = GetTarget()->ToPlayer())
            for (uint32 const spellId : CAUTERIZER_SPELLS)
                player->learnSpell(spellId);
    }

    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget()->ToPlayer();
        if (!player || !player->GetSession() || player->GetSession()->PlayerLogout())
            return;

        for (uint32 const spellId : CAUTERIZER_SPELLS)
            player->removeSpell(spellId, SPEC_MASK_ALL, false);
    }

    void Register() override
    {
        AfterEffectApply += AuraEffectApplyFn(spell_blessings_fire_cauterizer::HandleApply, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_blessings_fire_cauterizer::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Favor of the Pyromancer: effect 0 holds the percent of the burn that is copied (amount) and the range in yards (misc value). The proc row selects Fireball, Pyroblast and
// Frostfire Bolt; the victim already carries the burn when the hit procs (auras are applied before the proc), so the copy is taken from it. Triggered casts are ignored.
class spell_blessings_fire_pyromancer : public AuraScript
{
    PrepareAuraScript(spell_blessings_fire_pyromancer);

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Player* caster = GetTarget()->ToPlayer();
        Unit* victim = eventInfo.GetActionTarget();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!caster || !victim || !IsMageBurn(info) || IsLivingBomb(info))
            return;

        if (Spell const* spell = eventInfo.GetProcSpell())
            if (spell->IsTriggered())
                return;

        Aura const* burn = victim->GetAura(info->Id, caster->GetGUID());
        if (!burn)
            return;

        float const range = float(GetSpellInfo()->Effects[EFFECT_0].MiscValue);
        SpreadBurn(caster, victim, burn, PYROMANCER_MAX_COPIES, range, aurEff->GetAmount(), 0);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_fire_pyromancer::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Spreading Embers (an enemy dies while burning) and Wildfire Sovereign (a burn expires). One hook on every aura removal; everything is rejected early unless the aura is a Mage
// fire burn of a player on a unit, removed by death or expiry.
class MoonlitFireSpreadScript : public UnitScript
{
public:
    MoonlitFireSpreadScript() : UnitScript("MoonlitFireSpreadScript", true, { UNITHOOK_ON_AURA_REMOVE }) { }

    void OnAuraRemove(Unit* unit, AuraApplication* aurApp, AuraRemoveMode mode) override
    {
        if (!unit || !aurApp || (mode != AURA_REMOVE_BY_DEATH && mode != AURA_REMOVE_BY_EXPIRE))
            return;

        Aura const* aura = aurApp->GetBase();
        if (!aura || aura->GetType() != UNIT_AURA_TYPE || !aura->GetCasterGUID().IsPlayer() || !IsMageBurn(aura->GetSpellInfo()))
            return;

        Player* caster = ObjectAccessor::GetPlayer(*unit, aura->GetCasterGUID());
        if (!caster || !caster->IsAlive() || !caster->IsInWorld())
            return;

        if (mode == AURA_REMOVE_BY_DEATH)
        {
            // Spreading Embers: the burn that was left passes on (Living Bomb explodes by itself, see mod-living-bomb)
            AuraEffect const* embers = IsLivingBomb(aura->GetSpellInfo()) ? nullptr : caster->GetAuraEffect(SPELL_SPREADING_EMBERS, EFFECT_0);
            if (!embers || aura->GetDuration() < MIN_COPY_DURATION_MS)
                return;

            SpreadBurn(caster, unit, aura, uint32(std::max(embers->GetAmount(), 0)), float(embers->GetSpellInfo()->Effects[EFFECT_0].MiscValue), 100, aura->GetDuration());
        }
        else
        {
            // Wildfire Sovereign: a burn that runs out spreads once; a copy (marked) does not spread again
            AuraEffect const* wildfire = caster->GetAuraEffect(SPELL_WILDFIRE_SOVEREIGN, EFFECT_0);
            if (!wildfire || unit->HasAura(SPELL_SPREAD_MARKER, caster->GetGUID()))
                return;

            SpreadBurn(caster, unit, aura, uint32(std::max(wildfire->GetAmount(), 0)), float(wildfire->GetSpellInfo()->Effects[EFFECT_0].MiscValue), 100, 0);
        }
    }
};

// Phoenix Ember. UnitScript::OnDamage runs inside Unit::DealDamage after every absorb has been taken, so `damage` is what will really come off the mage's health.
class MoonlitFirePhoenixScript : public UnitScript
{
public:
    MoonlitFirePhoenixScript() : UnitScript("MoonlitFirePhoenixScript", true, { UNITHOOK_ON_DAMAGE }) { }

    void OnDamage(Unit* /*attacker*/, Unit* victim, uint32& damage) override
    {
        if (!damage || !victim || !victim->IsPlayer() || !victim->IsAlive())
            return;

        Player* player = victim->ToPlayer();
        AuraEffect const* ember = player->GetAuraEffect(SPELL_PHOENIX_EMBER, EFFECT_0);
        uint32 const health = player->GetHealth();
        if (!ember || damage < health || player->HasSpellCooldown(SPELL_PHOENIX_EMBER))
            return;

        // A killing blow: leave the mage at the passive's percent of maximum health.
        uint32 const left = player->CountPctFromMaxHealth(ember->GetAmount());
        if (health > left)
            damage = health - left;
        else
        {
            damage = 0;
            player->SetHealth(left);
        }

        AuraEffect const* cooldown = player->GetAuraEffect(SPELL_PHOENIX_EMBER, EFFECT_1);
        player->AddSpellCooldown(SPELL_PHOENIX_EMBER, 0, uint32(cooldown ? std::max(cooldown->GetAmount(), 0) : 300) * IN_MILLISECONDS);

        // The burst comes a moment later (it must not run inside the damage that is being dealt).
        AuraEffect const* burst = player->GetAuraEffect(SPELL_PHOENIX_EMBER, EFFECT_2);
        int32 const burstDamage = burst ? int32(player->CountPctFromMaxHealth(burst->GetAmount())) : 0;
        if (burstDamage > 0)
        {
            player->m_Events.AddEventAtOffset([player, burstDamage]()
            {
                if (player->IsInWorld() && player->IsAlive())
                    player->CastCustomSpell(SPELL_PHOENIX_EMBER_BURST, SPELLVALUE_BASE_POINT0, burstDamage, player, true);
            }, Milliseconds(100));
        }
    }
};

// Cauterizer's Conversion: effect 0 is the proc trigger whose value is the percent of the damage dealt (150 = 75% of the normal damage after the 50% loss). The heal goes to the group
// member with the lowest health percent within 40 yards; whatever the ally cannot take becomes a heal over time on that ally. Ignite rewrite: when the hit was a critical strike,
// the ally also gains a heal over time worth 10% per rank of the heal.
class spell_blessings_fire_cauterizer_conversion : public AuraScript
{
    PrepareAuraScript(spell_blessings_fire_cauterizer_conversion);

    bool Validate(SpellInfo const* spellInfo) override
    {
        return ValidateSpellInfo({ spellInfo->Effects[EFFECT_0].TriggerSpell, SPELL_CAUTERIZER_EMBERS });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        DamageInfo* damageInfo = eventInfo.GetDamageInfo();
        if (!damageInfo || !damageInfo->GetDamage())
            return;

        Unit* caster = GetTarget();
        Unit* ally = LowestHealthPercentFriend(caster, CONVERSION_RANGE);
        if (!ally)
            return;

        int32 const heal = CalculatePct(int32(damageInfo->GetDamage()), aurEff->GetAmount());
        if (heal <= 0)
            return;

        int32 const missing = int32(ally->GetMaxHealth() - ally->GetHealth());
        int32 const direct = std::min(heal, missing);
        int32 extra = heal - direct;
        if (eventInfo.GetHitMask() & PROC_EX_CRITICAL_HIT)
            if (uint32 const rank = VariantRank(caster, SPELL_GLYPH_IGNITE, 5))
                extra += CalculatePct(heal, 10 * rank);

        uint32 const healSpellId = aurEff->GetSpellInfo()->Effects[aurEff->GetEffIndex()].TriggerSpell;
        if (direct > 0)
            caster->CastCustomSpell(healSpellId, SPELLVALUE_BASE_POINT0, direct, ally, true, nullptr, aurEff);

        if (extra >= EMBERS_TICKS)
            caster->CastCustomSpell(SPELL_CAUTERIZER_EMBERS, SPELLVALUE_BASE_POINT0, extra / EMBERS_TICKS, ally, true, nullptr, aurEff);
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_fire_cauterizer_conversion::HandleProc, EFFECT_0, SPELL_AURA_PROC_TRIGGER_SPELL);
    }
};

// Hot Streak rewrite (the stock talent's own counter, its buff replaced): two non-periodic spell critical strikes in a row, then a chance (the effect's value) to cast the
// Phoenix Streak buff. Same rule as the stock script (spell_mage_hot_streak).
class spell_blessings_fire_phoenix_streak : public AuraScript
{
    PrepareAuraScript(spell_blessings_fire_phoenix_streak);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_GLYPH_PHOENIX_STREAK });
    }

    void HandleProc(AuraEffect const* aurEff, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        if (!(eventInfo.GetHitMask() & PROC_EX_CRITICAL_HIT))
        {
            _critStreak = 0;
            return;
        }

        if (++_critStreak >= STREAK_CRITS)
        {
            _critStreak = 0;
            if (roll_chance_i(aurEff->GetAmount()))
                GetTarget()->CastSpell(GetTarget(), SPELL_GLYPH_PHOENIX_STREAK, true, nullptr, aurEff);
        }
    }

    void Register() override
    {
        OnEffectProc += AuraEffectProcFn(spell_blessings_fire_phoenix_streak::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }

private:
    uint32 _critStreak = 0;
};

// Fiery Payback rewrite: the conversion heal and Cauterize heal more on an ally below 35% health. The core hands the two units to this hook in an order that depends on the kind of
// heal, so the healer is the one that carries the talent.
class MoonlitFireCauterizerScript : public UnitScript
{
public:
    MoonlitFireCauterizerScript() : UnitScript("MoonlitFireCauterizerScript", true, { UNITHOOK_MODIFY_HEAL_RECEIVED }) { }

    void ModifyHealReceived(Unit* target, Unit* healer, uint32& heal, SpellInfo const* spellInfo) override
    {
        if (!spellInfo || (spellInfo->Id != SPELL_CONVERSION_HEAL && spellInfo->Id != SPELL_CAUTERIZE && spellInfo->Id != SPELL_CAUTERIZER_EMBERS))
            return;

        Unit* receiver = target;
        uint32 rank = healer ? VariantRank(healer, SPELL_GLYPH_PAYBACK, 2) : 0;
        if (!rank && target)
        {
            rank = VariantRank(target, SPELL_GLYPH_PAYBACK, 2);
            receiver = healer;
        }

        if (!rank || !receiver || !receiver->GetMaxHealth())
            return;

        if (receiver->GetHealthPct() < float(PAYBACK_BELOW_PCT))
            heal += CalculatePct(heal, 15 * rank);
    }
};

void AddMoonlitBlessingsFireScripts()
{
    RegisterSpellScript(spell_blessings_fire_searing_resolve);
    RegisterSpellScript(spell_blessings_fire_streak);
    RegisterSpellScript(spell_blessings_fire_hot_streak_buff);
    RegisterSpellScript(spell_blessings_fire_cauterizer);
    RegisterSpellScript(spell_blessings_fire_pyromancer);
    RegisterSpellScript(spell_blessings_fire_cauterizer_conversion);
    RegisterSpellScript(spell_blessings_fire_phoenix_streak);
    new MoonlitFireCauterizerScript();
    new MoonlitFireSpreadScript();
    new MoonlitFirePhoenixScript();
}
