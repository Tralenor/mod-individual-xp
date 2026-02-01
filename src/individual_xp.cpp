#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Object.h"
#include "DataMap.h"
#include "ObjectAccessor.h"

using namespace Acore::ChatCommands;

/*
Coded by Talamortis - For Azerothcore
Thanks to Rochet for the assistance
*/

struct IndividualXpModule
{
    bool Enabled, AnnounceModule, AnnounceRatesOnLogin;
    float MaxRate, DefaultRate;
};

IndividualXpModule individualXp;

enum IndividualXPAcoreString
{
    ACORE_STRING_CREDIT = 35411,
    ACORE_STRING_MODULE_DISABLED,
    ACORE_STRING_RATES_DISABLED,
    ACORE_STRING_COMMAND_VIEW,
    ACORE_STRING_MAX_RATE,
    ACORE_STRING_MIN_RATE,
    ACORE_STRING_COMMAND_SET,
    ACORE_STRING_COMMAND_DISABLED,
    ACORE_STRING_COMMAND_ENABLED,
    ACORE_STRING_COMMAND_DEFAULT,
    ACORE_STRING_PERSONAL_MAX_RATE
};

class IndividualXPConf : public WorldScript
{
public:
    IndividualXPConf() : WorldScript("IndividualXPConf") { }

    void OnBeforeConfigLoad(bool /*reload*/) override
    {
        individualXp.Enabled = sConfigMgr->GetOption<bool>("IndividualXp.Enabled", true);
        individualXp.AnnounceModule = sConfigMgr->GetOption<bool>("IndividualXp.Announce", true);
        individualXp.AnnounceRatesOnLogin = sConfigMgr->GetOption<bool>("IndividualXp.AnnounceRatesOnLogin", true);
        individualXp.MaxRate = sConfigMgr->GetOption<float>("IndividualXp.MaxXPRate", 10.0f);
        individualXp.DefaultRate = sConfigMgr->GetOption<float>("IndividualXp.DefaultXPRate", 1.0f);
    }
};

class PlayerXpRate : public DataMap::Base {
public:
    PlayerXpRate() {}
    PlayerXpRate(float XPRate) : XPRate(XPRate) {}

    float XPRate = 1.0f;
};


class IndividualXP : public PlayerScript {
public:
    IndividualXP() : PlayerScript("IndividualXP") {}

    void OnPlayerLogin(Player* player) override
    {
        QueryResult result = CharacterDatabase.Query("SELECT `XPRate` FROM `individualxp` WHERE `CharacterGUID`='{}'",
                                                     player->GetGUID().GetCounter());

        if (!result)
        {
            player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = individualXp.DefaultRate;
        }
        else
        {
            Field* fields = result->Fetch();
            player->CustomData.Set("IndividualXP", new PlayerXpRate(fields[0].Get<float>()));
        }

        if (individualXp.Enabled)
        {
            // Announce Module
            if (individualXp.AnnounceModule)
            {
                ChatHandler(player->GetSession()).SendSysMessage(ACORE_STRING_CREDIT);
            }

            // Announce Rates
            if (individualXp.AnnounceRatesOnLogin)
            {
                if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
                }
                else
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_VIEW, player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate);
                    ChatHandler(player->GetSession()).PSendSysMessage(ACORE_STRING_MAX_RATE, individualXp.MaxRate);
                }
            }
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        if (PlayerXpRate *data = player->CustomData.Get<PlayerXpRate>("IndividualXP")) {
            CharacterDatabase.DirectExecute(
                    "REPLACE INTO `individualxp` (`CharacterGUID`, `XPRate`) VALUES ('{}', '{}');",
                    player->GetGUID().GetCounter(), data->XPRate);
        }
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (individualXp.Enabled)
        {
            if (PlayerXpRate* data = player->CustomData.Get<PlayerXpRate>("IndividualXP"))
            {
                amount = static_cast<uint32>(std::round(static_cast<float>(amount) * data->XPRate));
            }
        }
    }
};

class IndividualXPCommand : public CommandScript {
public:
    IndividualXPCommand() : CommandScript("IndividualXPCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable CharCommandTable =
                {
                        { "set",  HandleCharSetCommand,  SEC_GAMEMASTER, Console::No },
                        { "set_override",  HandleCharOverrideSetCommand,  SEC_GAMEMASTER, Console::No },
                        { "view", HandleCharViewCommand, SEC_GAMEMASTER, Console::No },

                };

        static ChatCommandTable AccountCommandTable =
                {
                        { "set_max",  HandleAccountSetMaxCommand,  SEC_GAMEMASTER, Console::No },
                        { "view_max", HandleAccountViewMaxCommand, SEC_GAMEMASTER, Console::No },
                };

        static ChatCommandTable IndividualXPCommandTable =
                {
                        { "enable",  HandleEnableCommand,  SEC_PLAYER,     Console::No },
                        { "disable", HandleDisableCommand, SEC_PLAYER,     Console::No },
                        { "view",    HandleViewCommand,    SEC_PLAYER,     Console::No },
                        { "set",     HandleSetCommand,     SEC_PLAYER,     Console::No },
                        { "default", HandleDefaultCommand, SEC_PLAYER,     Console::No },

                        { "char",    CharCommandTable },
                        { "account", AccountCommandTable },
                };

        static ChatCommandTable IndividualXPBaseTable =
                {
                        { "xp", IndividualXPCommandTable }
                };

        return IndividualXPBaseTable;
    }

    // -------------------------
    // NEW helpers
    // -------------------------
    static bool GetCharacterGuidAndAccount(std::string& name, uint32& outGuidLow, uint32& outAccountId)
    {
        if (!normalizePlayerName(name))
            return false;

        QueryResult r = CharacterDatabase.Query(
                "SELECT `guid`, `account` FROM `characters` WHERE `name`='{}'",
                name);

        if (!r)
            return false;

        Field* f = r->Fetch();
        outGuidLow = f[0].Get<uint32>();
        outAccountId = f[1].Get<uint32>();
        return true;
    }

    static bool GetAccountIdByName(std::string const& accountName, uint32& outAccountId)
    {
        QueryResult r = LoginDatabase.Query(
                "SELECT `id` FROM `account` WHERE UPPER(`username`) = UPPER('{}')",
                accountName);

        if (!r)
            return false;

        outAccountId = r->Fetch()[0].Get<uint32>();
        return true;
    }

    static float GetPersonalMaxRateForAccount(uint32 accountId)
    {
        float personalMaxRate = 2.0f;

        QueryResult r = LoginDatabase.Query(
                "SELECT `PersonalMaxXPRate` FROM `account_individualxp` WHERE `AccountGUID`='{}'",
                accountId);

        if (r)
        {
            Field* f = r->Fetch();
            personalMaxRate = static_cast<float>(f[0].Get<int>());
        }

        return personalMaxRate;
    }

    static float GetCharacterRateFromDB(uint32 guidLow)
    {
        QueryResult r = CharacterDatabase.Query(
                "SELECT `XPRate` FROM `individualxp` WHERE `CharacterGUID`='{}'",
                guidLow);

        if (!r)
            return individualXp.DefaultRate;

        return r->Fetch()[0].Get<float>();
    }

    // -------------------------
    // NEW: xp char view <name>
    // -------------------------
    static bool HandleCharViewCommand(ChatHandler* handler, std::string name)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 guidLow = 0;
        uint32 accountId = 0;

        if (!GetCharacterGuidAndAccount(name, guidLow, accountId))
        {
            handler->PSendSysMessage("Character '%s' not found.", name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        float rate = GetCharacterRateFromDB(guidLow);
        handler->PSendSysMessage("XP rate for '%s' is %.2f (default=%.2f).", name.c_str(), rate, individualXp.DefaultRate);
        handler->PSendSysMessage("Global max is %.2f.", individualXp.MaxRate);

        float personalMax = GetPersonalMaxRateForAccount(accountId);
        handler->PSendSysMessage("Account personal max for '%s' is %.2f.", name.c_str(), personalMax);

        return true;
    }

    // -------------------------
    // NEW: xp char set <name> <rate>
    // -------------------------
    static bool HandleCharSetCommand(ChatHandler* handler, std::string name, float rate)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!rate)
            return false;

        if (rate < 0.1f)
        {
            handler->PSendSysMessage(ACORE_STRING_MIN_RATE);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (rate > individualXp.MaxRate)
        {
            handler->PSendSysMessage(ACORE_STRING_MAX_RATE, individualXp.MaxRate);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 guidLow = 0;
        uint32 accountId = 0;

        if (!GetCharacterGuidAndAccount(name, guidLow, accountId))
        {
            handler->PSendSysMessage("Character '%s' not found.", name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        float personalMax = GetPersonalMaxRateForAccount(accountId);
        if (rate > personalMax)
        {
            handler->PSendSysMessage(ACORE_STRING_PERSONAL_MAX_RATE, personalMax);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Persist for offline/online character
        CharacterDatabase.DirectExecute(
                "REPLACE INTO `individualxp` (`CharacterGUID`, `XPRate`) VALUES ('{}', '{}')",
                guidLow, rate);

        // If online, update live CustomData too
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        if (Player* target = ObjectAccessor::FindPlayer(guid))
            target->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = rate;

        handler->PSendSysMessage("Set XP rate for '%s' to %.2f.", name.c_str(), rate);
        return true;
    }

    static bool HandleCharOverrideSetCommand(ChatHandler* handler, std::string name, float rate)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!rate)
            return false;

        if (rate < 0.1f)
        {
            handler->PSendSysMessage(ACORE_STRING_MIN_RATE);
            handler->SetSentErrorMessage(true);
            return false;
        }


        uint32 guidLow = 0;
        uint32 accountId = 0;

        if (!GetCharacterGuidAndAccount(name, guidLow, accountId))
        {
            handler->PSendSysMessage("Character '%s' not found.", name.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }


        // Persist for offline/online character
        CharacterDatabase.DirectExecute(
                "REPLACE INTO `individualxp` (`CharacterGUID`, `XPRate`) VALUES ('{}', '{}')",
                guidLow, rate);

        // If online, update live CustomData too
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        if (Player* target = ObjectAccessor::FindPlayer(guid))
            target->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = rate;

        handler->PSendSysMessage("Set XP rate for '%s' to %.2f.", name.c_str(), rate);
        return true;
    }

    // -------------------------
    // NEW: xp account view_max <account_name>
    // -------------------------
    static bool HandleAccountViewMaxCommand(ChatHandler* handler, std::string accountName)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = 0;
        if (!GetAccountIdByName(accountName, accountId))
        {
            handler->PSendSysMessage("Account '%s' not found.", accountName.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        float personalMax = GetPersonalMaxRateForAccount(accountId);
        handler->PSendSysMessage("Account '%s' personal max XP rate is %.2f (global max=%.2f).",
                                 accountName.c_str(), personalMax, individualXp.MaxRate);

        return true;
    }

    // -------------------------
    // NEW: xp account set_max <account_name> <rate>
    // -------------------------
    static bool HandleAccountSetMaxCommand(ChatHandler* handler, std::string accountName, float rate)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!rate)
            return false;

        if (rate < 0.1f)
        {
            handler->PSendSysMessage(ACORE_STRING_MIN_RATE);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (rate > individualXp.MaxRate)
        {
            handler->PSendSysMessage(ACORE_STRING_MAX_RATE, individualXp.MaxRate);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 accountId = 0;
        if (!GetAccountIdByName(accountName, accountId))
        {
            handler->PSendSysMessage("Account '%s' not found.", accountName.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        LoginDatabase.DirectExecute(
                "REPLACE INTO `account_individualxp` (`AccountGUID`, `PersonalMaxXPRate`) VALUES ('{}', '{}')",
                accountId, rate);

        handler->PSendSysMessage("Set account '%s' personal max XP rate to %.2f.", accountName.c_str(), rate);
        return true;
    }


    static bool HandleViewCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player *player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN)) {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;

        }
        else
        {
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_VIEW, player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate);
        }
        return true;
    }

    static bool HandleSetCommand(ChatHandler* handler, float rate)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!rate)
            return false;

        Player *player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        float personalMaxRate{2.0f};
        QueryResult result = LoginDatabase.Query(
                "SELECT `PersonalMaxXPRate` FROM `account_individualxp` WHERE `AccountGUID`='{}'",
                player->GetSession()->GetAccountId());
        if (!result) {
        } else {
            Field *fields = result->Fetch();
            personalMaxRate = static_cast<float>(fields[0].Get<int>());
        }

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else
        {
            if (rate > personalMaxRate) {
                handler->PSendSysMessage(ACORE_STRING_PERSONAL_MAX_RATE, personalMaxRate);
                handler->SetSentErrorMessage(true);
                return false;
            }
            if (rate > individualXp.MaxRate)
            {
                handler->PSendSysMessage(ACORE_STRING_MAX_RATE, individualXp.MaxRate);
                handler->SetSentErrorMessage(true);
                return false;
            }

            if (rate < 0.1f)
            {
                handler->PSendSysMessage(ACORE_STRING_MIN_RATE);
                handler->SetSentErrorMessage(true);
                return false;
            }

            player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = rate;
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_SET, rate);
            return true;
        }
    }


    static bool HandleDisableCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player *player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (!player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            // Turn Disabled On But Don't Change Value...
            player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_DISABLED);
            return true;
        }
        else
        {
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            return false;
        }
    }

    static bool HandleEnableCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return true;
        }

        Player *player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            player->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN);
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_ENABLED);
        }
        else
        {
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_RATES_DISABLED);
        }

        return true;
    }


    static bool HandleDefaultCommand(ChatHandler* handler)
    {
        if (!individualXp.Enabled)
        {
            handler->PSendSysMessage(ACORE_STRING_MODULE_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player *player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_NO_XP_GAIN))
        {
            handler->PSendSysMessage(ACORE_STRING_RATES_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }
        else
        {
            player->CustomData.GetDefault<PlayerXpRate>("IndividualXP")->XPRate = individualXp.DefaultRate;
            ChatHandler(handler->GetSession()).PSendSysMessage(ACORE_STRING_COMMAND_DEFAULT, individualXp.DefaultRate);
            return true;
        }
    }
};

void AddIndividualXPScripts() {
    new IndividualXPConf();
    new IndividualXP();
    new IndividualXPCommand();
}
