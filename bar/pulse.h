#pragma once

// a libpulse main loop api that runs on the bar's sd-event loop, so pulseaudio (here
// pipewire-pulse) is driven by the same single threaded loop as everything else.
#include <pulse/mainloop-api.h>

// the api, ready to give to pa_context_new.
pa_mainloop_api *
PulseApi(void);
