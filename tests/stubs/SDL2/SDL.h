/* Cabecera mínima SOLO para revisar la sintaxis. */
#pragma once
#include <stdint.h>
typedef struct SDL_Window SDL_Window; typedef void* SDL_GLContext; typedef struct _SDL_GameController SDL_GameController;
enum { SDL_INIT_VIDEO = 0x20, SDL_INIT_GAMECONTROLLER = 0x2000 };
enum { SDL_GL_DEPTH_SIZE = 6, SDL_GL_CONTEXT_PROFILE_MASK = 21, SDL_GL_CONTEXT_MAJOR_VERSION = 17, SDL_GL_CONTEXT_MINOR_VERSION = 18 };
enum { SDL_GL_CONTEXT_PROFILE_ES = 4 };
enum { SDL_WINDOW_OPENGL = 2, SDL_WINDOW_FULLSCREEN_DESKTOP = 0x1001 };
#define SDL_WINDOWPOS_UNDEFINED 0x1FFF0000
enum { SDL_QUIT = 0x100, SDL_KEYDOWN = 0x300, SDL_CONTROLLERBUTTONDOWN = 0x651 };
enum { SDLK_ESCAPE = 27, SDLK_SPACE = 32, SDLK_RIGHT = 0x4000004F, SDLK_LEFT = 0x40000050,
       SDLK_PAGEDOWN = 0x4000004E, SDLK_PAGEUP = 0x4000004B };
typedef uint8_t Uint8;
enum { SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82,
       SDL_SCANCODE_MINUS = 45, SDL_SCANCODE_EQUALS = 46 };
enum { SDLK_c = 'c', SDLK_t = 't', SDLK_l = 'l', SDLK_y = 'y', SDLK_n = 'n' };
enum { SDL_CONTROLLER_AXIS_LEFTX = 0, SDL_CONTROLLER_AXIS_LEFTY = 1, SDL_CONTROLLER_AXIS_TRIGGERLEFT = 4,
       SDL_CONTROLLER_AXIS_TRIGGERRIGHT = 5 };
enum { SDL_CONTROLLER_BUTTON_B = 1, SDL_CONTROLLER_BUTTON_X = 2, SDL_CONTROLLER_BUTTON_Y = 3,
       SDL_CONTROLLER_BUTTON_BACK = 4, SDL_CONTROLLER_BUTTON_DPAD_UP = 11, SDL_CONTROLLER_BUTTON_DPAD_DOWN = 12 };
enum { SDL_CONTROLLER_BUTTON_A = 0, SDL_CONTROLLER_BUTTON_START = 6, SDL_CONTROLLER_BUTTON_LEFTSHOULDER = 9,
       SDL_CONTROLLER_BUTTON_RIGHTSHOULDER = 10, SDL_CONTROLLER_BUTTON_DPAD_LEFT = 13,
       SDL_CONTROLLER_BUTTON_DPAD_RIGHT = 14 };
typedef struct { int32_t sym; } SDL_Keysym;
typedef struct { uint32_t type; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct { uint32_t type; uint8_t button; } SDL_ControllerButtonEvent;
typedef union { uint32_t type; SDL_KeyboardEvent key; SDL_ControllerButtonEvent cbutton; } SDL_Event;
extern "C" {
int SDL_Init(uint32_t); const char* SDL_GetError(void); int SDL_GL_SetAttribute(int, int);
SDL_Window* SDL_CreateWindow(const char*, int, int, int, int, uint32_t); SDL_GLContext SDL_GL_CreateContext(SDL_Window*);
int SDL_GL_SetSwapInterval(int); int SDL_NumJoysticks(void); int SDL_IsGameController(int);
SDL_GameController* SDL_GameControllerOpen(int); void SDL_GL_GetDrawableSize(SDL_Window*, int*, int*);
uint32_t SDL_GetTicks(void); int SDL_PollEvent(SDL_Event*); void SDL_Delay(uint32_t); void SDL_GL_SwapWindow(SDL_Window*);
void SDL_GL_DeleteContext(SDL_GLContext); const Uint8* SDL_GetKeyboardState(int*);
uint8_t SDL_GameControllerGetButton(SDL_GameController*, int); int16_t SDL_GameControllerGetAxis(SDL_GameController*, int); void SDL_DestroyWindow(SDL_Window*); void SDL_Quit(void);
}
