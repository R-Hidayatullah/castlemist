/// @file
/// @brief Process entry point.
///
/// Deliberately empty of logic: everything the application does lives in the
/// `ui` layer, so the entry point is the one place that never needs changing.

#include <windows.h>

#include "castlemist/ui/application.h"

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show_command) {
    return castlemist::ui::run(instance, show_command);
}
