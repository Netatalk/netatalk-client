Netatalk Client Improvements
============================

Protocol features
-----------------

* ACL support
* connection recovery
  * open files
  * locked files

Authentication
--------------

* ClientKRB
* Reconnect UAM
* Open directory integration

Performance
-----------

* asynchronous unlocking
* use rx and tx quantums properly
* queue writes to be one tx quantum
* optimize locking
* don't go back through the select loop to read what comes after the DSI
packet
* make a preallocated pool of dsi requests
* make a preallocated pool for dsi messages
* check to see how Mac OS does locking on writes

Protocol bugs
-------------

* Netatalk Client doesn't handle the situation where the server is shutdown
* reconnect isn't reliable
* If a DSI stream gets broken or there's a protocol error, the connection
  should be reset
* for fpCreateFile, use soft creates
* honour volume's HasConfigInfo flag
