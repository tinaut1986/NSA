#pragma once

#define APP_NAME "NSA"

// Data folder on the SD card. Upstream FBI used "/fbi/"; NSA never writes there.
#define APP_DATA_DIR "/nsa/"
#define APP_LEGACY_DATA_DIR "/fbi/"

#define APP_REPOSITORY "tinaut1986/NSA"
// The GitHub Pages site (branch feat/website, folder /docs): the web page and the changelog's pages.
#define APP_PAGES_URL "https://tinaut1986.github.io/NSA/"

// NSA has its own version numbers; this is the FBI release it is based on.
#define APP_UPSTREAM_VERSION "FBI 2.6.1"
