#pragma once

// Tests on the consoles, for DEBUG builds: tools/console_tests.py drives them over
// the FTP server. Faults armed by name make a step fail on purpose (an SD write, a
// chunk on a connection...), and commands open screens and say what is on screen.
// See docs/PLAN.md, "Tests on the consoles".

void selftest_init();

#if NSA_DEBUG
// True when the fault `point` is armed and its turn has come: the caller fails
// as the real thing would. Any thread.
bool selftest_fault(const char* point);
#else
static inline bool selftest_fault(const char* point) {
    (void) point;
    return false;
}
#endif

// SITE FAULT (FTP thread): arms `point` to fail `count` times after `skip` hits.
void selftest_fault_arm(const char* point, u32 skip, u32 count);
void selftest_fault_clear();
// "point hits/fired ..." for every armed fault.
void selftest_fault_list(char* out, size_t size);

// SITE SHA256 (FTP thread): the hash of a file, in hex; false when it can not be read.
bool selftest_sha256(const char* path, char* hex, size_t size);

// SITE TEST (main thread): runs `command` and writes the reply line, code first.
void selftest_run(const char* command, char* reply, size_t size);
