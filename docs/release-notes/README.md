# Release notes

One file per tag, `vX.Y.Z.md`, written and merged into `release` **before** the tag
exists. The release workflow copies it into the release body between
`<!-- nsa-notes -->` markers, and the updater on the console shows that block
(the commit list when the file is missing).

- 3 to 6 short lines, each starting with `- `, plain text for the user, in English.
- A **beta** lists only what is new since the previous tag.
- A **stable** lists everything since the previous *stable* tag, betas included:
  someone on the stable channel never sees the beta pages, although the updater
  joins the notes of every release newer than the installed one.
