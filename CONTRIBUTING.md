# Contributing to TermTalk

TermTalk is a C11 / Windows API project in early development. The implementation
and public API comments are documented in Russian; the interface supports EN/RU.
The project is distributed under the [MIT License](LICENSE).

## Build and test

Use the [README](README.md) build instructions. Configure with Python 3 installed
for all 15 CTest checks. For legacy MinGW Make use `comp.bat`; generated files
belong in `build/` and must not be committed. See [verification](docs/BUILD-VERIFICATION.md).
The proposed Windows GitHub Actions workflow has not yet run on GitHub.

## Making a change

1. Read the relevant [architecture](docs/ARCHITECTURE.md) and [protocol](docs/PROTOCOL.md)
   sections. A proposed feature is not an implemented contract.
2. Keep changes focused. Explain the problem, resulting behavior and compatibility.
3. Update meaningful tests, documentation and `docs/roadmap.json` when behavior changes.
4. Review the diff, build affected targets and run the relevant checks.

Use four-space indentation, explicit braces and module-consistent names.
Keep internal functions `static` and the public API small. Do not introduce
mutable globals or mix console rendering, file I/O and socket ownership.
Document public types/functions in Russian Doxygen: purpose, parameters, results,
preconditions, ownership and limits. Explain synchronization, partial operations
and non-obvious decisions in implementation comments. See [Doxygen](docs/DOXYGEN.md).

Sockets and handles have a single owner. Stop/join workers before destroying
resources they can use. Validate lengths before narrowing integer conversions.
Do not commit generated binaries, runtime history, received files or secrets.
Keep local scratch programs outside application targets and the public repository.

## Pull requests

Use the PR template. Include what changed, why, validation and material limitations.
Do not claim CI, compiler or platform coverage that has not been verified.
Use the issue templates for reproducible bugs and feature proposals; do not post
private conversation history or sensitive files. See [security](SECURITY.md).

## Versioning

Follow [Semantic Versioning and the release process](docs/VERSIONING.md).
Before 1.0, new features raise MINOR and compatible fixes raise PATCH.
A release requires matching CMake version, changelog and an annotated `vX.Y.Z` tag
on a verified commit. Never move published tags. Push and publication are separate
steps. The public main branch starts with the GitHub license commit and a clean
project import; earlier development branches and tags remain local.
