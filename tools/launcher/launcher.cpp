// Monado launcher: edits xcde.ini (graphics, scaling, controls, audio, language) and starts the game.
// Standalone (SDL3 + Dear ImGui with the SDL renderer); the game reads the same file when its window opens
// (runtime/host/settings_ini.c). Build: tools/launcher/build.sh. Run: "Play Monado.bat" at the repository root.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <ctype.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include <string>
#include <sys/stat.h>
#include <algorithm>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "../../runtime/host/ini_writer.h"
#include "../../runtime/nvn/vulkan/gpu_scan.h"
#include "../../runtime/ui/gpu_picker.h"
#include "../../runtime/ui/theme.h"
#include "../../runtime/ui/quality_controls.h"
#include "../../runtime/grass_lod.h"
#include "../../runtime/graphics_options.h"
// [XCDE] GitHub #18: the build ids of the game NSOs this exe's code was lifted from (gen.py writes these
// with the functions). Older checkouts without them build fine, just without the version check below.
#if __has_include("../../generated/main/main_id.h")
#include "../../generated/main/main_id.h"
#endif
#if __has_include("../../generated/sdk/sdk_id.h")
#include "../../generated/sdk/sdk_id.h"
#endif

#ifdef _WIN32
#include <windows.h>
#define GAME_EXE "build/xcde.exe"
#else
// [XCDE] T-LINUX: the same launcher on Linux. The game is build/linux/monado (scripts/build_linux.sh); processes are
// started with posix_spawn, folders and messages go through SDL3.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#define GAME_EXE "build/linux/monado"
#endif

// ------------------------------------------------------------------ settings (same keys as settings_ini.c)
// the Switch inputs, then the keyboard / mouse actions (runtime/input_bind.h BIND_ART1..: the arts palette, the party)
enum { NACT = 35, NPAD = 16, NSTICK = 24 };
static const char *const k_ini[NACT] = {"a", "b", "x", "y", "lstick", "rstick", "l", "r", "zl", "zr", "plus", "minus",
                                        "left", "up", "right", "down", "ls_up", "ls_down", "ls_left", "ls_right",
                                        "rs_up", "rs_down", "rs_left", "rs_right",
                                        "art1", "art2", "art3", "art4", "art5", "art6", "art7", "art8", "talent",
                                        "party_up", "party_down"};
static const char *const k_names[NACT] = {"A (acción, usar arte, aceptar)", "B (saltar, atrás)", "X (menú principal)", "Y (mapa)",
                                          "Pulsar stick izq.", "Pulsar stick der.", "L (objetivo anterior)", "R (objetivo siguiente)",
                                          "ZL (cámara)", "ZR (comandos de combate)", "+ (menú del sistema)", "- (Minus)",
                                          "Cruceta izquierda (arte)", "Cruceta arriba", "Cruceta derecha (arte)", "Cruceta abajo",
                                          "Avanzar", "Retroceder", "Izquierda", "Derecha",
                                          "Cámara arriba", "Cámara abajo", "Cámara izquierda", "Cámara derecha",
                                          "Arte 1 (izquierda)", "Arte 2", "Arte 3", "Arte 4 (junto al centro)",
                                          "Arte 5 (junto al centro)", "Arte 6", "Arte 7", "Arte 8 (derecha)",
                                          "Arte de talento (centro)", "Personaje de arriba", "Personaje de abajo"};
// mouse buttons / wheel bind like keys (runtime/input_bind.h BIND_MOUSE_*; their xcde.ini names)
enum { MOUSE_BASE = 0x400, MOUSE_END = MOUSE_BASE + 8, M_LEFT = MOUSE_BASE + 1, M_MIDDLE, M_RIGHT, M_X1, M_X2, M_WUP, M_WDOWN };
static const char *const k_mouse_ini[8] = {"", "Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2", "Wheel Up", "Wheel Down"};
static const char *const k_mouse_ui[8] = {"", "Ratón izq.", "Ratón central", "Ratón der.", "Ratón lateral 1", "Ratón lateral 2",
                                          "Rueda arriba", "Rueda abajo"};
// controller buttons by position (runtime/hle/bindings.c k_pad_ini; 26/27 = the analog triggers)
enum { PADB_LT = 26, PADB_RT, PADB_COUNT };
static const char *const k_pad_ini[PADB_COUNT] = {
    "south", "east", "west", "north", "back", "guide", "start", "lstick", "rstick", "lshoulder", "rshoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "rpaddle1", "lpaddle1", "rpaddle2", "lpaddle2",
    "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6", "ltrigger", "rtrigger"};
static const char *const k_pad_label[PADB_COUNT] = {
    "Abajo (Xbox A / PS Cruz / Switch B)", "Derecha (Xbox B / PS Círculo / Switch A)", "Izquierda (Xbox X / PS Cuadrado / Switch Y)",
    "Arriba (Xbox Y / PS Triángulo / Switch X)", "Back / View / Share", "Guide / Home", "Start / Menu / Options",
    "Pulsar stick izq.", "Pulsar stick der.", "LB / L1 / L", "RB / R1 / R", "Cruceta arriba", "Cruceta abajo",
    "Cruceta izquierda", "Cruceta derecha", "Misc 1", "Paleta der. 1", "Paleta izq. 1", "Paleta der. 2", "Paleta izq. 2",
    "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6", "LT / L2 / ZL", "RT / R2 / ZR"};
static const int k_def_pad[NPAD] = {1, 0, 3, 2, 7, 8, 9, 10, PADB_LT, PADB_RT, 6, 4, 13, 11, 14, 12};
// (runtime/hle/bindings.c DEF_KEY / DEF_KEY2: keyboard + mouse as an MMO)
static const int k_def_key[NACT] = {
    SDL_SCANCODE_F, SDL_SCANCODE_SPACE, SDL_SCANCODE_I, SDL_SCANCODE_M, SDL_SCANCODE_LSHIFT, M_MIDDLE,
    SDL_SCANCODE_Q, SDL_SCANCODE_E, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_J,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_5,
    SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_6,
    SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7,
    SDL_SCANCODE_8, SDL_SCANCODE_T, SDL_SCANCODE_Z, SDL_SCANCODE_X};
static const int k_def_key2[NACT] = {
    SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, 0, 0, 0, SDL_SCANCODE_KP_0, 0, SDL_SCANCODE_TAB, 0, 0, 0, 0,
    0, SDL_SCANCODE_R, 0, SDL_SCANCODE_G};
static const char *T(const char *es);  // (launcher language, below)
static const char *key_ini_name(int k) { return k > MOUSE_BASE && k < MOUSE_END ? k_mouse_ini[k - MOUSE_BASE] : k ? SDL_GetScancodeName((SDL_Scancode)k) : "none"; }
static const char *key_ui_name(int k) { return k > MOUSE_BASE && k < MOUSE_END ? T(k_mouse_ui[k - MOUSE_BASE]) : k ? SDL_GetScancodeName((SDL_Scancode)k) : T("(ninguna)"); }
static int key_from_ini(const char *v) {
    for (int i = 1; i < 8; i++) if (!strcmp(v, k_mouse_ini[i])) return MOUSE_BASE + i;
    return strcmp(v, "none") ? SDL_GetScancodeFromName(v) : SDL_SCANCODE_UNKNOWN;
}

// [XCDE] Environment variables for the game (Diagnostics tab): NAME=VALUE lines in xcde_env.ini, applied to the game
// process when the launcher starts it (for example XCDE_RSC_PIXEL=1). A separate file because the in-game menu
// rewrites xcde.ini when the player applies settings (and xcde_ini_commit keeps old keys, so removals would survive).
enum { NENV = 32 };
struct EnvVar { char name[96]; char value[512]; };

struct Cfg {
    char gpu_uuid[33] = "";
    int vulkan_api = 0;
    bool fps_overlay = true, upscaler_auto = true;
    int gpu_index = -1;
    int quality_preset=0, dynamic_min_height=0, dynamic_max_height=0;
    unsigned effects_off=0;
    int gpu_profile = 0, gpu_gpl = -1;
    int occlusion = -1;     // [XCDE][FSR3] -1 automatic (off on AMD iGPUs), 0 off, 1 on (xcde.ini occlusion)
    int grass_lod = GRASS_DEFAULT;  // [XCDE] grass draw distance preset (xcde.ini grass_lod)
    int amd_fix = -1;       // issue #7: -1 automatic (on for AMD RDNA 3), 0 off, 1 on (xcde.ini amd_fix)
    GraphicsOptions graphics = {};  // [XCDE] graphics options (xcde.ini keys in graphics_options.h)
    int session_fix = -1;   // issue #25: -1 automatic (worker stays on), 0 off, 1 on = no async render worker (xcde.ini session_fix)
    bool gpu_memory_budget = true, gpu_performance = false, gpu_keep_heavy = false;
    bool fullscreen = false;
    int aspect = ASPECT_UNLOCKED;
    int win = 0;            // index into k_wins
    int res = 0;            // index into k_res
    int upscaler = 1;       // 0 bilinear, 1 FSR 1, 2 FSR 3.1, 3 DLSS
    float sharpness = -1.0f;  // [XCDE] RCAS strength 0..1; < 0: each upscaler's default (xcde.ini upscale_sharpness)
    int uquality = 1;       // FSR 3 / DLSS: 0 native AA, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    bool frame_gen = false;
    bool smaa = false;
    bool dof = true;        // the game's depth of field (T183)
    bool game_blur = true;  // the game's own AA / temporal AA / motion blur (lib_nx.ini)
    bool taa = true;        // the game's temporal AA alone (lib_nx.ini tmaa; false removes only it)
    bool vsync = true;      // FIFO presentation at 30 / 60 fps
    int cutscenes = 0;      // T224: 0 at the FPS setting, 1 at 30 (xcde.ini cutscenes = same | 30)
    bool cut_fg = false;    // T224: cutscenes at 30 + frame generation, gameplay without it (xcde.ini cutscenes = 30fg)
    bool party_switch = false;  // party mod: switch the controlled character (F2 / L2 + L1 / R1), runtime/hle/party.c
    bool show_fps = false;
    int fps = 0;  // index into k_fps_ini
    int volume = 100;
    bool movie_audio = true;
    int lang = 0;           // index into k_langs
    int pad[NPAD];
    int key[NACT], key2[NACT];  // SDL_Scancode or mouse (MOUSE_BASE + ...)
    int mouse_look = 1;         // the mouse moves the camera: 0 no, 1 with the right button held, 2 always
    int mouse_sens = 50;
    int stick_deadzone = 0;     // % of the stick's travel read as centred (0: raw)
    bool mouse_invert = false;
    bool rumble = true;     // [XCDE] hand the game's controller vibration to SDL (xcde.ini rumble)
    int ui_lang = 1;            // the launcher's own language: 0 Spanish, 1 English (xcde.ini launcher_lang)
    int icons = 0;              // button prompts: 0 auto (the connected controller, else the keyboard), 1 Switch, 2 Xbox, 3 PlayStation, 4 keyboard
    EnvVar env[NENV] = {};      // [XCDE] environment variables for the game (xcde_env.ini)
    int nenv = 0;
};
// [XCDE] button prompt packs: romfs mods the player installs (GameBanana, CompC: "Xbox control layout" 694794,
// "PS5 DualSense control layout" 541180 - CC BY-NC-ND, so not shipped) in these folders; the launcher turns the one
// for the chosen / connected controller on (xcde.ini mod_<folder>) and keeps them out of the Mods tab.
// (keyboard: drawn by the game from the key bindings, runtime/services/kbicons.c - xcde.ini mod_icons_keyboard)
static const char *const k_icon_ini[] = {"auto", "switch", "xbox", "playstation", "keyboard"};
static const char *const k_icon_mod[] = {"icons_xbox", "icons_playstation"};
static bool is_icon_mod(const std::string &n) { return n == k_icon_mod[0] || n == k_icon_mod[1] || n == "icons_keyboard"; }
// 2 Xbox, 3 PlayStation, 4 keyboard (auto without a controller), else 1 (Switch)
static int icons_effective(int sel) {
    if (sel) return sel;
    int n = 0, t = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) t = SDL_GetGamepadTypeForID(ids[0]);
    SDL_free(ids);
    if (n <= 0) return 4;
    return t == SDL_GAMEPAD_TYPE_XBOX360 || t == SDL_GAMEPAD_TYPE_XBOXONE ? 2 :
           t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5 ? 3 : 1;
}
static const char *const k_ups_ini[] = {"bilinear", "fsr", "fsr3", "dlss", "fsr4lite"};
static const char *const k_uq_ini[] = {"native", "quality", "balanced", "performance", "ultra"};
static const char *const k_wins[] = {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"};
static const char *const k_res_ini[] = {"match", "1", "1.5", "2", "3", "75%", "50%", "dynamic"};
static const char *const k_res_ui[] = {"Igual que la ventana (recomendado)", "1x - 720p (como Switch)", "1.5x - 1080p",
                                       "2x - 1440p", "3x - 2160p (4K)", "75% de la ventana", "50% de la ventana",
                                       "Dinámica (según rendimiento)"};
static const char *const k_fps_ini[] = {"30", "60", "120", "144", "165", "unlimited"};
static const char *const k_lang_ini[] = {"auto", "en-US", "es", "fr", "de", "it", "ja", "ko", "zh-Hans", "zh-Hant"};
static const char *const k_lang_ui[] = {"Inglés (por defecto)", "Inglés", "Español", "Francés", "Alemán", "Italiano", "Japonés", "Coreano", "Chino simplificado", "Chino tradicional"};

static Cfg g;

// [XCDE] launcher language (xcde.ini launcher_lang = es | en): the Spanish text is the key, T() gives the English one.
static const char *const k_en[][2] = {
    {"A (acción, usar arte, aceptar)", "A (action, use art, confirm)"}, {"B (saltar, atrás)", "B (jump, back)"},
    {"X (menú principal)", "X (main menu)"}, {"Y (mapa)", "Y (map)"}, {"Pulsar stick izq.", "Left stick press"},
    {"Pulsar stick der.", "Right stick press"}, {"L (objetivo anterior)", "L (previous target)"},
    {"R (objetivo siguiente)", "R (next target)"}, {"ZL (cámara)", "ZL (camera)"}, {"ZR (comandos de combate)", "ZR (battle commands)"},
    {"+ (menú del sistema)", "+ (system menu)"}, {"Cruceta izquierda (arte)", "D-pad left (art)"}, {"Cruceta arriba", "D-pad up"},
    {"Cruceta derecha (arte)", "D-pad right (art)"}, {"Cruceta abajo", "D-pad down"}, {"Avanzar", "Forward"},
    {"Retroceder", "Back"}, {"Izquierda", "Left"}, {"Derecha", "Right"}, {"Cámara arriba", "Camera up"},
    {"Cámara abajo", "Camera down"}, {"Cámara izquierda", "Camera left"}, {"Cámara derecha", "Camera right"},
    {"Arte 1 (izquierda)", "Art 1 (left)"}, {"Arte 2", "Art 2"}, {"Arte 3", "Art 3"}, {"Arte 4 (junto al centro)", "Art 4 (next to centre)"},
    {"Arte 5 (junto al centro)", "Art 5 (next to centre)"}, {"Arte 6", "Art 6"}, {"Arte 7", "Art 7"}, {"Arte 8 (derecha)", "Art 8 (right)"},
    {"Arte de talento (centro)", "Talent art (centre)"}, {"Personaje de arriba", "Character above"}, {"Personaje de abajo", "Character below"},
    {"Ratón izq.", "Mouse left"}, {"Ratón central", "Mouse middle"}, {"Ratón der.", "Mouse right"}, {"Ratón lateral 1", "Mouse side 1"},
    {"Ratón lateral 2", "Mouse side 2"}, {"Rueda arriba", "Wheel up"}, {"Rueda abajo", "Wheel down"}, {"(ninguna)", "(none)"},
    {"Abajo (Xbox A / PS Cruz / Switch B)", "Bottom (Xbox A / PS Cross / Switch B)"},
    {"Derecha (Xbox B / PS Círculo / Switch A)", "Right (Xbox B / PS Circle / Switch A)"},
    {"Izquierda (Xbox X / PS Cuadrado / Switch Y)", "Left (Xbox X / PS Square / Switch Y)"},
    {"Arriba (Xbox Y / PS Triángulo / Switch X)", "Top (Xbox Y / PS Triangle / Switch X)"},
    {"Cruceta izquierda", "D-pad left"}, {"Cruceta derecha", "D-pad right"}, {"Paleta der. 1", "Right paddle 1"},
    {"Paleta izq. 1", "Left paddle 1"}, {"Paleta der. 2", "Right paddle 2"}, {"Paleta izq. 2", "Left paddle 2"},
    {"Igual que la ventana (recomendado)", "Match the window (recommended)"}, {"1x - 720p (como Switch)", "1x - 720p (like Switch)"},
    {"75% de la ventana", "75% of the window"}, {"50% de la ventana", "50% of the window"},
    {"Dinámica (según rendimiento)", "Dynamic (by performance)"},
    {"Inglés (por defecto)", "English (default)"}, {"Inglés", "English"}, {"Español", "Spanish"}, {"Francés", "French"},
    {"Alemán", "German"}, {"Italiano", "Italian"}, {"Japonés", "Japanese"}, {"Coreano", "Korean"},
    {"Chino simplificado", "Chinese (Simplified)"}, {"Chino tradicional", "Chinese (Traditional)"},
    {"Pantalla", "Display"}, {"Pantalla completa", "Fullscreen"}, {"Tamaño de la ventana", "Window size"},
    {"Resolución y escalado", "Resolution and scaling"}, {"Resolución interna", "Internal resolution"},
    {"El juego original renderiza a 720p. Más resolución = imagen más nítida y más carga para la GPU.",
     "The original game renders at 720p. More resolution = a sharper image and more GPU load."},
    {"Bilineal (suave)", "Bilinear (soft)"}, {"AMD FSR 1.0 (nítido)", "AMD FSR 1.0 (sharp)"},
    {"AMD FSR 3.1 (temporal, cualquier GPU)", "AMD FSR 3.1 (temporal, any GPU)"},
    {"NVIDIA DLSS (temporal, GPU RTX)", "NVIDIA DLSS (temporal, RTX GPU)"}, {"Escalado / antialiasing", "Upscaling / anti-aliasing"},
    {"FSR 1.0: escalado espacial, solo cuando la resolución interna es menor que la ventana.\n"
     "FSR 3.1 y DLSS: reconstrucción temporal - renderizan a menos resolución (según la calidad)\n"
     "y reconstruyen la imagen de la ventana con varios frames: más FPS y bordes sin dientes.\n"
     "Sustituyen al antialiasing temporal del juego. DLSS necesita una GPU NVIDIA RTX; si no\n"
     "está disponible se usa FSR 3.1.",
     "FSR 1.0: spatial upscaling, only when the internal resolution is below the window.\n"
     "FSR 3.1 and DLSS: temporal reconstruction - they render at a lower resolution (by quality)\n"
     "and rebuild the window's image from several frames: more FPS and smooth edges.\n"
     "They replace the game's temporal anti-aliasing. DLSS needs an NVIDIA RTX GPU; without\n"
     "one, FSR 3.1 is used."},
    {"Nativo (solo antialiasing: DLAA / FSR Native AA)", "Native (anti-aliasing only: DLAA / FSR Native AA)"},
    {"Calidad (67 %)", "Quality (67 %)"}, {"Equilibrado (59 %)", "Balanced (59 %)"}, {"Rendimiento (50 %)", "Performance (50 %)"},
    {"Ultra rendimiento (33 %)", "Ultra performance (33 %)"}, {"Calidad del escalado", "Upscaling quality"},
    {"Con 'Resolución interna' en 'Igual que la ventana': la resolución a la que se renderiza\n"
     "antes de reconstruir (porcentaje del lado de la ventana).",
     "With 'Internal resolution' on 'Match the window': the resolution rendered\n"
     "before reconstruction (percentage of the window's side)."},
    {"Generación de frames (FSR 3)", "Frame generation (FSR 3)"},
    {"Inserta un frame interpolado entre cada dos del juego en todo el juego: el doble de imágenes\n"
     "por segundo (p. ej. FPS 30 -> 60 en pantalla). La de AMD: funciona con FSR 3.1 y con DLSS.\n"
     "Añade algo de latencia. Se aplica al iniciar el juego.",
     "Inserts an interpolated frame between every two of the game's, everywhere: twice the images\n"
     "per second (e.g. 30 FPS -> 60 on screen). AMD's: works with FSR 3.1 and with DLSS.\n"
     "Adds some latency. Applied when the game starts."},
    {"Generación de frames solo en cinemáticas", "Frame generation in cutscenes only"},
    {"El juego va a tus FPS sin generación de frames; en las cinemáticas (donde el juego anima a\n"
     "los personajes a 30) baja a 30 parejos y la generación las lleva a 60 en pantalla.\n"
     "Pensado para jugar a 60 en un monitor de 60 Hz. Se aplica al iniciar el juego.",
     "The game runs at your FPS without frame generation; in cutscenes (where the game animates\n"
     "characters at 30) it drops to an even 30 and generation brings them to 60 on screen.\n"
     "Meant for playing at 60 on a 60 Hz monitor. Applied when the game starts."},
    {"Se aplica cuando la resolución interna es menor que la ventana.", "Applies when the internal resolution is below the window."},
    {"Calidad", "Quality"}, {"Antialiasing SMAA", "SMAA anti-aliasing"},
    {"Profundidad de campo (desenfoque del juego)", "Depth of field (the game's blur)"},
    {"El desenfoque de lo cercano y lo lejano que el juego aplica también en Switch.\n"
     "Desactivado: toda la escena nítida (y algo menos de trabajo para la GPU).",
     "The near / far blur the game also applies on Switch.\n"
     "Off: the whole scene sharp (and a bit less GPU work)."},
    {"Antialiasing y desenfoque de movimiento del juego", "The game's anti-aliasing and motion blur"},
    {"Antialiasing temporal (TAA) del juego", "The game's temporal anti-aliasing (TAA)"},
    {"Solo el TAA, sin tocar el desenfoque de movimiento ni el del pelo.\nDesactivado: quita el emborronado del TAA (combínalo con SMAA para los bordes).",
     "Only the TAA, leaving motion blur and hair blur alone.\nOff: removes the TAA smearing (combine with SMAA for the edges)."},
    {"El antialiasing temporal (TMAA), el desenfoque de movimiento y el del pelo que el juego\n"
     "aplica para los 720p de la Switch: a 1080p o más suavizan toda la imagen.\n"
     "Desactivado: imagen nítida (combínalo con SMAA para los bordes).",
     "The temporal anti-aliasing (TMAA), motion blur and hair blur the game applies for the\n"
     "Switch's 720p: at 1080p or more they soften the whole image.\n"
     "Off: a sharp image (combine it with SMAA for the edges)."},
    {"Mostrar FPS en la barra de título", "Show FPS in the title bar"}, {"Velocidad", "Speed"},
    {"30 (como en Switch)", "30 (like Switch)"}, {"Sin límite", "Unlímited"},
    {"Más de 30: la velocidad del mundo, el reloj y las escenas no cambian (probado a 60 y 120);\n"
     "la cámara queda un poco más lejos. El V-Sync forzado en el panel de NVIDIA / AMD o el límite\n"
     "de RivaTuner también limitan los FPS: déjalos en 'configuración de la aplicación' / 0.",
     "Above 30: world speed, the clock and scenes don't change (tested at 60 and 120);\n"
     "the camera sits a bit farther. V-Sync forced in the NVIDIA / AMD panel or a RivaTuner\n"
     "limit also cap the FPS: leave them on 'application setting' / 0."},
    {"V-Sync (con 30 / 60 FPS)", "V-Sync (at 30 / 60 FPS)"},
    {"Cada imagen se muestra exactamente una vez, sincronizada con la pantalla: movimiento\n"
     "fluido (sin imágenes repetidas o saltadas). Con más de 60 FPS o sin límite no se aplica.",
     "Each image is shown exactly once, in sync with the display: smooth motion\n"
     "(no repeated or skipped images). Not applied above 60 FPS or unlimited."},
    {"Como el resto del juego", "Like the rest of the game"}, {"30 estables (como en Switch)", "Steady 30 (like Switch)"},
    {"Cinemáticas", "Cutscenes"},
    {"En las cinemáticas el juego anima a los personajes a 30 aunque juegues a más FPS: con la\n"
     "cámara a 60 encima se notan saltos (p. ej. el pelo de Dickson).\n"
     "30 estables: las cinemáticas van a 30 parejos, cámara y personajes juntos (con V-Sync en un\n"
     "monitor de 60 Hz cada imagen dura exactamente dos refrescos).\n"
     "Con 'Generación de frames solo en cinemáticas' también van a 30, con generación encima.",
     "In cutscenes the game animates characters at 30 even at higher FPS: with the camera\n"
     "at 60 on top, jumps show (e.g. Dickson's hair).\n"
     "Steady 30: cutscenes run at an even 30, camera and characters together (with V-Sync on a\n"
     "60 Hz monitor each image lasts exactly two refreshes).\n"
     "With 'Frame generation in cutscenes only' they also run at 30, with generation on top."},
    {"Pulsa %s para \"%s\"... (%d s, Supr = ninguna)", "Press %s for \"%s\"... (%d s, Del = none)"},
    {"un botón del mando", "a controller button"}, {"una tecla o un botón del ratón", "a key or a mouse button"},
    {"Haz clic en una asignación y pulsa la tecla, botón del ratón o del mando nuevo. Si ya estaba en uso, se intercambian.",
     "Click a binding and press the new key, mouse button or controller button. If it was in use, they swap."},
    {"Restaurar teclado", "Reset keyboard"}, {"Restaurar mando", "Reset controller"},
    {"   Mando conectado: %s", "   Controller connected: %s"}, {"   (ningún mando detectado)", "   (no controller detected)"},
    {"Cambiar de personaje como en Xenoblade 3 (mantén L2 y pulsa L1 / R1: el de arriba / abajo; F2)",
     "Switch characters like in Xenoblade 3 (hold L2 and press L1 / R1: the one above / below; F2)"},
    {"Pasa el control al siguiente miembro del grupo, en el campo y en combate. Cada uno se queda\n"
     "donde está y conserva sus recargas de artes; el anterior pasa a la IA. No funciona en menús ni\n"
     "escenas, y en combate salta a los miembros caídos.",
     "Hands control to the next party member, in the field and in battle. Each stays where it is\n"
     "and keeps its art cooldowns; the previous one goes to the AI. Not in menus or scenes,\n"
     "and in battle it skips downed members."},
    {"Automático (el mando conectado; sin mando, teclado)", "Automatic (the connected controller; without one, keyboard)"},
    {"Teclado (las teclas asignadas)", "Keyboard (the bound keys)"}, {"Iconos de botones", "Button icons"},
    {"Falta el pack: descarga \"%s\" de GameBanana y pon su carpeta romfs en mods\\%s",
     "Pack missing: download \"%s\" from GameBanana and put its romfs folder in mods\\%s"},
    {"Xbox / PlayStation: packs de iconos de CompC (GameBanana). Teclado: el juego dibuja en cada\n"
     "icono la tecla asignada a ese botón (también ratón). Se aplica al iniciar el juego.",
     "Xbox / PlayStation: CompC's icon packs (GameBanana). Keyboard: the game draws on each icon\n"
     "the key bound to that button (mouse too). Applied when the game starts."},
    {"Manteniendo el botón derecho", "Holding the right button"}, {"Siempre", "Always"},
    {"Cámara con el ratón", "Mouse camera"},
    {"Siempre: el ratón mueve la cámara directamente y el cursor queda oculto dentro del juego.\n"
     "F1 (menú) o Alt+Tab lo liberan; al mover el ratón sobre el juego vuelve a capturarse.",
     "Always: the mouse moves the camera directly and the cursor is hidden in the game.\n"
     "F1 (menu) or Alt+Tab release it; moving the mouse over the game captures it again."},
    {"Sensibilidad", "Sensitivity"}, {"Invertir eje Y", "Invert Y axis"}, {"Zona muerta de los sticks (%)", "Stick dead zone (%)"},
    {"Para un mando con drift (el personaje o la cámara se mueven solos): esa parte del recorrido\n"
     "del stick cuenta como centrada. 0 = sin cambios (el juego ya tiene la suya).",
     "For a controller with drift (the character or camera move by themselves): that part of the\n"
     "stick's travel counts as centred. 0 = unchanged (the game has its own)."},
    {"Artes 1-8 y talento: en combate seleccionan el arte de ese hueco de la paleta y lo usan.",
     "Arts 1-8 and talent: in battle they select the art in that palette slot and use it."},
    {"Acción (botón de Switch)", "Action (Switch button)"}, {"Tecla", "Key"}, {"Tecla 2", "Key 2"}, {"Mando", "Controller"},
    {"(stick del mando)", "(controller stick)"}, {"(cruceta + A)", "(d-pad + A)"},
    {"Pon cada mod en su carpeta: mods\\<nombre>\\romfs\\... (la misma estructura que en Ryujinx / yuzu). "
     "Sus archivos reemplazan o añaden archivos del juego. Se aplican en orden alfabético: si dos mods "
     "cambian el mismo archivo, gana el último. Los mods de código (exefs, parches .pchtxt / IPS) no "
     "funcionan en esta versión recompilada.",
     "Put each mod in its own folder: mods\\<name>\\romfs\\... (the same layout as Ryujinx / yuzu). "
     "Its files replace or add game files. They apply in alphabetical order: if two mods change the "
     "same file, the last one wins. Code mods (exefs, .pchtxt / IPS patches) don't work in this "
     "recompiled version."},
    {"Abrir carpeta de mods", "Open mods folder"}, {"Actualizar lista", "Refresh list"}, {"No hay mods instalados.", "No mods installed."},
    {"Volumen", "Volume"}, {"Sonido de los vídeos", "Video sound"}, {"Idioma del juego", "Game language"}, {"Idioma", "Language"},
    {"Textos del juego. Solo los idiomas que incluya tu copia; si uno falla, vuelve a Inglés.",
     "The game's text. Only the languages your copy includes; if one fails, it goes back to English."},
    {"Recompilación nativa para PC · configuración", "Native PC recompilation · settings"},
    {"Gráficos", "Graphics"}, {"Controles", "Controls"}, {"Audio e idioma", "Audio and language"},
    {"Faltan los archivos del juego en targets/ (exefs y romfs de tu copia).", "The game files are missing in targets/ (exefs and romfs of your copy)."},
    {"Falta build/xcde.exe (compila el juego primero).", "build/xcde.exe is missing (build the game first)."},
    {"Listo. Guardado: save/   Ajustes: xcde.ini", "Ready. Saves: save/   Settings: xcde.ini"},
    {"Listo. La primera vez, JUGAR prepara los shaders antes (unos minutos)", "Ready. The first time, PLAY prepares the shaders first (a few minutes)"},
    {"Omitir", "Skip"}, {"Detener", "Stop"},
    {"Paso final: pipelines completos %llu de %llu (puedes pulsar Omitir: el juego los compila al jugar)",
     "Final pass: whole pipelines %llu of %llu (you can press Skip: the game compiles them while playing)"},
    {"Precompilación detenida (se puede seguir otro día: lo ya hecho se conserva)", "Precompilation stopped (it can go on another day: what's done is kept)"},
    {"JUGAR", "PLAY"}, {"Precompilar shaders", "Precompile shaders"},
    {"Traduce y compila todos los shaders del juego por adelantado, para que la primera visita\n"
     "a una zona no se detenga a compilarlos. Solo hace falta una vez (o tras actualizar el juego).",
     "Translates and compiles all of the game's shaders ahead of time, so the first visit to an\n"
     "area doesn't stop to compile them. Only needed once (or after updating the game)."},
    {"Guardar ajustes", "Save settings"}, {"Ajustes guardados en xcde.ini", "Settings saved to xcde.ini"},
    {"No se pudo escribir xcde.ini", "Could not write xcde.ini"}, {"Crear reporte", "Create report"},
    {"Junta en un .zip los logs de las dos últimas partidas, los ajustes (xcde.ini / xcde_env.ini)\n"
     "y el último informe de cierre inesperado (sin partidas guardadas ni archivos del juego), para adjuntarlo a un reporte.",
     "Puts in a .zip the logs of the last two sessions, the settings (xcde.ini / xcde_env.ini)\n"
     "and the latest crash report (no saves or game files), to attach to a bug report."},
    {"AMD FSR 4 Lite (temporal, nativo)", "AMD FSR 4 Lite (temporal, native)"},
    {"Salir", "Quit"}, {"No se pudo iniciar build/xcde.exe (error ", "Could not start build/xcde.exe (error "},
    {"Juego iniciado. Registro: build/last_game.log", "Game started. Log: build/last_game.log"},
    {"No se pudo iniciar la precompilación (error ", "Could not start the precompilation (error "},
    {"Buscando shaders en los archivos del juego...", "Looking for shaders in the game files..."},
    {"Preparando shaders antes de la primera partida (solo esta vez)", "Preparing shaders before the first game (only this time)"},
    {"Precompilando shaders", "Precompiling shaders"}, {"Variantes conocidas (semilla)…", "Known variants (seed)…"},
    {"Archivos %llu de %llu · programas de shaders listos %llu de %llu", "Files %llu of %llu · shader programs ready %llu of %llu"},
    {"El controlador gráfico falló en un shader: se salta y se sigue (intento ", "The graphics driver failed on a shader: it is skipped (attempt "},
    {"Shaders precompilados", "Shaders precompiled"},
    {"La precompilación falló (registro: build/last_precompile.log)", "Precompilation failed (log: build/last_precompile.log)"},
    {"No hay nada que reportar todavía (ni logs ni xcde.ini)", "Nothing to report yet (no logs or xcde.ini)"},
    {"No se pudo crear el reporte (falta tar.exe de Windows)", "Could not create the report (Windows' tar.exe is missing)"},
    {"No se pudo crear el reporte", "Could not create the report"}, {"Reporte creado: ", "Report created: "},
    {" (adjúntalo al reportar el problema)", " (attach it to the bug report)"},
    {"Vibración del mando", "Gamepad vibration"},
    {"Activa el motor del mando cuando el juego manda vibración (por ejemplo en combate).\n"
     "También se puede cambiar dentro del juego, en el menú, pestaña Controles.",
     "Turns on the controller's motor when the game sends vibration (for example in battle).\n"
     "It can also be changed in the game, in the menu's Controls tab."},
    {"Resuelve lo que se ve tras el suelo / el suelo que desaparece con FSR 3 en algunas GPUs.\n"
     "Automático: solo lo apaga en las gráficas integradas AMD donde falla.",
     "Resolves what is seen behind the floor / the floor disappearing with FSR 3 on some GPUs.\n"
     "Automatic: only turns it off on AMD integrated graphics where it fails."},
    {"Puntos de color de 1-2 px en gráficas AMD RDNA 3 (issue #7).\n"
     "Automático: solo se activa en RDNA 3. Desactivado si te baja el rendimiento.",
     "1-2 px colour dots on AMD RDNA 3 graphics (issue #7).\n"
     "Automatic: only enabled on RDNA 3. Turn it off if performance drops."},
    {"Evita que modelos y texturas se corrompan en partidas largas.\n"
     "Desactiva el hilo de render asíncrono (algo menos de rendimiento).",
     "Prevents models and textures from corrupting in long sessions.\n"
     "Disables the async render thread (slightly less performance)."},
    {"Idioma del launcher", "Launcher language"},
    {"Variables de entorno", "Environment variables"},
    {"Se pasan al juego al iniciar. Ejemplo: XCDE_RSC_PIXEL=1 (se aplica la próxima vez que pulses JUGAR).",
     "Passed to the game when it starts. Example: XCDE_RSC_PIXEL=1 (applied the next time you press PLAY)."},
    {"Se guardan en xcde_env.ini (también puedes editarlas ahí).", "Saved in xcde_env.ini (you can also edit them there)."},
    {"Añadir variable", "Add variable"}, {"Nombre", "Name"}, {"Valor", "Value"}, {"Quitar", "Remove"},
    {"No hay variables de entorno.", "No environment variables."},
    {"Nombre no válido: usa letras, números y _ (sin empezar por un número).",
     "Invalid name: use letters, digits and _ (not starting with a digit)."},
    {"No se pudieron guardar los ajustes", "Could not save the settings"},
};
static const char *T(const char *es) {
    if (g.ui_lang != 1) return es;
    for (const auto &e : k_en) if (!strcmp(e[0], es)) return e[1];
    return es;
}
// (combo lists: each element through T)
struct TList { const char *p[48]; };
static TList TL(const char *const *a, int n) { TList t{}; for (int i = 0; i < n && i < 48; i++) t.p[i] = T(a[i]); return t; }
static std::string g_root;  // repository root (where targets/, build/, xcde.ini live)

static std::string path(const char *rel) { return g_root + "/" + rel; }
static bool exists(const char *rel) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(path(rel).c_str(), &pi);
}
#ifndef _WIN32
// [XCDE] T-LINUX: start args[0] in g_root with stdout/stderr in log (truncated), in its own process group; 0 = failed.
static pid_t spawn_logged(const std::vector<std::string> &args, const std::string &log, bool background) {
    std::vector<char *> av;
    for (const std::string &a : args) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(NULL);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addchdir_np(&fa, g_root.c_str());
    if (!log.empty()) {
        posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);
    pid_t pid = 0;
    int err = posix_spawnp(&pid, av[0], &fa, &at, av.data(), environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (err) { errno = err; return 0; }
    if (background) setpriority(PRIO_PROCESS, (id_t)pid, 10);
    return pid;
}
static void open_folder(const std::string &dir) {
    std::string url = "file://" + dir;
    if (!SDL_OpenURL(url.c_str())) spawn_logged({"xdg-open", dir}, "", false);
}
#endif
struct Mod { std::string name; bool on = true; bool present = true; };
static std::vector<Mod> g_mods;  // (mods tab; xcde.ini mod_<name>)

static int find(const char *const *list, int n, const char *v) {
    for (int i = 0; i < n; i++) if (!strcmp(list[i], v)) return i;
    return -1;
}

// [XCDE] game environment (Diagnostics tab). Only conventional names are passed to the OS: letters, digits and '_',
// not starting with a digit (Windows and POSIX both reject/ignore '=' and other separators).
static const char *const k_env_file = "xcde_env.ini";
static bool env_name_valid(const char *s) {
    if (!s || !*s || !(isalpha((unsigned char)*s) || *s == '_')) return false;
    for (s++; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '_')) return false;
    return true;
}
static void apply_env() {
    for (int i = 0; i < g.nenv; i++) {
        if (!env_name_valid(g.env[i].name) || !g.env[i].name[0]) continue;
#ifdef _WIN32
        SetEnvironmentVariableA(g.env[i].name, g.env[i].value);
#else
        setenv(g.env[i].name, g.env[i].value, 1);
#endif
    }
}
static void load_env() {
    g.nenv = 0;
    FILE *f = fopen(path(k_env_file).c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f) && g.nenv < NENV) {
        char *s = line;
        if (strlen(s) >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) s += 3;  // (a UTF-8 BOM: the file was saved from Notepad)
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *name = s, *value = eq + 1;
        for (char *n = name + strlen(name); n > name && (n[-1] == ' ' || n[-1] == '\t'); ) *--n = 0;
        while (*value == ' ' || *value == '\t') value++;
        if (!env_name_valid(name) || strlen(name) >= sizeof g.env[0].name) continue;
        snprintf(g.env[g.nenv].name, sizeof g.env[0].name, "%s", name);
        snprintf(g.env[g.nenv].value, sizeof g.env[0].value, "%s", value);
        g.nenv++;
    }
    fclose(f);
}
static bool save_env() {
    std::string p = path(k_env_file);
    char tmp[1200];
    FILE *f = xcde_ini_begin(p.c_str(), tmp, sizeof tmp);
    if (!f) return false;
    fprintf(f, "# Monado environment variables (written by the launcher)\n");
    fprintf(f, "# NAME=VALUE lines; passed to the game when it starts (Diagnostics tab)\n");
    for (int i = 0; i < g.nenv; i++)
        if (env_name_valid(g.env[i].name)) fprintf(f, "%s=%s\n", g.env[i].name, g.env[i].value);
    bool ok = !ferror(f) && fflush(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = xcde_atomic_replace(tmp, p.c_str()) != 0;
    if (!ok) remove(tmp);
    return ok;
}
// blank rows are editing space only: drop them when saving / applying / playing (they are never written to the file)
static void env_prune() {
    int n = 0;
    for (int i = 0; i < g.nenv; i++) {
        if (!g.env[i].name[0]) continue;
        if (n != i) g.env[n] = g.env[i];
        n++;
    }
    g.nenv = n;
}

static void load() {
    g.nenv = 0;
    load_env();
    for (int i = 0; i < NPAD; i++) g.pad[i] = k_def_pad[i];
    for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
    FILE *f = fopen(path("xcde.ini").c_str(), "r");
    if (!f) return;
    int layout = 1;  // (bind_layout: an older file's keyboard lines are the old layout - the new defaults replace them)
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        if (strlen(k) >= 3 && !memcmp(k, "\xEF\xBB\xBF", 3)) k += 3;  // (a UTF-8 BOM: the file was saved from Notepad)
        while (*k == ' ' || *k == '\t') k++;
        for (char *e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'); ) *--e = 0;
        int i;
        if (!strcmp(k, "fullscreen")) g.fullscreen = atoi(v) != 0;
        else if (!strcmp(k, "aspect")) g.aspect = !strcmp(v, "original") ? ASPECT_ORIGINAL : ASPECT_UNLOCKED;
        else if (!strcmp(k, "window") && (i = find(k_wins, 5, v)) >= 0) g.win = i;
        else if (!strcmp(k, "resolution") && (i = find(k_res_ini, 8, v)) >= 0) g.res = i;
        else if (!strcmp(k, "upscaler")) { g.upscaler_auto = !strcmp(v, "auto"); g.upscaler = !strcmp(v, "fsr") || g.upscaler_auto ? 1 : !strcmp(v, "fsr3") ? 2 : !strcmp(v, "dlss") ? 3 : !strcmp(v, "fsr4lite") ? 4 : 0; }
        else if (!strcmp(k, "upscale_quality") && (i = find(k_uq_ini, 5, v)) >= 0) g.uquality = i;
        else if (!strcmp(k, "upscale_sharpness")) {
            float x = strcmp(v, "default") ? (float)atof(v) : -1.0f;
            g.sharpness = x < 0.0f ? -1.0f : x > 1.0f ? 1.0f : x;
        }
        else if (!strcmp(k, "frame_gen")) g.frame_gen = atoi(v) != 0;
        else if (!strcmp(k, "antialias")) g.smaa = !strcmp(v, "smaa");
        else if (!strcmp(k, "depth_of_field")) g.dof = atoi(v) != 0;
        else if (!strcmp(k, "game_blur")) g.game_blur = atoi(v) != 0;
        else if (!strcmp(k, "taa")) g.taa = atoi(v) != 0;
        else if (!strcmp(k, "vsync")) g.vsync = atoi(v) != 0;
        else if (!strcmp(k, "cutscenes")) { g.cutscenes = !strcmp(v, "30") || !strcmp(v, "30fg") ? 1 : 0; g.cut_fg = !strcmp(v, "30fg"); }
        else if (!strcmp(k, "party_switch")) g.party_switch = atoi(v) != 0;
        else if (!strcmp(k, "show_fps")) g.show_fps = atoi(v) != 0;
        else if (!strcmp(k, "vulkan_api")) g.vulkan_api = !strcmp(v, "1.2") ? 12 : !strcmp(v, "1.3") ? 13 : !strcmp(v, "1.4") ? 14 : 0;
        else if (!strcmp(k, "fps_overlay")) g.fps_overlay = atoi(v) != 0;
        else if (!strcmp(k, "gpu_uuid")) snprintf(g.gpu_uuid, sizeof g.gpu_uuid, "%s", v);
        else if (!strcmp(k, "gpu_index")) g.gpu_index = std::clamp(atoi(v), -1, 255);
        else if (!strcmp(k, "gpu_profile")) g.gpu_profile = !strcmp(v, "compatibility");
        else if (!strcmp(k, "gpu_gpl")) g.gpu_gpl = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
        else if (!strcmp(k, "gpu_memory_budget")) g.gpu_memory_budget = atoi(v) != 0;
        else if (!strcmp(k, "occlusion")) g.occlusion = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
        else if (!strcmp(k, "grass_lod")) g.grass_lod = grass_lod_parse(v);
        else if (!strcmp(k, "amd_fix")) g.amd_fix = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
        else if (!strcmp(k, "session_fix")) g.session_fix = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
        else if (!strcmp(k, "rumble")) g.rumble = atoi(v) != 0;
        else if (gfx_load_key(&g.graphics, k, v)) {}  // [XCDE]
        else if (!strcmp(k, "gpu_preset")) { g.quality_preset=xcde_quality_parse(v); g.gpu_performance=g.quality_preset==QUALITY_PERFORMANCE || g.quality_preset==QUALITY_POTATO; }
        else if (!strcmp(k, "low_memory")) g.gpu_performance=atoi(v)!=0;
        else if (!strcmp(k, "dynamic_min_height")) g.dynamic_min_height=std::clamp(atoi(v),0,4320);
        else if (!strcmp(k, "dynamic_max_height")) g.dynamic_max_height=std::clamp(atoi(v),0,4320);
        else if (!strcmp(k, "effects_off")) g.effects_off=(unsigned)strtoul(v,nullptr,10)&31u;
        else if (!strcmp(k, "gpu_keep_heavy")) g.gpu_keep_heavy = atoi(v) != 0;
        else if (!strcmp(k, "fps")) { g.fps = 0; for (int j = 0; j < 6; j++) if (!strcmp(v, k_fps_ini[j])) g.fps = j; if (!strcmp(v, "unlímited") || !strcmp(v, "0")) g.fps = 5; }
        else if (!strcmp(k, "volume")) g.volume = atoi(v);
        else if (!strcmp(k, "movie_audio")) g.movie_audio = atoi(v) != 0;
        else if (!strcmp(k, "language") && (i = find(k_lang_ini, 10, v)) >= 0) g.lang = i;
        else if (!strncmp(k, "mod_", 4)) g_mods.push_back(Mod{k + 4, atoi(v) != 0, false});
        else if (!strncmp(k, "bind_pad_", 9) && (i = find(k_ini, NPAD, k + 9)) >= 0) {
            int h = find(k_pad_ini, PADB_COUNT, v);
            if (h >= 0) g.pad[i] = h;
        } else if (!strncmp(k, "bind_key_", 9) && (i = find(k_ini, NACT, k + 9)) >= 0) {
            g.key[i] = key_from_ini(v);
        } else if (!strncmp(k, "bind_key2_", 10) && (i = find(k_ini, NACT, k + 10)) >= 0) {
            g.key2[i] = key_from_ini(v);
        } else if (!strcmp(k, "bind_layout")) layout = atoi(v);
        else if (!strcmp(k, "mouse_look")) g.mouse_look = std::clamp(atoi(v), 0, 2);
        else if (!strcmp(k, "mouse_sens")) g.mouse_sens = std::clamp(atoi(v), 1, 100);
        else if (!strcmp(k, "stick_deadzone")) g.stick_deadzone = std::clamp(atoi(v), 0, 30);
        else if (!strcmp(k, "launcher_lang")) g.ui_lang = !strcmp(v, "es") ? 0 : 1;
        else if (!strcmp(k, "mouse_invert")) g.mouse_invert = atoi(v) != 0;
        else if (!strcmp(k, "button_icons") && (i = find(k_icon_ini, 5, v)) >= 0) g.icons = i;
    }
    fclose(f);
    if (layout < 2) for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
}

static bool save() {
    env_prune();  // [XCDE] blank environment rows are not settings
    std::string settings_path = path("xcde.ini");
    char tmp[1200];
    FILE *f = xcde_ini_begin(settings_path.c_str(), tmp, sizeof tmp);
    if (!f) return false;
    fprintf(f, "# Monado settings (written by the launcher)\n");
    fprintf(f, "gpu_uuid = %s\ngpu_index = %d\ngpu_profile = %s\ngpu_gpl = %s\ngpu_memory_budget = %d\ngpu_preset = %s\ngpu_keep_heavy = %d\n",
        g.gpu_uuid, g.gpu_index, g.gpu_profile ? "compatibility" : "auto", g.gpu_gpl < 0 ? "auto" : g.gpu_gpl ? "on" : "off",
        g.gpu_memory_budget, xcde_quality_name(g.quality_preset), g.gpu_keep_heavy);
    fprintf(f, "occlusion = %s\n", g.occlusion == 1 ? "on" : g.occlusion == 0 ? "off" : "auto");
    fprintf(f, "grass_lod = %s\n", grass_lod_ini[grass_lod_valid(g.grass_lod)]);
    fprintf(f, "amd_fix = %s\n", g.amd_fix == 1 ? "on" : g.amd_fix == 0 ? "off" : "auto");
    fprintf(f, "session_fix = %s\n", g.session_fix == 1 ? "on" : g.session_fix == 0 ? "off" : "auto");
    gfx_save(f, &g.graphics);  // [XCDE]
    fprintf(f,"low_memory = %d\ndynamic_min_height = %d\ndynamic_max_height = %d\neffects_off = %u\n",g.gpu_performance,g.dynamic_min_height,g.dynamic_max_height,g.effects_off);
    fprintf(f, "fullscreen = %d\nwindow = %s\nresolution = %s\nupscaler = %s\nantialias = %s\nshow_fps = %d\n", g.fullscreen,
            k_wins[g.win], k_res_ini[g.res], g.upscaler_auto ? "auto" : k_ups_ini[g.upscaler], g.smaa ? "smaa" : "off", g.show_fps);
    fprintf(f, "aspect = %s\n", g.aspect == ASPECT_ORIGINAL ? "original" : "auto");
    fprintf(f, "vulkan_api = %s\nfps_overlay = %d\n", g.vulkan_api == 12 ? "1.2" : g.vulkan_api == 13 ? "1.3" : g.vulkan_api == 14 ? "1.4" : "auto", g.fps_overlay);
    fprintf(f, "upscale_quality = %s\nframe_gen = %d\n", k_uq_ini[g.uquality], g.frame_gen);
    if (g.sharpness < 0.0f) fprintf(f, "upscale_sharpness = default\n");
    else fprintf(f, "upscale_sharpness = %.3f\n", g.sharpness);
    fprintf(f, "depth_of_field = %d\n", g.dof);
    fprintf(f, "game_blur = %d\n", g.game_blur);
    fprintf(f, "taa = %d\n", g.taa);
    fprintf(f, "vsync = %d\n", g.vsync);
    fprintf(f, "cutscenes = %s\n", g.cut_fg ? "30fg" : g.cutscenes ? "30" : "same");
    fprintf(f, "party_switch = %d\n", g.party_switch);
    fprintf(f, "volume = %d\nmovie_audio = %d\nlanguage = %s\nfps = %s\n", g.volume, g.movie_audio, k_lang_ini[g.lang], k_fps_ini[g.fps]);
    for (const Mod &m : g_mods) if (!is_icon_mod(m.name)) fprintf(f, "mod_%s = %d", m.name.c_str(), m.on), fputc(10, f);
    int ic = icons_effective(g.icons);
    fprintf(f, "button_icons = %s\nmod_%s = %d\nmod_%s = %d\nmod_icons_keyboard = %d\n", k_icon_ini[g.icons], k_icon_mod[0], ic == 2,
            k_icon_mod[1], ic == 3, ic == 4);
    for (int i = 0; i < NPAD; i++) fprintf(f, "bind_pad_%s = %s\n", k_ini[i], k_pad_ini[g.pad[i]]);
    fprintf(f, "bind_layout = 2\n");
    for (int i = 0; i < NACT; i++) {
        fprintf(f, "bind_key_%s = %s\n", k_ini[i], key_ini_name(g.key[i]));
        fprintf(f, "bind_key2_%s = %s\n", k_ini[i], key_ini_name(g.key2[i]));
    }
    fprintf(f, "mouse_look = %d\nmouse_sens = %d\nmouse_invert = %d\n", g.mouse_look, g.mouse_sens, g.mouse_invert);
    fprintf(f, "stick_deadzone = %d\nrumble = %d\n", g.stick_deadzone, g.rumble);
    fprintf(f, "launcher_lang = %s\n", g.ui_lang ? "en" : "es");
    if (xcde_ini_commit(f, tmp, settings_path.c_str()) == 0) return false;
    return save_env();  // [XCDE] the game's environment (xcde_env.ini)
}


// [XCDE] GitHub #18: game files from another game version can't run on this build (the recompiled code
// matches exactly one NSO revision): find it out here so the player sees a clear message in the launcher
// instead of a window-less game whose log only says "no function at ..." deep in nn::init::Start.
static char g_mismatch_desc[8] = "";    // "" = ok, else the exefs file that differs ("main" / "sdk")
static char g_mismatch_got[65], g_mismatch_want[65];
static bool nso_matches(const char *rel, const char *want, char got[65]) {
    FILE *f = fopen(path(rel).c_str(), "rb");
    if (!f) return true;  // (the file is missing: the game-files message covers that case)
    uint8_t hdr[0x60];
    bool ok = fread(hdr, 1, sizeof hdr, f) == sizeof hdr && !memcmp(hdr, "NSO0", 4);
    fclose(f);
    if (!ok) return true;  // (not a plain NSO: let the runtime's own loader explain)
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        got[2 * i] = hex[hdr[0x40 + i] >> 4];
        got[2 * i + 1] = hex[hdr[0x40 + i] & 15];
    }
    got[64] = 0;
    for (int l = 64; l >= 2 && got[l - 1] == '0' && got[l - 2] == '0'; l -= 2) got[l - 2] = 0;  // (nso.py's rstrip)
    return !memcmp(got, want, strlen(want));
}
static void check_game_version() {
#ifdef XCDE_GAME_MAIN_ID
    if (exists("targets/exefs/main") && !nso_matches("targets/exefs/main", XCDE_GAME_MAIN_ID,
                                                     g_mismatch_got)) {
        snprintf(g_mismatch_desc, sizeof g_mismatch_desc, "main");
        snprintf(g_mismatch_want, sizeof g_mismatch_want, "%s", XCDE_GAME_MAIN_ID);
        return;
    }
#endif
#ifdef XCDE_GAME_SDK_ID
    if (exists("targets/exefs/sdk") && !nso_matches("targets/exefs/sdk", XCDE_GAME_SDK_ID,
                                                    g_mismatch_got)) {
        snprintf(g_mismatch_desc, sizeof g_mismatch_desc, "sdk");
        snprintf(g_mismatch_want, sizeof g_mismatch_want, "%s", XCDE_GAME_SDK_ID);
    }
#endif
}

static std::string g_status;
static bool g_mod_dirty;
static bool g_launched_ok;  // (launch() started the game: the launcher closes)
static float g_scale = 1;

#ifndef _WIN32
// [XCDE] T-LINUX-DUMP-PICKER: choose the dump folder with the desktop's folder dialog. The game and the launcher read it
// as targets/ (here a link, which the AppImage's AppRun also maintains): the choice is remembered by that link.
// A real targets/ folder (a dump copied into the checkout) is never replaced.
static SDL_Mutex *g_dump_lock;
static bool g_dump_pending, g_dump_ready;
static std::string g_dump_selected, g_dump_error;
static void SDLCALL dump_selected(void *, const char *const *files, int) {
    SDL_LockMutex(g_dump_lock);
    g_dump_selected = files && files[0] ? files[0] : "";
    g_dump_error = files ? "" : std::string("Could not open the folder dialog: ") + SDL_GetError();
    g_dump_ready = true;
    SDL_UnlockMutex(g_dump_lock);
}
static void check_game_version();
static void dump_poll() {
    if (!g_dump_lock) return;
    SDL_LockMutex(g_dump_lock);
    bool ready = g_dump_ready;
    std::string chosen = g_dump_selected, error = g_dump_error;
    g_dump_ready = false;
    SDL_UnlockMutex(g_dump_lock);
    if (!ready) return;
    g_dump_pending = false;
    if (!error.empty()) { g_status = error; return; }
    if (chosen.empty()) return;  // (cancel: keep the current dump)
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo((chosen + "/exefs/main").c_str(), &pi) || !SDL_GetPathInfo((chosen + "/romfs").c_str(), &pi) ||
        pi.type != SDL_PATHTYPE_DIRECTORY) {
        g_status = g.ui_lang ? "Not a dump: choose the folder that contains exefs/main and romfs/." :
                               "No es un volcado: elige la carpeta que contiene exefs/main y romfs/.";
        return;
    }
    std::string link = path("targets");
    struct stat ls;
    if (lstat(link.c_str(), &ls) == 0 && !S_ISLNK(ls.st_mode)) {
        g_status = g.ui_lang ? "targets/ is a real folder here: move or rename it to use another dump." :
                               "targets/ es una carpeta real: muévela o renómbrala para usar otro volcado.";
        return;
    }
    remove(link.c_str());
    if (symlink(chosen.c_str(), link.c_str()) != 0) { g_status = std::string("Could not link targets/: ") + strerror(errno); return; }
    g_mismatch_desc[0] = 0;
    check_game_version();  // [XCDE] GitHub #18: the version check again, for the new files
    g_status = g.ui_lang ? "Game dump selected." : "Volcado del juego seleccionado.";
}
#endif

static void launch() {
    if (!save()) { g_status = T("No se pudieron guardar los ajustes"); return; }
    apply_env();  // [XCDE] the variables from the Diagnostics tab (xcde_env.ini); the game inherits them
#ifdef _WIN32
    std::string exe = path("build/xcde.exe");
    std::string cmd = "\"" + exe + "\" targets";
    // the game's DLLs (SDL3, Vulkan loader, FFmpeg, opus) come from MSYS2
    const char *old = getenv("PATH");
    std::string p = g_root + "\\build;" + "C:\\msys64\\ucrt64\\bin;" + (old ? old : "");  // [portable] the folder's own DLLs first
    SetEnvironmentVariableA("PATH", p.c_str());
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::string log = path("build/last_game.log");
    // [XCDE] T260: the previous run's log is kept (last_game.prev.log): the report needs the run that failed
    MoveFileExA(log.c_str(), path("build/last_game.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE h = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = h;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessA(exe.c_str(), buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_root.c_str(), &si, &pi);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { g_status = std::string(T("No se pudo iniciar build/xcde.exe (error ")) + std::to_string(GetLastError()) + ")"; return; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g_status = T("Juego iniciado. Registro: build/last_game.log");
    g_launched_ok = true;
#else
    std::string exe = path(GAME_EXE), log = path("build/last_game.log");
    rename(log.c_str(), path("build/last_game.prev.log").c_str());  // [XCDE] T260: keep the previous run's log
    pid_t pid = spawn_logged({exe, "targets"}, log, false);
    if (!pid) { g_status = std::string("Could not start " GAME_EXE " (") + strerror(errno) + ")"; return; }
    g_status = T("Juego iniciado. Registro: build/last_game.log");
    g_launched_ok = true;
#endif
}

// ------------------------------------------------------------------ shader precompilation (T187)
// "Precompilar shaders" runs `build/xcde.exe --precompile targets` (runtime/nvn/vulkan/precompile.c): it translates every
// shader in the game's files into cache/spirv, so the first visit to an area doesn't stop to compile. It reports
// progress in cache/precompile_progress.txt and writes cache/precompile_done.txt at the end. JUGAR runs it first
// when it was never done (once, a few minutes; "Omitir" skips it).
#ifdef _WIN32
static HANDLE g_pc_proc;
#else
static pid_t g_pc_proc;  // [XCDE] T-LINUX
#endif
static bool g_pc_then_play, g_pc_play_now;
static float g_pc_frac;
static std::string g_pc_text;

// [XCDE] T211: done for THIS build - precompile_done.txt names the build/xcde.exe (size, time) it was made with; a new
// build translates shaders differently, so JUGAR precompiles again (unchanged shaders are cache hits: quick)
static bool pc_done() {
    FILE *f = fopen(path("cache/precompile_done.txt").c_str(), "r");
    if (!f) return false;
    char line[256];
    long long sz = -1, mt = -1;
    while (fgets(line, sizeof line, f)) sscanf(line, "exe %lld %lld", &sz, &mt);
    fclose(f);
    struct stat es;
    if (stat(path(GAME_EXE).c_str(), &es) != 0) return true;
    return sz == (long long)es.st_size && mt == (long long)es.st_mtime;
}
static bool pc_running() {
#ifdef _WIN32
    return g_pc_proc != NULL;
#else
    return g_pc_proc != 0;
#endif
}

static void pc_start(bool then_play) {
    apply_env();  // [XCDE] the Diagnostics tab variables also reach the shader precompiler
#ifdef _WIN32
    if (g_pc_proc) return;
    std::string exe = path("build/xcde.exe");
    std::string cmd = "\"" + exe + "\" --precompile targets";
    const char *old = getenv("PATH");
    std::string p = g_root + "\\build;" + "C:\\msys64\\ucrt64\\bin;" + (old ? old : "");  // [portable] the folder's own DLLs first
    SetEnvironmentVariableA("PATH", p.c_str());
    CreateDirectoryA(path("cache").c_str(), NULL);
    DeleteFileA(path("cache/precompile_progress.txt").c_str());
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    std::string log = path("build/last_precompile.log");
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE h = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = h;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessA(exe.c_str(), buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, NULL,
                             g_root.c_str(), &si, &pi);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { g_status = std::string(T("No se pudo iniciar la precompilación (error ")) + std::to_string(GetLastError()) + ")"; return; }
    CloseHandle(pi.hThread);
    g_pc_proc = pi.hProcess;
    g_pc_then_play = then_play;
    g_pc_frac = 0;
    g_pc_text = T("Buscando shaders en los archivos del juego...");
    g_status = then_play ? T("Preparando shaders antes de la primera partida (solo esta vez)") : T("Precompilando shaders");
#else
    if (g_pc_proc) return;
    SDL_CreateDirectory(path("cache").c_str());
    remove(path("cache/precompile_progress.txt").c_str());
    pid_t pid = spawn_logged({path(GAME_EXE), "--precompile", "targets"}, path("build/last_precompile.log"), true);
    if (!pid) { g_status = std::string(T("No se pudo iniciar la precompilación (error ")) + strerror(errno) + ")"; return; }
    g_pc_proc = pid;
    g_pc_then_play = then_play;
    g_pc_frac = 0;
    g_pc_text = T("Buscando shaders en los archivos del juego...");
    g_status = then_play ? T("Preparando shaders antes de la primera partida (solo esta vez)") : T("Precompilando shaders");
#endif
}

static void pc_stop() {
#ifdef _WIN32
    if (!g_pc_proc) return;
    TerminateProcess(g_pc_proc, 1);
    WaitForSingleObject(g_pc_proc, 2000);
    CloseHandle(g_pc_proc);
    g_pc_proc = NULL;
#else
    if (!g_pc_proc) return;
    kill(-g_pc_proc, SIGTERM);  // (its process group: the driver's shader threads too)
    bool ended = false;
    for (int i = 0; i < 20 && !ended; i++) { ended = waitpid(g_pc_proc, NULL, WNOHANG) == g_pc_proc; if (!ended) SDL_Delay(100); }
    if (!ended) { kill(-g_pc_proc, SIGKILL); waitpid(g_pc_proc, NULL, 0); }
    g_pc_proc = 0;
#endif
}

// (every frame while it runs) progress from the file; at the end, the result and JUGAR if it was waiting
static void pc_poll() {
    if (!g_pc_proc) return;
    static Uint64 last;
    Uint64 now = SDL_GetTicks();
    if (now - last < 200) return;
    last = now;
    FILE *f = fopen(path("cache/precompile_progress.txt").c_str(), "r");
    if (f) {
        char phase[16] = "";
        unsigned long long fd = 0, ft = 0, found = 0, done = 0;
        if (fscanf(f, "%15s %llu %llu %llu %llu", phase, &fd, &ft, &found, &done) == 5 && ft) {
            // reading the files is ~1/5 of the time, building the programs found the rest (it follows the reading)
            float fs = (float)fd / (float)ft, fc = found ? (float)done / (float)found : 0.0f;
            g_pc_frac = 0.15f * fs + 0.85f * fs * fc;
            char t[160];
            if (!strcmp(phase, "pipelines")) {  // [PRECOMPILE-STALL-FIX] the last pass (whole pipelines) has its own count: the bar was stuck at 100 % while it ran
                g_pc_frac = fs;
                snprintf(t, sizeof t, T("Paso final: pipelines completos %llu de %llu (puedes pulsar Omitir: el juego los compila al jugar)"), fd, ft);
            } else if (!strcmp(phase, "seed")) snprintf(t, sizeof t, "%s", T("Variantes conocidas (semilla)…"));  // [XCDE] T211: the seed pass
            else snprintf(t, sizeof t, T("Archivos %llu de %llu · programas de shaders listos %llu de %llu"), fd, ft, done, found);
            g_pc_text = t;
        }
        fclose(f);
    }
#ifdef _WIN32
    if (WaitForSingleObject(g_pc_proc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 1;
    GetExitCodeProcess(g_pc_proc, &code);
    CloseHandle(g_pc_proc);
    g_pc_proc = NULL;
#else
    int st = 0;
    if (waitpid(g_pc_proc, &st, WNOHANG) != g_pc_proc) return;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    g_pc_proc = 0;
#endif
    // [XCDE] T240: a run that died while the driver was building (cache/precompile_inflight.bin left behind) starts
    // again by itself - the new run skips what was being built (precompile.c crash-safe) - up to 3 times
    static int retries;
    if (!(code == 0 && pc_done()) && exists("cache/precompile_inflight.bin") && retries < 3) {
        retries++;
        bool then_play = g_pc_then_play;
        pc_start(then_play);
        g_status = std::string(T("El controlador gráfico falló en un shader: se salta y se sigue (intento ")) + std::to_string(retries + 1) + ")";
        return;
    }
    retries = 0;
    g_status = code == 0 && pc_done() ? T("Shaders precompilados") : T("La precompilación falló (registro: build/last_precompile.log)");
    if (g_pc_then_play) g_pc_play_now = true;
}

// ------------------------------------------------------------------ game code (release packages)
// [XCDE] A release package ships the runtime without recompiled game code (CMake MONADO_RELEASE). On first start the
// launcher runs the packaged builder (tools/gamebuild.py, frozen as monado-gamebuild): it lifts the code from the
// player's own targets/exefs and links gamebuild/lasgame.{dll,so} in this folder, which the runtime loads
// (runtime/gamelib.c). Source builds have the code in the executable: no gamebuild/link.json, and everything below
// reports ready.
#ifdef _WIN32
static HANDLE g_gc_proc;
static HANDLE g_gc_job;
#define GC_LIB "lasgame.dll"
#define GC_TOOL "monado-gamebuild.exe"
#else
static pid_t g_gc_proc;
#define GC_LIB "lasgame.so"
#define GC_TOOL "monado-gamebuild"
#endif
static bool g_gc_then_play, g_gc_play_now, g_gc_auto;
static float g_gc_frac;
static std::string g_gc_text;
static std::string gc_dir() {  // the packaged builder and its resources (read-only)
    const char *env = getenv("XCDE_GAMEBUILD_DIR");  // (AppRun: inside the AppImage)
    if (env && *env) return env;
    return path("build/gamebuild");                  // (Windows ZIP: <package>/build/gamebuild)
}
static bool gc_available() {
    SDL_PathInfo pi;
    return SDL_GetPathInfo((gc_dir() + "/link.json").c_str(), &pi);
}
static std::string read_line(const std::string &file) {
    std::string line;
    FILE *f = fopen(file.c_str(), "r");
    if (!f) return line;
    char buf[128];
    if (fgets(buf, sizeof buf, f)) line = buf;
    fclose(f);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}
// A library this runtime accepts: gamebuild/<lib>.id (written by the builder) equals the package's gamelib-id.txt.
static bool gc_ready() {
    if (!gc_available()) return true;
    std::string id = read_line(path("gamebuild/" GC_LIB ".id"));
    return !id.empty() && id == read_line(gc_dir() + "/gamelib-id.txt");
}
static bool gc_running() {
#ifdef _WIN32
    return g_gc_proc != NULL;
#else
    return g_gc_proc != 0;
#endif
}
static bool gc_start(bool then_play) {
    if (gc_running() || pc_running() || !gc_available() || gc_ready()) return false;
    std::string tool = gc_dir() + "/" GC_TOOL;
    SDL_PathInfo pi;
    if (!SDL_GetPathInfo(tool.c_str(), &pi)) {
        g_status = g.ui_lang ? "The package is incomplete: build/gamebuild/" GC_TOOL " is missing." :
                               "El paquete está incompleto: falta build/gamebuild/" GC_TOOL ".";
        return false;
    }
    SDL_CreateDirectory(path("gamebuild").c_str());
    SDL_CreateDirectory(path("build").c_str());
    remove(path("gamebuild/progress.txt").c_str());
    std::string root = path("targets"), out = path("gamebuild"), log = path("build/last_gamebuild.log");
#ifdef _WIN32
    std::string cmd = "\"" + tool + "\" --root \"" + root + "\" --out \"" + out + "\" --resources \"" + gc_dir() + "\"";
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi2 = {};
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE h = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = h;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    HANDLE job = CreateJobObjectA(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    BOOL ok = CreateProcessA(tool.c_str(), buf.data(), NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, NULL,
                             g_root.c_str(), &si, &pi2);
    DWORD err = GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) {
        if (job) CloseHandle(job);
        g_status = std::string(g.ui_lang ? "Could not start the game code build (error " : "No se pudo preparar el código del juego (error ") + std::to_string(err) + ")";
        return false;
    }
    if (job && !AssignProcessToJobObject(job, pi2.hProcess)) {
        CloseHandle(job);
        job = NULL;
    }
    ResumeThread(pi2.hThread);
    CloseHandle(pi2.hThread);
    g_gc_proc = pi2.hProcess;
    g_gc_job = job;
#else
    pid_t pid = spawn_logged({tool, "--root", root, "--out", out, "--resources", gc_dir()}, log, true);
    if (!pid) { g_status = std::string(g.ui_lang ? "Could not start the game code build (" : "No se pudo preparar el código del juego (") + strerror(errno) + ")"; return false; }
    g_gc_proc = pid;
#endif
    g_gc_then_play = then_play;
    g_gc_frac = 0;
    g_gc_text = g.ui_lang ? "Reading the game files..." : "Leyendo los archivos del juego...";
    g_status = g.ui_lang ? "Preparing the game code from your game files (only once, a few minutes)" :
                           "Preparando el código del juego desde tus archivos (solo una vez, unos minutos)";
    return true;
}
static void gc_stop() {  // (closing the launcher: the builder keeps what it compiled and continues next time)
    if (!gc_running()) return;
#ifdef _WIN32
    if (g_gc_job) TerminateJobObject(g_gc_job, 1);
    else TerminateProcess(g_gc_proc, 1);
    WaitForSingleObject(g_gc_proc, 2000);
    CloseHandle(g_gc_proc);
    g_gc_proc = NULL;
    if (g_gc_job) CloseHandle(g_gc_job);
    g_gc_job = NULL;
#else
    kill(-g_gc_proc, SIGTERM);
    bool ended = false;
    for (int i = 0; i < 20 && !ended; i++) { ended = waitpid(g_gc_proc, NULL, WNOHANG) == g_gc_proc; if (!ended) SDL_Delay(100); }
    if (!ended) { kill(-g_gc_proc, SIGKILL); waitpid(g_gc_proc, NULL, 0); }
    g_gc_proc = 0;
#endif
}
static void gc_poll() {
    if (!gc_running()) return;
    static Uint64 last;
    Uint64 now = SDL_GetTicks();
    if (now - last < 250) return;
    last = now;
    FILE *f = fopen(path("gamebuild/progress.txt").c_str(), "r");
    if (f) {
        char phase[24] = "";
        unsigned long long done = 0, total = 0;
        if (fscanf(f, "%23s %llu %llu", phase, &done, &total) == 3 && total) {
            int step = !strcmp(phase, "generate") ? 0 : !strcmp(phase, "compile") ? 1 : 2;
            g_gc_frac = (step + (float)done / (float)total) / 3.0f;
            char t[160];
            if (step == 0) snprintf(t, sizeof t, g.ui_lang ? "Reading the game code: %llu of %llu" : "Leyendo el código del juego: %llu de %llu", done, total);
            else if (step == 1) snprintf(t, sizeof t, g.ui_lang ? "Compiling the game code: %llu of %llu files" : "Compilando el código del juego: %llu de %llu archivos", done, total);
            else snprintf(t, sizeof t, "%s", g.ui_lang ? "Linking the game code..." : "Enlazando el código del juego...");
            g_gc_text = t;
        }
        fclose(f);
    }
#ifdef _WIN32
    if (WaitForSingleObject(g_gc_proc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 1;
    GetExitCodeProcess(g_gc_proc, &code);
    CloseHandle(g_gc_proc);
    g_gc_proc = NULL;
    if (g_gc_job) CloseHandle(g_gc_job);
    g_gc_job = NULL;
#else
    int st = 0;
    if (waitpid(g_gc_proc, &st, WNOHANG) != g_gc_proc) return;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    g_gc_proc = 0;
#endif
    bool ok = code == 0 && gc_ready();
    g_status = ok ? (g.ui_lang ? "Game code ready." : "Código del juego listo.") :
                    (g.ui_lang ? "Preparing the game code failed (log: build/last_gamebuild.log)" :
                                 "Falló la preparación del código del juego (registro: build/last_gamebuild.log)");
    if (ok && g_gc_then_play) g_gc_play_now = true;
}

// ------------------------------------------------------------------ FSR 4 Lite model files
// [XCDE] FSR 4 Lite replays AMD's Lite INT8 network; its model files are recorded on this PC from AMD's own upscaler
// DLL (downloaded from AMD's GitHub) by the packaged monado-fsr4prep (tools/fsr4_prepare.py, bbport's capture tooling
// in third_party/fsr4cap) and kept in fsr4/ here. Nothing of AMD's is shipped. Linux records through the player's
// Proton; Windows natively on vkd3d-proton.
#ifdef _WIN32
static HANDLE g_fp_proc;
#define FP_TOOL "monado-fsr4prep.exe"
#else
static pid_t g_fp_proc;
#define FP_TOOL "monado-fsr4prep"
#endif
static float g_fp_frac;
static std::string g_fp_text;
static std::string fp_dir() {  // the packaged tool and capture tooling
    const char *env = getenv("XCDE_FSR4PREP_DIR");  // (AppRun: inside the AppImage)
    if (env && *env) return env;
    return path("build/fsr4prep");
}
static bool fp_available() { SDL_PathInfo pi; return SDL_GetPathInfo((fp_dir() + "/" FP_TOOL).c_str(), &pi); }
static bool fp_ready() {
    SDL_PathInfo pi;
    const char *env = getenv("MONADO_FSR4_ASSET_DIR");
    if (env && *env && SDL_GetPathInfo((std::string(env) + "/lite/t1080_m0/initializer.bin").c_str(), &pi)) return true;
    return SDL_GetPathInfo(path("fsr4/lite/t1080_m0/initializer.bin").c_str(), &pi) ||
           SDL_GetPathInfo(path("build/fsr4/lite/t1080_m0/initializer.bin").c_str(), &pi);
}
static bool fp_running() {
#ifdef _WIN32
    return g_fp_proc != NULL;
#else
    return g_fp_proc != 0;
#endif
}
static void fp_start() {
    if (fp_running() || !fp_available()) return;
    std::string tool = fp_dir() + "/" FP_TOOL, out = path("fsr4"), log = path("build/last_fsr4prep.log");
    remove(path("fsr4-progress.txt").c_str());
    SDL_CreateDirectory(path("build").c_str());  // (the log's folder: missing in a fresh data root)
#ifdef _WIN32
    std::string cmd = "\"" + tool + "\" --out \"" + out + "\" --tools \"" + fp_dir() + "\"";
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi2 = {};
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE h = CreateFileA(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = h;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    BOOL ok = CreateProcessA(tool.c_str(), buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, g_root.c_str(), &si, &pi2);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) { g_status = std::string(g.ui_lang ? "Could not start the FSR 4 Lite preparation (error " : "No se pudo preparar FSR 4 Lite (error ") + std::to_string(GetLastError()) + ")"; return; }
    CloseHandle(pi2.hThread);
    g_fp_proc = pi2.hProcess;
#else
    pid_t pid = spawn_logged({tool, "--out", out, "--tools", fp_dir()}, log, true);
    if (!pid) { g_status = std::string(g.ui_lang ? "Could not start the FSR 4 Lite preparation (" : "No se pudo preparar FSR 4 Lite (") + strerror(errno) + ")"; return; }
    g_fp_proc = pid;
#endif
    g_fp_frac = 0;
    g_fp_text = g.ui_lang ? "Downloading AMD's upscaler DLL..." : "Descargando la DLL de AMD...";
    g_status = g.ui_lang ? "Preparing FSR 4 Lite on this PC (a few minutes, once)" : "Preparando FSR 4 Lite en este PC (unos minutos, una vez)";
}
static void fp_stop() {
    if (!fp_running()) return;
#ifdef _WIN32
    TerminateProcess(g_fp_proc, 1);
    WaitForSingleObject(g_fp_proc, 2000);
    CloseHandle(g_fp_proc);
    g_fp_proc = NULL;
#else
    kill(-g_fp_proc, SIGTERM);
    bool ended = false;
    for (int i = 0; i < 20 && !ended; i++) { ended = waitpid(g_fp_proc, NULL, WNOHANG) == g_fp_proc; if (!ended) SDL_Delay(100); }
    if (!ended) { kill(-g_fp_proc, SIGKILL); waitpid(g_fp_proc, NULL, 0); }
    g_fp_proc = 0;
#endif
}
static void fp_poll() {
    if (!fp_running()) return;
    static Uint64 last;
    Uint64 now = SDL_GetTicks();
    if (now - last < 250) return;
    last = now;
    FILE *f = fopen(path("fsr4-progress.txt").c_str(), "r");
    if (f) {
        char phase[24] = "";
        unsigned long long done = 0, total = 0;
        if (fscanf(f, "%23s %llu %llu", phase, &done, &total) == 3 && total) {
            char t[160];
            if (!strcmp(phase, "download")) { g_fp_frac = 0.05f * done / total; snprintf(t, sizeof t, "%s", g.ui_lang ? "Downloading AMD's upscaler DLL..." : "Descargando la DLL de AMD..."); }
            else if (!strcmp(phase, "record")) { g_fp_frac = 0.05f + 0.85f * done / total; snprintf(t, sizeof t, g.ui_lang ? "Recording FSR 4's passes: %llu of %llu" : "Grabando las pasadas de FSR 4: %llu de %llu", done, total); }
            else { g_fp_frac = 0.95f; snprintf(t, sizeof t, "%s", g.ui_lang ? "Translating the shaders..." : "Traduciendo los shaders..."); }
            g_fp_text = t;
        }
        fclose(f);
    }
#ifdef _WIN32
    if (WaitForSingleObject(g_fp_proc, 0) != WAIT_OBJECT_0) return;
    DWORD code = 1;
    GetExitCodeProcess(g_fp_proc, &code);
    CloseHandle(g_fp_proc);
    g_fp_proc = NULL;
#else
    int st = 0;
    if (waitpid(g_fp_proc, &st, WNOHANG) != g_fp_proc) return;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    g_fp_proc = 0;
#endif
    if (code == 0 && fp_ready()) g_status = g.ui_lang ? "FSR 4 Lite is ready." : "FSR 4 Lite está listo.";
    else if (code == 3) g_status = g.ui_lang ? "FSR 4 Lite needs Proton (Steam's Proton or GE-Proton) to prepare its model files on Linux." :
                                               "FSR 4 Lite necesita Proton (el de Steam o GE-Proton) para preparar sus modelos en Linux.";
    else g_status = g.ui_lang ? "Preparing FSR 4 Lite failed (log: build/last_fsr4prep.log)" :
                                "Falló la preparación de FSR 4 Lite (registro: build/last_fsr4prep.log)";
}
// (Graphics page, under the upscaler when FSR 4 Lite is chosen)
static void fp_section() {
    fp_poll();
    if (fp_ready()) {
        ImGui::TextDisabled("%s", g.ui_lang ? "FSR 4 Lite: model files ready." : "FSR 4 Lite: modelos listos.");
        return;
    }
    if (fp_running()) {
        ImGui::ProgressBar(g_fp_frac, ImVec2(-1, 0), g_fp_text.c_str());
        return;
    }
    ImGui::TextWrapped("%s", g.ui_lang ?
        "FSR 4 Lite needs its model files, made once on this PC from AMD's own upscaler DLL: it is downloaded from AMD's "
        "GitHub (58 MB) and its passes are recorded on your GPU (a few minutes). On Linux this needs Proton installed "
        "(Steam's Proton or GE-Proton). Until then FSR 1 is used." :
        "FSR 4 Lite necesita sus modelos, hechos una vez en este PC a partir de la DLL de AMD: se descarga del GitHub de "
        "AMD (58 MB) y sus pasadas se graban en tu GPU (unos minutos). En Linux hace falta Proton instalado (el de Steam "
        "o GE-Proton). Mientras tanto se usa FSR 1.");
    ImGui::BeginDisabled(!fp_available());
    if (ImGui::Button(g.ui_lang ? "Prepare FSR 4 Lite" : "Preparar FSR 4 Lite")) fp_start();
    ImGui::EndDisabled();
    if (!fp_available()) ImGui::SetItemTooltip("%s", g.ui_lang ? "This build has no FSR 4 preparation tools (release packages bring them)." :
                                                               "Este build no trae las herramientas de FSR 4 (los paquetes de release sí).");
}

// ------------------------------------------------------------------ remapping capture
static int cap_dev = -1, cap_slot = -1;  // dev 0 keyboard, 1 controller, 2 keyboard (second key)
static Uint64 cap_until, cap_since;

// a key / mouse input for the slot being captured: one already in use elsewhere (either column) takes the slot's old one
static void capture_key(int k) {
    int *t = cap_dev == 2 ? g.key2 : g.key, *o = cap_dev == 2 ? g.key : g.key2;
    for (int i = 0; i < NACT; i++) {
        if (t[i] == k && i != cap_slot) t[i] = t[cap_slot];
        if (o[i] == k) o[i] = t[cap_slot];
    }
    t[cap_slot] = k;
    cap_dev = -1;
}

static bool capture_event(const SDL_Event &e) {
    if (cap_dev < 0) return false;
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.scancode == SDL_SCANCODE_DELETE && cap_dev != 1) { capture_key(0); return true; }  // (Supr: none)
        if (e.key.scancode == SDL_SCANCODE_ESCAPE && SDL_GetTicks() - cap_since < 700) { cap_dev = -1; return true; }
        if (cap_dev != 1) capture_key(e.key.scancode);
        else if (e.key.scancode == SDL_SCANCODE_ESCAPE) cap_dev = -1;
        return true;
    }
    // mouse buttons / wheel (not the click that started the capture)
    if (cap_dev != 1 && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && SDL_GetTicks() - cap_since > 250) {
        capture_key(MOUSE_BASE + (e.button.button < 6 ? e.button.button : 5));
        return true;
    }
    if (cap_dev != 1 && e.type == SDL_EVENT_MOUSE_WHEEL && e.wheel.y) {
        capture_key(e.wheel.y > 0 ? M_WUP : M_WDOWN);
        return true;
    }
    if (cap_dev == 1 && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        int h = e.gbutton.button;
        for (int i = 0; i < NPAD; i++) if (g.pad[i] == h && i != cap_slot) g.pad[i] = g.pad[cap_slot];
        g.pad[cap_slot] = h;
        cap_dev = -1;
        return true;
    }
    if (cap_dev == 1 && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && e.gaxis.value > 20000 &&
        (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || e.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) {
        int h = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? PADB_LT : PADB_RT;
        for (int i = 0; i < NPAD; i++) if (g.pad[i] == h && i != cap_slot) g.pad[i] = g.pad[cap_slot];
        g.pad[cap_slot] = h;
        cap_dev = -1;
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ UI

// [XCDE] T259: adapt the legacy launcher model at one boundary; policy lives in quality_policy.h.
static Settings quality_settings() {
    Settings v{};
    static const int resolutions[]={RES_MATCH,1,RES_1_5,2,3,RES_Q75,RES_Q50,RES_DYNAMIC};
    static const int upscalers[]={UPSCALE_BILINEAR,UPSCALE_FSR1,UPSCALE_FSR3,UPSCALE_DLSS,UPSCALE_FSR4_LITE};
    static const int fps[]={30,60,120,144,165,0};
    v.quality_preset=g.quality_preset; v.gpu_performance=g.gpu_performance;
    v.res_mode=resolutions[std::clamp(g.res,0,7)]; v.upscaler=upscalers[std::clamp(g.upscaler,0,4)];
    v.upscaler_auto=g.upscaler_auto; v.antialias=g.smaa ? AA_SMAA : AA_OFF;
    v.upscale_quality=g.uquality;
    v.dof_off=!g.dof; v.game_blur=g.game_blur; v.frame_gen=g.frame_gen;
    v.dynamic_min_height=g.dynamic_min_height; v.dynamic_max_height=g.dynamic_max_height;
    v.effects_off=g.effects_off; v.fps=fps[std::clamp(g.fps,0,5)];
    v.cutscene_mode=g.cut_fg ? 2 : g.cutscenes ? 1 : 0; return v;
}
static void quality_from_settings(const Settings &v) {
    static const int resolutions[]={RES_MATCH,1,RES_1_5,2,3,RES_Q75,RES_Q50,RES_DYNAMIC};
    static const int upscalers[]={UPSCALE_BILINEAR,UPSCALE_FSR1,UPSCALE_FSR3,UPSCALE_DLSS,UPSCALE_FSR4_LITE};
    static const int fps[]={30,60,120,144,165,0};
    for (int i=0;i<8;i++) if (resolutions[i]==v.res_mode) g.res=i;
    for (int i=0;i<5;i++) if (upscalers[i]==v.upscaler) g.upscaler=i;
    for (int i=0;i<6;i++) if (fps[i]==v.fps) g.fps=i;
    g.quality_preset=v.quality_preset; g.gpu_performance=v.gpu_performance;
    g.upscaler_auto=v.upscaler_auto; g.smaa=v.antialias==AA_SMAA;
    g.uquality=v.upscale_quality;
    g.dof=!v.dof_off; g.game_blur=v.game_blur; g.frame_gen=v.frame_gen;
    g.dynamic_min_height=v.dynamic_min_height; g.dynamic_max_height=v.dynamic_max_height; g.effects_off=v.effects_off;
    g.cut_fg=v.cutscene_mode==2; g.cutscenes=v.cutscene_mode ? 1 : 0;
}
static XcdeGpu *adapters;
static uint32_t adapter_count;
static bool probed;
static char gpu_error[512];
static void ensure_adapters() { if (!probed) { adapters=xcde_gpu_scan(&adapter_count,gpu_error,sizeof gpu_error); probed=true; } }
static void tab_home() {
    ensure_adapters();
    ImGui::SeparatorText(g.ui_lang ? "Ready to play" : "Preparar partida");
    const XcdeGpuInfo *gpu=xcde_gpu_preference(adapters,adapter_count,g.gpu_uuid,g.gpu_index);
    ImGui::TextDisabled("%s",g.ui_lang ? "Selected graphics adapter" : "Adaptador gráfico seleccionado");
    ImGui::TextWrapped("%s",gpu ? gpu->name : (g.ui_lang ? "No compatible adapter detected" : "Sin adaptador compatible detectado"));
    if (*gpu_error) ImGui::TextWrapped("%s",gpu_error);
    ImGui::Spacing();
    Settings v=quality_settings(); monado_quality_controls(&v,g.ui_lang!=0); quality_from_settings(v);
    ImGui::Spacing();
    ImGui::TextWrapped("%s",g.ui_lang ? "For modest hardware, start with Potato and a 30 FPS target. Quality and frame rate are independent." :
        "En equipos modestos, empieza con Patata y un objetivo de 30 FPS. La calidad y los FPS se eligen por separado.");
    ImGui::SeparatorText(g.ui_lang ? "Shader preparation" : "Preparación de shaders");
    static uint64_t checked; static bool prepared;
    if (!checked || SDL_GetTicks()-checked>500) { prepared=pc_done(); checked=SDL_GetTicks(); }
    ImGui::TextWrapped("%s",prepared ? (g.ui_lang ? "Prepared for this executable. New areas can still need additional variants." :
        "Preparados para este ejecutable. Algunas zonas aún pueden necesitar variantes adicionales.") :
        (g.ui_lang ? "Play prepares the shader cache first. Progress can be resumed." : "Jugar prepara primero la cache de shaders. El progreso se puede retomar."));
}
static void make_report();
static void tab_diagnostics() {
    if (ImGui::Button(T("Crear reporte"))) make_report();
    ImGui::TextWrapped("%s",g.ui_lang ? "Includes configuration and logs; excludes saves and game files." : "Incluye configuración y registros; excluye partidas y archivos del juego.");

    // [XCDE] environment variables for the game: NAME=VALUE rows, saved to xcde_env.ini and applied when PLAY starts
    // the game (or the shader precompiler). Useful for the XCDE_* diagnostic switches (e.g. XCDE_RSC_PIXEL=1).
    ImGui::SeparatorText(T("Variables de entorno"));
    ImGui::TextWrapped(T("Se pasan al juego al iniciar. Ejemplo: XCDE_RSC_PIXEL=1 (se aplica la próxima vez que pulses JUGAR)."));
    ImGui::TextDisabled(T("Se guardan en xcde_env.ini (también puedes editarlas ahí)."));
    int focus_row = -1;  // (an existing blank row is reused instead of adding another one)
    ImGui::BeginDisabled(g.nenv >= NENV);
    if (ImGui::Button(T("Añadir variable"))) {
        for (int i = 0; i < g.nenv; i++) if (!g.env[i].name[0]) { focus_row = i; break; }
        if (focus_row < 0) {
            EnvVar &e = g.env[g.nenv++];
            memset(&e, 0, sizeof e);
            focus_row = g.nenv - 1;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d", g.nenv, NENV);
    if (ImGui::BeginTable("env_vars", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Nombre"), ImGuiTableColumnFlags_WidthStretch, 0.36f);
        ImGui::TableSetupColumn(T("Valor"), ImGuiTableColumnFlags_WidthStretch, 0.64f);
        ImGui::TableSetupColumn("##remove", ImGuiTableColumnFlags_WidthFixed, 88 * g_scale);
        ImGui::TableHeadersRow();
        int remove = -1;
        for (int i = 0; i < g.nenv; i++) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (i == focus_row) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##name", g.env[i].name, sizeof g.env[i].name, ImGuiInputTextFlags_CharsNoBlank);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##value", g.env[i].value, sizeof g.env[i].value);
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton(T("Quitar"))) remove = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (remove >= 0) {
            for (int i = remove; i + 1 < g.nenv; i++) g.env[i] = g.env[i + 1];
            g.nenv--;
        }
    }
    if (!g.nenv) ImGui::TextDisabled(T("No hay variables de entorno."));
    bool bad_name = false;
    for (int i = 0; i < g.nenv; i++) if (g.env[i].name[0] && !env_name_valid(g.env[i].name)) bad_name = true;
    if (bad_name) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", T("Nombre no válido: usa letras, números y _ (sin empezar por un número)."));

    ensure_adapters();
    ImGui::SeparatorText(g.ui_lang ? "Adapter diagnostics" : "Diagnóstico del adaptador");
    for (uint32_t i=0;i<adapter_count;i++) {
        const XcdeGpuInfo &v=adapters[i].info;
        ImGui::TextWrapped("%s",v.name); ImGui::TextWrapped("%s",v.reason);
        ImGui::Text("Vulkan %u.%u | driver %u | max buffer %u MiB",VK_API_VERSION_MAJOR(v.api),VK_API_VERSION_MINOR(v.api),v.driver,v.max_storage_range>>20);
        ImGui::Separator();
    }
    ImGui::TextWrapped("%s",g.ui_lang ? "Gameplay compatibility is verified per GPU and driver. Capability detection alone is not certification." :
        "La compatibilidad jugable se comprueba por GPU y controlador. Detectar capacidades no certifica el juego.");
    ImGui::TextWrapped("%s",g.ui_lang ? "Frame metrics: start using Play Monado (performance metrics).bat. Reports distinguish native FPS, GPU work and waits." :
        "Metricas: inicia con Play Monado (performance metrics).bat. El informe distingue FPS nativos, trabajo GPU y esperas.");
}

static void tab_graphics() {
    Settings initial=quality_settings();
    Settings editable=initial; monado_quality_controls(&editable,g.ui_lang!=0); quality_from_settings(editable);
    XcdeQuality before=xcde_quality_current(&editable); int preset_before=g.quality_preset;

    ensure_adapters();
    const XcdeGpuInfo *selected_gpu=xcde_gpu_preference(adapters,adapter_count,g.gpu_uuid,g.gpu_index);
    static uint32_t loader_api=xcde_vk_loader_version();
    bool route_fallback;
    int route=selected_gpu ? xcde_vulkan_route(selected_gpu->vulkan_routes,10+(int)VK_API_VERSION_MINOR(loader_api),g.vulkan_api,&route_fallback) : 0;
    bool dlss_files=exists("build/sl.interposer.dll") && exists("build/sl.common.dll") && exists("build/sl.dlss.dll") && exists("build/nvngx_dlss.dll");
    bool dlss_candidate=selected_gpu && selected_gpu->vendor==0x10de && route>=13 && dlss_files;
    if (ImGui::CollapsingHeader(g.ui_lang ? "Advanced GPU / Vulkan (restart)" : "GPU / Vulkan avanzado (reiniciar)")) {
    ImGui::SeparatorText("GPU / Vulkan");
    xcde_gpu_picker(g.ui_lang ? "Graphics adapter" : "Adaptador gráfico", g.gpu_uuid, sizeof g.gpu_uuid,
                    &g.gpu_index, adapters, adapter_count, g.ui_lang != 0);
    xcde_vulkan_picker(selected_gpu, loader_api, &g.vulkan_api, g.ui_lang != 0);
    if (*gpu_error) ImGui::TextWrapped("%s", gpu_error);
    if (ImGui::Button(g.ui_lang ? "Refresh adapters" : "Actualizar adaptadores")) { free(adapters); adapters = nullptr; probed = false; }
    ImGui::Checkbox(g.ui_lang ? "Restore heavy AMD volume effects" : "Restaurar efectos volumétricos pesados AMD", &g.gpu_keep_heavy);
    {   // [XCDE][FSR3] GitHub #14: occlusion culling misresolves on AMD integrated GPUs (the floor-vanishing
        // class of bug, oq_enabled in exec.c); automatic = off there, on elsewhere, overridable from here
        // and from the in-game menu (xcde.ini occlusion = auto | off | on, read at startup).
        static const char *occl_ui[] = {"Automático (AMD integrada: apagada)", "Desactivada", "Activada"};
        static const char *occl_en[] = {"Automatic (AMD integrated: off)", "Off", "On"};
        int occl = g.occlusion == 0 ? 1 : g.occlusion == 1 ? 2 : 0;
        if (monado_combo(g.ui_lang ? "Occlusion culling (may help FSR 3) (restart)" : "Culling por oclusión (puede ayudar a FSR 3) (al reiniciar)",
                         &occl, g.ui_lang ? occl_en : occl_ui, 3))
            g.occlusion = occl == 1 ? 0 : occl == 2 ? 1 : -1;
        ImGui::SetItemTooltip(T("Resuelve lo que se ve tras el suelo / el suelo que desaparece con FSR 3 en algunas GPUs.\n"
                                "Automático: solo lo apaga en las gráficas integradas AMD donde falla."));
    }
    {   // [XCDE] issue #7: confirmed on RDNA 3 (RX 7800 XT and Ryzen 8600G iGPU).
        static const char *amd_ui[] = {"Automático (RDNA 3: activado)", "Desactivado", "Activado"};
        static const char *amd_en[] = {"Automatic (RDNA 3: on)", "Off", "On"};
        int fix = g.amd_fix == 0 ? 1 : g.amd_fix == 1 ? 2 : 0;
        if (monado_combo(g.ui_lang ? "AMD flicker fix (restart)" : "Fix de parpadeo AMD (al reiniciar)",
                         &fix, g.ui_lang ? amd_en : amd_ui, 3))
            g.amd_fix = fix == 1 ? 0 : fix == 2 ? 1 : -1;
        ImGui::SetItemTooltip(T("Puntos de color de 1-2 px en gráficas AMD RDNA 3 (issue #7).\n"
                                "Automático: solo se activa en RDNA 3. Desactivado si te baja el rendimiento."));
    }
    {   // [XCDE] issue #25: long-session corruption. Conservative mode: no async render worker.
        static const char *ss_ui[] = {"Automático (activado)", "Desactivado", "Activado"};
        static const char *ss_en[] = {"Automatic (on)", "Off", "On"};
        int ss = g.session_fix == 0 ? 1 : g.session_fix == 1 ? 2 : 0;
        if (monado_combo(g.ui_lang ? "Long-session compatibility (restart)" : "Compatibilidad de sesión larga (al reiniciar)",
                         &ss, g.ui_lang ? ss_en : ss_ui, 3))
            g.session_fix = ss == 1 ? 0 : ss == 2 ? 1 : -1;
        ImGui::SetItemTooltip(T("Evita que modelos y texturas se corrompan en partidas largas.\n"
                                "Desactiva el hilo de render asíncrono (algo menos de rendimiento)."));
    }
    ImGui::TextDisabled(g.ui_lang ? "GPU changes apply on next launch." : "Los cambios GPU se aplican al iniciar el juego.");
    }
    ImGui::SeparatorText(T("Pantalla"));
    ImGui::Checkbox(T("Pantalla completa"), &g.fullscreen);
    const char *aspects[] = {g.ui_lang ? "Automatic" : "Automático", g.ui_lang ? "Original 16:9" : "16:9 original"};
    monado_combo(g.ui_lang ? "Aspect ratio" : "Relación de aspecto", &g.aspect, aspects, 2);
    ImGui::SetNextItemWidth(260 * g_scale);
    monado_combo(T("Tamaño de la ventana"), &g.win, k_wins, 5);
    ImGui::SeparatorText(T("Resolución y escalado"));
    ImGui::SetNextItemWidth(360 * g_scale);
    monado_combo(T("Resolución interna"), &g.res, TL(k_res_ui, 8).p, 8);
    ImGui::TextDisabled(T("El juego original renderiza a 720p. Más resolución = imagen más nítida y más carga para la GPU."));
    const char *ups[] = {"Bilineal (suave)", "AMD FSR 1.0 (nítido)", "AMD FSR 3.1 (temporal, cualquier GPU)",
                         "NVIDIA DLSS (temporal, GPU RTX)", "AMD FSR 4 Lite (temporal, nativo)"};
    const int nups = 5;  // [XCDE] T-FSR4-LITE: the native component (Linux and Windows)
    ImGui::SetNextItemWidth(320 * g_scale);
    const char *up_preview = g.upscaler_auto ? (g.ui_lang ? "Automatic (DLSS on supported RTX)" : "Automático (DLSS en RTX compatible)") : T(ups[g.upscaler]);
    if (monado_begin_combo(T("Escalado / antialiasing"), up_preview)) {
        if (ImGui::Selectable(g.ui_lang ? "Automatic (recommended)" : "Automático (recomendado)", g.upscaler_auto)) g.upscaler_auto = true;
        for (int i = 0; i < nups; ++i) {
            ImGui::BeginDisabled(i == 3 && !dlss_candidate);
            if (ImGui::Selectable(T(ups[i]), !g.upscaler_auto && g.upscaler == i)) { g.upscaler_auto = false; g.upscaler = i; }
            ImGui::EndDisabled();
            if (i == 3 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", g.ui_lang ? "Requires NVIDIA RTX, Vulkan 1.3+ and official DLLs. SDK confirms support at launch." :
                    "Requiere NVIDIA RTX, Vulkan 1.3+ y DLL oficiales. El SDK confirma compatibilidad al iniciar.");
        }
        ImGui::EndCombo();
    }
    if (!dlss_files) ImGui::TextDisabled(g.ui_lang ? "DLSS: official NVIDIA runtime not installed." : "DLSS: falta el paquete oficial NVIDIA.");
    if (!g.upscaler_auto && g.upscaler == 4) fp_section();  // [XCDE] FSR 4 Lite's model files (prepared on this PC)
    else if (dlss_candidate) ImGui::TextDisabled(g.ui_lang ? "DLSS runtime installed; GPU/driver checked on launch." : "DLSS instalado; la GPU/controlador se comprueba al iniciar.");
    ImGui::SetItemTooltip(T("FSR 1.0: escalado espacial, solo cuando la resolución interna es menor que la ventana.\n"
                          "FSR 3.1 y DLSS: reconstrucción temporal - renderizan a menos resolución (según la calidad)\n"
                          "y reconstruyen la imagen de la ventana con varios frames: más FPS y bordes sin dientes.\n"
                          "Sustituyen al antialiasing temporal del juego. DLSS necesita una GPU NVIDIA RTX; si no\n"
                          "está disponible se usa FSR 3.1."));
    if (g.upscaler == 1 || g.upscaler == 2) {  // [XCDE] RCAS strength (FSR 1 / FSR 3), also live in F1
        float sharp = g.sharpness >= 0.0f ? g.sharpness : g.upscaler == 1 ? 0.9f : 0.3f;
        ImGui::SetNextItemWidth(320 * g_scale);
        if (ImGui::SliderFloat(g.ui_lang ? "Sharpening (RCAS)" : "Nitidez (RCAS)", &sharp, 0.0f, 1.0f, "%.2f")) g.sharpness = sharp;
        ImGui::SetItemTooltip("%s", g.ui_lang ? "0 = off, 1 = maximum. Default: FSR 1 0.90, FSR 3 0.30 (as before)."
                                              : "0 = desactivada, 1 = máxima. Predeterminada: FSR 1 0,90, FSR 3 0,30 (como antes).");
        if (g.sharpness >= 0.0f) {
            ImGui::SameLine();
            if (ImGui::SmallButton(g.ui_lang ? "Default" : "Predeterminada")) g.sharpness = -1.0f;
        }
    }
    if (g.upscaler >= 2 || (g.upscaler_auto && dlss_candidate)) {
        const char *uq[] = {"Nativo (solo antialiasing: DLAA / FSR Native AA)", "Calidad (67 %)", "Equilibrado (59 %)",
                            "Rendimiento (50 %)", "Ultra rendimiento (33 %)"};
        ImGui::SetNextItemWidth(320 * g_scale);
        monado_combo(T("Calidad del escalado"), &g.uquality, TL(uq, 5).p, 5);
        ImGui::SetItemTooltip(T("Con 'Resolución interna' en 'Igual que la ventana': la resolución a la que se renderiza\n"
                              "antes de reconstruir (porcentaje del lado de la ventana)."));
        ImGui::Checkbox(T("Generación de frames (FSR 3)"), &g.frame_gen);
        ImGui::SetItemTooltip(T("Inserta un frame interpolado entre cada dos del juego en todo el juego: el doble de imágenes\n"
                              "por segundo (p. ej. FPS 30 -> 60 en pantalla). La de AMD: funciona con FSR 3.1 y con DLSS.\n"
                              "Añade algo de latencia. Se aplica al iniciar el juego."));
        ImGui::BeginDisabled(g.frame_gen);
        ImGui::Checkbox(T("Generación de frames solo en cinemáticas"), &g.cut_fg);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(T("El juego va a tus FPS sin generación de frames; en las cinemáticas (donde el juego anima a\n"
                              "los personajes a 30) baja a 30 parejos y la generación las lleva a 60 en pantalla.\n"
                              "Pensado para jugar a 60 en un monitor de 60 Hz. Se aplica al iniciar el juego."));
    } else {
        ImGui::TextDisabled(T("Se aplica cuando la resolución interna es menor que la ventana."));
    }
    ImGui::SeparatorText(T("Calidad"));
    ImGui::Checkbox(T("Antialiasing SMAA"), &g.smaa);
    ImGui::Checkbox(T("Profundidad de campo (desenfoque del juego)"), &g.dof);
    ImGui::SetItemTooltip(T("El desenfoque de lo cercano y lo lejano que el juego aplica también en Switch.\n"
                          "Desactivado: toda la escena nítida (y algo menos de trabajo para la GPU)."));
    ImGui::Checkbox(T("Antialiasing y desenfoque de movimiento del juego"), &g.game_blur);
    ImGui::SetItemTooltip(T("El antialiasing temporal (TMAA), el desenfoque de movimiento y el del pelo que el juego\n"
                          "aplica para los 720p de la Switch: a 1080p o más suavizan toda la imagen.\n"
                          "Desactivado: imagen nítida (combínalo con SMAA para los bordes)."));
    ImGui::Checkbox(T("Antialiasing temporal (TAA) del juego"), &g.taa);
    ImGui::SetItemTooltip(T("Solo el TAA, sin tocar el desenfoque de movimiento ni el del pelo.\nDesactivado: quita el emborronado del TAA (combínalo con SMAA para los bordes)."));
    g.grass_lod = grass_lod_valid(g.grass_lod);  // [XCDE] grass draw distance (also live in F1)
    monado_combo(g.ui_lang ? "Grass distance" : "Distancia de la hierba", &g.grass_lod,
                 g.ui_lang ? grass_lod_labels : grass_lod_labels_es, GRASS_PRESET_COUNT);
    ImGui::SetItemTooltip("%s", g.ui_lang
        ? "Keeps grass visible farther away. Higher settings cost FPS. Original keeps High's nearby detail at the original distance; Low halves reach. Also live in F1."
        : "Mantiene la hierba visible más lejos. Los valores altos cuestan FPS. Original conserva el detalle cercano de Alta a la distancia original; Baja reduce el alcance a la mitad. También en vivo en F1.");
    ImGui::Checkbox(g.ui_lang ? "On-screen FPS counter" : "Contador FPS en pantalla", &g.fps_overlay);
    ImGui::Checkbox(T("Mostrar FPS en la barra de título"), &g.show_fps);
    ImGui::SeparatorText(g.ui_lang ? "Shadows, detail and effects" : "Sombras, detalle y efectos");  // [XCDE]
    for (int i : gfx_ui_options) {
        const char *items[5];
        for (int v = 0; v < gfx_count(i); v++) items[v] = gfx_display_lang(i, v, g.ui_lang);
        ImGui::SetNextItemWidth(260 * g_scale);
        monado_combo(gfx_label(i, g.ui_lang), &g.graphics.value[i], items, gfx_count(i));
        if (i == GFX_SHADOWS) ImGui::SetItemTooltip("%s", g.ui_lang ? "Off removes dynamic shadow shading (cloud shadows too); baked shading stays."
                                                                   : "Desactivado quita el sombreado dinámico (también las sombras de nubes); el sombreado precalculado se queda.");
        if (i == GFX_PROP_LOD) ImGui::SetItemTooltip("%s", g.ui_lang ? "Intermediate mesh-LOD transitions of recognized trees, ivy and rocks only; final distances stay."
                                                                    : "Solo las transiciones intermedias de LOD de árboles, hiedra y rocas reconocidos; las distancias finales no cambian.");
    }
    ImGui::TextDisabled("%s", g.ui_lang ? "Original keeps the game's (and mods') own values. Also in F1 (live, except \"restart\")."
                                        : "Original conserva los valores del juego (y de los mods). También en F1 (en vivo, salvo \"al reiniciar\").");
    ImGui::SeparatorText(T("Velocidad"));
    ImGui::Checkbox(T("V-Sync (con 30 / 60 FPS)"), &g.vsync);
    ImGui::SetItemTooltip(T("Cada imagen se muestra exactamente una vez, sincronizada con la pantalla: movimiento\n"
                          "fluido (sin imágenes repetidas o saltadas). Con más de 60 FPS o sin límite no se aplica."));
    static const char *cut_ui[] = {"Como el resto del juego", "30 estables (como en Switch)"};
    ImGui::SetNextItemWidth(320 * g_scale);
    monado_combo(T("Cinemáticas"), &g.cutscenes, TL(cut_ui, 2).p, 2);
    ImGui::SetItemTooltip(T("En las cinemáticas el juego anima a los personajes a 30 aunque juegues a más FPS: con la\n"
                          "cámara a 60 encima se notan saltos (p. ej. el pelo de Dickson).\n"
                          "30 estables: las cinemáticas van a 30 parejos, cámara y personajes juntos (con V-Sync en un\n"
                          "monitor de 60 Hz cada imagen dura exactamente dos refrescos).\n"
                          "Con 'Generación de frames solo en cinemáticas' también van a 30, con generación encima."));
    editable=quality_settings(); monado_effect_controls(&editable,g.ui_lang!=0); quality_from_settings(editable);
    if (g.quality_preset==preset_before && !xcde_quality_equal(before,xcde_quality_current(&editable))) g.quality_preset=QUALITY_CUSTOM;
    if (ImGui::Button(g.ui_lang ? "Restore graphics" : "Restaurar gráficos")) {
        xcde_quality_apply(&editable,QUALITY_BALANCED); quality_from_settings(editable);
    }

}

static void tab_controls() {
    if (cap_dev >= 0) {
        int left = (int)((cap_until - SDL_GetTicks()) / 1000) + 1;
        if (SDL_GetTicks() > cap_until) cap_dev = -1;
        ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), T("Pulsa %s para \"%s\"... (%d s, Supr = ninguna)"),
                           cap_dev == 1 ? T("un botón del mando") : T("una tecla o un botón del ratón"), T(k_names[cap_slot]), left);
    } else {
        ImGui::TextDisabled(T("Haz clic en una asignación y pulsa la tecla, botón del ratón o del mando nuevo. Si ya estaba en uso, se intercambian."));
    }
    if (ImGui::Button(T("Restaurar teclado"))) for (int i = 0; i < NACT; i++) g.key[i] = k_def_key[i], g.key2[i] = k_def_key2[i];
    ImGui::SameLine();
    if (ImGui::Button(T("Restaurar mando"))) for (int i = 0; i < NPAD; i++) g.pad[i] = k_def_pad[i];
    ImGui::SameLine();
    static uint64_t pad_at; static std::string pad_name;
    if (!pad_at || SDL_GetTicks()-pad_at>500) {
        int npads=0; SDL_JoystickID *ids=SDL_GetGamepads(&npads);
        const char *name=npads>0 ? SDL_GetGamepadNameForID(ids[0]) : nullptr;
        pad_name=name ? name : ""; SDL_free(ids); pad_at=SDL_GetTicks();
    }
    if (!pad_name.empty()) ImGui::Text(T("   Mando conectado: %s"),pad_name.c_str());
    else ImGui::TextDisabled(T("   (ningún mando detectado)"));
    ImGui::Checkbox(T("Cambiar de personaje como en Xenoblade 3 (mantén L2 y pulsa L1 / R1: el de arriba / abajo; F2)"), &g.party_switch);
    ImGui::SetItemTooltip(T("Pasa el control al siguiente miembro del grupo, en el campo y en combate. Cada uno se queda\n"
                          "donde está y conserva sus recargas de artes; el anterior pasa a la IA. No funciona en menús ni\n"
                          "escenas, y en combate salta a los miembros caídos."));
    ImGui::Checkbox(T("Vibración del mando"), &g.rumble);  // [XCDE] the game's vibration -> SDL gamepad rumble
    ImGui::SetItemTooltip(T("Activa el motor del mando cuando el juego manda vibración (por ejemplo en combate).\n"
                            "También se puede cambiar dentro del juego, en el menú, pestaña Controles."));
    {
        static const char *ic_ui[] = {"Automático (el mando conectado; sin mando, teclado)", "Switch", "Xbox", "PlayStation",
                                      "Teclado (las teclas asignadas)"};
        ImGui::SetNextItemWidth(340 * g_scale);
        monado_combo(T("Iconos de botones"), &g.icons, TL(ic_ui, 5).p, 5);
        int ic = icons_effective(g.icons);
        if ((ic == 2 || ic == 3) && !exists((std::string("mods/") + k_icon_mod[ic - 2] + "/romfs").c_str()))  // (4: keyboard, no pack)
            ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), T("Falta el pack: descarga \"%s\" de GameBanana y pon su carpeta romfs en mods\\%s"),
                               ic == 2 ? "Xbox control layout" : "PS5 DualSense control layout", k_icon_mod[ic - 2]);
        ImGui::SetItemTooltip(T("Xbox / PlayStation: packs de iconos de CompC (GameBanana). Teclado: el juego dibuja en cada\n"
                              "icono la tecla asignada a ese botón (también ratón). Se aplica al iniciar el juego."));
    }
    {
        static const char *ml_ui[] = {"No", "Manteniendo el botón derecho", "Siempre"};
        ImGui::SetNextItemWidth(260 * g_scale);
        monado_combo(T("Cámara con el ratón"), &g.mouse_look, TL(ml_ui, 3).p, 3);
        ImGui::SetItemTooltip(T("Siempre: el ratón mueve la cámara directamente y el cursor queda oculto dentro del juego.\n"
                              "F1 (menú) o Alt+Tab lo liberan; al mover el ratón sobre el juego vuelve a capturarse."));
    }
    ImGui::BeginDisabled(!g.mouse_look);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140 * g_scale);
    ImGui::SliderInt(T("Sensibilidad"), &g.mouse_sens, 1, 100);
    ImGui::SameLine();
    ImGui::Checkbox(T("Invertir eje Y"), &g.mouse_invert);
    ImGui::EndDisabled();
    ImGui::SetNextItemWidth(140 * g_scale);
    ImGui::SliderInt(T("Zona muerta de los sticks (%)"), &g.stick_deadzone, 0, 30);
    ImGui::SetItemTooltip(T("Para un mando con drift (el personaje o la cámara se mueven solos): esa parte del recorrido\n"
                          "del stick cuenta como centrada. 0 = sin cambios (el juego ya tiene la suya)."));
    ImGui::TextDisabled(T("Artes 1-8 y talento: en combate seleccionan el arte de ese hueco de la paleta y lo usan."));
    if (ImGui::BeginTable("binds", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupColumn(T("Acción (botón de Switch)"), ImGuiTableColumnFlags_WidthFixed, 220 * g_scale);
        ImGui::TableSetupColumn(T("Tecla"), ImGuiTableColumnFlags_WidthFixed, 130 * g_scale);
        ImGui::TableSetupColumn(T("Tecla 2"), ImGuiTableColumnFlags_WidthFixed, 130 * g_scale);
        ImGui::TableSetupColumn(T("Mando"));
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int i = 0; i < NACT; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(T(k_names[i]));
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            char lbl[96];
            for (int c = 0; c < 2; c++) {
                int dev = c ? 2 : 0, k = c ? g.key2[i] : g.key[i];
                snprintf(lbl, sizeof lbl, "%s##k%d", cap_dev == dev && cap_slot == i ? "..." : key_ui_name(k), c);
                if (ImGui::Button(lbl, ImVec2(120 * g_scale, 0))) { cap_dev = dev; cap_slot = i; cap_since = SDL_GetTicks(); cap_until = cap_since + 5000; }
                ImGui::TableNextColumn();
            }
            if (i < NPAD) {
                snprintf(lbl, sizeof lbl, "%s##p", cap_dev == 1 && cap_slot == i ? "..." : T(k_pad_label[g.pad[i]]));
                if (ImGui::Button(lbl, ImVec2(-1, 0))) { cap_dev = 1; cap_slot = i; cap_since = SDL_GetTicks(); cap_until = cap_since + 5000; }
            } else if (i < NSTICK) {
                ImGui::TextDisabled(T("(stick del mando)"));
            } else if (i < NACT - 2) {
                ImGui::TextDisabled(T("(cruceta + A)"));
            } else {
                ImGui::TextDisabled(i == NACT - 2 ? "(L2 + L1)" : "(L2 + R1)");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// Mods: every folder in mods/ with a romfs/ inside (runtime/services/romfs_virtual.c applies them alphabetically,
// a later one wins for the same file); xcde.ini mod_<folder> = 0/1, on by default.
#ifndef _WIN32
// [XCDE] T-LINUX: the names in dir matching pattern (SDL_GlobDirectory, sorted as the Windows listing order is unspecified)
static std::vector<std::string> list_dir(const char *rel, const char *pattern) {
    std::vector<std::string> out;
    int n = 0;
    char **names = SDL_GlobDirectory(path(rel).c_str(), pattern, 0, &n);
    for (int i = 0; names && i < n; i++) out.push_back(names[i]);
    SDL_free(names);
    std::sort(out.begin(), out.end());
    return out;
}
#endif
static void scan_mods() {
    std::vector<Mod> found;
#ifndef _WIN32
    for (const std::string &name : list_dir("mods", "*")) {
        SDL_PathInfo pi;
        if (name[0] == '.' || !SDL_GetPathInfo(path(("mods/" + name + "/romfs").c_str()).c_str(), &pi) ||
            pi.type != SDL_PATHTYPE_DIRECTORY) continue;
        Mod m{name, true};
        for (const Mod &o : g_mods) if (o.name == m.name) m.on = o.on;
        found.push_back(m);
    }
#else
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(path("mods\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
            std::string rf = path((std::string("mods\\") + fd.cFileName + "\\romfs").c_str());
            DWORD a = GetFileAttributesA(rf.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            Mod m{fd.cFileName, true};
            for (const Mod &o : g_mods) if (o.name == m.name) m.on = o.on;
            found.push_back(m);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#endif
    std::sort(found.begin(), found.end(), [](const Mod &a, const Mod &b) { return a.name < b.name; });
    for (const Mod &o : g_mods) {  // (keep the settings of folders not present now)
        bool here = false;
        for (const Mod &m : found) here |= m.name == o.name;
        if (!here) { Mod k = o; k.present = false; found.push_back(k); }
    }
    g_mods = found;
}

// [XCDE] "Crear reporte": reports\xcde_report_<date>.zip with what a bug report needs - the last two game logs (this
// run's and the previous one, build\last_game*.log), every other build\*.log, xcde.ini / xcde_env.ini and the newest crash report (crash\, text + minidump).
// No saves or game files. Zipped by the tar.exe that Windows 10+ ships; the folder opens when it is done.
static void make_report() {
#ifndef _WIN32
    // [XCDE] T-LINUX: the same files, zipped by bsdtar (libarchive's tar: -a picks the format from .zip)
    std::vector<std::string> files;
    for (const char *f : {"build/last_game.log", "build/last_game.prev.log", "build/last_precompile.log", "build/launcher.log",
                          "build/lasrecomp.log", "build/lasrecomp.prev.log", "xcde.ini", "xcde_env.ini"})
        if (exists(f)) files.push_back(f);
    for (const std::string &n : list_dir("build", "*.log")) {
        std::string rel = "build/" + n;
        if (std::find(files.begin(), files.end(), rel) == files.end()) files.push_back(rel);
    }
    std::vector<std::string> crashes = list_dir("crash", "crash_*.txt");
    if (!crashes.empty()) files.push_back("crash/" + crashes.back());  // (crash_<stamp>.txt sorts by date)
    if (files.empty()) { g_status = T("No hay nada que reportar todavía (ni logs ni xcde.ini)"); return; }
    SDL_CreateDirectory(path("reports").c_str());
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&now));
    std::string zip = std::string("reports/xcde_report_") + stamp + ".zip";
    std::vector<std::string> args = {"bsdtar", "-a", "-c", "-f", zip};
    for (const std::string &f : files) args.push_back(f);
    pid_t pid = spawn_logged(args, "", false);
    int st = 1;
    if (!pid || waitpid(pid, &st, 0) != pid || !WIFEXITED(st) || WEXITSTATUS(st)) {
        g_status = "Could not create the report (bsdtar from libarchive is needed)";
        return;
    }
    g_status = std::string(T("Reporte creado: ")) + zip + T(" (adjúntalo al reportar el problema)");
    open_folder(path("reports"));
#else
    std::vector<std::string> files;
    for (const char *f : {"build/last_game.log", "build/last_game.prev.log", "build/last_precompile.log", "build/launcher.log",
                          "build/lasrecomp.log", "build/lasrecomp.prev.log", "xcde.ini", "xcde_env.ini"})
        if (GetFileAttributesA(path(f).c_str()) != INVALID_FILE_ATTRIBUTES) files.push_back(f);
    WIN32_FIND_DATAA fd;
    {   // [XCDE] T260: any other build\*.log (the report only had xcde.ini: the game log is build\last_game.log, not lasrecomp.log)
        HANDLE hl = FindFirstFileA(path("build/*.log").c_str(), &fd);
        if (hl != INVALID_HANDLE_VALUE) {
            do {
                std::string rel = std::string("build/") + fd.cFileName;
                bool dup = false;
                for (const std::string &f : files) if (_stricmp(f.c_str(), rel.c_str()) == 0) dup = true;
                if (!dup) files.push_back(rel);
            } while (FindNextFileA(hl, &fd));
            FindClose(hl);
        }
    }
    std::string newest;
    HANDLE h = FindFirstFileA(path("crash/crash_*.txt").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do if (newest.empty() || strcmp(fd.cFileName, newest.c_str()) > 0) newest = fd.cFileName; while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!newest.empty()) {  // (crash_<stamp>.txt sorts by date)
        files.push_back("crash/" + newest);
        std::string dmp = "crash/" + newest.substr(0, newest.size() - 4) + ".dmp";
        if (GetFileAttributesA(path(dmp.c_str()).c_str()) != INVALID_FILE_ATTRIBUTES) files.push_back(dmp);
    }
    if (files.empty()) { g_status = T("No hay nada que reportar todavía (ni logs ni xcde.ini)"); return; }
    CreateDirectoryA(path("reports").c_str(), NULL);
    char stamp[32];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&now));
    std::string zip = std::string("reports/xcde_report_") + stamp + ".zip";
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, sizeof sys);
    std::string tar = std::string(sys) + "\\tar.exe";
    std::string cmd = "\"" + tar + "\" -a -c -f \"" + zip + "\"";
    for (const std::string &f : files) cmd += " \"" + f + "\"";
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(tar.c_str(), buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, g_root.c_str(), &si, &pi)) {
        g_status = T("No se pudo crear el reporte (falta tar.exe de Windows)");
        return;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code) { g_status = T("No se pudo crear el reporte"); return; }
    g_status = std::string(T("Reporte creado: ")) + zip + T(" (adjúntalo al reportar el problema)");
    ShellExecuteA(NULL, "open", path("reports").c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
}

static void tab_mods() {
    static bool scanned;
    if (!scanned) { scanned = true; scan_mods(); }
    ImGui::TextWrapped(T("Pon cada mod en su carpeta: mods\\<nombre>\\romfs\\... (la misma estructura que en Ryujinx / yuzu). "
                       "Sus archivos reemplazan o añaden archivos del juego. Se aplican en orden alfabético: si dos mods "
                       "cambian el mismo archivo, gana el último. Los mods de código (exefs, parches .pchtxt / IPS) no "
                       "funcionan en esta versión recompilada."));
    if (ImGui::Button(T("Abrir carpeta de mods"))) {
#ifdef _WIN32
        CreateDirectoryA(path("mods").c_str(), NULL);
        ShellExecuteA(NULL, "open", path("mods").c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
        SDL_CreateDirectory(path("mods").c_str());
        open_folder(path("mods"));
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar lista"))) scan_mods();
    ImGui::Separator();
    int shown = 0;
    for (Mod &m : g_mods) {
        if (!m.present || is_icon_mod(m.name)) continue;
        if (ImGui::Checkbox(m.name.c_str(), &m.on)) g_mod_dirty=true;
        shown++;
    }
    if (!shown) ImGui::TextDisabled(T("No hay mods instalados."));
}

static void tab_audio() {
    ImGui::SeparatorText(T("Audio"));
    ImGui::SetNextItemWidth(300 * g_scale);
    ImGui::SliderInt(T("Volumen"), &g.volume, 0, 200, "%d%%");
    ImGui::Checkbox(T("Sonido de los vídeos"), &g.movie_audio);
    ImGui::SeparatorText(T("Idioma del juego"));
    ImGui::SetNextItemWidth(260 * g_scale);
    monado_combo(T("Idioma"), &g.lang, TL(k_lang_ui, 10).p, 10);
    ImGui::TextDisabled(T("Textos del juego. Solo los idiomas que incluya tu copia; si uno falla, vuelve a Inglés."));
}

// [XCDE] start-up trace (build/launcher.log) and a visible error instead of a silent exit: a PC where the launcher "did
// nothing" (no window, no message) had no way to tell why.
static FILE *g_llog;
static void llog(const char *fmt, ...) {
    if (!g_llog) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_llog, fmt, ap);
    va_end(ap);
    fputc('\n', g_llog);
    fflush(g_llog);
}
static int fail(const char *what) {
    char msg[1024];
    snprintf(msg, sizeof msg, "%s\n\n%s\n\n(build\\launcher.log)", what, SDL_GetError());
    llog("ERROR: %s: %s", what, SDL_GetError());
#ifdef _WIN32
    MessageBoxA(NULL, msg, "Monado - Launcher", MB_OK | MB_ICONERROR);
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Monado - Launcher", msg, NULL);
#endif
    return 1;
}
#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    char msg[256];
    snprintf(msg, sizeof msg, "The launcher crashed (exception %#lx at %p).\n\n(build\\launcher.log)",
             e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    llog("CRASH: exception %#lx at %p", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    MessageBoxA(NULL, msg, "Monado - Launcher", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// [XCDE] Launcher fonts: body 17 px and headings 34 px at the window's display scale, rasterized at the physical
// pixel density (HiDPI/Wayland/macOS-style scaling) and drawn back at 1/density. Rebuilt when the window changes scale.
static ImFont *g_heading_font;
static void launcher_fonts(ImGuiIO &io, float scale, float density) {
    io.Fonts->Clear();
    ImFontConfig cfg; cfg.OversampleH = 2; cfg.OversampleV = 2;
#ifdef _WIN32
    static const char *body_files[] = {"C:\\Windows\\Fonts\\segoeui.ttf"};
    static const char *head_files[] = {"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeui.ttf"};
#else
    static const char *body_files[] = {"/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                       "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                       "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"};
    static const char *head_files[] = {"/usr/share/fonts/noto/NotoSans-SemiBold.ttf", "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
                                       "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                       "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                       "/usr/share/fonts/liberation/LiberationSans-Bold.ttf"};
#endif
    auto add = [&](const char *const *files, size_t n, float px) -> ImFont * {
        for (size_t i = 0; i < n; i++) { SDL_PathInfo pi; if (SDL_GetPathInfo(files[i], &pi)) return io.Fonts->AddFontFromFileTTF(files[i], px, &cfg); }
        return nullptr;
    };
    ImFont *body = add(body_files, sizeof body_files / sizeof *body_files, 17.0f * scale * density);
    if (!body) { ImFontConfig d; d.SizePixels = 13.0f * scale * density; body = io.Fonts->AddFontDefault(&d); }
    ImFont *head = add(head_files, sizeof head_files / sizeof *head_files, 34.0f * scale * density);
    g_heading_font = head ? head : body;
    io.FontGlobalScale = 1.0f / density;
}

// [XCDE] Header banner: original vector scenery (gradient, two ridges, a ring-and-blade emblem). No image assets.
static void launcher_header(float scale) {
    ImVec2 hp = ImGui::GetCursorScreenPos();
    float hw = ImGui::GetContentRegionAvail().x, hh = 132 * scale;
    ImDrawList *art = ImGui::GetWindowDrawList();
    art->AddRectFilledMultiColor(hp, ImVec2(hp.x + hw, hp.y + hh),
        IM_COL32(25,39,52,255), IM_COL32(25,55,66,255), IM_COL32(9,19,27,255), IM_COL32(14,22,33,255));
    for (int layer = 0; layer < 2; layer++) {
        ImVec2 ridge[7] = {{hp.x+hw*.54f,hp.y+hh}, {hp.x+hw*.62f,hp.y+hh*.67f}, {hp.x+hw*.70f,hp.y+hh*.80f},
            {hp.x+hw*.77f,hp.y+hh*(.38f+layer*.14f)}, {hp.x+hw*.87f,hp.y+hh*.68f}, {hp.x+hw,hp.y+hh*.53f}, {hp.x+hw,hp.y+hh}};
        art->AddConcavePolyFilled(ridge, 7, layer ? IM_COL32(16,30,38,255) : IM_COL32(37,66,75,255));
    }
    ImVec2 ring(hp.x + hw - 100 * scale, hp.y + hh * .43f);
    art->AddCircle(ring, 29*scale, IM_COL32(219,48,76,255), 48, 9*scale);
    art->AddLine(ImVec2(ring.x+18*scale, ring.y-24*scale), ImVec2(ring.x+39*scale, ring.y-57*scale), IM_COL32(219,48,76,255), 10*scale);
    art->AddLine(ImVec2(ring.x-16*scale, ring.y+25*scale), ImVec2(ring.x-36*scale, ring.y+59*scale), IM_COL32(219,48,76,255), 6*scale);
    art->AddCircle(ring, 15*scale, IM_COL32(95,221,230,255), 48, 2*scale);
    ImGui::SetCursorScreenPos(ImVec2(hp.x + 22*scale, hp.y + 19*scale));
    ImGui::TextColored(monado_teal(), "%s", g.ui_lang ? "NATIVE PC EDITION" : "EDICIÓN NATIVA PARA PC");
    ImGui::SetCursorScreenPos(ImVec2(hp.x + 22*scale, ImGui::GetCursorScreenPos().y));
    ImGui::PushFont(g_heading_font);
    ImGui::TextUnformatted("MONADO");
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(hp.x + 22*scale, ImGui::GetCursorScreenPos().y));
    ImGui::TextDisabled("Xenoblade Chronicles: Definitive Edition");
    ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + hh + 14*scale));
}

int main(int argc, char **argv) {
    // root: the folder above the launcher's build/ folder (or the working directory)
    const char *base = SDL_GetBasePath();
    g_root = base ? base : ".";
    while (!g_root.empty() && (g_root.back() == '/' || g_root.back() == '\\')) g_root.pop_back();
#ifndef _WIN32
    if (g_root.size() > 6 && g_root.compare(g_root.size() - 6, 6, "/linux") == 0) g_root.resize(g_root.size() - 6);  // build/linux/
#endif
    if (g_root.size() > 6 && (g_root.compare(g_root.size() - 6, 6, "\\build") == 0 || g_root.compare(g_root.size() - 6, 6, "/build") == 0))
        g_root.resize(g_root.size() - 6);
#ifndef _WIN32
    // [XCDE] T-LINUX-APPIMAGE: a read-only package (AppImage) names the writable folder with the same layout instead
    const char *data_root = getenv("LARECOMP_DATA_ROOT");
    if (data_root && *data_root) g_root = data_root;
#endif
    // --shot <file.bmp> [tab] [precompile]: render a few frames of that tab, save the window as a BMP and quit (tests)
    const char *shot = argc > 2 && !strcmp(argv[1], "--shot") ? argv[2] : NULL;
    int shot_tab = argc > 3 ? atoi(argv[3]) : 0, frame_no = 0;
    bool shot_pc = shot && argc > 4 && !strcmp(argv[4], "precompile");  // (... <tab> precompile: the bar after ~5 s of a run)
#ifdef _WIN32
    g_llog = fopen((g_root + "\\build\\launcher.log").c_str(), "w");
    SetUnhandledExceptionFilter(crash_filter);
#else
    g_llog = fopen((g_root + "/build/launcher.log").c_str(), "w");
#endif
    llog("launcher start, root %s", g_root.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        llog("SDL_Init with gamepads failed (%s): video only", SDL_GetError());
        if (!SDL_Init(SDL_INIT_VIDEO)) return fail("Could not start SDL (video).");
    }
    SDL_Window *win = SDL_CreateWindow("Monado - Launcher", 1100, 820, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) return fail("Could not create the launcher window.");
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {  // (an old or missing graphics driver: the CPU renderer still shows the launcher)
        llog("GPU renderer failed (%s): software renderer", SDL_GetError());
        ren = SDL_CreateRenderer(win, "software");
    }
    if (!ren) return fail("Could not create a renderer for the launcher window (graphics driver?).");
    llog("renderer %s", SDL_GetRendererName(ren));
    SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    // Windows reports the scale as display scale with density 1; Wayland/macOS as pixel density with scale 1.
    float density = SDL_GetWindowPixelDensity(win);
    if (density <= 0) density = 1;
    float scale = shot && argc>6 ? (float)atof(argv[6]) : SDL_GetWindowDisplayScale(win) / density;
    if (scale <= 0) scale = 1;
    g_scale = scale;
    SDL_SetWindowMinimumSize(win,820,620);
    SDL_SetWindowSize(win, (int)(1100 * scale), (int)(820 * scale));
    if (shot && argc>5) { int h=std::clamp(atoi(argv[5]),540,2160); SDL_SetWindowSize(win,h*16/9,h); }
    SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (!shot) SDL_ShowWindow(win);
    launcher_fonts(io, scale, density);
    monado_launcher_theme(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    load();
    check_game_version();  // [XCDE] GitHub #18: refuse launching on another game version's files
    if (shot && argc>4 && !strcmp(argv[4],"es")) g.ui_lang=0;
    bool have_game = exists("targets/exefs/main") || exists("targets/exefs");
    bool have_exe = exists(GAME_EXE);
    bool run = true, launched = false;
    Cfg saved_cfg; memcpy(&saved_cfg,&g,sizeof g);
    int section=shot ? (shot_tab==4 ? 0 : shot_tab==5 ? 5 : shot_tab==0 ? 1 : shot_tab==1 ? 2 : shot_tab==2 ? 3 : 4) : 0;
    uint64_t last_input=SDL_GetTicks(); bool got_event=false;
    if (shot_pc) pc_start(false);
    while (run) {
        SDL_Event e;
        got_event=false;
        while (SDL_PollEvent(&e)) {
            got_event=true; last_input=SDL_GetTicks();
            if (capture_event(e)) continue;
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) run = false;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) SDL_OpenGamepad(e.gdevice.which);
        }
        if (!shot) {  // the window moved to a monitor with another scale: re-rasterize the fonts and resize the style
            float nd = SDL_GetWindowPixelDensity(win); if (nd <= 0) nd = 1;
            float ns = SDL_GetWindowDisplayScale(win) / nd; if (ns <= 0) ns = 1;
            if (fabsf(nd - density) > 0.01f || fabsf(ns - scale) > 0.01f) {
                ImGui_ImplSDLRenderer3_DestroyFontsTexture();
                launcher_fonts(io, ns, nd);
                ImGui::GetStyle().ScaleAllSizes(ns / scale);
                scale = ns; density = nd; g_scale = scale;
            }
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        const ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("main", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        launcher_header(scale);
#ifndef _WIN32
        if (g_dump_pending) {
            dump_poll();
            have_game = exists("targets/exefs/main") || exists("targets/exefs");  // (the dump may have changed)
        }
        ImGui::BeginDisabled(pc_running() || g_dump_pending);
        if (ImGui::Button(g_dump_pending ? "..." : g.ui_lang ? "Select game dump" : "Elegir volcado del juego")) {
            if (!g_dump_lock) g_dump_lock = SDL_CreateMutex();
            g_dump_pending = true;
            SDL_ShowOpenFolderDialog(dump_selected, nullptr, win, NULL, false);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("%s", g.ui_lang ? "The folder with exefs/ and romfs/ (version 2.0.0). Remembered as targets/."
                                              : "La carpeta con exefs/ y romfs/ (versión 2.0.0). Se recuerda como targets/.");
        ImGui::SameLine();
        ImGui::TextUnformatted(have_game ? (g.ui_lang ? "Game files found" : "Archivos del juego encontrados")
                                         : (g.ui_lang ? "Select your game files" : "Elige los archivos del juego"));
        ImGui::SameLine();
#endif
        {   // [XCDE] the launcher's own language (Spanish / English), saved with the settings
            static const char *ll[] = {"Español", "English"};
            const char *lbl = T("Idioma del launcher");
            float w = 150 * scale + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(lbl).x;
            ImGui::SetCursorPosX(fmaxf(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
            ImGui::SetNextItemWidth(150 * scale);
            ImGui::Combo(lbl, &g.ui_lang, ll, 2);
        }
        float bottom = ImGui::GetFrameHeightWithSpacing() * (pc_running() ? 3.3f : 2.2f) + ImGui::GetStyle().ItemSpacing.y;

        ImGui::BeginChild("navigation",ImVec2(224*scale,-bottom),0,ImGuiWindowFlags_AlwaysUseWindowPadding);
        ImGui::TextDisabled("%s", g.ui_lang ? "CONFIGURATION" : "CONFIGURACIÓN");
        ImGui::Spacing();
        const char *es[]={"Inicio","Gráficos","Controles","Audio e idioma","Mods","Diagnóstico"};
        const char *en[]={"Home","Graphics","Controls","Audio & language","Mods","Diagnostics"};
        static int shown_section = -1;
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(.50f,.13f,.19f,1));  // the selected page: the crimson accent
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(.65f,.18f,.24f,1));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(.75f,.20f,.26f,1));
        for (int i=0;i<6;i++) if (ImGui::Selectable(g.ui_lang ? en[i] : es[i],section==i,0,ImVec2(0,38*scale))) section=i;
        ImGui::PopStyleColor(3);
        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
        ImGui::TextColored(monado_teal(), "MONADO");
        ImGui::TextWrapped("%s", g.ui_lang ? "Your world. Your setup." : "Tu mundo. Tu configuración.");
        ImGui::EndChild(); ImGui::SameLine();
        if (shown_section != section) { ImGui::SetNextWindowScroll(ImVec2(0, 0)); shown_section = section; }
        ImGui::BeginChild("content",ImVec2(0,-bottom),0,ImGuiWindowFlags_AlwaysUseWindowPadding);
        ImGui::PushItemWidth(fminf(340*scale,ImGui::GetContentRegionAvail().x*0.60f));
        switch(section) {
        case 0: tab_home(); break; case 1: tab_graphics(); break; case 2: tab_controls(); break;
        case 3: tab_audio(); break; case 4: tab_mods(); break; default: tab_diagnostics(); break;
        }
        ImGui::PopItemWidth(); ImGui::EndChild();
        ImGui::Separator();
        if (!have_game) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", T("Faltan los archivos del juego en targets/ (exefs y romfs de tu copia)."));
        else if (!have_exe) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", T("Falta build/xcde.exe (compila el juego primero)."));
        else if (*g_mismatch_desc) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), g.ui_lang ?
            "The game files in targets\\exefs\\%s are from another game version (their build id is %s).\n"
            "This build only works with the game version inside this package (build id %s). Restore the\n"
            "package's original targets\\exefs (the exact same game version) and start again." :
            "Los archivos targets\\exefs\\%s son de otra versión del juego (su build id es %s).\n"
            "Este build solo funciona con la versión que trae el paquete (build id %s). Restaura los\n"
            "targets\\exefs originales del paquete (esa misma versión del juego) y vuelve a empezar.",
            g_mismatch_desc, g_mismatch_got, g_mismatch_want);
        else ImGui::TextDisabled(T("%s"), g_status.empty() ? (pc_done() ? T("Listo. Guardado: save/   Ajustes: xcde.ini")
                                                                   : T("Listo. La primera vez, JUGAR prepara los shaders antes (unos minutos)"))
                                                       : g_status.c_str());
        pc_poll();
        gc_poll();
        fp_poll();
        // [XCDE] release packages: prepare the game code once, as soon as the game files are there
        if (!shot && have_game && have_exe && !*g_mismatch_desc && !g_gc_auto && gc_available() && !gc_ready() &&
            !gc_running() && !pc_running())
            g_gc_auto = gc_start(false);
        if (gc_running()) ImGui::ProgressBar(g_gc_frac, ImVec2(-1, 0), g_gc_text.c_str());
        if (g_gc_play_now) {
            g_gc_play_now = false;
            if (!pc_done()) pc_start(true);
            else { launch(); launched = g_launched_ok; if (launched) run = false; }
        }
        if (pc_running()) {
            ImGui::ProgressBar(g_pc_frac, ImVec2(-150 * scale, 0), g_pc_text.c_str());
            ImGui::SameLine();
            if (ImGui::Button(g_pc_then_play ? T("Omitir") : T("Detener"), ImVec2(-1, 0))) {
                pc_stop();
                g_status = T("Precompilación detenida (se puede seguir otro día: lo ya hecho se conserva)");
                if (g_pc_then_play) g_pc_play_now = true;
            }
        }
        if (g_pc_play_now) {
            g_pc_play_now = false;
            launch();
            launched = g_launched_ok;
            if (launched) run = false;
        }
        ImGui::BeginDisabled(!have_game || !have_exe || launched || pc_running() || gc_running() || *g_mismatch_desc);
        ImGui::PushStyleColor(ImGuiCol_Button, monado_crimson());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f, 0.20f, 0.30f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.96f, 0.30f, 0.38f, 1));
        if (ImGui::Button(T("JUGAR"), ImVec2(200 * scale, 0))) {
            if (!gc_ready()) gc_start(true);
            else if (!pc_done()) pc_start(true);
            else { launch(); launched = g_launched_ok; if (launched) run = false; }
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(pc_running() || gc_running() || !have_exe || *g_mismatch_desc);
        if (ImGui::Button(T("Precompilar shaders"))) pc_start(false);
        ImGui::SetItemTooltip("%s", g.ui_lang ? "Prepare every shader ahead of time, so new areas don't stop to compile them. Progress is kept."
                                              : "Prepara todos los shaders por adelantado, para que las zonas nuevas no se paren a compilarlos. El progreso se conserva.");
        ImGui::EndDisabled();
        ImGui::SameLine();
        bool pending=g_mod_dirty || memcmp(&saved_cfg,&g,sizeof g)!=0;
        if (ImGui::Button(g.ui_lang ? "Apply" : "Aplicar")) {
            if (save()) { memcpy(&saved_cfg,&g,sizeof g); g_mod_dirty=false; g_status=T("Ajustes guardados en xcde.ini"); }
            else g_status=T("No se pudieron guardar los ajustes");
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Salir"))) run=false;
        if (pending) { ImGui::SameLine(); ImGui::TextColored(monado_rgb(0xE4C477),"%s",g.ui_lang ? "Unsaved" : "Sin guardar"); }
        ImGui::End();
        ImGui::Render();
        // [XCDE] HiDPI: ImGui's vertices are in window coordinates, the renderer draws in pixels. The SDL renderer backend
        // scales only the clip rectangles, so on a scaled display (Wayland / macOS: pixel density 1.5 or 2) the UI was
        // drawn into a corner at 1/density and clipped. Scale the renderer to the framebuffer every frame (resizes and
        // monitor changes included).
        const ImVec2 fb_scale = ImGui::GetDrawData()->FramebufferScale;
        SDL_SetRenderScale(ren, fb_scale.x, fb_scale.y);
        SDL_SetRenderDrawColor(ren, 9, 13, 18, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (shot && ++frame_no == (shot_pc ? 300 : 5)) {
            SDL_Surface *sf = SDL_RenderReadPixels(ren, NULL);
            if (sf) { SDL_SaveBMP(sf, shot); SDL_DestroySurface(sf); }
            run = false;
        }
        SDL_RenderPresent(ren);
        // T261: event-aware idle throttling; input immediately wakes the launcher.
        if (!shot && !pc_running() && !got_event && SDL_GetTicks()-last_input>1500) {
            SDL_Event wake; if (SDL_WaitEventTimeout(&wake,(SDL_GetWindowFlags(win)&SDL_WINDOW_MINIMIZED) ? 250 : 80)) SDL_PushEvent(&wake);
        }
    }
    if (!launched) { pc_stop(); gc_stop(); fp_stop(); }  // (closing the launcher stops a precompilation; what it wrote is kept)
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}