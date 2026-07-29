/// @file
/// @brief The application's entry point, as seen by `WinMain`.
/// @ingroup ui

#pragma once

#include <windows.h>

/// @defgroup ui ui -- the Win32 shell
/// @brief The window, its widgets, and everything that reacts to the user.
///
/// This layer owns the only mutable application state and the only Win32
/// handles. Everything below it (extract, render, format, db) is callable from
/// a worker thread and knows nothing about the window.
/// @{

namespace castlemist::ui {

/// @brief Register the window classes, create the main window and run the message loop.
///
/// @param instance    The `HINSTANCE` handed to `WinMain`.
/// @param show_command The `nCmdShow` handed to `WinMain`.
/// @return The process exit code (the final message's `wParam`).
int run(HINSTANCE instance, int show_command);

} // namespace castlemist::ui

/// @}
