/* system_state.c
 *
 * Definition of the shared state object (see system_state.h). Single
 * definition, linked by the firmware and every host test target.
 */
#include "system_state.h"

/* Diagnostic builds draw the pattern from the first frame; normal builds
 * raise the request only if boot finds no program (see core1/lua_main.c). */
#if defined(SPICOMPUTER_VIDEO_TEST_PATTERN)
system_state_t g_system_state = {
    .video_pattern_request = true,
};
#else
system_state_t g_system_state;
#endif
