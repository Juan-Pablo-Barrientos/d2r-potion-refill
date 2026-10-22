// Writes this mod's settings where the Potion Refill plugin looks for them:
// data/d2rloader/potion-refill/settings.toml in the output mod.
// D2RMM cannot install the plugin DLL itself. It must already be in
// <game>\d2rloader\plugins.

const settings = [
  '# Written by D2RMM. Change these settings in D2RMM.',
  '[potion-refill]',
  'enabled = ' + (config.enabled ? 'true' : 'false'),
  'match = "' + (config.match === 'exact' ? 'exact' : 'family') + '"',
  'log_refills = ' + (config.logRefills ? 'true' : 'false'),
  '',
].join('\r\n');

D2RMM.writeTxt('d2rloader\\potion-refill\\settings.toml', settings);

console.log(
  'Potion Refill: settings written. Make sure d2rl-potion-refill.dll is in ' +
    '<game>\\d2rloader\\plugins and start the game with D2RLoader.',
);
