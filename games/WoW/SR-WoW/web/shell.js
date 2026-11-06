var Shell = { startTab: null, error: false };

function Shell_LogLine(component, text)
{
  text = String(text);
  return /^\[(Game|Server|P2P|D3D9|Shell)\] /.test(text) ? text : '[' + component + '] ' + text;
}

function Shell_Print(text)
{
  Module.print(Shell_LogLine('Shell', text));
}


function Shell_StartGroups()
{
  return Game_SettingsGroupOrder.filter(function (group) {
    return Game_SettingsFields.some(function (field) { return field.start && field.group === group; });
  });
}

function Shell_ClearStartRows()
{
  var tabs = document.getElementById('start-tabs'), rows = document.getElementById('start-rows');

  if (tabs) tabs.innerHTML = '';
  if (rows) rows.innerHTML = '';
}

function Shell_BuildStart()
{
  var tabs = document.getElementById('start-tabs'), rows = document.getElementById('start-rows'), panel = document.getElementById('settings-panel');
  var groups = Shell_StartGroups(), params = Game_ParseFragmentParams();

  if (!tabs || !rows) return;
  if (panel) panel.innerHTML = '';
  Shell_ClearStartRows();
  if (groups.indexOf(Shell.startTab) < 0) Shell.startTab = groups[0] || null;
  groups.forEach(function (group) {
    var tab = document.createElement('button');

    tab.type = 'button';
    tab.className = 'start-tab' + (group === Shell.startTab ? ' active' : '');
    tab.textContent = Game_SettingsGroupLabels[group];
    tab.setAttribute('role', 'tab');
    tab.setAttribute('aria-selected', group === Shell.startTab ? 'true' : 'false');
    tab.addEventListener('click', function () { Shell.startTab = group; Shell_BuildStart(); });
    tabs.appendChild(tab);
  });
  var table = Game_BuildFieldTable();
  Game_SettingsFields.forEach(function (field) {
    if (field.start && field.group === Shell.startTab) table.tBodies[0].appendChild(Game_BuildFieldRow(field, params));
  });
  rows.appendChild(table);
}

function Shell_ViewShown(page)
{
  Shell_ClearError();
  if (page === 'settings') Game_BuildSettingsPanel();
  else if (page === 'game' && Game_StartOverlayActive) Shell_BuildStart();
  else Shell_ClearStartRows();
}

function Shell_FocusField(key)
{
  var field = Game_SettingsFields.filter(function (f) { return f.key === key; })[0];

  if (!field) return;
  if (field.start && Game_StartOverlayActive)
  {
    Shell.startTab = field.group;
    showPanel('game');
  }
  else showPanel('settings');
  var control = document.querySelector('[data-key="' + key + '"]');

  if (control)
  {
    if (control.scrollIntoView) control.scrollIntoView({ block: 'nearest' });
    control.focus();
  }
}

function Shell_Refresh()
{
  var area = document.getElementById('shell-message'), text = document.getElementById('status-text'), bar = document.getElementById('shell-progress');

  if (area) area.classList.toggle('has-text', !!text.textContent || !bar.hidden);
}

function Shell_Message(text, kind)
{
  var element = document.getElementById('status-text');

  if (!element) return;
  if (kind !== 'error' && Shell.error) return;
  Shell.error = kind === 'error';
  element.textContent = text;
  element.classList.toggle('error', Shell.error);
  Shell_Refresh();
}

function Shell_ClearError()
{
  if (!Shell.error) return;
  Shell.error = false;
  if (typeof Module === 'object' && Module.setStatus) Module.setStatus.last = null;
  Shell_Message('');
}

function Shell_Progress(label, fraction)
{
  var bar = document.getElementById('shell-progress'), text = document.getElementById('shell-progress-label'), fill = document.getElementById('shell-progress-fill');

  if (!bar) return;
  bar.hidden = label === null || label === undefined;
  if (!bar.hidden)
  {
    text.textContent = label;
    bar.classList.toggle('unknown', fraction === null || fraction === undefined);
    fill.style.width = (fraction === null || fraction === undefined) ? '' : Math.max(0, Math.min(1, fraction)) * 100 + '%';
  }
  Shell_Refresh();
}

function Shell_MessageToGame()
{
  var area = document.getElementById('shell-message'), container = document.getElementById('ui-container');

  if (area && container) container.appendChild(area);
}

function Shell_QualityFill(latency)
{
  if (!(latency > 50)) return 1;
  if (latency >= 1000) return 0;
  return 1 - Math.log(latency / 50) / Math.log(1000 / 50);
}

function Shell_QualityText(q)
{
  if (q.state !== 'around' && q.state !== 'everyone') return 'no connection';
  var ms = Math.round(q.latency), n = q.count, who = q.state === 'around' ? (n === 1 ? ' character around' : ' characters around') : (n === 1 ? ' character connected, none around' : ' characters connected, none around');

  return 'effective latency ' + (ms >= 1000 ? '1000 ms or worse' : ms + ' ms') + ', loss ' + Math.round(q.loss * 100) + ' %, ' + n + who;
}

function Shell_QualityWidth()
{
  var bar = document.getElementById('topbar'), box = document.getElementById('quality');

  if (bar && box) box.style.width = Math.max(240, bar.offsetWidth) + 'px';
}

function Shell_Quality(q)
{
  var box = document.getElementById('quality'), fill = document.getElementById('quality-fill'), label = document.getElementById('quality-text');

  if (!box || !fill) return;
  if (!q || q.state === 'off')
  {
    box.hidden = true;
    return;
  }
  var live = q.state === 'around' || q.state === 'everyone';

  box.hidden = false;
  Shell_QualityWidth();
  fill.style.width = (live ? Shell_QualityFill(q.latency) * 100 : 0) + '%';
  var text = Shell_QualityText(q);

  if (label) label.textContent = text;
  box.setAttribute('aria-label', 'Connection quality: ' + text);
}

if (typeof ResizeObserver === 'function' && document.getElementById('topbar')) new ResizeObserver(Shell_QualityWidth).observe(document.getElementById('topbar'));

Shell_BuildStart();
