// The authserver side of the embedded authentication. Kept apart from EmbeddedAuth.cpp because the headers of the two servers clash
// (AuthSession.h and the world's Opcodes.h both define STATUS_AUTHED), so no file may include both.
#include "DatabaseEnv.h"
#include "QueryCallback.h"
#include "AuthSocketMgr.h"
#include "Config.h"
#include "IPLocation.h"
#include "IoContext.h"
#include "Log.h"
#include "RealmList.h"
#include "SecretMgr.h"
#include <memory>
#include <thread>

namespace
{
    std::unique_ptr<Acore::Asio::IoContext> g_ioContext;
    std::thread g_thread;
    bool g_running = false;
}

bool EmbeddedAuthStart()
{
    g_ioContext = std::make_unique<Acore::Asio::IoContext>();

    sSecretMgr->Initialize();
    sIPLocation->Load();
    sRealmList->Initialize(*g_ioContext, sConfigMgr->GetOption<int32>("RealmsStateUpdateDelay", 20));

    int32 port = sConfigMgr->GetOption<int32>("RealmServerPort", 3724);
    std::string bindIp = sConfigMgr->GetOption<std::string>("BindIP", "0.0.0.0");
    if (port < 0 || port > 0xFFFF || !sAuthSocketMgr.StartNetwork(*g_ioContext, bindIp, uint16(port)))
    {
        LOG_ERROR("server.authserver", "The logon network could not be started on {}:{}.", bindIp, port);
        return false;
    }

    g_running = true;
    g_thread = std::thread([] { g_ioContext->run(); });
    LOG_INFO("server.authserver", "The logon network is listening on {}:{}.", bindIp, port);
    return true;
}

void EmbeddedAuthStop()
{
    if (!g_running)
        return;

    g_running = false;
    sAuthSocketMgr.StopNetwork();
    sRealmList->Close();
    g_ioContext->stop();
    if (g_thread.joinable())
        g_thread.join();
}
