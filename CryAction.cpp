/*************************************************************************
  Crytek Source File.
  Copyright (C), Crytek Studios, 2001-2004.
 -------------------------------------------------------------------------
	$Id$
	$DateTime$

 -------------------------------------------------------------------------
  History:
  - 20:7:2004   11:07 : Created by Marco Koegler
	- 3:8:2004		16:00 : Taken-ver by Marcio Martins
	- 2005              : Changed by everyone

*************************************************************************/
#include "StdAfx.h"
#include "CryAction.h"

#if _WIN32_WINNT < 0x0500 
  #define _WIN32_WINNT 0x0500
#endif

#define CRYACTION_DEBUG_MEM   // debug memory usage
#undef  CRYACTION_DEBUG_MEM

#include <CryLibrary.h>
#include <platform_impl.h>

#include "GameRulesSystem.h"
#include "ScriptBind_ActorSystem.h"
#include "ScriptBind_ItemSystem.h"
#include "ScriptBind_Inventory.h"
#include "ScriptBind_ActionMapManager.h"
#include "Network/ScriptBind_Network.h"
#include "ScriptBind_AI.h"
//#include "ScriptBind_FlowSystem.h"
#include "ScriptBind_UI.h"
#include "ScriptBind_Action.h"
#include "ScriptBind_VehicleSystem.h"

#include "Network/GameClientChannel.h"
#include "Network/GameServerChannel.h"
#include "Network/ScriptRMI.h"
#include "Network/GameQueryListener.h"
#include "Network/GameContext.h"
#include "Network/GameServerNub.h"
#include "Network/VoiceListener.h"
#include "Network/NetworkCVars.h"
#include "Network/GameStatsConfig.h"

#include "Serialization/GameSerialize.h"

#include "DevMode.h"
#include "ActionGame.h"

#include "AIHandler.h"

#include "CryActionCVars.h"

// game object extensions
#include "Inventory.h"

#include "FlowSystem/FlowSystem.h"
#include "IVehicleSystem.h"
#include "GameTokens/GameTokenSystem.h"
#include "EffectSystem/EffectSystem.h"
#include "VehicleSystem/ScriptBind_Vehicle.h"
#include "VehicleSystem/ScriptBind_VehicleSeat.h"
#include "AnimationGraph/AnimationGraphManager.h"
#include "MaterialEffects/MaterialEffects.h"
#include "MaterialEffects/MaterialEffectsCVars.h"
#include "MaterialEffects/ScriptBind_MaterialEffects.h"
#include "MusicLogic/MusicLogic.h"
#include "GameObjects/GameObjectSystem.h"
#include "AnimationGraph/AnimationGraphManager.h"
#include "ViewSystem/ViewSystem.h"
#include "GameplayRecorder/GameplayRecorder.h"
#include "Analyst.h"

#include "Network/GameClientNub.h"

#include "DialogSystem/DialogSystem.h"
#include "DialogSystem/ScriptBind_DialogSystem.h"
#include "SubtitleManager.h"

#include "DebrisMgr/DebrisMgr.h"
#include "LevelSystem.h"
#include "ActorSystem.h"
#include "ItemSystem.h"
#include "VehicleSystem.h"
#include "ActionMapManager.h"
//#include "UISystem.h"

#include "UIDraw/UIDraw.h"
#include "GameRulesSystem.h"
#include "ActionGame.h"
#include "IGameObject.h"
#include "CallbackTimer.h"
#include "PersistantDebug.h"
#include "ITextModeConsole.h"
#include "TimeOfDayScheduler.h"
#include "Network/CVarListProcessor.h"

#include "AnimationGraph/DebugHistory.h"

#include "PlayerProfiles/PlayerProfileManager.h"
#include "PlayerProfiles/PlayerProfileImplFS.h"

#include "RemoteControl/RConServerListener.h"
#include "RemoteControl/RConClientListener.h"

#include "SimpleHttpServer/SimpleHttpServerListener.h"

#ifdef GetUserName
#undef GetUserName
#endif

#ifdef _XENON_WAT
#include "RemoteControl/GameRemoteControl.h"
#endif

#include "TimeDemo/TimeDemoRecorder.h"
#include "ViewNote.h"

#include "IAnimationGraph.h"
#include "INetworkService.h"

CCryAction * CCryAction::m_pThis = 0;
//for gamespy browser testing
static CGameQueryListener m_GQListener;

#ifndef XENON
#define DLL_INITFUNC_SYSTEM "CreateSystemInterface"
#else
#define DLL_INITFUNC_SYSTEM (LPCSTR)1
#endif

#ifdef _LIB
//If in static library
extern "C" 
{
	ISystem* CreateSystemInterface(const SSystemInitParams &initParams );
}
#endif //_LIB

//////////////////////////////////////////////////////////////////////////
struct CSystemEventListner_Action : public ISystemEventListener
{
public:
	virtual void OnSystemEvent( ESystemEvent event,UINT_PTR wparam,UINT_PTR lparam )
	{
		switch (event)
		{
		case ESYSTEM_EVENT_RANDOM_SEED:
			g_random_generator.seed(wparam);
			break;

		case ESYSTEM_EVENT_LEVEL_RELOAD:
			{
				STLALLOCATOR_CLEANUP;
				break;
			}
		}
	}
};
static CSystemEventListner_Action g_system_event_listener_action;

void CCryAction::DumpMemInfo(const char* format, ...)
{
	CryModuleMemoryInfo memInfo;
	CryGetMemoryInfoForModule(&memInfo);

	va_list args;
	va_start(args,format);
	gEnv->pSystem->GetILog()->LogV( ILog::eAlways,format,args );
	va_end(args);

	gEnv->pSystem->GetILog()->LogWithType( ILog::eAlways, "Alloc=%I64d kb  String=%I64d kb  STL-alloc=%I64d kb  STL-wasted=%I64d kb", (memInfo.allocated - memInfo.freed) >> 10 , memInfo.CryString_allocated >> 10, memInfo.STL_allocated >> 10 , memInfo.STL_wasted >> 10);
	// gEnv->pSystem->GetILog()->LogV( ILog::eAlways, "%s alloc=%llu kb  instring=%llu kb  stl-alloc=%llu kb  stl-wasted=%llu kb", text, memInfo.allocated >> 10 , memInfo.CryString_allocated >> 10, memInfo.STL_allocated >> 10 , memInfo.STL_wasted >> 10);
}


#define CALL_FRAMEWORK_LISTENERS(func) \
{ \
	*m_pGameFrameworkListenersTemp = *m_pGameFrameworkListeners; \
	TGameFrameworkListeners::iterator iter=m_pGameFrameworkListenersTemp->begin(); \
	TGameFrameworkListeners::iterator iterEnd=m_pGameFrameworkListenersTemp->end(); \
	while (iter != iterEnd) \
  { \
		const SGameFrameworkListener& listenerEntry = *iter; \
		listenerEntry.pListener->func; \
		++iter; \
	} \
}

//------------------------------------------------------------------------
CCryAction::CCryAction()
: m_paused(false),
	m_forcedpause(false),
	m_pSystem(0),
	m_pNetwork(0),
	m_p3DEngine(0),
	m_pScriptSystem(0),
	m_pEntitySystem(0),
	m_pTimer(0),
	m_pLog(0),
	m_systemDll(0),
	m_pGame(0),
	m_pLevelSystem(0),
	m_pActorSystem(0),
	m_pItemSystem(0),
	m_pVehicleSystem(0),
	m_pActionMapManager(0),
	m_pViewSystem(0),
	m_pGameplayRecorder(0),
	m_pGameplayAnalyst(0),
	m_pGameRulesSystem(0),
	m_pFlowSystem(0),
//	m_pUISystem(0),
	m_pUIDraw(0),
	m_pGameObjectSystem(0),
	m_pScriptRMI(0),
	m_pAnimationGraphManager(0),
	m_pMaterialEffects(0),
	m_pPlayerProfileManager(0),
	m_pDialogSystem(0),
	m_pDebrisMgr(0),
	m_pSubtitleManager(0),
	m_pGameTokenSystem(0),
	m_pEffectSystem(0),
	m_pGameSerialize(0),
	m_pCallbackTimer(0),
	m_pLanQueryListener(0),
	m_pDevMode(0),
	m_pTimeDemoRecorder(0),
	m_pGameQueryListener(0),
	m_pScriptA(0),
	m_pScriptIS(0),
	m_pScriptAS(0),
	m_pScriptAI(0),
	m_pScriptNet(0),
	m_pScriptAMM(0),
	m_pScriptVS(0),
	m_pScriptBindVehicle(0),
	m_pScriptBindVehicleSeat(0),
	m_pScriptInventory(0),
	m_pScriptBindDS(0),
  m_pScriptBindMFX(0),
	m_pPersistantDebug(0),
	m_pMaterialEffectsCVars(0),
	m_pEnableLoadingScreen(0),
	m_pShowLanBrowserCVAR(0),
	m_bShowLanBrowser(false),
	m_isEditing(false),
	m_bScheduleLevelEnd(false),
	m_pMusicLogic(0),
	m_pMusicGraphState(0),
	m_delayedSaveGameMethod(0),
	m_pLocalAllocs(0),
	m_bAllowSave(true),
	m_bAllowLoad(true),
  m_pGameFrameworkListeners(0),
	m_pGameFrameworkListenersTemp(0),
  m_nextFrameCommand(0),
	m_lastSaveLoad(0),
	m_pbSvEnabled(false),
	m_pbClEnabled(false),
	m_cdKeyOK(false),
	m_pStoredServerConnectionInfo(0)
{
	assert( !m_pThis );
	m_pThis = this;

	m_editorLevelName[0] = 0;
	m_editorLevelFolder[0] = 0;
	strncpy(m_gameGUID, "{00000000-0000-0000-0000-000000000000}", sizeof(m_gameGUID));
	m_gameGUID[sizeof(m_gameGUID)-1] = '\0';
}

//------------------------------------------------------------------------
CCryAction::~CCryAction()
{
	if (s_rcon_server)
		s_rcon_server->Stop();
	if (s_rcon_client)
		s_rcon_client->Disconnect();
	s_rcon_server = NULL;
	s_rcon_client = NULL;

	if (s_http_server)
		s_http_server->Stop();
	s_http_server = NULL;

	if(m_pGameplayAnalyst)
		m_pGameplayRecorder->UnregisterListener(m_pGameplayAnalyst);

	assert(0 == m_pGameFrameworkListeners->size());
  SAFE_DELETE(m_pGameFrameworkListeners);
	SAFE_DELETE(m_pGameFrameworkListenersTemp);
	EndGameContext();
	m_pEntitySystem->Reset();

	IMovieSystem *movieSys = GetISystem()->GetIMovieSystem();
	if (movieSys != NULL)
		movieSys->SetUser( NULL );

	// profile manager needs to shut down (logout users, ...)
	// while most systems are still up
	if (m_pPlayerProfileManager)
		m_pPlayerProfileManager->Shutdown();

	if (m_pDialogSystem)
		m_pDialogSystem->Shutdown();

	SAFE_RELEASE(m_pActionMapManager);
	SAFE_RELEASE(m_pActorSystem);
	SAFE_RELEASE(m_pItemSystem);
	SAFE_RELEASE(m_pLevelSystem);
	SAFE_RELEASE(m_pViewSystem);
	SAFE_RELEASE(m_pGameplayRecorder);
	SAFE_RELEASE(m_pGameplayAnalyst);
	SAFE_RELEASE(m_pGameRulesSystem);
	SAFE_RELEASE(m_pVehicleSystem);
	SAFE_DELETE(m_pMaterialEffects);
	SAFE_DELETE(m_pSubtitleManager);
	SAFE_DELETE(m_pUIDraw);
	SAFE_DELETE(m_pScriptRMI);
	SAFE_DELETE(m_pGameTokenSystem);
	SAFE_DELETE(m_pEffectSystem);
	SAFE_DELETE(m_pMusicLogic);
	SAFE_RELEASE(m_pMusicGraphState);


	SAFE_DELETE(m_pGameObjectSystem);
	SAFE_DELETE(m_pAnimationGraphManager);
	SAFE_DELETE(m_pTimeDemoRecorder);
	SAFE_RELEASE(m_pFlowSystem);
	SAFE_DELETE(m_pGameSerialize);
	SAFE_DELETE(m_pPersistantDebug);
	SAFE_DELETE(m_pPlayerProfileManager);
	SAFE_DELETE(m_pDialogSystem); // maybe delete before
	SAFE_DELETE(m_pDebrisMgr);
	SAFE_DELETE(m_pTimeOfDayScheduler);
	SAFE_DELETE(m_pLocalAllocs);

	ReleaseScriptBinds();
	ReleaseCVars();

	SAFE_DELETE(m_pDevMode);

	// having a dll handle means we did create the system interface
	// so we must release it
	if (m_systemDll)
	{
		SAFE_RELEASE(m_pSystem);

		CryFreeLibrary(m_systemDll);
		m_systemDll = 0;
	}

  SAFE_DELETE(m_nextFrameCommand);
	SAFE_DELETE(m_pCallbackTimer);

	SAFE_DELETE(m_pStoredServerConnectionInfo);

	m_pThis = 0;
}

#if 0
// TODO: REMOVE: Temporary for testing (Craig)
void CCryAction::FlowTest( IConsoleCmdArgs* args )
{
	IFlowGraphPtr pFlowGraph = GetCryAction()->m_pFlowSystem->CreateFlowGraph();
	pFlowGraph->SerializeXML( ::GetISystem()->LoadXmlFile( "Libs/FlowNodes/testflow.xml" ), true );
	GetCryAction()->m_pFlowSystem->SetActiveFlowGraph(pFlowGraph);
}
#endif

//------------------------------------------------------------------------
void CCryAction::DumpMapsCmd( IConsoleCmdArgs* args )
{
	int nlevels = GetCryAction()->GetILevelSystem()->GetLevelCount();
	if (!nlevels)
		CryLogAlways("$3No levels found!");
	else
		CryLogAlways("$3Found %d levels:", nlevels);

	for (int i=0; i<nlevels; i++)
	{
		ILevelInfo *level = GetCryAction()->GetILevelSystem()->GetLevelInfo(i);

		CryLogAlways("  %s [$9%s$o]", level->GetName(), level->GetPath());
	}
}

//------------------------------------------------------------------------
void CCryAction::UnloadCmd(IConsoleCmdArgs* args)
{
	if (::GetISystem()->IsEditor())
	{
		GameWarning( "Won't unload level in editor" );
		return;
	}

	// Free context
	gEnv->pGame->GetIGameFramework()->GetIItemSystem()->Reset();
	GetCryAction()->EndGameContext();
//	gEnv->pEntitySystem->G


	gEnv->pEntitySystem->Reset();
	//gEnv->pCharacterManager->ClearResources();

	if (gEnv->pSoundSystem)
		gEnv->pSoundSystem->Silence(true, true);
	
	GetCryAction()->GetIItemSystem()->ClearGeometryCache();
	GetCryAction()->GetIItemSystem()->ClearSoundCache();

	// Delete 3d engine resources
	gEnv->p3DEngine->UnloadLevel(true);

}

//------------------------------------------------------------------------
void CCryAction::StaticSetPbSvEnabled(IConsoleCmdArgs* pArgs)
{
	if(!m_pThis)
		return;

	if (pArgs->GetArgCount() != 2)
		GameWarning("usage: net_pb_sv_enable <true|false>");
	else
	{
		string cond = pArgs->GetArg(1);
		if (cond == "true")
			m_pThis->m_pbSvEnabled = true;
		else if (cond == "false")
			m_pThis->m_pbSvEnabled = false;
	}

#ifdef __WITH_PB__
	GameWarning("PunkBuster server will be %s for the next MP session", m_pThis->m_pbSvEnabled ? "enabled" : "disabled");
#endif
}

//------------------------------------------------------------------------
void CCryAction::StaticSetPbClEnabled(IConsoleCmdArgs* pArgs)
{
	if (pArgs->GetArgCount() != 2)
		GameWarning("usage: net_pb_cl_enable <true|false>");
	else
	{
		string cond = pArgs->GetArg(1);
		if (cond == "true")
			m_pThis->m_pbClEnabled = true;
		else if (cond == "false")
			m_pThis->m_pbClEnabled = false;
	}

#ifdef __WITH_PB__
	GameWarning("PunkBuster client will be %s for the next MP session", m_pThis->m_pbClEnabled ? "enabled" : "disabled");
#endif
}

//------------------------------------------------------------------------
void CCryAction::SvPasswordChanged(ICVar *sv_password)
{
	static bool recursing=false;
	if (recursing)
		return;

	string password=sv_password->GetString();
	if (password.empty() || password=="0")
	{
		recursing=true;
		sv_password->ForceSet("");
		recursing=false;
		password.resize(0);
	}

	if (CGameServerNub *pServerNub=CCryAction::GetCryAction()->GetGameServerNub())
		pServerNub->SetPassword(password.c_str());
	if (INetworkService *srv = gEnv->pNetwork->GetService("GameSpy"))
	{
		if(srv->GetState() == eNSS_Ok)
		{
			if(IServerReport* sr = srv->GetServerReport())
			{
				if(sr->IsAvailable())
				{
					sr->SetServerValue("password",password.empty()?"0":"1");
					sr->Update();
				}
			}
		}
	}
}

//------------------------------------------------------------------------
void CCryAction::MapCmd( IConsoleCmdArgs* args )
{
	uint32 flags = 0;

	// not available in the editor
	if (::GetISystem()->IsEditor())
	{
		GameWarning( "Won't load level in editor" );
		return;
	}

	class CParamCheck
	{
	public:
		void AddParam( const string& param ) { m_commands.insert(param); }
		const string* GetFullParam( const string& shortParam )
		{
			m_temp = m_commands;

			for (string::const_iterator pChar = shortParam.begin(); pChar != shortParam.end(); ++pChar)
			{
				typedef std::set<string>::iterator I;
				I next;
				for (I iter = m_temp.begin(); iter != m_temp.end();)
				{
					next = iter;
					++next;
					if ((*iter)[pChar - shortParam.begin()] != *pChar)
						m_temp.erase(iter);
					iter = next;
				}
			}

			const char * warning = 0;
			const string * ret = 0;
			switch (m_temp.size())
			{
			case 0:
				warning = "Unknown command %s";
				break;
			case 1:
				ret = &*m_temp.begin();
				break;
			default:
				warning = "Ambiguous command %s";
				break;
			}
			if (warning)
				GameWarning(warning, shortParam.c_str());

			return ret;
		}

	private:
		std::set<string> m_commands;
		std::set<string> m_temp;
	};

	IConsole *pConsole = gEnv->pConsole;

	// check if a map name was provided
	if (args->GetArgCount()>1)
	{
		// set sv_map
		string mapname=args->GetArg(1);
		mapname.replace("\\", "/");

#ifdef SP_DEMO
		mapname=string('i')+string('s')+string('l')+string('a')+string('n')+string('d');
#endif

		if (mapname.find("/")==string::npos)
		{
			const char *gamerules = pConsole->GetCVar("sv_gamerules")->GetString();

			int i=0;
			const char *loc=0;
			string tmp;
			while(loc=CCryAction::GetCryAction()->m_pGameRulesSystem->GetGameRulesLevelLocation(gamerules, i++))
			{
				tmp=loc;
				tmp.append(mapname);

				if (CCryAction::GetCryAction()->m_pLevelSystem->GetLevelInfo(tmp.c_str()))
				{
					mapname=tmp;
					break;
				}
			}
		}

		pConsole->GetCVar("sv_map")->Set(mapname);
	}

	string tempGameRules = pConsole->GetCVar("sv_gamerules")->GetString();

	if (const char *correctGameRules = CCryAction::GetCryAction()->m_pGameRulesSystem->GetGameRulesName(tempGameRules.c_str()))
		tempGameRules=correctGameRules;

	string tempLevel = pConsole->GetCVar("sv_map")->GetString();
	string tempDemoFile;

	if (!CCryAction::GetCryAction()->m_pLevelSystem->GetLevelInfo(tempLevel.c_str()))
	{
		GameWarning( "Couldn't find map '%s'", tempLevel.c_str() );
		return;
	}

	SGameContextParams ctx;
	ctx.gameRules = tempGameRules.c_str();
	ctx.levelName = tempLevel.c_str();

	// check if we want to run a dedicated server
	bool dedicated = false;
	bool server = false;
	bool forceNewContext = false;
	bool recording = false;

	//if running dedicated network server - default nonblocking mode
	bool blocking = true;
	if (GetCryAction()->StartedGameContext())
	{
		blocking = !::GetISystem()->IsDedicated();
	}

	if (args->GetArgCount()>2)
	{
		CParamCheck paramCheck;
		paramCheck.AddParam("dedicated");
		paramCheck.AddParam("record");
		paramCheck.AddParam("server");
		paramCheck.AddParam("nonblocking");
		paramCheck.AddParam("x");

		for (int i = 2; i < args->GetArgCount(); i++)
		{
			string param(args->GetArg(i));
			const string * pArg = paramCheck.GetFullParam(param);
			if (!pArg)
				return;
			const char* arg = pArg->c_str();

			// if 'd' or 'dedicated' is specified as a second argument we are server only
			if (!strcmp(arg, "dedicated") || !strcmp(arg, "d"))
			{
				dedicated = true;
        blocking = false;
			}
			else if (!strcmp(arg, "record"))
			{
				int j = i+1;
				if (j >= args->GetArgCount())
					continue;
				tempDemoFile = args->GetArg(j);
				i=j;

				ctx.demoRecorderFilename = tempDemoFile.c_str();

				flags |= eGSF_DemoRecorder;
				server = true; // otherwise we will not be able to create more than one GameChannel when starting DemoRecorder
			}
			else if (!strcmp(arg, "server"))
			{
				server = true;
			}
			else if (!strcmp(arg, "nonblocking"))
			{
				blocking = false;
			}
			else if (!strcmp(arg, "x"))
			{
				flags |= eGSF_ImmersiveMultiplayer;
			}
			else
			{
				GameWarning("Added parameter %s to paramCheck, but no action performed", arg);
				return;
			}
		}
	}

	if (blocking)
	{
		flags |= eGSF_BlockingClientConnect | /*eGSF_LocalOnly | eGSF_NoQueries |*/ eGSF_BlockingMapLoad;
		forceNewContext = true;
	}

	if (::GetISystem()->IsDedicated())
		dedicated = true;
	if (dedicated)
	{
//		tempLevel = "Multiplayer/" + tempLevel;
//		ctx.levelName = tempLevel.c_str();
		server = true;
	}

  if(server)
  {
    //TODO: Find a better place for that
    gEnv->pNetwork->GetService("GameSpy");
  }

	bool startedContext = false;
	// if we already have a game context, then we just change it
	if (GetCryAction()->StartedGameContext())
	{
		if (ctx.levelName)
			CCryAction::GetCryAction()->GetILevelSystem()->PrepareNextLevel( ctx.levelName );

		if (forceNewContext)
			GetCryAction()->EndGameContext();
		else
		{
			GetCryAction()->ChangeGameContext(&ctx);
			startedContext = true;
		}
	}
	if (!startedContext)
	{
		assert(!GetCryAction()->StartedGameContext());

		SGameStartParams params;
		params.flags = flags | eGSF_Server;

		if (!dedicated)
		{
			params.flags |= eGSF_Client;
			params.hostname = "localhost";
		}
		if (server)
		{
      ICVar* max_players = gEnv->pConsole->GetCVar("sv_maxplayers");
      params.maxPlayers = max_players?max_players->GetIVal():32; 
      ICVar* loading = gEnv->pConsole->GetCVar("g_enableloadingscreen");
			if(loading)
        loading->Set(0);
			//gEnv->pConsole->GetCVar("g_enableitems")->Set(0);
		}
		else
		{
			params.flags |= eGSF_LocalOnly;
			params.maxPlayers = 1;
		}

		params.pContextParams = &ctx;
		params.port = pConsole->GetCVar("sv_port")->GetIVal();

		GetCryAction()->StartGameContext(&params);
	}
}

//------------------------------------------------------------------------
void CCryAction::PlayCmd( IConsoleCmdArgs* args )
{
	IConsole *pConsole = gEnv->pConsole;

	if (GetCryAction()->StartedGameContext())
	{
		GameWarning("Must stop the game before commencing playback");
		return;
	}
	if (args->GetArgCount() < 2)
	{
		GameWarning("Usage: \\play demofile");
		return;
	}

	SGameStartParams params;
	SGameContextParams context;

	params.pContextParams = &context;
	context.demoPlaybackFilename = args->GetArg(1);
	params.maxPlayers = 1;
	params.flags = eGSF_Client | eGSF_Server | eGSF_DemoPlayback | eGSF_NoGameRules | eGSF_NoLevelLoading;
	params.port = pConsole->GetCVar("sv_port")->GetIVal();
	GetCryAction()->StartGameContext( &params );
}

//------------------------------------------------------------------------
void CCryAction::ConnectCmd( IConsoleCmdArgs* args )
{
  if(!gEnv->bServer)
  {
    if(INetChannel* pCh = GetCryAction()->GetClientChannel())
      pCh->Disconnect(eDC_UserRequested,"User left the game");
  }
  GetCryAction()->EndGameContext();

	IConsole *pConsole = gEnv->pConsole;

  //TODO find a better place for this
  gEnv->pNetwork->GetService("GameSpy");

	// check if a server address was provided
	if (args->GetArgCount()>1)
	{
		// set cl_serveraddr
		pConsole->GetCVar("cl_serveraddr")->Set(args->GetArg(1));
	}

	// check if a port was provided
	if (args->GetArgCount()>2)
	{
		// set cl_serverport
		pConsole->GetCVar("cl_serverport")->Set(args->GetArg(2));
	}

	string tempHost = pConsole->GetCVar("cl_serveraddr")->GetString();

	SGameStartParams params;
	params.flags = eGSF_Client /*| eGSF_BlockingClientConnect*/;
	params.hostname = tempHost.c_str();
	params.pContextParams = NULL;
	params.port = pConsole->GetCVar("cl_serverport")->GetIVal();

	GetCryAction()->StartGameContext(&params);
}

//------------------------------------------------------------------------
void CCryAction::DisconnectCmd( IConsoleCmdArgs* args )
{
	if(!gEnv->bServer)
  {
    if(INetChannel* pCh = GetCryAction()->GetClientChannel())
      pCh->Disconnect(eDC_UserRequested,"User left the game");
  }
  GetCryAction()->EndGameContext();
}

//------------------------------------------------------------------------
void CCryAction::StatusCmd( IConsoleCmdArgs* args )
{
	CGameServerNub * pServerNub = CCryAction::GetCryAction()->GetGameServerNub();
	if (!pServerNub)
		return;

	CryLogAlways("-----------------------------------------");
	CryLogAlways("Server Status:");
	CryLogAlways("name: %s", "");
	CryLogAlways("ip: %s", gEnv->pNetwork->GetHostName());

	CGameContext *pGameContext=CCryAction::GetCryAction()->m_pGame->GetGameContext();
	if (!pGameContext)
		return;

	CryLogAlways("level: %s", pGameContext->GetLevelName().c_str());
	CryLogAlways("gamerules: %s", pGameContext->GetRequestedGameRules().c_str());
	CryLogAlways("players: %d/%d", pServerNub->GetPlayerCount(), pServerNub->GetMaxPlayers());

	if (IGameRules * pRules = CCryAction::GetCryAction()->GetIGameRulesSystem()->GetCurrentGameRules())
		pRules->ShowStatus();

	if (pServerNub->GetPlayerCount()<1)
		return;

	CryLogAlways("\n-----------------------------------------");
	CryLogAlways("Connection Status:");

	TServerChannelMap * pChannelMap = pServerNub->GetServerChannelMap();
	for (TServerChannelMap::iterator iter = pChannelMap->begin(); iter != pChannelMap->end(); ++iter)
	{
		const char *name="";
		IActor *pActor=CCryAction::GetCryAction()->m_pActorSystem->GetActorByChannelId(iter->first);

		if (pActor)
			name=pActor->GetEntity()->GetName();

		INetChannel *pNetChannel = iter->second->GetNetChannel();
		const char *ip=pNetChannel->GetName();
		int	ping=(int)(pNetChannel->GetPing(true)*1000);
		int state=pNetChannel->GetChannelConnectionState();
		int profileId = pNetChannel->GetProfileId();

		CryLogAlways("name: %s  id: %d  ip: %s  ping: %d  state: %d profile: %d", name, iter->first, ip, ping, state, profileId);
	}
}

//------------------------------------------------------------------------
void CCryAction::GenStringsSaveGameCmd( IConsoleCmdArgs * pArgs )
{
	GetCryAction()->m_pGameSerialize->SaveGame( GetCryAction(), "string-extractor", "SaveGameStrings.cpp", eSGR_Command );
}

//------------------------------------------------------------------------
void CCryAction::SaveGameCmd( IConsoleCmdArgs* args )
{
	string sSavePath(PathUtil::GetGameFolder());
	if(args->GetArgCount() > 1)
	{
		sSavePath.append("/");
		sSavePath.append(args->GetArg(1));
		sSavePath = PathUtil::ReplaceExtension(sSavePath, "CRYSISJMSF");
		GetCryAction()->SaveGame( sSavePath, false, false, eSGR_QuickSave, true );
	}
	else
	{
		//sSavePath.append("/quicksave.CRYSISJMSF"); //because of the french law we can't do this ...
		sSavePath.append("/nomad.CRYSISJMSF");
		GetCryAction()->SaveGame( sSavePath, true, false);
	}
}

//------------------------------------------------------------------------
void CCryAction::LoadGameCmd( IConsoleCmdArgs* args )
{
	if(args->GetArgCount() > 1)
	{
		bool quick = args->GetArgCount() > 2;
		GetCryAction()->LoadGame( args->GetArg(1), quick);
	}
	else
	{
		//GetCryAction()->LoadGame("quicksave.CRYSISJMSF", true); //because of the french law we can't do this ...
		GetCryAction()->LoadGame("nomad.CRYSISJMSF", true);
	}
}


void CCryAction::KickPlayerCmd( IConsoleCmdArgs* pArgs )
{
  if(!gEnv->bServer)
  {
    CryLog("This only usable on server");
    return;
  }
  if(pArgs->GetArgCount() > 1)
  {
		string name(pArgs->GetArg(1));
		for(int x=2; x<pArgs->GetArgCount(); ++x)
		{
			name.append(" ");
			name.append(pArgs->GetArg(x));
		}
    IEntity* pEntity = gEnv->pEntitySystem->FindEntityByName(name);
    if(pEntity)
    {
      IActor* pActor = GetCryAction()->GetIActorSystem()->GetActor(pEntity->GetId());
      if(pActor && pActor->IsPlayer())
      {
        if(pActor != GetCryAction()->GetClientActor())
          GetCryAction()->GetServerNetNub()->DisconnectPlayer(eDC_Kicked,pActor->GetEntityId(),"Kicked from server");
        else
          CryLog("Cannot kick local player");
      }
      else
        CryLog("%s not a player.",pArgs->GetArg(1));
    }
    else
      CryLog("Player not found");
  }
  else
    CryLog("Usage: kick player_name");
}

void CCryAction::KickPlayerByIdCmd(IConsoleCmdArgs* pArgs)
{
    if(!gEnv->bServer)
  {
    CryLog("This only usable on server");
    return;
  }

  if(pArgs->GetArgCount() > 1)
  {    
    int id = atoi(pArgs->GetArg(1));

		CGameServerNub * pServerNub = CCryAction::GetCryAction()->GetGameServerNub();
		if (!pServerNub)
			return;

		TServerChannelMap * pChannelMap = pServerNub->GetServerChannelMap();
		TServerChannelMap::iterator it = pChannelMap->find(id);
		if(it!=pChannelMap->end() && (!GetCryAction()->GetClientActor() || GetCryAction()->GetClientActor() != GetCryAction()->m_pActorSystem->GetActorByChannelId(id)) )
		{
			it->second->GetNetChannel()->Disconnect(eDC_Kicked,"Kicked from server");
		}
		else
		{
			CryLog("Player with id %d not found",id);
		}
  }
  else
    CryLog("Usage: kickid player_id");
}

void CCryAction::BanPlayerCmd(IConsoleCmdArgs* args)
{
	if(args->GetArgCount() > 1)
	{    
		int id = atoi(args->GetArg(1));
		CGameServerNub * pServerNub = CCryAction::GetCryAction()->GetGameServerNub();
		if (!pServerNub)
			return;
		TServerChannelMap * pChannelMap = pServerNub->GetServerChannelMap();
		for(TServerChannelMap::iterator it = pChannelMap->begin();it!= pChannelMap->end();++it)
		{
			if(it->second->GetNetChannel()->GetProfileId() == id)
			{
				pServerNub->BanPlayer(it->first,"Banned from server");
				break;
			}
		}
		CryLog("Player with profileid %d not found",id);
	}
	else
	{
		CryLog("Usage: ban profileid");
	}
}

void CCryAction::BanStatusCmd(IConsoleCmdArgs* args)
{
	if(CCryAction::GetCryAction()->GetGameServerNub())
		CCryAction::GetCryAction()->GetGameServerNub()->BannedStatus();
}

void CCryAction::UnbanPlayerCmd(IConsoleCmdArgs* args)
{
	if(args->GetArgCount() > 1)
	{    
		int id = atoi(args->GetArg(1));
		CGameServerNub * pServerNub = CCryAction::GetCryAction()->GetGameServerNub();
		if (!pServerNub)
			return;
		pServerNub->UnbanPlayer(id);
	}
	else
	{
		CryLog("Usage: ban_remove profileid");
	}
}

void CCryAction::OpenURLCmd(IConsoleCmdArgs* args)
{
  if(args->GetArgCount()>1)
    GetCryAction()->ShowPageInBrowser(args->GetArg(1));
}

void CCryAction::DumpAnalysisStatsCmd(IConsoleCmdArgs* args)
{
	if(CCryAction::GetCryAction()->m_pGameplayAnalyst)
		CCryAction::GetCryAction()->m_pGameplayAnalyst->DumpToTXT();
}

//------------------------------------------------------------------------
ISimpleHttpServer* CCryAction::s_http_server = NULL;

#define HTTP_DEFAULT_PORT 80

//------------------------------------------------------------------------
void CCryAction::http_startserver(IConsoleCmdArgs* args)
{
	if (args->GetArgCount() > 3)
	{
		GameWarning("usage: http_startserver [port:<port>] [pass:<pass>] (no spaces or tabs allowed for individule argument)");
		return;
	}

	if (s_http_server)
	{
		GameWarning("HTTP server already started");
		return;
	}

	uint16 port = HTTP_DEFAULT_PORT; string http_password;
	int nargs = args->GetArgCount();
	for (int i = 1; i < nargs; ++i)
	{
		string arg(args->GetArg(i));
		string head = arg.substr(0, 5), body = arg.substr(5);
		if (head == "port:")
		{
			int pt = atoi(body);
			if (pt <= 0 || pt > 65535)
				GameWarning("Invalid port specified, default port will be used");
			else
				port = (uint16)pt;
		}
		else if (head == "pass:")
		{
			http_password = body;
		}
		else
		{
			GameWarning("usage: http_startserver [port:<port>] [pass:<pass>] (no spaces or tabs allowed for individule argument)");
			return;
		}
	}

	s_http_server = gEnv->pNetwork->GetSimpleHttpServerSingleton();
	s_http_server->Start(port, http_password, &CSimpleHttpServerListener::GetSingleton(s_http_server));
}

//------------------------------------------------------------------------
void CCryAction::http_stopserver(IConsoleCmdArgs* args)
{
	if (args->GetArgCount() != 1)
		GameWarning("usage: http_stopserver (no parameters) - continue anyway ...");

	if (s_http_server == NULL)
	{
		GameWarning("HTTP: server not started");
		return;
	}

	s_http_server->Stop();
	s_http_server = NULL;

	gEnv->pLog->LogToConsole("HTTP: server stopped");
}

//------------------------------------------------------------------------
IRemoteControlServer* CCryAction::s_rcon_server = NULL;
IRemoteControlClient* CCryAction::s_rcon_client = NULL;

CRConClientListener* CCryAction::s_rcon_client_listener = NULL;

//string CCryAction::s_rcon_password;

#define RCON_DEFAULT_PORT 9999

//------------------------------------------------------------------------
//void CCryAction::rcon_password(IConsoleCmdArgs* args)
//{
//	if (args->GetArgCount() > 2)
//	{
//		GameWarning("usage: rcon_password [password] (password cannot contain spaces or tabs");
//		return;
//	}
//
//	if (args->GetArgCount() == 1)
//	{
//		if (s_rcon_password.empty())
//			gEnv->pLog->LogToConsole("RCON system password has not been set");
//		else
//			gEnv->pLog->LogToConsole("RCON system password has been set (not displayed for security reasons)");
//		return;
//	}
//
//	if (args->GetArgCount() == 2)
//	{
//		s_rcon_password = args->GetArg(1);
//	}
//}

//------------------------------------------------------------------------
void CCryAction::rcon_startserver(IConsoleCmdArgs* args)
{
	if (args->GetArgCount() > 3)
	{
		GameWarning("usage: rcon_startserver [port:<port>] [pass:<pass>] (no spaces or tabs allowed for individule argument)");
		return;
	}

	if (s_rcon_server)
	{
		GameWarning("RCON server already started");
		return;
	}

	uint16 port = RCON_DEFAULT_PORT; string password;
	int nargs = args->GetArgCount();
	for (int i = 1; i < nargs; ++i)
	{
		string arg(args->GetArg(i));
		string head = arg.substr(0, 5), body = arg.substr(5);
		if (head == "port:")
		{
			int pt = atoi(body);
			if (pt <= 0 || pt > 65535)
				GameWarning("Invalid port specified, default port will be used");
			else
				port = (uint16)pt;
		}
		else if (head == "pass:")
		{
			password = body;
		}
		else
		{
			GameWarning("usage: rcon_startserver [port:<port>] [pass:<pass>] (no spaces or tabs allowed for individule argument)");
			return;
		}
	}

	s_rcon_server = gEnv->pNetwork->GetRemoteControlSystemSingleton()->GetServerSingleton();
	s_rcon_server->Start( port, password, &CRConServerListener::GetSingleton(s_rcon_server) );
}

//------------------------------------------------------------------------
void CCryAction::rcon_stopserver(IConsoleCmdArgs* args)
{
	if (args->GetArgCount() != 1)
		GameWarning("usage: rcon_stopserver (no parameters) - continue anyway ...");

	if (s_rcon_server == NULL)
	{
		GameWarning("RCON: server not started");
		return;
	}

	s_rcon_server->Stop();
	s_rcon_server = NULL;
	//s_rcon_password = "";

	gEnv->pLog->LogToConsole("RCON: server stopped");
}

//------------------------------------------------------------------------
void CCryAction::rcon_connect(IConsoleCmdArgs* args)
{
	if (args->GetArgCount() > 4)
	{
		GameWarning("usage: rcon_connect [addr:<addr>] [port:<port>] [pass:<pass>]");
		return;
	}

	if (s_rcon_client != NULL)
	{
		GameWarning("RCON client already started");
		return;
	}

	uint16 port = RCON_DEFAULT_PORT; string addr = "127.0.0.1"; string password;
	int nargs = args->GetArgCount();
	for (int i = 1; i < nargs; ++i)
	{
		string arg(args->GetArg(i));
		string head = arg.substr(0, 5), body = arg.substr(5);
		if (head == "port:")
		{
			int pt = atoi(body);
			if (pt <= 0 || pt > 65535)
				GameWarning("Invalid port specified, default port will be used");
			else
				port = (uint16)pt;
		}
		else if (head == "pass:")
		{
			password = body;
		}
		else if (head == "addr:")
		{
			addr = body;
		}
		else
		{
			GameWarning("usage: rcon_connect [addr:<addr>] [port:<port>] [pass:<pass>]");
			return;
		}
	}

	s_rcon_client = gEnv->pNetwork->GetRemoteControlSystemSingleton()->GetClientSingleton();
	s_rcon_client_listener = &CRConClientListener::GetSingleton(s_rcon_client);
	s_rcon_client->Connect(addr, port, password, s_rcon_client_listener);
}

//------------------------------------------------------------------------
void CCryAction::rcon_disconnect(IConsoleCmdArgs* args)
{
	if (args->GetArgCount() != 1)
		GameWarning("usage: rcon_disconnect (no parameters) - continue anyway ...");

	if (s_rcon_client == NULL)
	{
		GameWarning("RCON client not started");
		return;
	}

	s_rcon_client->Disconnect();
	s_rcon_client = NULL;
	//s_rcon_password = "";

	gEnv->pLog->LogToConsole("RCON: client disconnected");
}

//------------------------------------------------------------------------
void CCryAction::rcon_command(IConsoleCmdArgs* args)
{
	if (s_rcon_client == NULL || !s_rcon_client_listener->IsSessionAuthorized())
	{
		GameWarning("RCON: cannot issue commands unless the session is authorized");
		return;
	}

	if (args->GetArgCount() < 2)
	{
		GameWarning("usage: rcon_command [console_command arg1 arg2 ...]");
		return;
	}

	string command; int nargs = args->GetArgCount();
	for (int i = 1; i < nargs; ++i)
	{
		command += args->GetArg(i);
		command += " "; // a space in between
	}

	uint32 cmdid = s_rcon_client->SendCommand(command);
	if (0 == cmdid)
		GameWarning("RCON: failed sending RCON command %s to server", command.c_str());
	else
		gEnv->pLog->LogToConsole("RCON: command [%08x]%s is sent to server for execution", cmdid, command.c_str());
}

//------------------------------------------------------------------------
bool CCryAction::Init(SSystemInitParams &startupParams)
{
	m_pSystem = startupParams.pSystem;

	if (!startupParams.pSystem)
	{
#if !defined(_LIB) && !defined(LINUX) && !defined(PS3)
		m_systemDll = CryLoadLibrary("crysystem.dll");

		if (!m_systemDll)
		{
			return false;
		}
		PFNCREATESYSTEMINTERFACE CreateSystemInterface = (PFNCREATESYSTEMINTERFACE)CryGetProcAddress(m_systemDll, DLL_INITFUNC_SYSTEM);
#endif _LIB

		// initialize the system
		m_pSystem = CreateSystemInterface(startupParams);

		if (!m_pSystem)
		{
			return false;
		}
	}
	else
	{
		if (*startupParams.szUserPath)
			startupParams.pSystem->ChangeUserPath(startupParams.szUserPath);
	}
	ModuleInitISystem(m_pSystem); // Needed by GetISystem();

	m_pSystem->GetISystemEventDispatcher()->RegisterListener( &g_system_event_listener_action );

	// fill in interface pointers
	m_pNetwork = m_pSystem->GetINetwork();
	m_p3DEngine = m_pSystem->GetI3DEngine();
	m_pScriptSystem = m_pSystem->GetIScriptSystem();
	m_pEntitySystem = m_pSystem->GetIEntitySystem();
	m_pTimer = m_pSystem->GetITimer();
	m_pLog = m_pSystem->GetILog();

	ReadCDKey();

#ifdef CRYACTION_DEBUG_MEM
	DumpMemInfo("CryAction::Init Start");
#endif

	InitCVars();
	InitCommands();

	if (m_pSystem->IsDevMode())
		m_pDevMode = new CDevMode();

	m_pTimeDemoRecorder = new CTimeDemoRecorder( m_pSystem );

	CScriptRMI::RegisterCVars();
	CGameObject::CreateCVars();
	m_pScriptRMI = new CScriptRMI();

//	m_pSystem->GetIProfileSystem()->Enable( true, false );

	// initialize subsystems
	m_pGameTokenSystem = new CGameTokenSystem;
	m_pEffectSystem = new CEffectSystem;
	m_pEffectSystem->Init();

	m_pUIDraw = new CUIDraw;
	m_pLevelSystem = new CLevelSystem(m_pSystem, "levels");

	m_pActorSystem = new CActorSystem(m_pSystem, m_pEntitySystem);
	m_pItemSystem = new CItemSystem(this, m_pSystem);
	m_pActionMapManager = new CActionMapManager(m_pSystem->GetIInput());
	m_pViewSystem = new CViewSystem(m_pSystem);
	m_pGameplayRecorder = new CGameplayRecorder(this);
	//m_pGameplayAnalyst = new CGameplayAnalyst();
	m_pGameRulesSystem = new CGameRulesSystem(m_pSystem,this);
	m_pVehicleSystem = new CVehicleSystem( m_pSystem, m_pEntitySystem );

	m_pNetworkCVars = new CNetworkCVars();
	m_pCryActionCVars = new CCryActionCVars();

	m_pGameObjectSystem = new CGameObjectSystem();
	if (!m_pGameObjectSystem->Init())
		return false;
	else
	{
		// init game object events of CryAction
		m_pGameObjectSystem->RegisterEvent(eGFE_PauseGame, "PauseGame");
		m_pGameObjectSystem->RegisterEvent(eGFE_ResumeGame, "ResumeGame");
		m_pGameObjectSystem->RegisterEvent(eGFE_OnCollision, "OnCollision");
		m_pGameObjectSystem->RegisterEvent(eGFE_OnPostStep, "OnPostStep");
		m_pGameObjectSystem->RegisterEvent(eGFE_OnStateChange, "OnStateChange");
		m_pGameObjectSystem->RegisterEvent(eGFE_ResetAnimationGraphs, "ResetAnimationGraphs");
		m_pGameObjectSystem->RegisterEvent(eGFE_OnBreakable2d, "OnBreakable2d");
		m_pGameObjectSystem->RegisterEvent(eGFE_OnBecomeVisible, "OnBecomeVisible");
		m_pGameObjectSystem->RegisterEvent(eGFE_PreFreeze, "PreFreeze");
		m_pGameObjectSystem->RegisterEvent(eGFE_PreShatter, "PreShatter");
		m_pGameObjectSystem->RegisterEvent(eGFE_BecomeLocalPlayer, "BecomeLocalPlayer");
		m_pGameObjectSystem->RegisterEvent(eGFE_DisablePhysics, "DisablePhysics");
		m_pGameObjectSystem->RegisterEvent(eGFE_EnablePhysics, "EnablePhysics");
	}

	m_pAnimationGraphManager = new CAnimationGraphManager();
	m_pGameSerialize = new CGameSerialize();
	m_pCallbackTimer = new CCallbackTimer();
	m_pPersistantDebug = new CPersistantDebug();
#if !defined(XENON) && !defined(PS3)
	m_pPlayerProfileManager = new CPlayerProfileManager(new CPlayerProfileImplFSDir());
#else
//  m_pPlayerProfileManager = new CPlayerProfileManager(new CPlayerProfileImplFS());
#endif
	m_pDialogSystem = new CDialogSystem();
	m_pDialogSystem->Init();
	m_pDebrisMgr = new CDebrisMgr;
	m_pTimeOfDayScheduler = new CTimeOfDayScheduler();
	m_pSubtitleManager = new CSubtitleManager();

	IMovieSystem *movieSys = GetISystem()->GetIMovieSystem();
	if (movieSys != NULL)
		movieSys->SetUser( m_pViewSystem );

	m_pVehicleSystem->Init();

	REGISTER_FACTORY((IGameFramework*)this, "Inventory", CInventory, false);

	if (m_pLevelSystem)
		m_pLevelSystem->AddListener(m_pItemSystem);

	InitScriptBinds();

	// m_pGameRulesSystem = new CGameRulesSystem(m_pSystem, this);

	// TODO: temporary testing stuff
//	gEnv->pConsole->AddCommand( "flow_test", FlowTest );

#ifdef _XENON_WAT
	InitGameRemoteControl();
#endif

	m_pLocalAllocs = new SLocalAllocs();

#if 0
	BeginLanQuery();
#endif

	if (m_pVehicleSystem)
		m_pVehicleSystem->RegisterVehicles(this);
	if (m_pGameObjectSystem)
		m_pGameObjectSystem->RegisterFactories(this);
	if (m_pGameSerialize)
		m_pGameSerialize->RegisterFactories(this);
	CGameContext::RegisterExtensions(this);

	// Player profile stuff
	if (m_pPlayerProfileManager)
	{
		bool ok = m_pPlayerProfileManager->Initialize();
		if (!ok)
			GameWarning("[PlayerProfiles] CCryAction::Init: Cannot initialize PlayerProfileManager");
	}

#ifdef CRYACTION_DEBUG_MEM
	DumpMemInfo("CryAction::Init End");
#endif

  m_pGameFrameworkListeners = new TGameFrameworkListeners();
	m_pGameFrameworkListenersTemp = new TGameFrameworkListeners();
	m_pGameFrameworkListenersTemp->reserve(16);

  m_nextFrameCommand = new string();

	m_pGameStatsConfig = new CGameStatsConfig();
	m_pGameStatsConfig->ReadConfig();

	return true;
}
//------------------------------------------------------------------------
bool CCryAction::CompleteInit()
{
#ifdef CRYACTION_DEBUG_MEM
	DumpMemInfo("CryAction::CompleteInit Start");
#endif

	m_pAnimationGraphManager->RegisterFactories(this);
#if 1
	if (IAnimationGraphPtr pMusicGraph = m_pAnimationGraphManager->LoadGraph( "MusicLogic.xml", false, true ))
		m_pMusicGraphState = pMusicGraph->CreateState();
	if (!m_pMusicGraphState)
		GameWarning("Unable to load music logic graph");
#endif

	m_pMusicLogic = new CMusicLogic(m_pMusicGraphState);

	if (m_pMusicLogic)
		m_pMusicLogic->Init();

	EndGameContext();

	SAFE_DELETE(m_pFlowSystem);
	m_pFlowSystem = new CFlowSystem();
	m_pSystem->SetIFlowSystem(m_pFlowSystem);
	m_pSystem->SetIAnimationGraphSystem(m_pAnimationGraphManager);
	m_pSystem->SetIDialogSystem(m_pDialogSystem);

	if(m_pGameplayAnalyst)
		m_pGameplayRecorder->RegisterListener(m_pGameplayAnalyst);

	m_pMaterialEffects = new CMaterialEffects();
  m_pScriptBindMFX = new CScriptBind_MaterialEffects( m_pSystem, m_pMaterialEffects );
	// load spreadsheet and fx libraries (but not flowgraphs)
	m_pMaterialEffects->Load("MaterialEffects.xml");
  m_pSystem->SetIMaterialEffects(m_pMaterialEffects);

	// in pure game mode we load the equipment packs from disk
	// in editor mode, this is done in GameEngine.cpp
	if (gEnv->pSystem->IsEditor() == false)
	{
		m_pItemSystem->GetIEquipmentManager()->DeleteAllEquipmentPacks();
		m_pItemSystem->GetIEquipmentManager()->LoadEquipmentPacksFromPath("Libs/EquipmentPacks");
	}

	if (IGame* pGame = gEnv->pGame)
	{
		pGame->CompleteInit();
	}
	
	// load flowgraphs (done after Game has initialized, because it might add additional flownodes)
	if (m_pMaterialEffects)
		m_pMaterialEffects->LoadFlowGraphLibs();

	if (!m_pScriptRMI->Init())
		return false;

	// after everything is initialized, run our main script
	m_pScriptSystem->ExecuteFile("scripts/main.lua");
	m_pScriptSystem->BeginCall("OnInit");
	m_pScriptSystem->EndCall();

#ifdef CRYACTION_DEBUG_MEM
	DumpMemInfo("CryAction::CompleteInit End");
#endif

 	return true;
}

//------------------------------------------------------------------------
void CCryAction::InitScriptBinds()
{
	m_pScriptNet = new CScriptBind_Network(m_pSystem, this);
	m_pScriptA = new CScriptBind_Action(this);
	m_pScriptIS = new CScriptBind_ItemSystem(m_pSystem, m_pItemSystem, this);
	m_pScriptAS = new CScriptBind_ActorSystem(m_pSystem, this);
	m_pScriptAI = gEnv->pAISystem? new CScriptBind_AI(m_pSystem) : 0;
	m_pScriptAMM = new CScriptBind_ActionMapManager(m_pSystem, m_pActionMapManager);
	m_pScriptVS = new CScriptBind_VehicleSystem(m_pSystem, m_pVehicleSystem);
	m_pScriptBindVehicle = new CScriptBind_Vehicle( m_pSystem, this );
	m_pScriptBindVehicleSeat = new CScriptBind_VehicleSeat( m_pSystem, this );
	m_pScriptInventory = new CScriptBind_Inventory( m_pSystem, this );
	m_pScriptBindDS = new CScriptBind_DialogSystem( m_pSystem, m_pDialogSystem);  
	//	m_pScriptFS = new CScriptBind_FlowSystem(m_pSystem, m_pFlowSystem);
}

//------------------------------------------------------------------------
void CCryAction::ReleaseScriptBinds()
{
	// before we release script binds call out main "OnShutdown"
	m_pScriptSystem->BeginCall("OnShutdown");
	m_pScriptSystem->EndCall();

	SAFE_RELEASE(m_pScriptA);
	SAFE_RELEASE(m_pScriptIS);
	SAFE_RELEASE(m_pScriptAS);
	SAFE_RELEASE(m_pScriptAI);
	SAFE_RELEASE(m_pScriptAMM);
	//SAFE_RELEASE(m_pScriptFS);
	SAFE_RELEASE(m_pScriptVS);
	SAFE_RELEASE(m_pScriptNet);
	SAFE_RELEASE(m_pScriptBindVehicle);
	SAFE_RELEASE(m_pScriptBindVehicleSeat);
	SAFE_RELEASE(m_pScriptInventory);
	SAFE_RELEASE(m_pScriptBindDS);
  SAFE_RELEASE(m_pScriptBindMFX);
}

//------------------------------------------------------------------------
void CCryAction::Shutdown()
{
	// we are not dynamically allocated (see Main.cpp)... therefore
	// we must not call delete here (it will cause big problems)...
	// call the destructor manually instead
	this->~CCryAction();
}

//------------------------------------------------------------------------
f32 g_fPrintLine=0.0f;

bool CCryAction::PreUpdate(bool haveFocus, unsigned int updateFlags)
{

	g_fPrintLine=10.0f;

  if(!m_nextFrameCommand->empty())
  {
    gEnv->pConsole->ExecuteString(*m_nextFrameCommand);
    m_nextFrameCommand->resize(0);
  }

	CheckEndLevelSchedule();

	if (ITextModeConsole * pTextModeConsole = gEnv->pSystem->GetITextModeConsole())
		pTextModeConsole->BeginDraw();

/*
	IRenderer * pRend = gEnv->pRenderer;
	float white[4] = {1,1,1,1};
	pRend->Draw2dLabel( 10, 10, 3, white, false, "TIME: %f", m_pSystem->GetITimer()->GetFrameStartTime().GetSeconds() );
*/
	bool gameRunning = IsGameStarted();

	bool bGameIsPaused = !gameRunning || IsGamePaused(); // slightly different from m_paused (check's gEnv->pTimer as well)
	if (m_pTimeDemoRecorder && !IsGamePaused())
		m_pTimeDemoRecorder->PreUpdate();

	// TODO: Craig - this probably needs to be updated after CSystem::Update
	// update the callback system
	if (!bGameIsPaused)
		m_pCallbackTimer->UpdateTimer();

	bool bRetRun=true;

	if (!(updateFlags&ESYSUPDATE_EDITOR))
		m_pSystem->GetIProfileSystem()->StartFrame();

	m_pSystem->RenderBegin();

	float frameTime(m_pSystem->GetITimer()->GetFrameTime());

	// when we are updated by the editor, we should not update the system
	if (!(updateFlags & ESYSUPDATE_EDITOR))
	{
		int updateLoopPaused = (!gameRunning || m_paused) ? 1 : 0;
		if (gEnv->bMultiplayer && !gEnv->bServer)
			updateLoopPaused = 0;


		const bool bGameWasPaused = bGameIsPaused;

		bRetRun=m_pSystem->Update(updateFlags, updateLoopPaused);

		// notify listeners
		OnActionEvent(SActionEvent(eAE_earlyPreUpdate));

		// during m_pSystem->Update call the Game might have been paused or un-paused
		gameRunning = IsGameStarted();
		bGameIsPaused = !gameRunning || IsGamePaused();

		// if the Game had been paused and is now unpaused, don't update viewsystem until next frame
		// if the Game had been unpaused and is now paused, don't update viewsystem until next frame
		if (!bGameIsPaused && !bGameWasPaused) // don't update view if paused
			if (m_pViewSystem)
				m_pViewSystem->Update(min(frameTime,0.1f));

		if (!bGameIsPaused && !bGameWasPaused) // don't update gameplayrecorder if paused
			if (m_pGameplayRecorder)
				m_pGameplayRecorder->Update(frameTime);

		if (!bGameIsPaused && gameRunning)
		{
			if (m_pFlowSystem)
			{
				m_pFlowSystem->Update();
			}
		}
	}

	m_pActionMapManager->Update();

	if (!bGameIsPaused)
	{
		m_pItemSystem->Update();

	//	m_pUISystem->PreUpdate(frameTime, m_pSystem->GetIRenderer()->GetFrameID(false));

		if (m_pMaterialEffects)
			m_pMaterialEffects->Update(frameTime);

		if (m_pDialogSystem)
			m_pDialogSystem->Update(frameTime);

		if (m_pMusicLogic)
			m_pMusicLogic->Update();

		if (m_pMusicGraphState)
			m_pMusicGraphState->Update();

		if (m_pDebrisMgr)
			m_pDebrisMgr->Update();
	}

//	if (INetworkServicePtr pSvc = gEnv->pNetwork->GetService("GameSpy"))
//		CryLogAlways("GameSpy: %d", pSvc->GetState());

	CRConServerListener::GetSingleton().Update();
	CSimpleHttpServerListener::GetSingleton().Update();

	return bRetRun;
}

//------------------------------------------------------------------------
void CCryAction::PostUpdate(bool haveFocus, unsigned int updateFlags)
{
	if(m_pShowLanBrowserCVAR->GetIVal() == 0)
	{
		if(m_bShowLanBrowser) 
		{
			m_bShowLanBrowser = false;
			EndCurrentQuery();
			m_pLanQueryListener = 0;
		}
	}
	else
	{
		if(!m_bShowLanBrowser)
		{
			m_bShowLanBrowser = true;
			BeginLanQuery();
		}
	}

	float delta = m_pSystem->GetITimer()->GetFrameTime();
	const bool bGameIsPaused = IsGamePaused(); // slightly different from m_paused (check's gEnv->pTimer as well)

	if (!bGameIsPaused)
	{
		if(m_pEffectSystem)
			m_pEffectSystem->Update(delta);

		if(m_lastSaveLoad)
		{
			m_lastSaveLoad -= delta;
			if(m_lastSaveLoad < 0.0f)
				m_lastSaveLoad = 0.0f;
		}
	}

	m_pSystem->Render();

	CALL_FRAMEWORK_LISTENERS(OnPostUpdate(delta));

//#ifndef XENON
//	m_pUISystem->PostUpdate(delta, m_pSystem->GetIRenderer()->GetFrameID(false));
//#endif
	if (m_pPersistantDebug)
		m_pPersistantDebug->PostUpdate(delta);

	m_pGameObjectSystem->PostUpdate(delta);

	m_pSystem->RenderEnd();

	if (m_pGame)
	{
		if (m_pGame->Update())
		{
			m_pGame = 0;
		}
	}

	if (CGameServerNub *pServerNub=GetGameServerNub())
		pServerNub->Update();

	if (m_pTimeDemoRecorder && !IsGamePaused())
		m_pTimeDemoRecorder->Update();

	if (!(updateFlags&ESYSUPDATE_EDITOR))
		m_pSystem->GetIProfileSystem()->EndFrame();

	if (m_delayedSaveGameMethod != 0 && m_pLocalAllocs)
	{
		const bool quick = m_delayedSaveGameMethod == 1 ? true : false;
		SaveGame(m_pLocalAllocs->m_delayedSaveGameName, quick, true, m_delayedSaveGameReason, false, m_pLocalAllocs->m_checkPointName.c_str());
		m_delayedSaveGameMethod = 0;
		m_pLocalAllocs->m_delayedSaveGameName.assign ("");
	}


	if (ITextModeConsole * pTextModeConsole = gEnv->pSystem->GetITextModeConsole())
		pTextModeConsole->EndDraw();

	CGameObject::UpdateSchedulingProfiles();
}

void CCryAction::Reset(bool clients)
{
	CGameContext * pGC = GetGameContext();
	if (pGC && pGC->HasContextFlag(eGSF_Server))
	{
		pGC->ResetMap(true);
		if(gEnv->bServer && GetGameServerNub())
			GetGameServerNub()->ResetOnHoldChannels();
	}

	if(m_pGameplayRecorder)
		m_pGameplayRecorder->Event(0, GameplayEvent(eGE_GameReset));

	// reset MusicLogic
	if (m_pMusicLogic)
		m_pMusicLogic->Stop();
}

void CCryAction::PauseGame(bool pause, bool force)
{
	// we should generate some events here
	// who is up to receive them ?

 	if (!force && m_paused && m_forcedpause)
	{
		return;
	}

	if (m_paused != pause || m_forcedpause != force)
	{
		gEnv->pTimer->PauseTimer(ITimer::ETIMER_GAME, pause);

		// no game input should happen during pause
		m_pActionMapManager->Enable(!pause);
		// sounds should pause / stop
		if(gEnv->pSoundSystem)
			gEnv->pSoundSystem->Pause(pause);

		if(pause && gEnv->pInput)	//disable rumble
			gEnv->pInput->ForceFeedbackEvent( SFFOutputEvent(eDI_XI, eFF_Rumble_Basic, 0.0f, 0.0f, 0.0f) );

		gEnv->p3DEngine->GetTimeOfDay()->SetPaused(pause);

		// pause EntitySystem Timers
		gEnv->pEntitySystem->PauseTimers(pause);

		m_paused = pause;
		m_forcedpause = force;

		if (gEnv->pMovieSystem)
		{
			if (pause)
				gEnv->pMovieSystem->Pause();
			else
				gEnv->pMovieSystem->Resume();
		}

		if(m_paused)
		{
			SGameObjectEvent evt(eGFE_PauseGame, eGOEF_ToAll);
			m_pGameObjectSystem->BroadcastEvent( evt );
		}
		else
		{
			SGameObjectEvent evt(eGFE_ResumeGame, eGOEF_ToAll);
			m_pGameObjectSystem->BroadcastEvent( evt );
		}
	}
}

bool CCryAction::IsGamePaused()
{
	return m_paused || !gEnv->pTimer->IsTimerEnabled();
}

bool CCryAction::IsGameStarted()
{
	return (m_pGame&&m_pGame->GetGameContext()) ? m_pGame->GetGameContext()->IsGameStarted() : false;
}

bool CCryAction::StartGameContext( const SGameStartParams * pGameStartParams )
{
	if (GetISystem()->IsEditor())
	{
		if (!m_pGame)
			CryError("Must have game around always for editor");
	}
	else
	{
		EndGameContext();
		m_pGame = new CActionGame( m_pScriptRMI );
	}

	if (!m_pGame->Init(pGameStartParams))
	{
		EndGameContext();
		GameWarning("Failed initializing game");
		return false;
	}

	return true;
}

bool CCryAction::BlockingSpawnPlayer()
{
	if (!m_pGame)
		return false;
	return m_pGame->BlockingSpawnPlayer();
}

void CCryAction::ResetBrokenGameObjects()
{
	if (m_pGame)
		m_pGame->FixBrokenObjects();
}

bool CCryAction::ChangeGameContext( const SGameContextParams * pGameContextParams )
{
	if (!m_pGame)
		return false;
	return m_pGame->ChangeGameContext( pGameContextParams );
}

void CCryAction::EndGameContext()
{
	// to make this function re-entrant, m_pGame pointer must be set to 0
	// BEFORE the destructor of CActionGame is invokded (Craig)
	_smart_ptr<CActionGame> pGame = m_pGame;
	m_pGame = 0;
	pGame = 0;

	// EDITOR HACK
	if (GetISystem())
		if (GetISystem()->IsEditor())
			m_pGame = new CActionGame( m_pScriptRMI );

	// reset MusicLogic
	if (m_pMusicLogic)
		m_pMusicLogic->Stop();
}

//------------------------------------------------------------------------
void CCryAction::SetEditorLevel(const char *levelName, const char *levelFolder)
{
	strncpy(m_editorLevelName, levelName, sizeof(m_editorLevelName));
	strncpy(m_editorLevelFolder, levelFolder, sizeof(m_editorLevelFolder));
}

//------------------------------------------------------------------------
void CCryAction::GetEditorLevel(char **levelName, char **levelFolder)
{
	if (levelName) *levelName = &m_editorLevelName[0];
	if (levelFolder) *levelFolder = &m_editorLevelFolder[0];
}

//------------------------------------------------------------------------
bool CCryAction::SaveGame( const char * path, bool bQuick, bool bForceImmediate, ESaveGameReason reason, bool ignoreDelay, const char*checkPointName)
{
	if(gEnv->bMultiplayer)
		return false;
  if(!gEnv->pGame->GetIGameFramework() || !gEnv->pGame->GetIGameFramework()->GetClientActor())
    return false;

	if(gEnv->pGame->GetIGameFramework()->GetClientActor()->GetHealth() <= 0)
	{
		//don't save when the player already died - savegame will be corrupt
		GameWarning("Saving failed : player is dead!");
		return false;
	}

	if (CanSave() == false)
	{
		GameWarning("CCryAction::SaveGame: Suppressing QS");
		return false;
	}

	if(m_lastSaveLoad > 0.0f)
	{
		if(ignoreDelay)
			m_lastSaveLoad = 0.0f;
		else
			return false;
	}

	bool bRet = true;
	if (bForceImmediate)
	{
		// check, if preSaveGame has been called already
		if (m_pLocalAllocs && m_pLocalAllocs->m_delayedSaveGameName.empty())
			OnActionEvent(SActionEvent(eAE_preSaveGame, (int) reason));
		CTimeValue elapsed = -gEnv->pTimer->GetAsyncTime();
		gEnv->pSystem->SerializingFile(bQuick?1:2);
		bRet = m_pGameSerialize->SaveGame(this, "xml", path, reason, checkPointName);
		gEnv->pSystem->SerializingFile(0);
		OnActionEvent(SActionEvent(eAE_postSaveGame, (int) reason, bRet ? "" : 0) );
		m_lastSaveLoad = 0.5f;
		elapsed += gEnv->pTimer->GetAsyncTime();
		GameWarning("[CryAction] SaveGame: '%s' %s. [Duration=%.4f secs]", path, bRet ? "done" : "failed", elapsed.GetSeconds());
	}
	else
	{
		m_delayedSaveGameMethod = bQuick ? 1 : 2;
		m_delayedSaveGameReason = reason;
		if (m_pLocalAllocs)
		{
			m_pLocalAllocs->m_delayedSaveGameName = path;
			if(checkPointName)
				m_pLocalAllocs->m_checkPointName = checkPointName;
			else
				m_pLocalAllocs->m_checkPointName.clear();
		}
		OnActionEvent(SActionEvent(eAE_preSaveGame, (int) reason));
	}
	return bRet;
}

//------------------------------------------------------------------------
bool CCryAction::LoadGame( const char * path, bool quick, bool ignoreDelay)
{
	if(gEnv->bMultiplayer)
		return false;

	if(m_lastSaveLoad > 0.0f)
	{
		if(ignoreDelay)
			m_lastSaveLoad = 0.0f;
		else
			return false;
	}

	if (CanLoad() == false)
	{
		GameWarning("CCryAction::LoadGame: Suppressing QL");
		return false;
	}

	CTimeValue elapsed = -gEnv->pTimer->GetAsyncTime();

	gEnv->pSystem->SerializingFile(quick?1:2);

	SGameStartParams params;
	params.flags = eGSF_Server | eGSF_Client;
	params.hostname = "localhost";
	params.maxPlayers = 1;
//	params.pContextParams = &ctx; (set by ::LoadGame)
	params.port = gEnv->pConsole->GetCVar("sv_port")->GetIVal();

	//pause entity event timers update
	gEnv->pEntitySystem->PauseTimers(true, false);

	GameWarning("[CryAction] LoadGame: '%s'", path);

	switch (m_pGameSerialize->LoadGame( this, "xml", path, params, quick, true))
	{
	case eLGR_Ok:
		gEnv->pEntitySystem->PauseTimers(false, false);
		GetISystem()->SerializingFile(0);

		// AllowSave never needs to be serialized, but reset here, because
		// if we had saved a game before it is obvious that at that point saving was not prohibited
		// otherwise we could not have saved it beforehand
		AllowSave(true); 

		elapsed += gEnv->pTimer->GetAsyncTime();
		GameWarning("[CryAction] LoadGame: '%s' done. [Duration=%.4f secs]", path, elapsed.GetSeconds());
		m_lastSaveLoad = 0.5f;
		return true;
	default:
		gEnv->pEntitySystem->PauseTimers(false, false);
		GameWarning("Unknown result code from CGameSerialize::LoadGame");
		// fall through
	case eLGR_FailedAndDestroyedState:
		GameWarning("[CryAction] LoadGame: '%s' failed. Ending GameContext", path);
		EndGameContext();
		// fall through
	case eLGR_Failed:
		GetISystem()->SerializingFile(0);
		return false;
	}
}

//------------------------------------------------------------------------
void CCryAction::OnEditorSetGameMode( int iMode )
{
	if (iMode<2)
	{
		/* AlexL: for now don't set time to 0.0 
		   (because entity timers might still be active and getting confused)
		if (iMode == 1)
			gEnv->pTimer->SetTimer(ITimer::ETIMER_GAME, 0.0f);
		*/

		if (m_pGame)
			m_pGame->OnEditorSetGameMode(iMode!=0);

		if (m_pMaterialEffects)
		{
			if (iMode && m_isEditing)
				m_pMaterialEffects->PreLoadAssets();
			m_pMaterialEffects->SetUpdateMode(iMode!=0);
		}

		m_isEditing = !iMode;
	} 
	else if (m_pGame)
	{
		m_pGame->FixBrokenObjects();
		m_pGame->ClearBreakHistory();
	}
}

//------------------------------------------------------------------------
IFlowSystem * CCryAction::GetIFlowSystem()
{
	return m_pFlowSystem;
}

//------------------------------------------------------------------------
void CCryAction::SetGameQueryListener( CGameQueryListener * pListener )
{
	if (m_pGameQueryListener)
	{
		m_pGameQueryListener->Complete();
		m_pGameQueryListener = NULL;
	}
	assert( m_pGameQueryListener == NULL );
	m_pGameQueryListener = pListener;
}

//------------------------------------------------------------------------
void CCryAction::BeginLanQuery()
{
	CGameQueryListener * pNewListener = new CGameQueryListener;
	m_pLanQueryListener = m_pNetwork->CreateLanQueryListener( pNewListener );
	pNewListener->SetNetListener( m_pLanQueryListener );
	SetGameQueryListener( pNewListener );
}

//------------------------------------------------------------------------
void CCryAction::EndCurrentQuery()
{
	SetGameQueryListener(NULL);
}

//------------------------------------------------------------------------
void CCryAction::InitCVars()
{
	IConsole * pC = ::gEnv->pConsole;
	m_pEnableLoadingScreen = pC->RegisterInt( "g_enableloadingscreen", 1, VF_DUMPTODISK, "Enable/disable the loading screen" );
	pC->RegisterInt( "g_enableitems", 1, 0, "Enable/disable the item system" );
	m_pShowLanBrowserCVAR = pC->RegisterInt( "net_lanbrowser", 0, VF_CHEAT, "enable LAN games browser" );
	pC->RegisterInt( "g_aimdebug", 0, VF_CHEAT, "Enable/disable debug drawing for aiming direction" );
	pC->RegisterInt( "g_groundeffectsdebug", 0, 0, "Enable/disable logging for GroundEffects" );
	pC->RegisterFloat( "g_breakImpulseScale", 1.0f, VF_REQUIRE_LEVEL_RELOAD, "How big do explosions need to be to break things?" );
	pC->RegisterInt( "g_breakage_particles_limit", 200, 0, "Imposes a limit on particles generated during 2d surfaces breaking" );
	pC->RegisterFloat("c_shakeMult", 1.0f, VF_CHEAT);

#ifdef USING_LICENSE_PROTECTION
	pC->RegisterString( "net_proxy_ip", "", VF_CHEAT, "IP address of a proxy to overrule direct connection to license server" );
	pC->RegisterInt( "net_proxy_port", 80, VF_CHEAT, "Port of a proxy to overrule direct connection to license server" );
	pC->RegisterString( "net_proxy_user", "", VF_CHEAT, "Username for proxy authentication" );
	pC->RegisterString( "net_proxy_pass", "", VF_CHEAT, "Password for proxy authentication" );
#endif // USING_LICENSE_PROTECTION

	pC->RegisterInt( "cl_packetRate", 30, 0, "Packet rate on client" );
	pC->RegisterInt( "sv_packetRate", 30, 0, "Packet rate on server" );
	pC->RegisterInt( "cl_bandwidth", 50000, 0, "Bit rate on client" );
	pC->RegisterInt( "sv_bandwidth", 50000, 0, "Bit rate on server" );
	pC->RegisterFloat( "g_localPacketRate", 50, 0, "Packet rate locally on faked network connection" );
	pC->RegisterInt( "sv_timeout_disconnect", 0, 0, "Timeout for fully disconnecting timeout connections" );

	//pC->Register( "cl_voice_playback",&m_VoicePlaybackEnabled);
	// NB this should be false by default - enabled when user holds down CapsLock
	pC->Register( "cl_voice_recording",&m_VoiceRecordingEnabled,0,0,"Enable client voice recording");

	pC->RegisterString("cl_serveraddr", "localhost", VF_DUMPTODISK, "Server address");
	pC->RegisterInt("cl_serverport", SERVER_DEFAULT_PORT, VF_DUMPTODISK, "Server address");
  pC->RegisterString("cl_serverpassword","",VF_DUMPTODISK,"Server password");
	pC->RegisterString("sv_map", "ps_port", 0, "The map the server should load", NULL);
	
  pC->RegisterString("sv_levelrotation", "levelrotation", 0, "Sequence of levels to load after each game ends", NULL);
	
  pC->RegisterString("sv_requireinputdevice", "dontcare", VF_DUMPTODISK | VF_REQUIRE_LEVEL_RELOAD, "Which input devices to require at connection (dontcare, none, gamepad, keyboard)");
	const char * defaultGameRules = "SinglePlayer";
	if (gEnv->pSystem->IsDedicated())
		defaultGameRules = "InstantAction";
	pC->RegisterString("sv_gamerules", defaultGameRules, 0, "The game rules that the server should use", NULL);
	pC->RegisterInt("sv_port", SERVER_DEFAULT_PORT, VF_DUMPTODISK, "Server address");
  pC->RegisterString("sv_password", "", VF_DUMPTODISK, "Server password", SvPasswordChanged);
	pC->RegisterInt("sv_lanonly", 0, VF_DUMPTODISK, "Set for LAN games");

	pC->RegisterString("sv_bind", "0.0.0.0", VF_REQUIRE_LEVEL_RELOAD, "Bind the server to a specific IP address if multiple network adapters exist");

  pC->RegisterString("sv_servername", "", VF_DUMPTODISK, "Server name will be displayed in server list. If empty, machine name will be used.");
  pC->RegisterInt("sv_maxplayers", 32, VF_DUMPTODISK, "Maximum number of players allowed to join server.", VerifyMaxPlayers);
  pC->RegisterInt("sv_maxspectators", 32, VF_DUMPTODISK, "Maximum number of players allowed to be spectators during the game.");
	pC->RegisterInt("ban_timeout", 30, VF_DUMPTODISK, "Ban timeout in minutes");
  pC->RegisterFloat("sv_timeofdaylength", 1.0f, VF_DUMPTODISK, "Sets time of day changing speed.");
  pC->RegisterFloat("sv_timeofdaystart", 12.0f, VF_DUMPTODISK, "Sets time of day start time.");
	pC->RegisterInt("sv_timeofdayenable", 1, VF_DUMPTODISK, "Enables time of day simulation.");
  
  //Added to test Gamespy functionality
  pC->RegisterString("cl_gs_password","",0,"Password for Gamespy login");
  pC->RegisterString("cl_gs_nick","",0,"Nickname for Gamespy login");
  pC->RegisterString("cl_gs_email","",0,"Email address for Gamespy login");
  pC->RegisterString("cl_gs_cdkey","",0,"CDKey for gamespy auth");

  pC->RegisterInt("sv_gs_trackstats", 1, 0, "Enable Gamespy stats tracking");
  pC->RegisterInt("sv_gs_report", 1, 0, "Enable Gamespy server reporting, this is necessary for NAT negotiation");

	pC->RegisterInt("g_spectatorcollisions", 1, 0, "If set, spectator camera will not be able to pass through buildings");

	pC->RegisterString("http_password", "password", 0);

	m_pMaterialEffectsCVars = new CMaterialEffectsCVars();

	CActionGame::RegisterCVars();
}

//------------------------------------------------------------------------
void CCryAction::ReleaseCVars()
{
	SAFE_DELETE(m_pMaterialEffectsCVars);
	SAFE_DELETE(m_pCryActionCVars);
}

void CCryAction::InitCommands()
{
	IConsole * pC = ::gEnv->pConsole;
	// create built-in commands
	pC->AddCommand( "map", MapCmd, 0, "Load a map");
	// for testing purposes
	pC->AddCommand( "unload", UnloadCmd ,0,"Unload current map" );
	pC->AddCommand( "dump_maps", DumpMapsCmd, 0, "Dumps currently scanned maps");
	pC->AddCommand( "play", PlayCmd, 0, "Play back a recorded game");
	pC->AddCommand( "connect", ConnectCmd, VF_RESTRICTEDMODE, "Start a client and connect to a server");
	pC->AddCommand( "disconnect", DisconnectCmd, 0, "Stop a game (or a client or a server)");
	pC->AddCommand( "status", StatusCmd, 0, "Shows connection status");
	pC->AddCommand( "save", SaveGameCmd, VF_RESTRICTEDMODE, "Save game");
	pC->AddCommand( "save_genstrings", GenStringsSaveGameCmd, VF_CHEAT|VF_NOHELP, "");
	pC->AddCommand( "load", LoadGameCmd, VF_RESTRICTEDMODE, "Load game");
	pC->AddCommand( "test_reset", TestResetCmd, VF_CHEAT|VF_NOHELP );
  pC->AddCommand( "open_url", OpenURLCmd, VF_NOHELP );

  pC->AddCommand( "test_timeout", TestTimeout, VF_CHEAT|VF_NOHELP );
	pC->AddCommand( "test_nsbrowse", TestNSServerBrowser, VF_CHEAT|VF_NOHELP );
	pC->AddCommand( "test_nsreport", TestNSServerReport, VF_CHEAT|VF_NOHELP );
	pC->AddCommand( "test_nschat", TestNSChat, VF_CHEAT|VF_NOHELP );
	pC->AddCommand( "test_nsstats", TestNSStats, VF_CHEAT|VF_NOHELP );
  pC->AddCommand( "test_nsnat", TestNSNat, VF_CHEAT|VF_NOHELP );

  pC->AddCommand( "test_playersBounds", TestPlayerBoundsCmd, VF_CHEAT|VF_NOHELP );
	pC->AddCommand( "g_dump_stats", DumpStatsCmd, VF_CHEAT|VF_NOHELP );

  pC->AddCommand( "kick", KickPlayerCmd, 0, "Kicks player from game");
  pC->AddCommand( "kickid", KickPlayerByIdCmd, 0, "Kicks player from game");

	pC->AddCommand( "ban", BanPlayerCmd, 0, "Bans player for 30 minutes from server.");
	pC->AddCommand( "ban_status", BanStatusCmd, 0, "Shows currently banned players." );
	pC->AddCommand( "ban_remove", UnbanPlayerCmd, 0, "Removes player from ban list.");
	
	pC->AddCommand( "dump_stats",  DumpAnalysisStatsCmd, 0, "Dumps some player statistics");

	// temporary: view note bind
	pC->AddCommand( "viewnote", ViewNote, 0, "View Note");

	// RCON system
	//pC->AddCommand( "rcon_password", rcon_password, 0, "Sets password for the RCON system" );
	pC->AddCommand( "rcon_startserver", rcon_startserver, 0, "Starts a remote control server" );
	pC->AddCommand( "rcon_stopserver", rcon_stopserver, 0, "Stops a remote control server" );
	pC->AddCommand( "rcon_connect", rcon_connect, 0, "Connects to a remote control server" );
	pC->AddCommand( "rcon_disconnect", rcon_disconnect, 0, "Disconnects from a remote control server" );
	pC->AddCommand( "rcon_command", rcon_command, 0, "Issues a console command from a RCON client to a RCON server" );

	// HTTP server
	pC->AddCommand( "http_startserver", http_startserver, 0, "Starts an HTTP server" );
	pC->AddCommand( "http_stopserver", http_stopserver, 0, "Stops an HTTP server" );

	pC->AddCommand("voice_mute", MutePlayer, 0, "Mute player's voice comms");

	pC->AddCommand("net_pb_sv_enable", StaticSetPbSvEnabled, 0, "Sets PunkBuster server enabled state");
	pC->AddCommand("net_pb_cl_enable", StaticSetPbClEnabled, 0, "Sets PunkBuster client enabled state");
}

//------------------------------------------------------------------------
void CCryAction::VerifyMaxPlayers( ICVar * pVar )
{
	int nPlayers = pVar->GetIVal();
	if (nPlayers < 2 || nPlayers > 32)
		pVar->Set( CLAMP(nPlayers, 2, 32) );
}

//------------------------------------------------------------------------
bool CCryAction::LoadingScreenEnabled() const
{
	return m_pEnableLoadingScreen? m_pEnableLoadingScreen->GetIVal() != 0 : true;
}

int CCryAction::NetworkExposeClass( IFunctionHandler * pFH )
{
	return m_pScriptRMI->ExposeClass( pFH );
}

//------------------------------------------------------------------------
void CCryAction::RegisterFactory(const char *name, ISaveGame *(*func)(), bool)
{
	m_pGameSerialize->RegisterSaveGameFactory( name, func );
}

//------------------------------------------------------------------------
void CCryAction::RegisterFactory(const char *name, ILoadGame *(*func)(), bool)
{
	m_pGameSerialize->RegisterLoadGameFactory( name, func );
}

CGameServerNub * CCryAction::GetGameServerNub()
{ 
	return m_pGame? m_pGame->GetGameServerNub() : NULL; 
}

CGameClientNub * CCryAction::GetGameClientNub()
{
	return m_pGame? m_pGame->GetGameClientNub() : NULL; 
}

IActor * CCryAction::GetClientActor() const
{ 
	return m_pGame? m_pGame->GetClientActor() : NULL; 
}

EntityId CCryAction::GetClientActorId() const
{ 
	if (m_pGame)
	{
		if (IActor *pActor=m_pGame->GetClientActor())
			return pActor->GetEntityId();
	}

	return 0;
}

INetChannel * CCryAction::GetClientChannel() const
{
	if (m_pGame)
	{
		CGameClientChannel *pChannel=m_pGame->GetGameClientNub()?m_pGame->GetGameClientNub()->GetGameClientChannel():NULL;
		if (pChannel)
			return pChannel->GetNetChannel();
	}

	return NULL;
}

IGameObject * CCryAction::GetGameObject(EntityId id)
{
	if (IEntity * pEnt = gEnv->pEntitySystem->GetEntity(id))
		if (CGameObject * pGameObject = (CGameObject*) pEnt->GetProxy(ENTITY_PROXY_USER))
			return pGameObject;

	return NULL;
}

bool CCryAction::GetNetworkSafeClassId(uint16 &id, const char *className)
{
	if (CGameContext * pGameContext = GetGameContext())
		return pGameContext->ClassIdFromName(id, className);
	else
		return false;
}

bool CCryAction::GetNetworkSafeClassName(char *className, size_t maxn, uint16 id)
{
	string name;
	if (CGameContext * pGameContext = GetGameContext())
	{
		if (pGameContext->ClassNameFromId(name, id))
		{
			strncpy(className,name.c_str(), maxn);
			return true;
		}
	}

	return false;
}


IGameObjectExtension * CCryAction::QueryGameObjectExtension( EntityId id, const char * name )
{
	if (IGameObject * pObj = GetGameObject(id))
		return pObj->QueryExtension(name);
	else
		return NULL;
}

bool CCryAction::ControlsEntity( EntityId id ) const
{ 
	return m_pGame? m_pGame->ControlsEntity(id) : false; 
}

CTimeValue CCryAction::GetServerTime()
{
	if (gEnv->bServer)
		return gEnv->pTimer->GetFrameStartTime();

	return GetClientChannel()?GetClientChannel()->GetRemoteTime():CTimeValue(0.0f);
}

uint16 CCryAction::GetGameChannelId(INetChannel *pNetChannel)
{
	if (gEnv->bServer)
	{
		CGameServerNub * pNub = GetGameServerNub();
		if (!pNub)
			return 0;

		return pNub->GetChannelId(pNetChannel);
	}

	return 0;
}

INetChannel *CCryAction::GetNetChannel(uint16 channelId)
{
	if (gEnv->bServer)
	{
		CGameServerNub * pNub = GetGameServerNub();
		if (!pNub)
			return 0;

		CGameServerChannel *pChannel=pNub->GetChannel(channelId);
		if (pChannel)
			return pChannel->GetNetChannel();
	}

	return 0;
}

bool CCryAction::IsChannelOnHold(uint16 channelId)
{
	if (CGameServerNub * pNub = GetGameServerNub())
		if (CGameServerChannel *pServerChannel=pNub->GetChannel(channelId))
			return pServerChannel->IsOnHold();

	return false;
}

CGameContext * CCryAction::GetGameContext()
{ 
	return m_pGame? m_pGame->GetGameContext() : NULL; 
}

void CCryAction::RegisterFactory(const char *name, IActorCreator * pCreator, bool isAI) 
{ 
	m_pActorSystem->RegisterActorClass(name, pCreator, isAI); 
}

void CCryAction::RegisterFactory(const char *name, IItemCreator * pCreator, bool isAI) 
{ 
	m_pItemSystem->RegisterItemClass(name, pCreator); 
}

void CCryAction::RegisterFactory(const char *name, IVehicleCreator * pCreator, bool isAI) 
{ 
	m_pVehicleSystem->RegisterVehicleClass(name, pCreator, isAI); 
}

void CCryAction::RegisterFactory(const char *name, IGameObjectExtensionCreator * pCreator, bool isAI ) 
{ 
	m_pGameObjectSystem->RegisterExtension(name, pCreator, NULL); 
}

void CCryAction::RegisterFactory(const char *name, IAnimationStateNodeFactory *(*func)(), bool isAI ) 
{ 
	m_pAnimationGraphManager->RegisterStateFactory(name, func); 
}

IActionMapManager * CCryAction::GetIActionMapManager()
{ 
	return m_pActionMapManager; 
}

IUIDraw *CCryAction::GetIUIDraw() 
{ 
	return m_pUIDraw; 
}

ILevelSystem * CCryAction::GetILevelSystem() 
{ 
	return m_pLevelSystem; 
}

IActorSystem * CCryAction::GetIActorSystem() 
{ 
	return m_pActorSystem; 
}

IItemSystem * CCryAction::GetIItemSystem() 
{ 
	return m_pItemSystem; 
}

IVehicleSystem * CCryAction::GetIVehicleSystem() 
{ 
	return m_pVehicleSystem; 
}

IViewSystem * CCryAction::GetIViewSystem() 
{ 
	return m_pViewSystem; 
}

IGameplayRecorder * CCryAction::GetIGameplayRecorder() 
{ 
	return m_pGameplayRecorder; 
}

IGameRulesSystem * CCryAction::GetIGameRulesSystem() 
{ 
	return m_pGameRulesSystem; 
}

IGameObjectSystem * CCryAction::GetIGameObjectSystem() 
{ 
	return m_pGameObjectSystem; 
}

IGameTokenSystem * CCryAction::GetIGameTokenSystem() 
{ 
	return m_pGameTokenSystem; 
}

IEffectSystem * CCryAction::GetIEffectSystem()
{
	return m_pEffectSystem;
}

IMaterialEffects * CCryAction::GetIMaterialEffects()
{
	return m_pMaterialEffects;
}

IDialogSystem * CCryAction::GetIDialogSystem()
{
	return m_pDialogSystem;
}

IPlayerProfileManager * CCryAction::GetIPlayerProfileManager()
{
	return m_pPlayerProfileManager;
}

int CCryAction::AddTimer(CTimeValue nInterval, bool bRepeat, void (*pFunction)(void*, int), void *pUserData)
{
	return m_pCallbackTimer->AddTimer(nInterval, bRepeat, pFunction, pUserData);
}

void* CCryAction::RemoveTimer(int nIndex)
{
	return m_pCallbackTimer->RemoveTimer(nIndex);
}

const char * CCryAction::GetLevelName()
{
	ISystem * pSystem = GetISystem();

	const char * levelName = NULL;
	if (pSystem->IsEditor())
	{
		char * levelFolder = 0;
		char * levelNameTemp = 0;
		GetEditorLevel( &levelNameTemp, &levelFolder );
		if (strcmp( levelFolder, "" ) && strcmp( levelNameTemp, "" ))
			levelName = strrchr( levelNameTemp, '\\' ) + 1;
	}
	else
	{
		if (StartedGameContext())
			if (ILevel * pLevel = GetILevelSystem()->GetCurrentLevel())
				if (ILevelInfo * pLevelInfo = pLevel->GetLevelInfo())
					levelName = pLevelInfo->GetName();
	}
	return levelName;
}

const char *CCryAction::GetAbsLevelPath()
{
	static char szFullPath[512];

	if (gEnv->pSystem->IsEditor())
	{
		char *levelFolder=0;

		GetEditorLevel(0,&levelFolder);

		strcpy(szFullPath,levelFolder);

		// todo: abs path
		return szFullPath;
	}
	else
	{
		if (StartedGameContext())
			if (ILevel * pLevel = GetILevelSystem()->GetCurrentLevel())
				if (ILevelInfo * pLevelInfo = pLevel->GetLevelInfo())
				{
					sprintf(szFullPath,"%s/",PathUtil::GetGameFolder().c_str());
					strcpy(&szFullPath[strlen(szFullPath)],pLevelInfo->GetPath());

					// todo: abs path
					return szFullPath;
				}
	}

	return 0;			// no level loaded
}

bool CCryAction::IsInLevelLoad()
{
	if (CGameContext * pGameContext = GetGameContext())
		return pGameContext->IsInLevelLoad();
	return false;
}

bool CCryAction::IsLoadingSaveGame()
{
	if (CGameContext * pGameContext = GetGameContext())
		return pGameContext->IsLoadingSaveGame();
	return false;
}

bool CCryAction::CanCheat()
{
	return !gEnv->bMultiplayer || gEnv->pSystem->IsDevMode();
}

IPersistantDebug* CCryAction::GetIPersistantDebug()
{
	return m_pPersistantDebug;
}

IGameStatsConfig* CCryAction::GetIGameStatsConfig()
{
	return m_pGameStatsConfig;
}

void CCryAction::TestResetCmd(IConsoleCmdArgs* args)
{
	GetCryAction()->Reset(true);
//	if (CGameContext * pCtx = GetCryAction()->GetGameContext())
//		pCtx->GetNetContext()->RequestReconfigureGame();
}

void CCryAction::TestTimeout(IConsoleCmdArgs* args)
{
	if (!gEnv->bServer || args->GetArgCount()<1)
		return;

	if (IEntity *pEntity=gEnv->pEntitySystem->FindEntityByName(args->GetArg(1)))
	{
		if (IActor *pActor=CCryAction::GetCryAction()->GetIActorSystem()->GetActor(pEntity->GetId()))
			CCryAction::GetCryAction()->GetServerNetNub()->DisconnectPlayer(eDC_Timeout, pActor->GetEntityId(), "timeout");
	}
}

void CCryAction::TestNSServerBrowser(IConsoleCmdArgs* args)
{
	INetworkService *serv=GetCryAction()->GetISystem()->GetINetwork()->GetService("GameSpy");
	serv->GetTestInterface()->TestBrowser(&m_GQListener);
}

void CCryAction::TestNSServerReport(IConsoleCmdArgs* args)
{
	INetworkService *serv=GetCryAction()->GetISystem()->GetINetwork()->GetService("GameSpy");
	serv->GetTestInterface()->TestReport();
}

void CCryAction::TestNSChat(IConsoleCmdArgs* args)
{
	INetworkService *serv=GetCryAction()->GetISystem()->GetINetwork()->GetService("GameSpy");
	serv->GetTestInterface()->TestChat();
}

void CCryAction::TestNSStats(IConsoleCmdArgs* args)
{
	INetworkService *serv=GetCryAction()->GetISystem()->GetINetwork()->GetService("GameSpy");
	serv->GetTestInterface()->TestStats();
}

void CCryAction::TestNSNat(IConsoleCmdArgs* args)
{
    INetNub* pNub = CCryAction::GetCryAction()->GetServerNetNub();
    if(!pNub)
        return;
    if(args->GetArgCount()>1)
    {
        int a = atoi(args->GetArg(1));
        pNub->OnNatCookieReceived(a);
    }
    else
    {
        pNub->OnNatCookieReceived(100);
    }
}

void CCryAction::TestPlayerBoundsCmd(IConsoleCmdArgs* args)
{
  TServerChannelMap * pChannelMap = GetCryAction()->GetGameServerNub()->GetServerChannelMap();
  for (TServerChannelMap::iterator iter = pChannelMap->begin(); iter != pChannelMap->end(); ++iter)
  {
    const char *name="";
    IActor *pActor=CCryAction::GetCryAction()->m_pActorSystem->GetActorByChannelId(iter->first);

    if(!pActor)
      continue;

    AABB aabb,aabb2;
    IEntity* pEntity = pActor->GetEntity();
    if (ICharacterInstance* pCharInstance = pEntity->GetCharacter(0))
    {
      if (ISkeletonAnim* pSkeletonAnim = pCharInstance->GetISkeletonAnim())
      {
        aabb = pCharInstance->GetAABB();
        CryLog("%s ca_local_bb (%.4f %.4f %.4f) (%.4f %.4f %.4f)",pEntity->GetName(),aabb.min.x,aabb.min.y,aabb.min.z,aabb.max.x,aabb.max.y,aabb.max.z);
        aabb.min = pEntity->GetWorldTM()*aabb.min;
        aabb.max = pEntity->GetWorldTM()*aabb.max;
        pEntity->GetWorldBounds(aabb2);

        CryLog("%s ca_bb (%.4f %.4f %.4f) (%.4f %.4f %.4f)",pEntity->GetName(),aabb.min.x,aabb.min.y,aabb.min.z,aabb.max.x,aabb.max.y,aabb.max.z);
        CryLog("%s es_bb (%.4f %.4f %.4f) (%.4f %.4f %.4f)",pEntity->GetName(),aabb2.min.x,aabb2.min.y,aabb2.min.z,aabb2.max.x,aabb2.max.y,aabb2.max.z);
      }
    }
  }
}

void CCryAction::DumpStatsCmd(IConsoleCmdArgs* args)
{
	CActionGame* pG = CActionGame::Get();
	if(!pG)
		return;
	pG->DumpStats();
}

void CCryAction::RegisterListener(IGameFrameworkListener *pGameFrameworkListener, const char * name, EFRAMEWORKLISTENERPRIORITY eFrameworkListenerPriority)
{
	// listeners must be unique
	for(TGameFrameworkListeners::iterator iter=m_pGameFrameworkListeners->begin(); iter!=m_pGameFrameworkListeners->end(); ++iter)
	{
		if((*iter).pListener == pGameFrameworkListener)
		{
			assert(false);
			return;
		}
	}

	SGameFrameworkListener listener;
	listener.pListener = pGameFrameworkListener;
	listener.name = name;
	listener.eFrameworkListenerPriority = eFrameworkListenerPriority;

	for(TGameFrameworkListeners::iterator iter=m_pGameFrameworkListeners->begin(); iter!=m_pGameFrameworkListeners->end(); ++iter)
	{
		if((*iter).eFrameworkListenerPriority > eFrameworkListenerPriority)
		{
			m_pGameFrameworkListeners->insert(iter,listener);
			return;
		}
	}
	m_pGameFrameworkListeners->push_back(listener);
}

void CCryAction::UnregisterListener(IGameFrameworkListener *pGameFrameworkListener)
{
	for (TGameFrameworkListeners::iterator iter = m_pGameFrameworkListeners->begin(); iter != m_pGameFrameworkListeners->end(); ++iter)
	{
		if (iter->pListener == pGameFrameworkListener)
		{
			m_pGameFrameworkListeners->erase(iter);
			return;
		}
	}
}

CGameStatsConfig* CCryAction::GetGameStatsConfig()
{
	return m_pGameStatsConfig;
}

void CCryAction::ResetMusicGraph()
{
	if (m_pMusicGraphState)
		m_pMusicGraphState->Reset();
}

void CCryAction::ScheduleEndLevel(const char* nextLevel)
{
	m_bScheduleLevelEnd = true;
	if (!m_pLocalAllocs)
    return;
	m_pLocalAllocs->m_nextLevelToLoad = nextLevel;
}

void CCryAction::CheckEndLevelSchedule()
{
	if (!m_bScheduleLevelEnd)
		return;
	m_bScheduleLevelEnd = false;
	if (m_pLocalAllocs == 0)
	{
		assert (false);
		return;
	}

	const bool bHaveNextLevel = (m_pLocalAllocs->m_nextLevelToLoad.empty() == false);

	IEntity* pGameRules = CCryAction::GetCryAction()->GetIGameRulesSystem()->GetCurrentGameRulesEntity();
	if (pGameRules != 0)
	{
		IScriptSystem * pSS = gEnv->pScriptSystem;
		SmartScriptTable table = pGameRules->GetScriptTable();
		SmartScriptTable params( pSS );
		if (bHaveNextLevel)
			params->SetValue( "nextlevel", m_pLocalAllocs->m_nextLevelToLoad.c_str() );
		Script::CallMethod( table, "EndLevel", params );
	}

	CALL_FRAMEWORK_LISTENERS(OnLevelEnd(m_pLocalAllocs->m_nextLevelToLoad.c_str()));

	if (bHaveNextLevel)
	{
		CryFixedStringT<256> cmd ("map ");
		cmd+=m_pLocalAllocs->m_nextLevelToLoad;
		if (gEnv->pSystem->IsEditor())
		{
			GameWarning("CCryAction: Suppressing loading of next level '%s' in Editor!", m_pLocalAllocs->m_nextLevelToLoad.c_str());
			m_pLocalAllocs->m_nextLevelToLoad.assign("");
		}
		else
		{
			GameWarning("CCryAction: Loading next level '%s'.", m_pLocalAllocs->m_nextLevelToLoad.c_str());	
			m_pLocalAllocs->m_nextLevelToLoad.assign("");
			gEnv->pConsole->ExecuteString(cmd.c_str());
		}
	}
	else
	{
		GameWarning("CCryAction:LevelEnd");
	}
}

void CCryAction::ExecuteCommandNextFrame(const char* cmd)
{
  assert(m_nextFrameCommand && m_nextFrameCommand->empty());
  (*m_nextFrameCommand) = cmd;
}

void CCryAction::PrefetchLevelAssets( const bool bEnforceAll )
{
	// Marcio needs to fill this
	m_pItemSystem->PrecacheLevel();
}

bool CCryAction::GetModInfo(SModInfo *modInfo, const char *modPath)
{
	return CModManager::GetModInfo(modInfo, modPath);
}

void CCryAction::ShowPageInBrowser(const char* URL)
{
#if (defined(WIN32) || defined(WIN64))
  ShellExecute(0,0,URL,0,0,SW_SHOWNORMAL);
#endif
}

bool CCryAction::StartProcess(const char* cmd_line)
{
#if  defined(WIN32) || defined (WIN64)
  //save all stuff
  STARTUPINFO si;
  ZeroMemory( &si, sizeof(si) );
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi;
  return 0 != CreateProcess(NULL, const_cast<char*>(cmd_line), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
#endif 
  return false;
}

bool CCryAction::SaveServerConfig(const char* path)
{
  class CConfigWriter : public ICVarListProcessorCallback
  {
  public:
    CConfigWriter(const char* path)
    {
      m_file = gEnv->pCryPak->FOpen(path,"wb");
    }
    ~CConfigWriter()
    {
      gEnv->pCryPak->FClose(m_file);
    }
    void OnCVar(ICVar* pV)
    {
      string szValue = pV->GetString();
      int pos;

      // replace \ with \\ 
      pos = 1;
      for(;;)
      {
        pos = szValue.find_first_of("\\", pos);

        if (pos == string::npos)
        {
          break;
        }

        szValue.replace(pos, 1, "\\\\", 2);
        pos+=2;
      }

      // replace " with \" 
      pos = 1;
      for(;;)
      {
        pos = szValue.find_first_of("\"", pos);

        if (pos == string::npos)
        {
          break;
        }

        szValue.replace(pos, 1, "\\\"", 2);
        pos+=2;
      }

      string szLine = pV->GetName();

      if(pV->GetType()==CVAR_STRING)
        szLine += " = \"" + szValue + "\"\r\n";
      else
        szLine += " = " + szValue + "\r\n";

			gEnv->pCryPak->FWrite( szLine.c_str(), szLine.length(), m_file );
    }
		bool IsOk()const
		{
			return m_file!=0;
		}
    FILE* m_file;
  };
	CCVarListProcessor p(PathUtil::GetGameFolder() + "/Scripts/Network/server_cvars.txt");
	CConfigWriter cw(path);
	if(cw.IsOk())
	{
		p.Process(&cw);
		return true;
	}
	return false;
}

void  CCryAction::OnActionEvent(const SActionEvent& ev)
{
	CALL_FRAMEWORK_LISTENERS(OnActionEvent(ev));
}

INetNub * CCryAction::GetServerNetNub()
{
	return m_pGame? m_pGame->GetServerNetNub() : 0;
}

void CCryAction::NotifyGameFrameworkListeners(ISaveGame* pSaveGame)
{
	CALL_FRAMEWORK_LISTENERS(OnSaveGame(pSaveGame));
}

void CCryAction::NotifyGameFrameworkListeners(ILoadGame* pLoadGame)
{
	CALL_FRAMEWORK_LISTENERS(OnLoadGame(pLoadGame));
}

void CCryAction::SetGameGUID(const char * gameGUID)
{
	strncpy(m_gameGUID, gameGUID, sizeof(m_gameGUID)-1);
	m_gameGUID[sizeof(m_gameGUID)-1] = '\0';
}

INetContext* CCryAction::GetNetContext()
{
	//return GetGameContext()->GetNetContext();

	// Julien: This was crashing sometimes when exiting game!
	// I've replaced with a safe pointer access and an assert so that anyone who
	// knows why we were accessing this unsafe pointer->func() can fix it the correct way

	CGameContext *pGameContext = GetGameContext();
	//CRY_ASSERT(pGameContext); - GameContext can be NULL when the game is exiting
	if(!pGameContext)
		return NULL;
	return pGameContext->GetNetContext();
}

void CCryAction::EnableVoiceRecording(const bool enable)
{
	m_VoiceRecordingEnabled=enable ? 1 : 0;
}

IDebugHistoryManager* CCryAction::CreateDebugHistoryManager()
{
  return new CDebugHistoryManager();
}


void CCryAction::GetMemoryStatistics(ICrySizer * s)
{
	s->Add(*this);
#define CHILD_STATISTICS(x) if (x) (x)->GetMemoryStatistics(s)
	CHILD_STATISTICS(m_pGame);
	CHILD_STATISTICS(m_pLevelSystem);
	CHILD_STATISTICS(m_pActorSystem);
	CHILD_STATISTICS(m_pItemSystem);
	CHILD_STATISTICS(m_pVehicleSystem);
	CHILD_STATISTICS(m_pActionMapManager);
	CHILD_STATISTICS(m_pViewSystem);
	CHILD_STATISTICS(m_pGameRulesSystem);
	CHILD_STATISTICS(m_pFlowSystem);
	CHILD_STATISTICS(m_pUIDraw);
	CHILD_STATISTICS(m_pGameObjectSystem);
	CHILD_STATISTICS(m_pScriptRMI);
	CHILD_STATISTICS(m_pAnimationGraphManager);
	CHILD_STATISTICS(m_pMaterialEffects);
	CHILD_STATISTICS(m_pPlayerProfileManager);
	CHILD_STATISTICS(m_pDialogSystem);
	CHILD_STATISTICS(m_pMusicLogic);
	CHILD_STATISTICS(m_pGameTokenSystem);
	CHILD_STATISTICS(m_pEffectSystem);
	CHILD_STATISTICS(m_pGameSerialize);
	CHILD_STATISTICS(m_pCallbackTimer);
	CHILD_STATISTICS(m_pLanQueryListener);
	CHILD_STATISTICS(m_pDevMode);
	CHILD_STATISTICS(m_pTimeDemoRecorder);
	CHILD_STATISTICS(m_pGameQueryListener);
	CHILD_STATISTICS(m_pDebrisMgr);
	CHILD_STATISTICS(m_pGameplayAnalyst);
	CHILD_STATISTICS(m_pTimeOfDayScheduler);
	s->Add(*m_pScriptA);
	s->Add(*m_pScriptIS);
	s->Add(*m_pScriptAS);
	s->Add(*m_pScriptAI);
	s->Add(*m_pScriptNet);
	s->Add(*m_pScriptAMM);
	s->Add(*m_pScriptVS);
	s->Add(*m_pScriptBindVehicle);
	s->Add(*m_pScriptBindVehicleSeat);
	s->Add(*m_pScriptInventory);
	s->Add(*m_pScriptBindDS);
  s->Add(*m_pScriptBindMFX);
	s->Add(*m_pMaterialEffectsCVars);
	s->AddContainer(*m_pGameFrameworkListeners);
	s->AddContainer(*m_pGameFrameworkListenersTemp);
  s->Add(*m_nextFrameCommand);
	CAIHandler::s_ReadabilityManager.GetMemoryStatistics(s);
	// music graph comes from anim graph manager
}

ISubtitleManager* CCryAction::GetISubtitleManager()
{
	return m_pSubtitleManager;
}

void CCryAction::MutePlayer(IConsoleCmdArgs* pArgs)
{
	if(pArgs->GetArgCount() != 2)
		return;

	IEntity* pEntity = gEnv->pEntitySystem->FindEntityByName(pArgs->GetArg(1));
	if(pEntity)
	{
		GetCryAction()->MutePlayerById(pEntity->GetId());
	}
}

void CCryAction::MutePlayerById(EntityId playerId)
{
	IActor *pRequestingActor = gEnv->pGame->GetIGameFramework()->GetClientActor();
	if(pRequestingActor && pRequestingActor->IsPlayer())
	{
		IActor* pActor = GetCryAction()->GetIActorSystem()->GetActor(playerId);
		if(pActor && pActor->IsPlayer())
		{
			if(pActor->GetEntityId() != pRequestingActor->GetEntityId())
			{
				IVoiceContext* pVoiceContext = GetCryAction()->GetGameContext()->GetNetContext()->GetVoiceContext();
				bool muted = pVoiceContext->IsMuted(pRequestingActor->GetEntityId(), pActor->GetEntityId());
				pVoiceContext->Mute(pRequestingActor->GetEntityId(), pActor->GetEntityId(), !muted);

				if(!gEnv->bServer)
				{
					SMutePlayerParams muteParams;
					muteParams.requestor = pRequestingActor->GetEntityId();
					muteParams.id = pActor->GetEntityId();
					muteParams.mute = !muted;

					if (CGameClientChannel* pGCC = CCryAction::GetCryAction()->GetGameClientNub()->GetGameClientChannel())
						CGameServerChannel::SendMutePlayerWith(muteParams, pGCC->GetNetChannel());
				}
			}
		}
	}
}

bool CCryAction::IsImmersiveMPEnabled()
{
	if (CGameContext * pGameContext = GetGameContext())
		return pGameContext->HasContextFlag(eGSF_ImmersiveMultiplayer);
	else
		return false;
}

//////////////////////////////////////////////////////////////////////////
bool CCryAction::IsInTimeDemo()
{
	if (m_pTimeDemoRecorder && m_pTimeDemoRecorder->IsPlaying() || m_pTimeDemoRecorder->IsRecording())
		return true;
	return false;
}

//////////////////////////////////////////////////////////////////////////
bool CCryAction::CanSave()
{
	const bool bViewSystemAllows = m_pViewSystem ? m_pViewSystem->IsPlayingCutScene() == false : true;
	return bViewSystemAllows && m_bAllowSave && !IsInTimeDemo();
}

//////////////////////////////////////////////////////////////////////////
bool CCryAction::CanLoad()
{
	return m_bAllowLoad;
}


//////////////////////////////////////////////////////////////////////////
#ifdef SP_DEMO
	void CCryAction::CreateDevMode()
	{
		m_pDevMode = new CDevMode();
	}
	void CCryAction::RemoveDevMode()
	{
		SAFE_DELETE( m_pDevMode );
	}
#endif

void CCryAction::StoreServerInfo(SServerConnectionInfo& info)
{
	if(!m_pStoredServerConnectionInfo)
		m_pStoredServerConnectionInfo = new SServerConnectionInfo;

	if(m_pStoredServerConnectionInfo)
	{
		*m_pStoredServerConnectionInfo = info;
	}
}

SServerConnectionInfo* CCryAction::GetStoredServerInfo()
{
	return m_pStoredServerConnectionInfo;
}

void CCryAction::ClearStoredServerInfo()
{
	SAFE_DELETE(m_pStoredServerConnectionInfo);
	m_pStoredServerConnectionInfo = NULL;
}

static const char* g_regLocation = "SOFTWARE\\Electronic Arts\\Electronic Arts\\crysis\\ergc\\";

void CCryAction::ReadCDKey()
{
#if (defined(WIN32) || defined(WIN64))
	HKEY  key;
	DWORD type;
	// Open the appropriate registry key
	LONG result = RegOpenKeyEx( HKEY_CURRENT_USER, g_regLocation, 0, KEY_READ|KEY_WOW64_32KEY, &key );
	if( ERROR_SUCCESS == result )
	{
		std::vector<char> cdkey(32);

		DWORD size=cdkey.size();

		result = RegQueryValueEx( key, NULL, NULL, &type, (LPBYTE)&cdkey[0], &size );

		if(ERROR_SUCCESS==result && type==REG_SZ)
		{
			m_pNetwork->SetCDKey(&cdkey[0]);
			m_cdKeyOK = true;
		}
		else
			result = ERROR_BAD_FORMAT;
	}
	if(ERROR_SUCCESS != result)
	{
		// uncomment if the cdkey is used (ie: when using the gamespy sdk)
		// GameWarning("Failed to read CDKey from registry");
	}
#endif
}

void CCryAction::SaveCDKey(const char* cdKey)
{
#if (defined(WIN32) || defined(WIN64))
	HKEY  key;
	// Open the appropriate registry key, creating it if it doesn't exist
	LONG result = RegCreateKeyEx( HKEY_CURRENT_USER, g_regLocation, NULL, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE|KEY_WOW64_32KEY, NULL, &key, NULL);
	if( ERROR_SUCCESS == result )
	{
		DWORD size=strlen(cdKey);

		result = RegSetValueEx( key, NULL, NULL, REG_SZ, (LPBYTE)&cdKey[0], size );
	}
	if(ERROR_SUCCESS != result)
	{
		// uncomment if the cdkey is used (ie: when using the gamespy sdk)
		// GameWarning("Failed to set CDKey in registry");
	}
#endif
}

// TypeInfo implementations for CryAction
#include "TypeInfo_impl.h"
#include "TimeValue_info.h"
#include "Cry_Quat_info.h"
#include "IAnimationGraph_info.h"
#include "AnimationGraph/StateIndex_info.h"
#include "PlayerProfiles/RichSaveGameTypes_info.h"

STRUCT_INFO_T_INSTANTIATE(Quat_tpl, <float>)
