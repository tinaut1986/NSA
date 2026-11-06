#pragma once

// The guide: how NSA works, in short chapters of a few pages each (guide.c has them).
//
// Every chapter has a stable id and the version that brought it (`since`). settings.json keeps the ids already
// seen (`guide_seen`) and the newest `since` this console has been offered (`guide_known`), so:
// - a first start offers the essential chapters;
// - an update that adds chapters offers only those, with the chapters they build on that were not seen yet
//   chained before them, and "Everything" adds every other chapter not seen;
// - each question comes once: answering it (even "Later") moves `guide_known` on. Settings -> Guide has them all.

// At start, after the other questions: offers the chapters this console has not been offered, then runs `next`
// (when the guide closes, or at once when there is nothing to offer).
void guide_startup(void (*next)());

// Settings -> Guide: every chapter, to play one again or all those not seen.
void guide_open();

// How many chapters there are, and how many have been seen.
void guide_progress(u32* seen, u32* total);
