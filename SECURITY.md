# Security notes

The mesh currently authenticates and encrypts frames with a shared network group key. Every provisioned node that holds this key can generate traffic accepted as authenticated by other nodes. The design does not provide per-node cryptographic identity isolation, so the trusted-node threat model assumes provisioned nodes and operators are trusted.

Replay windows are held in RAM and reset at reboot. The replay source table holds 32 nodes, matching the route table capacity; when full it deterministically evicts the least recently active source. A sufficiently large active network can therefore cause an evicted source's replay window to be relearned. Deployments should keep trusted nodes within the configured table capacity.

The portal is HTTP-only. Session cookies use `HttpOnly` and `SameSite=Lax`; the `Secure` flag is omitted because the portal does not use HTTPS. Sessions expire after 15 minutes of inactivity and are invalidated on reboot.
