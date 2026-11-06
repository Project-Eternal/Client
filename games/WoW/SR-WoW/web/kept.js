var Kept = {
  realm: 1,
  realms: [1, 2],
  owners: []
};

Kept.owners.push({ name: 'game', label: 'game', root: '/WoW', dirs: ['WTF', 'Cache', 'Screenshots'], enabled: function () { return true; } });
Kept.owners.push({ name: 'acore', label: 'embedded server', root: '/AzerothCore', dirs: ['etc'].concat(Kept.realms.map(function (r) { return 'realm/' + r + '/db'; })),
                   enabled: function () { return typeof Game_EmbeddedServerWanted === 'function' && Game_EmbeddedServerWanted(); } });

function Kept_Owner(name)
{
  return Kept.owners.filter(function (owner) { return owner.name === name; })[0];
}

function Kept_Paths(owner)
{
  return owner.dirs.map(function (dir) { return owner.root + '/' + dir; });
}
