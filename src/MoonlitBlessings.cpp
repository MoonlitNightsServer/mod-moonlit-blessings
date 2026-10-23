// Moonlit Blessings (testing stage): the C++ half of the Blessings talent system.
//
// For the classes listed in MoonlitBlessings.Classes the stock ways of gaining talents are closed so that the Blessings system (Lua, mod-ale) is the only route:
//   * learning a talent from the talent window is refused;
//   * trainers show no spells and teach nothing (class spells arrive through Learn Spells, everything else through the Blessings);
//   * on login the stock talents are reset once and free talent points are set to 0.
// Spells granted by the Lua system go through Player::learnSpell and are NOT blocked: this module only uses the talent veto and the trainer hooks.
// ~Moonlit Nights Team

#include "Config.h"
#include "Creature.h"
#include "Log.h"
#include "NPCPackets.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Trainer.h"
#include "WorldScript.h"
#include "DatabaseEnv.h"
#include "SpellMgr.h"

#include <set>

namespace
{
    bool IsBlessingsClass(Player const* player)
    {
        if (!player || !sConfigMgr->GetOption<bool>("MoonlitBlessings.Enable", false))
            return false;

        std::string const classes = sConfigMgr->GetOption<std::string>("MoonlitBlessings.Classes", "1,2,3,4,5,6,7,8,9,11");
        std::string token;
        for (size_t i = 0; i <= classes.size(); ++i)
        {
            if (i == classes.size() || classes[i] == ',')
            {
                if (!token.empty() && static_cast<uint8>(std::stoul(token)) == player->getClass())
                    return true;
                token.clear();
            }
            else if (classes[i] != ' ')
                token += classes[i];
        }
        return false;
    }
}

class MoonlitBlessingsPlayer : public PlayerScript
{
public:
    MoonlitBlessingsPlayer() : PlayerScript("MoonlitBlessingsPlayer", {
        PLAYERHOOK_CAN_LEARN_TALENT,
        PLAYERHOOK_ON_GET_TRAINER_SPELL_STATE,
        PLAYERHOOK_ON_BEFORE_RECEIVE_SPELL_LIST_FROM_TRAINER,
        PLAYERHOOK_ON_LOGIN
    }) { }

    bool OnPlayerCanLearnTalent(Player* player, TalentEntry const* /*talent*/, uint32 /*rank*/) override
    {
        return !IsBlessingsClass(player);
    }

    void OnPlayerGetTrainerSpellState(Player const* player, uint32 /*trainerId*/, uint32 /*spellId*/, Trainer::SpellState& state) override
    {
        if (IsBlessingsClass(player))
            state = Trainer::SpellState::Unavailable;
    }

    void OnPlayerBeforeReceiveSpellListFromTrainer(Player* player, Creature* /*trainer*/, WorldPackets::NPC::TrainerList& list) override
    {
        if (IsBlessingsClass(player))
            list.Spells.clear();
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!IsBlessingsClass(player))
            return;

        // The stock talent system is closed for this class: take back anything learned before, once (a no-op when nothing is learned)
        if (player->GetFreeTalentPoints() > 0 || player->GetLevel() >= 10)
        {
            player->resetTalents(true);
            player->SetFreeTalentPoints(0);
        }
    }
};


// At start-up, check that every custom spell the Blessings use (table moonlit_blessing_spell, written by tools/phase2/gen_blessings_spells.py) exists in the spell store.
class MoonlitBlessingsWorld : public WorldScript
{
public:
    MoonlitBlessingsWorld() : WorldScript("MoonlitBlessingsWorld", { WORLDHOOK_ON_STARTUP }) { }

    void OnStartup() override
    {
        QueryResult result = WorldDatabase.Query("SELECT `spell_id`, `name` FROM `moonlit_blessing_spell`");
        uint32 total = 0, missing = 0;
        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                ++total;
                if (!sSpellMgr->GetSpellInfo(fields[0].Get<uint32>()))
                {
                    ++missing;
                    LOG_ERROR("server.loading", "Moonlit Blessings: custom spell {} ({}) is not in the spell store", fields[0].Get<uint32>(), fields[1].Get<std::string>());
                }
            } while (result->NextRow());
        }

        LOG_INFO("server.loading", ">> Moonlit Blessings: {} custom spells checked, {} missing", total, missing);
    }
};

// Druid, shared by every Blessing that teaches the stock Leader of the Pack talent spell (17007): the Feral kit item Leader of the Pack and the Guardian kit item Bear Form (which teaches it too). The core casts the aura
// Leader of the Pack (24932) on a shapeshift only for a druid who has the TALENT (Player::HasTalent(17007)), but the Blessings system teaches the spell as an ordinary spell
// (Player::learnSpell), which the core does not count as a talent, so the aura was never cast. For a druid who knows spell 17007 without having the talent, this casts the aura when he takes
// Cat Form, Bear Form or Dire Bear Form (the core removes it again when the form goes). It does nothing for a real talent and nothing when the aura is already there, so the core's own cast is
// never doubled. One script object for every druid Blessing: registered once, here. The delayed cast looks the player up by GUID.
class MoonlitDruidSharedScript : public UnitScript
{
public:
    MoonlitDruidSharedScript() : UnitScript("MoonlitDruidSharedScript", true, { UNITHOOK_ON_UNIT_SET_SHAPESHIFT_FORM }) { }

    void OnUnitSetShapeshiftForm(Unit* unit, uint8 form) override
    {
        if (!IsLeaderForm(form) || !unit || !unit->IsPlayer())
            return;

        Player* player = unit->ToPlayer();
        if (player->getClass() != CLASS_DRUID || !player->HasSpell(SPELL_LEADER_OF_THE_PACK_TALENT) || player->HasTalent(SPELL_LEADER_OF_THE_PACK_TALENT, player->GetActiveSpec()))
            return;

        // one millisecond later: the shapeshift is complete, and the cast does not run inside the code that decided on it
        ObjectGuid const guid = player->GetGUID();
        player->m_Events.AddEventAtOffset([guid]()
        {
            Player* now = ObjectAccessor::FindPlayer(guid);
            if (!now || !now->IsInWorld() || !now->IsAlive() || !IsLeaderForm(now->GetShapeshiftForm()) || now->HasAura(SPELL_LEADER_OF_THE_PACK))
                return;

            now->CastSpell(now, SPELL_LEADER_OF_THE_PACK, true);
        }, std::chrono::milliseconds(1));
    }

private:
    static bool IsLeaderForm(uint8 form) { return form == FORM_CAT || form == FORM_BEAR || form == FORM_DIREBEAR; }

    static constexpr uint32 SPELL_LEADER_OF_THE_PACK = 24932;           // the stock aura (raid area aura, Cat and Bear Form)
    static constexpr uint32 SPELL_LEADER_OF_THE_PACK_TALENT = 17007;    // the stock talent spell the kit teaches
};

void AddMoonlitBlessingsSpellScripts();
void AddMoonlitBlessingsElementalScripts();
void AddMoonlitBlessingsEnhancementScripts();
void AddMoonlitBlessingsRestorationScripts();
void AddMoonlitBlessingsWardenScripts();
void AddMoonlitBlessingsMartyrScripts();
void AddMoonlitBlessingsPenitentScripts();
void AddMoonlitBlessingsShadowScripts();
void AddMoonlitBlessingsVicarScripts();
void AddMoonlitBlessingsHolyScripts();
void AddMoonlitBlessingsInquisitorScripts();
void AddMoonlitBlessingsProtectionScripts();
void AddMoonlitBlessingsRetributionScripts();
void AddMoonlitBlessingsArcaneScripts();
void AddMoonlitBlessingsFireScripts();
void AddMoonlitBlessingsFrostScripts();
void AddMoonlitBlessingsSpellbladeScripts();
void AddMoonlitBlessingsBeastmasteryScripts();
void AddMoonlitBlessingsMarksmanshipScripts();
void AddMoonlitBlessingsSurvivalScripts();
void AddMoonlitBlessingsWildguardScripts();
void AddMoonlitBlessingsAfflictionScripts();
void AddMoonlitBlessingsDemonologyScripts();
void AddMoonlitBlessingsDestructionScripts();
void AddMoonlitBlessingsVoidbinderScripts();
void AddMoonlitBlessingsArmsScripts();
void AddMoonlitBlessingsFuryScripts();
void AddMoonlitBlessingsWarprotectionScripts();
void AddMoonlitBlessingsSkirmisherScripts();
void AddMoonlitBlessingsAssassinationScripts();
void AddMoonlitBlessingsCombatScripts();
void AddMoonlitBlessingsEvaderScripts();
void AddMoonlitBlessingsSubtletyScripts();
void AddMoonlitBlessingsBalanceScripts();
void AddMoonlitBlessingsFeralScripts();
void AddMoonlitBlessingsGuardianScripts();
void AddMoonlitBlessingsDruidrestorationScripts();
void AddMoonlitBlessingsBloodScripts();
void AddMoonlitBlessingsDkfrostScripts();
void AddMoonlitBlessingsRuneguardScripts();
void AddMoonlitBlessingsUnholyScripts();

void AddSC_moonlit_blessings()
{
    new MoonlitBlessingsPlayer();
    new MoonlitDruidSharedScript();
    AddMoonlitBlessingsSpellScripts();
    AddMoonlitBlessingsElementalScripts();
    AddMoonlitBlessingsEnhancementScripts();
    AddMoonlitBlessingsRestorationScripts();
    AddMoonlitBlessingsWardenScripts();
    AddMoonlitBlessingsMartyrScripts();
    AddMoonlitBlessingsPenitentScripts();
    AddMoonlitBlessingsShadowScripts();
    AddMoonlitBlessingsVicarScripts();
    AddMoonlitBlessingsHolyScripts();
    AddMoonlitBlessingsInquisitorScripts();
    AddMoonlitBlessingsProtectionScripts();
    AddMoonlitBlessingsRetributionScripts();
    AddMoonlitBlessingsArcaneScripts();
    AddMoonlitBlessingsFireScripts();
    AddMoonlitBlessingsFrostScripts();
    AddMoonlitBlessingsSpellbladeScripts();
    AddMoonlitBlessingsBeastmasteryScripts();
    AddMoonlitBlessingsMarksmanshipScripts();
    AddMoonlitBlessingsSurvivalScripts();
    AddMoonlitBlessingsWildguardScripts();
    AddMoonlitBlessingsAfflictionScripts();
    AddMoonlitBlessingsDemonologyScripts();
    AddMoonlitBlessingsDestructionScripts();
    AddMoonlitBlessingsVoidbinderScripts();
    AddMoonlitBlessingsArmsScripts();
    AddMoonlitBlessingsFuryScripts();
    AddMoonlitBlessingsWarprotectionScripts();
    AddMoonlitBlessingsSkirmisherScripts();
    AddMoonlitBlessingsAssassinationScripts();
    AddMoonlitBlessingsCombatScripts();
    AddMoonlitBlessingsEvaderScripts();
    AddMoonlitBlessingsSubtletyScripts();
    AddMoonlitBlessingsBalanceScripts();
    AddMoonlitBlessingsFeralScripts();
    AddMoonlitBlessingsGuardianScripts();
    AddMoonlitBlessingsDruidrestorationScripts();
    AddMoonlitBlessingsBloodScripts();
    AddMoonlitBlessingsDkfrostScripts();
    AddMoonlitBlessingsRuneguardScripts();
    AddMoonlitBlessingsUnholyScripts();
    new MoonlitBlessingsWorld();
}
