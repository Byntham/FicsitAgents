#include "FicsitAgents.h"
#include "FAServer.h"

DEFINE_LOG_CATEGORY(LogFicsitAgents);

void FFicsitAgentsModule::StartupModule()
{
	// The editor loads this module too. Only the real game should listen for agents.
	if (GIsEditor || IsRunningCommandlet() || IsRunningDedicatedServer())
	{
		return;
	}
	Server = MakeUnique<FFAServer>();
	Server->Start();
}

void FFicsitAgentsModule::ShutdownModule()
{
	if (Server)
	{
		Server->Stop();
		Server.Reset();
	}
}

IMPLEMENT_MODULE(FFicsitAgentsModule, FicsitAgents)
