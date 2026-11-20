Security Policy
===============

Knurbs is an independent fork of OBS Studio and is not supported by the OBS
Project. Please report security issues in Knurbs to Knurbs, not to the OBS
Project. If an issue also affects OBS Studio, you may report it to them
separately, following their own policy.

Supported versions
------------------

Knurbs is under heavy development and has no stable releases yet. Only the
`root` branch is supported; fixes land there.

Reporting a vulnerability
-------------------------

Report vulnerabilities privately through GitHub's
[private vulnerability reporting](https://github.com/Xaymar/knurbs/security/advisories/new)
on [Xaymar/knurbs](https://github.com/Xaymar/knurbs). Please do **not** open a
public issue or pull request for a vulnerability until it has been fixed.

A good report contains:

- the affected commit or build, and the platform it runs on;
- what an attacker can do, and what they need to be able to do it;
- steps to reproduce, ideally with a short proof of concept.

What to expect:

- an acknowledgement as soon as the report has been read;
- a discussion of the issue in the private advisory, and credit in the fix and
  the advisory if you want it;
- a fix, with a regression test, on `root`, after which the advisory is
  published.

Knurbs is a small project without a fixed response time. Please give it a
reasonable amount of time to fix an issue before you disclose it publicly.

Scope
-----

In scope is the code in this repository: libobs, the graphics backends, the
frontends and the bundled plugins, plus the way the build bundles third-party
code. Vulnerabilities in third-party dependencies are best reported to their
maintainers first; tell us as well if Knurbs is affected.

Out of scope:

- attacks that need physical access to the user's device, or that need the
  user to install untrusted software or plugins first (including DLL planting);
- reports from automated tools or scanners that have not been verified by hand;
- bugs without a security impact; please open a regular
  [issue](https://github.com/Xaymar/knurbs/issues) for those.

Good faith
----------

Research done in good faith under this policy is welcome. Do not access or
change other people's data, do not disrupt other people's systems, and keep the
details private until the issue is fixed. Knurbs has no bug bounty.
