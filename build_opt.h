// Arduino IDE picks this file up automatically when present in the sketch
// folder and forwards each line to the compiler as a -D flag.
//
// We only use NimBLE in observer (passive scan) mode — we don't connect to
// peers, expose a GATT server, advertise, or pair. These flags strip the
// connection, central, peripheral, broadcaster, and bond storage code paths,
// freeing ~10–15 KB of internal heap that would otherwise be reserved for
// connection state and ATT/SMP buffers we never use.
-DCONFIG_BT_NIMBLE_MAX_CONNECTIONS=0
-DCONFIG_BT_NIMBLE_ROLE_CENTRAL=0
-DCONFIG_BT_NIMBLE_ROLE_PERIPHERAL=0
-DCONFIG_BT_NIMBLE_ROLE_OBSERVER=1
-DCONFIG_BT_NIMBLE_ROLE_BROADCASTER=0
-DCONFIG_BT_NIMBLE_MAX_BONDS=0
-DCONFIG_BT_NIMBLE_PINNED_TO_CORE=1
