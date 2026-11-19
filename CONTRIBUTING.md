Contributing to Knurbs
======================

Knurbs accepts contributions as pull requests on
[Xaymar/knurbs](https://github.com/Xaymar/knurbs), against the `root` branch.
Bugs and feature requests go to the
[issue tracker](https://github.com/Xaymar/knurbs/issues) there, and security
issues go through the [security policy](SECURITY.md).

Knurbs is not OBS Studio. Please do not send Knurbs issues or pull requests to
the OBS Project, and do not send OBS Studio issues here.

Knurbs is under heavy development and changes direction quickly. If you plan a
larger change, open an issue first, so we can check that it fits where the
project is going (see the [README](README.rst)).

## What we look for

- **Fixes come with tests.** A bug fix goes in its own commit, together with a
  regression test that fails without the fix. If a fix changes behaviour that
  plugins or users may rely on, say so in the commit message.
- **New code goes in new places.** Fixes from upstream OBS Studio are still
  merged into Knurbs. Large new code belongs in new files or directories,
  rather than in rewrites of upstream code in place, so those merges stay
  manageable.
- **Features as plugins.** Where a feature can live in a plugin instead of the
  application shell or libobs, it should, so that alternatives can replace it.
- **The same behaviour everywhere.** A feature should work the same way on
  every supported platform. Platform specific code belongs behind a common
  interface.

The canonical language for source code and other untranslated material is
American English. That covers commit messages, identifiers, comments and file
names.

## Naming

Identifiers that people type, such as plugin IDs, source type IDs and setting
names, are lowercase and use only `-` or `.` as separators: no uppercase and no
`_`, so they never need Shift. C identifiers are the only exception. Knurbs' own
plugins and source types use a `knurbs` prefix and a descriptive name. When a
source replaces an OBS one, register the old OBS ID as an alias and migrate its
settings.

## Code style

Code follows the [code style guidelines](CODESTYLE.md). In short:

- C follows the
  [Linux kernel coding style](https://www.kernel.org/doc/html/latest/process/coding-style.html).
- C++ follows `CODESTYLE.md`, with the
  [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
  as a baseline.
- Objective-C follows Apple's
  [conventions](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/ProgrammingWithObjectiveC/Conventions/Conventions.html),
  and Swift the
  [API Design Guidelines](https://www.swift.org/documentation/api-design-guidelines/).
- Lines are at most 120 characters long.

Formatting is checked by CI. Use `clang-format` for C, C++, Objective-C and
Objective-C++, `swift-format` for Swift and `gersemi` for CMake. The scripts in
`build-aux` run them for you.

## Commits

Commit messages follow the "50/72" rule: a title of at most 50 characters, a
blank line, then a description wrapped at 72 characters.

```
libobs: Fix a race when releasing a source

Explain what was wrong, why it happened, and why it is fixed this way.
```

The prefix names the module the commit changes, such as `libobs`,
`libobs-vulkan` or `obs-ffmpeg`. Use `plugins` for similar changes across
several plugins, `CI` for continuous integration files and `cmake` for the
build system. The commit history is a good guide when in doubt.

The commit message is the main documentation of a change. Describe what
changed and, more importantly, why, and why in this way. A pull request with a
single commit should be able to use its message as the description.

Each commit is one logical unit of change that builds and runs on its own. If a
change needs some fixes to existing code first, those fixes are their own
commits, or better, their own pull request. Spelling and purely cosmetic
changes may be grouped into one commit.

During review it is fine to push small fixup commits. Squash them into the
commits they belong to before the pull request is merged.

When you pick up someone else's unfinished work, keep them as the author:
cherry-pick their commits, or add them with a `Co-authored-by:` trailer when the
commits had to be rewritten.

## Pull requests

- Describe what changed, why, and how you tested it.
- Avoid force-pushing during review where you can, so reviewers can see what
  changed since their last look. Rebase only when necessary.
- Do not resolve review comments yourself. The reviewer resolves a comment once
  they are satisfied with how it was addressed.

## AI assisted contributions

Contributions made with the help of AI tools are welcome, on the same terms as
any other: you are responsible for the change. Review and understand
everything you submit, make sure it is tested, and make sure it does not copy
code under an incompatible licence. Credit the tool with a `Co-Authored-By:`
trailer that names it and its model. Unreviewed, low effort submissions will be
closed.

## Licence

Knurbs is licensed under the GNU General Public License, version 2 or later
(see [LICENSE](LICENSE)). By contributing, you agree that your contribution is
licensed under the same terms.
