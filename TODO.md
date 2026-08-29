Netatalk Client Improvements
============================

Protocol features
-----------------

* ACL support and directory identity
  * AFP 3.2+ capability detection plus FPGetACL and FPSetACL
  * lossless AFP ACL model and platform-specific FUSE exposure
  * server-authoritative UUID/name mapping via FPMapID and FPMapName
* Connection continuity and recovery
  * recovery state machine, shutdown policy, and status reporting
  * server identity, session-token, and Reconnect UAM recovery
  * safe reopening of active forks after a new AFP session
  * persistent lock recovery, if the client adds persistent lock support
  * fault-injection tests for transport loss and server shutdown

Authentication
--------------

* Kerberos SSO
  * GSSAPI credential-cache login, AFP continuation loop, and session-key unwrap
  * directory-service principal discovery and explicit-principal override
  * optional build support and no-downgrade kerberos-required policy
* LDAP directory integration
  * optional GSSAPI-bound identity, UUID, and group resolution for local mapping
  * TLS, bounded identity cache, and Active Directory objectGUID support
* Support fpLoginExt command in addition to fpLogin

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
