/*
 * Runs the authentication service (what the authserver executable does) inside the worldserver process, for a server that has to live in one module
 * (WebAssembly: two modules cannot share the login database). The sources of the authserver (AuthSession, AuthCodes) are compiled in through the wrappers
 * next to this file; AuthBridge.cpp starts the realm list and the logon socket manager on the world's own LoginDatabase once the world is up.
 *
 * worldserver.conf:  EmbeddedAuth.Enable = 1 (default 0, then nothing happens);  RealmServerPort (default 3724), BindIP, RealmsStateUpdateDelay, WrongPass.* as in authserver.conf
 */
#include "Config.h"
#include "ScriptMgr.h"

bool EmbeddedAuthStart();   // AuthBridge.cpp
void EmbeddedAuthStop();

class EmbeddedAuth : public WorldScript
{
public:
    EmbeddedAuth() : WorldScript("EmbeddedAuth", { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_SHUTDOWN }) { }

    void OnStartup() override
    {
        if (sConfigMgr->GetOption<bool>("EmbeddedAuth.Enable", false))
            EmbeddedAuthStart();
    }

    void OnShutdown() override
    {
        EmbeddedAuthStop();
    }
};

void AddEmbeddedAuthScript()
{
    new EmbeddedAuth();
}
