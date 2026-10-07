/* Input the host gives the game: scripted clicks and keys, and a gamepad
 * (input.c, docs/architecture.md). */
#pragma once
#include <stdint.h>

/* Command-line options: --click x,y@s, --key NAME@s[:hold_ms]; s may be a<seconds>, --gamepad,
 * --no-gamepad. Returns how many argv entries were consumed (0: not ours). */
int input_arg(int argc, char** argv, int i);
void input_help(void);

/* After DirectInput8Create succeeds: hook the keyboard device it will create. */
void input_on_directinput(void* di8);

/* Every presented frame, from the SwapBuffers shim: the window the game draws in. */
void input_on_frame(void* hwnd);

/* The game's SetCursorPos(x, y) (screen coordinates). Headless: posts the
 * WM_MOUSEMOVE a shown window would get and returns 1; otherwise 0 (call the real one). */
int input_set_cursor(int x, int y);

/* The main menu is up (its music opened): the script's clock starts here. */
void input_menu_up(void);

/* The first frame after an area (modules\*.rim) loaded: the clock for a-times. */
void input_area_up(void);

/* Start the script and gamepad threads. headless: no gamepad unless --gamepad. */
void input_start(int headless);

/* kotor.exe --selftest-input: the gamepad mapping and the option parser. 0: ok. */
int input_selftest(void);
