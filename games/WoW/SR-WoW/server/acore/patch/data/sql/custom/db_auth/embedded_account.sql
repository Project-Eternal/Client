REPLACE INTO `account` (`id`, `username`, `salt`, `verifier`, `expansion`) VALUES
(1, 'PLAYER', X'03057331DD9860064E83478E1D8A1980B11A061CE1E5CA794C258CF5C9473886', X'F4C9679ACC8666AF5FD54261600F63CFCA7CA4954045CC78CDFFBFC1BBF09F4D', 2);
DELETE FROM `account_access` WHERE `id` = 1;
REPLACE INTO `account_access` (`id`, `gmlevel`, `RealmID`, `comment`) VALUES (1, 0, 1, 'Normal realm: no game master powers');
REPLACE INTO `account_access` (`id`, `gmlevel`, `RealmID`, `comment`) VALUES (1, 3, 2, 'Sandbox realm: game master of the highest level');
UPDATE `realmlist` SET `name` = 'Normal', `address` = '10.99.0.1', `localAddress` = '10.99.0.1', `port` = 8085, `flag` = 0 WHERE `id` = 1;
REPLACE INTO `realmlist` (`id`, `name`, `address`, `localAddress`, `localSubnetMask`, `port`, `icon`, `flag`, `timezone`, `allowedSecurityLevel`, `population`, `gamebuild`) VALUES
(2, 'Sandbox', '10.99.0.1', '10.99.0.1', '255.255.255.0', 8085, 0, 0, 1, 0, 0, 12340);
REPLACE INTO `motd` (`realmid`, `text`) VALUES (-1, 'Welcome to an EternalCore server (based on AzerothCore).');
