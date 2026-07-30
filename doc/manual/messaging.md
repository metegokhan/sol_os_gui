+++
id = "messaging"
title = "Messaging, contacts, and credential security"
section = "service"
summary = "Provider-neutral messaging identities, trust, persistence, and secret handling"
aliases = ["contacts.service", "credentials"]
keywords = "messaging contacts credentials trust endpoint gateway meshcore security"
packages_any = ["service_contacts", "service_credentials"]
+++
# Messaging identities and security

SolarOS separates messaging identity from transport. A contact is one local
address-book entry, while each gateway or MeshCore address is an endpoint with
its own trust and capabilities. Linking endpoints does not copy trust between
providers.

Trust has three states:

- `discovered`: a provider supplied a valid address or advert;
- `trusted`: the user explicitly accepts that endpoint identity;
- `blocked`: direct inbound and outbound messaging is denied.

A signed MeshCore advert proves ownership of its public key, not the human
identity using the device. New adverts therefore remain `discovered` until the
user trusts them.

Contacts keeps at most 64 contacts and 80 endpoints in PSRAM. Its bounded,
CRC-checked store uses alternating headers and data copies under
`/.contacts/contacts.bin`. Persistence writes and `fsync` occur after releasing
the service lock. If the filesystem is unavailable, the service stays usable
in volatile mode and reports the error through `contacts status`.

Credentials keeps at most 12 opaque NVS records with at most 128 secret bytes
each. Supported record kinds are asymmetric identities, shared keys, and
tokens. Public listing exposes only the record ID, provider, kind, and label.
Secret reads name one record and copy into a caller-owned buffer; temporary
buffers are wiped.

Credentials are never exposed through Inbox, logs, autocomplete, native-agent
tools, Python, or Lua. The current SolarOS NVS configuration is not encrypted,
so someone with physical flash access can recover the stored secrets. Flash
and NVS encryption provisioning are outside this release.

SSH keys, native-agent API keys, and existing provider tokens remain in their
current stores and are not migrated automatically.

See `contacts` for the TUI and mutation commands. See `chat` and `messages` for
provider-neutral conversations once those packages are compiled.

## Quick reference

```text
contacts
contacts status
contacts list [all|discovered|trusted|blocked]
contacts show CONTACT_ID
contacts rename CONTACT_ID NAME
contacts trust CONTACT_ID [ENDPOINT_ID]
contacts block CONTACT_ID [ENDPOINT_ID]
contacts remove CONTACT_ID
contacts link TARGET_CONTACT_ID SOURCE_CONTACT_ID
```
