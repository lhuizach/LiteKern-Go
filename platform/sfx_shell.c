/* The SDK mixer, built for the shell (see uisound.c): its one-shot player forks twice, as the shell owns SIGCHLD. */
#define SFX_IN_SHELL 1
#include "sdk/sfx.c"
