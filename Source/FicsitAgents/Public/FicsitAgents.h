#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FFAServer;

class FFicsitAgentsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	TUniquePtr<FFAServer> Server;
};
