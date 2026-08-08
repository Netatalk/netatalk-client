Netatalk Client Improvements
=====================

AFP 3.x
-------

* ACL support

AFP 2.x
-------

* use getsrvrinfo to get connection IP address to make room for AT
* connection recovery
  * open files
  * locked files
* desktop database support
* UTF8 flag is now server-specific, but it should be volume-specific

Authentitcation
---------------

* ClientKRB
* reconnection
* Open directory integration

Performance
-----------

* in mknod(), you only need to do the setfiledirparms if the mode or perms
are different
* asynchronous unlocking
* use rx and tx quantums properly
* queue writes to be one tx quantum
* optimize locking
* don't go back through the select loop to read what comes after the DSI
packet
* make a preallocated pool of dsi requests
* make a preallocated pool for dsi messages
* is_dir function should look in did cache
* check to see how Mac OS does locking on writes
* large block writes for FUSE 3.x

Protocol bugs
-------------

* Netatalk Client doesn't handle the situation where the server is shutdown
* reconnect isn't reliable
* If a DSI stream gets broken or there's a protocol error, the connection
  should be reset
* for logins, fpLoginExt should be used instead of fpLogin
* for fpCreateFile, use soft creates
* honour volume's HasConfigInfo flag
