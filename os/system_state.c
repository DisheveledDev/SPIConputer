/* system_state.c
 *
 * Definition of the shared state object (see system_state.h). Single
 * definition, linked by the firmware and every host test target.
 */
#include "system_state.h"

system_state_t g_system_state;
