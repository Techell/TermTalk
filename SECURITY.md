# Security

TermTalk currently sends messages and files without TLS or end-to-end encryption.
Nicknames are not authenticated identities. SHA-256 file checks detect content
mismatches; they do not authenticate a peer or a server. Local history is plaintext.
Use only with trusted peers on trusted networks and avoid sensitive data.

Do not post private history, keys, personal file contents or a working exploit
with sensitive data in public issues. A private reporting channel has not yet
been configured; one must be established before public distribution.

The [security design proposal](docs/SECURITY-DESIGN.md) describes threat models
and study plans. It is not a claim of implemented protection or an independent audit.
