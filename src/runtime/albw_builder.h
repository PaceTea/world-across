#ifndef ALBW_BUILDER_H
#define ALBW_BUILDER_H

// The builder: the translated game made on the player's machine, from their own card, the first
// time the launcher starts (albw_game.h). It runs what a developer runs by hand -- extracting the
// card's code, finding its functions, translating them to C, compiling and linking -- with the tools
// and, where the system has none, the Python and the compiler the package brings along.

enum { ALBW_BUILD_IDLE, ALBW_BUILD_RUNNING, ALBW_BUILD_DONE, ALBW_BUILD_FAILED };

struct albw_build_progress {
    int state;
    int stage, stages;                  // the stage now (from 1), of how many
    float fraction;                     // of this stage, or below 0 when it cannot be told
    char what[160];                     // what is being done, for the launcher to say
    char error[480];                    // why it failed
    char log[700];                      // the file everything it ran wrote to
};

// Where the module and the code it is a translation of are kept: <cache>/build.
const char *albw_build_dir(void);
const char *albw_build_module_path(void);
const char *albw_build_code_path(void);

// Builds from `card` on a thread of its own and calls `done` there at the end, with 1 if it worked.
// Returns 0 if a build is running already or none could be started.
int albw_build_start(const char *card, void (*done)(int ok));
void albw_build_progress(struct albw_build_progress *out);

// The same, here and now, printing how it goes: for a run with no window. Returns 1 if it worked.
int albw_build_run(const char *card);

// Whether the game there was made with this kit and has everything the last run met -- 0 after an
// update that brought another kit, or a run that reached untranslated code, and then it is made
// again.
int albw_build_current(void);

// Whether the last run met code the game lacks (missing_entries.txt beside the saves): the next
// build translates it as well.
int albw_build_has_missing(void);

#endif
