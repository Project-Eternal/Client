var Game_StartOverlayActive = true;
var Game_VideoOverlayActive = false;
var Game_VideoOverlayFinish = null;
var Game_RuntimeReady = false;
var Game_StartRequested = false;
var Game_Started = false;

function Game_MaybeStartGame()
{
  if (!Game_RuntimeReady || !Game_StartRequested || Game_Started)
  {
    return;
  }
  Game_Started = true;

  var firstStart = !FS.analyzePath('/WoW/WTF/Config.wtf').exists;

  Module.srwData.prepareClient(null, { data: DataSource.layer, mod: DataSource.modLayer, locale: DataSource.locale }).then(function ()
  {
    try
    {
      Game_ApplyBootOverrides(firstStart);
    }
    catch (e)
    {
      Shell_Print("The settings could not be applied: " + e + ".");
    }

    if (Module['acoreEnabled'] && typeof Acore_Start === 'function')
    {
      Acore_Start();
    }
    Module.callMain([]);
  });
}

(function () {
  var overlay = document.getElementById('start-overlay');
  var btnStart = document.getElementById('btn-start');
  var btnSettings = document.getElementById('btn-settings');
  var btnFullscreen = document.getElementById('btn-fullscreen');

  btnStart.addEventListener('click', function () {
    if (Game_StartRequested || DataSource.busy) return;
    Shell_ClearError();
    DataSource_Check().then(function (result) {
      if (result.ok) startGame(); else DataSource_Failed(result);
    });
  });

  function startGame() {
    Module.setStatus('');
    Shell_MessageToGame();
    overlay.style.display = 'none';
    Game_StartOverlayActive = false;
    document.body.classList.remove('start-pending');
    Game_UpdateBarIdle();
    Game_StartRequested = true;
    if (typeof UserExport_Update === 'function') UserExport_Update();

    showPanel('game');
    btnSettings.disabled = true;
    btnSettings.style.display = 'none';

    Game_MaybeStartGame();
  }

  btnFullscreen.addEventListener('click', Game_ToggleFullscreen);
})();

function Game_ToggleFullscreen()
{
  if (document.fullscreenElement)
  {
    document.exitFullscreen();
  }
  else
  {
    document.documentElement.requestFullscreen();
  }
}
