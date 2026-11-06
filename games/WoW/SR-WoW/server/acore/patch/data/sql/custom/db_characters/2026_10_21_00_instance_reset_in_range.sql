DELETE FROM `instance_reset` WHERE `resettime` > CAST(strftime('%s', 'now') AS INTEGER) + 2000000000;
