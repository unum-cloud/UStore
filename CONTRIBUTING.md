# Contributing to UKV

When contributing to the development of UKV, please first discuss the change you wish to make via issue, email, or any other method with the maintainers before making a change.

## Starting with Issues

Start contributing by adding new issue or continuing an [existing](https://github.com/unum-cloud/ukv/issues) one.
To create an effective and high quality ticket, try to add the following information:

* Steps to reproduce the issue, or a minimal test case.
* Hardware and Operating System environment specs.

Here is a template:

```txt
Title, like "UKV crashes when I do X"

The X consists of multiple steps...
1.
2.
3.

## Environment

* Compiler: GCC 10.
* OS: Ubuntu 20.04.
* ISA: x86.
* CPU: Intel Tiger Lake.

## Config

I am using the default config.
```

## Commits and Commit Messages

1. Subject ~~top~~ line is up to 50 characters.
   1. It shouldn't end with a period.
   2. It must start with verb.
   3. It can contain the programming language abbreaviation.
2. Description lines are optional and limited to 72 characters.
   1. Use the body to explain what and why vs. how. Code already answers the latter.
   2. If relevant, reference the issue at the end, using the hashtag notation.

A nice commit message can be:

```txt
Add: Support for shared memory exports

This feature minimizes the amount of data we need to transmit
through sockets. If the server and clients run on the same machine,
clients could access exported data in shared memory only knowing
its address (pointer).

See: #XXX
```

### Verbs

We agree on a short list of leading active verbs for the subject line:

* Add = Create a capability e.g. feature, test, dependency.
* Cut = Remove a capability e.g. feature, test, dependency.
* Fix = Fix an issue e.g. bug, typo, accident, misstatement.
* Make = Change the build process, dependencies, versions, or tooling.
* Refactor = A code change that MUST be just a refactoring.
* Form = Refactor of formatting, e.g. omit whitespace.
* Perf = Refactor of performance, e.g. speed up code.
* Docs = Refactor of documentation or spelling, e.g. help files.

Which is a well known and widely adopted set.

## Pull Request Process

1. Ensure your code compiles. Run `cmake . && make` before creating the pull request.
2. Feel free to open PR Drafts for larger changes, if you want a more active participation from maintainers and the community.
3. Merge into `dev` branch. The `main` branch is protected.
