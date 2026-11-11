# Release notes

One file per tag and interface language, written and merged into `release` **before** the
tag exists:

- `vX.Y.Z.md`: English. The release workflow copies it into the release body between
  `<!-- nsa-notes -->` markers, and the updater on the console shows that block when it
  offers an update (the commit list when the file is missing).
- `vX.Y.Z.<code>.md`, **one for every other language of the interface**: `es`, `ca`, `fr`,
  `de`, `it`, `pt`, `ru`, `ja`, `zh`, `tw` (the codes in `source/core/i18n.c`; a new
  language there needs a file for every tag too). Same lines as the English, in the same
  order. Menu, screen and setting names as the app shows them in that language (its
  `source/core/i18n_<code>.c`); paths, button names and product names unchanged.
  Portuguese is Brazilian, like the app; Japanese and Chinese use the system font (no emoji).

Settings -> Changelog reads the notes in the interface language (English where a file is
missing) from pages on GitHub Pages: the workflow runs `tools/make_changelog.py` after
publishing and pushes them to `feat/website` (`docs/changelog/<code>/<n>.json`). Run it by
hand to republish them after fixing a translation:

    python3 tools/make_changelog.py --out ../site/docs/changelog   # a checkout of feat/website

The release bodies keep only English: NSA v0.9.9 and older read every body at once.

- 3 to 6 short lines, each starting with `- `, plain text for the user.
- A **beta** lists only what is new since the previous tag.
- A **stable** lists what is new since the previous tag; it need not repeat what its betas
  said. The Changelog shows every release up to the installed one, betas included, on
  both channels, and the updater on the stable channel joins the notes of the betas
  before the stable it offers.
