#pragma once

// hyprland's ipc: requests on its .socket.sock, and the event stream of its .socket2.sock.

// sends request (such as "j/workspaces") and returns the reply, which the caller frees, or
// nullptr when hyprland cannot be reached.
char *
HyprlandRequest(const char *request);

// runs a dispatcher, as `hyprctl dispatch argument` does.
void
HyprlandDispatch(const char *argument);

// calls listener with each event's name and data ("workspace", "2"), from now on.
// a "connected" event comes after each (re)connection, as events may have been missed.
// the strings are only valid during the call.
void
HyprlandListen(void (*listener)(const char *name, const char *data));
