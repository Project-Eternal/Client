function shouldHandleKey(e)
{
  if (Game_StartOverlayActive)
  {
    if (e.type === 'keydown' && (e.key === 'Enter' || e.key === ' ')) return false;
    return true;
  }
  if (Game_VideoOverlayActive)
  {
    if (e.type === 'keydown' && e.key === 'Escape' && Game_VideoOverlayFinish) Game_VideoOverlayFinish();
    return true;
  }
  if (!document.body.classList.contains('page-game')) return true;
  if (e.shiftKey || e.altKey || e.metaKey) return false;
  if (e.key === 'F11') return !e.ctrlKey;
  if (e.key === 'F5') return true;
  return false;
}

function handleKeyEvent(e)
{
  if (shouldHandleKey(e))
  {
    e.stopImmediatePropagation();
  }
}
window.addEventListener('keydown', handleKeyEvent, true);
window.addEventListener('keyup', handleKeyEvent, true);
window.addEventListener('keypress', handleKeyEvent, true);

function handleMouseEvent(e)
{
  if (!document.body.classList.contains('page-game')) e.stopImmediatePropagation();
}
['mousedown', 'mouseup', 'mousemove', 'wheel'].forEach(function (name) { window.addEventListener(name, handleMouseEvent, true); });

function handleBeforeUnload(e)
{
  e.preventDefault();
  e.returnValue = "Remember to save the game before exiting the page.";
}
window.addEventListener('beforeunload', handleBeforeUnload);

var Game_Pages = ['game', 'settings', 'controls', 'log'];
var Game_PageButtons = {
  game: document.getElementById('btn-game'),
  settings: document.getElementById('btn-settings'),
  controls: document.getElementById('btn-controls'),
  log: document.getElementById('btn-log'),
};

function showPanel(page)
{
  if (page === true) page = 'log';
  else if (page === false) page = 'game';

  Game_Pages.forEach(function (p) {
    document.body.classList.toggle('page-' + p, p === page);
    if (Game_PageButtons[p]) Game_PageButtons[p].classList.toggle('active', p === page);
  });
  if (typeof Shell_ViewShown === 'function') Shell_ViewShown(page);
  Game_UpdateActivity();
}

var Game_PlayerInteracted = false;
var Game_ToldActive = null;

function Game_UpdateActivity()
{
  var gamePane = document.body.classList.contains('page-game');
  var active = gamePane && (!Game_PlayerInteracted || (document.visibilityState === 'visible' && document.hasFocus()));

  if (active === Game_ToldActive || typeof Module === 'undefined' || !Module.calledRun) return;
  try
  {
    Module.ccall('SRW_SetActive', null, ['number'], [active ? 1 : 0]);
    Game_ToldActive = active;
  }
  catch (e)
  {
  }
}

(function () {
  function interacted(e)
  {
    if (!Game_Started || !document.body.classList.contains('page-game')) return;
    if (e.target && e.target.closest && e.target.closest('#topbar')) return;
    if (!Game_PlayerInteracted)
    {
      Game_PlayerInteracted = true;
      Game_UpdateActivity();
    }
  }
  ['pointerdown', 'keydown', 'wheel'].forEach(function (name) { window.addEventListener(name, interacted, true); });
  document.addEventListener('visibilitychange', Game_UpdateActivity);
  window.addEventListener('focus', Game_UpdateActivity);
  window.addEventListener('blur', Game_UpdateActivity);
  setInterval(function () { if (Game_ToldActive === null) Game_UpdateActivity(); }, 1000);
})();

document.getElementById('topbar').addEventListener('click', function () {
  var a = document.activeElement, bar = document.getElementById('topbar');
  if (a && a !== document.body && bar.contains(a) && a.blur) a.blur();
});

(function () {
  var button = document.getElementById('btn-keyboard');

  button.addEventListener('pointerdown', function (e) { e.preventDefault(); });
  button.addEventListener('click', function () {
    if (typeof Module !== 'undefined' && Module.srwKeyboard) Module.srwKeyboard.toggle();
  });
})();

Game_Pages.forEach(function (p) {
  if (Game_PageButtons[p]) Game_PageButtons[p].addEventListener('click', function () { showPanel(p); });
});

var Game_Pointer = null;
var Game_BarMargin = 24;

function Game_PointerInBarArea()
{
  var bar = document.getElementById('topbar');

  if (!Game_Pointer || !bar) return false;
  var r = bar.getBoundingClientRect(), quality = document.getElementById('quality'), left = r.left, right = r.right, bottom = r.bottom;

  if (quality && !quality.hidden)
  {
    var q = quality.getBoundingClientRect();

    left = Math.min(left, q.left);
    right = Math.max(right, q.right);
    bottom = Math.max(bottom, q.bottom);
  }
  return Game_Pointer.x >= left - Game_BarMargin && Game_Pointer.x <= right + Game_BarMargin && Game_Pointer.y <= bottom + Game_BarMargin;
}

function Game_UpdateBarIdle()
{
  document.body.classList.toggle('ui-idle', !Game_PointerInBarArea());
}

window.addEventListener('mousemove', function (e) {
  Game_Pointer = { x: e.clientX, y: e.clientY };
  Game_UpdateBarIdle();
}, true);
