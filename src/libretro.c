#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libretro.h"

#include "emulator.h"
#include "emulator_state.h"
#include "types.h"
#include "nds.h"
#include "arm/arm.h"
#include "arm/thumb.h"

#ifndef VERSION
#define VERSION "0.1.0"
#endif

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static struct retro_log_callback logging;
static retro_log_printf_t log_cb;

static char* system_path;
static char* saves_path;

static char* game_path;
static char* save_path;

static int touch_x = 0;
static int touch_y = 0;

static bool show_touch_cursor;

static uint32_t clamp(uint32_t value, uint32_t min, uint32_t max)
{
  if (value < min) return min;
  if (value > max) return max;
  return value;
}

static char* concat(const char *s1, const char *s2)
{
  char *result = malloc(strlen(s1) + strlen(s2) + 1);
  strcpy(result, s1);
  strcat(result, s2);
  return result;
}

static char* normalize_path(const char* path, bool add_slash)
{
  char *new_path = malloc(strlen(path) + 1);
  strcpy(new_path, path);

  if (add_slash && new_path[strlen(new_path) - 1] != '/')
    strcat(new_path, "/");

#ifdef WINDOWS
  for (char* p = new_path; *p; p++)
    if (*p == '\\') *p = '/';
#endif

  return new_path;
}

static char* get_name_from_path(const char* path)
{
  char *base = malloc(strlen(path) + 1);
  strcpy(base, strrchr(path, '/') + 1);

  char* delims[] = { ".zip#", ".7z#", ".apk#" };
  for (int i = 0; i < 3; i++)
  {
    char* delim_pos = strstr(base, delims[i]);
    if (delim_pos) *delim_pos = '\0';
  }

  char* ext = strrchr(base, '.');
  if (ext) *ext = '\0';

  return base;
}

static void log_fallback(enum retro_log_level level, const char *fmt, ...)
{
  (void)level;
  va_list va;
  va_start(va, fmt);
  vfprintf(stderr, fmt, va);
  va_end(va);
}

static void load_save_file(GameCard* card, char* sav_filename)
{
  card->sav_filename = sav_filename;

  int fd = open(card->sav_filename, O_RDWR);
  if (fd < 0) return;

  struct stat st;
  fstat(fd, &st);

  card->sav_new = false;
  card->eeprom_size = st.st_size;

  if (card->eeprom_size == 512)
    card->addrtype = 1;
  else if (card->eeprom_size <= (1 << 16))
    card->addrtype = 2;
  else
    card->addrtype = 3;

  card->eeprom_detected = true;
  card->eeprom = mmap(NULL, card->eeprom_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  close(fd);
}

static char* fetch_variable(const char* key, const char* def)
{
  struct retro_variable var = {0};
  var.key = key;

  if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value == NULL)
  {
    log_cb(RETRO_LOG_WARN, "Fetching variable %s failed.", var.key);

    char* default_value = (char*)malloc(strlen(def) + 1);
    strcpy(default_value, def);

    return default_value;
  }

  char* value = (char*)malloc(strlen(var.value) + 1);
  strcpy(value, var.value);

  return value;
}

static bool fetch_variable_bool(const char* key, bool def)
{
  char* result = fetch_variable(key, def ? "enabled" : "disabled");
  bool is_enabled = strcmp(result, "enabled") == 0;

  free(result);
  return is_enabled;
}

static char* get_save_dir()
{
  char* dir = NULL;
  if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) || dir == NULL)
  {
    log_cb(RETRO_LOG_INFO, "No save directory provided by LibRetro.\n");
    return "agbemu";
  }
  return dir;
}

static char* get_system_dir()
{
  char* dir = NULL;
  if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || dir == NULL)
  {
    log_cb(RETRO_LOG_INFO, "No system directory provided by LibRetro.\n");
    return "ntremu";
  }
  return dir;
}

static bool get_button_state(unsigned id)
{
  return input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id);
}

static void init_input(void)
{
  static const struct retro_controller_description controllers[] = {
    { "Nintendo DS", RETRO_DEVICE_JOYPAD },
    { NULL, 0 },
  };

  static const struct retro_controller_info ports[] = {
    { controllers, 1 },
    { NULL, 0 },
  };

  environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

  struct retro_input_descriptor desc[] = {
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
    { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
    { 0 },
  };

  environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
}

static void init_config()
{
  static const struct retro_variable values[] = {
    { "ntremu_boot_bios", "Boot bios on startup; disabled|enabled" },
    { "ntremu_uncaped_speed", "Run at uncapped speed; enabled|disabled" },
    { "ntremu_touch_cursor", "Show touch cursor; disabled|enabled" },
    { NULL, NULL }
  };

  environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)values);
}

static void update_config()
{
  ntremu.bootbios = fetch_variable_bool("ntremu_boot_bios", false);
  ntremu.uncap = fetch_variable_bool("ntremu_uncaped_speed", true);
  show_touch_cursor = fetch_variable_bool("ntremu_touch_cursor", false);
}

static void check_config_variables()
{
  bool updated = false;
  environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated);

  if (updated) update_config();
}

static void draw_cursor(uint32_t *data, int32_t pointX, int32_t pointY, int32_t size)
{
  uint32_t scale = 1;

  uint32_t pos_x = clamp(pointX, size, (NDS_SCREEN_W / scale) - size);
  uint32_t pos_y = clamp(pointY, size, (NDS_SCREEN_H / scale) - size);

  uint32_t min_x = 0;
  uint32_t max_x = NDS_SCREEN_W;

  uint32_t min_y = NDS_SCREEN_H;
  uint32_t max_y = NDS_SCREEN_H * 2;

  uint32_t cur_x = (0 + (pos_x * scale));
  uint32_t cur_y = (NDS_SCREEN_H + (pos_y * scale));

  uint32_t cursor_size = (size * scale);

  uint32_t start_y = clamp(cur_y - cursor_size, min_y, max_y);
  uint32_t end_y = clamp(cur_y + cursor_size, min_y, max_y);

  uint32_t start_x = clamp(cur_x - cursor_size, min_x, max_x);
  uint32_t end_x = clamp(cur_x + cursor_size, min_x, max_x);

  for (uint32_t y = start_y; y < end_y; y++)
  {
    for (uint32_t x = start_x; x < end_x; x++)
    {
      uint32_t pixel = data[(y * max_x) + x];
      data[(y * max_x) + x] = (0xFFFFFF - pixel) | 0xFF000000;
    }
  }
}

static uint32_t convert_color(uint16_t color)
{
  uint8_t b = (color >> 10) & 0x1F;
  uint8_t g = (color >> 5) & 0x1F;
  uint8_t r = (color) & 0x1F;

  r = (r * 0xFF) / 0x1F;
  g = (g * 0xFF) / 0x1F;
  b = (b * 0xFF) / 0x1F;

  return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

void retro_get_system_info(struct retro_system_info* info)
{
  info->need_fullpath = true;
  info->valid_extensions = "nds";
  info->library_version = VERSION;
  info->library_name = "ntremu";
  info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info)
{
  info->geometry.base_width = NDS_SCREEN_W;
  info->geometry.base_height = NDS_SCREEN_H * 2;

  info->geometry.max_width = info->geometry.base_width;
  info->geometry.max_height = info->geometry.base_height;
  info->geometry.aspect_ratio = 2.0 / 3.0;

  info->timing.fps = 32.0f * 1024.0f * 1024.0f / 560190.0f;
  info->timing.sample_rate = 32.0f * 1024.0f;
}

void retro_set_environment(retro_environment_t cb)
{
  environ_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
  video_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
  audio_batch_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
}

void retro_set_input_poll(retro_input_poll_t cb)
{
  input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
  input_state_cb = cb;
}

void retro_init(void)
{
  enum retro_pixel_format xrgb888 = RETRO_PIXEL_FORMAT_XRGB8888;
  environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &xrgb888);

  if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
    log_cb = logging.log;
  else
    log_cb = log_fallback;

  system_path = normalize_path(get_system_dir(), true);
  saves_path = normalize_path(get_save_dir(), true);
}

void retro_deinit(void)
{
  log_cb = NULL;
}

bool retro_load_game(const struct retro_game_info* info)
{
  const char* name = get_name_from_path(info->path);
  const char* save = concat(name, ".sav");

  game_path = normalize_path(info->path, false);
  save_path = normalize_path(concat(saves_path, save), false);

  init_config();
  init_input();

  update_config();

  ntremu.romfile = game_path;
  ntremu.biosPath = system_path;

  const char* bios7_path = normalize_path(concat(system_path, "bios7.bin"), false);
  const char* bios9_path = normalize_path(concat(system_path, "bios9.bin"), false);
  const char* fware_path = normalize_path(concat(system_path, "firmware.bin"), false);

  int bios7fd = open(bios7_path, O_RDONLY);
  int bios9fd = open(bios9_path, O_RDONLY);
  int fwarefd = open(fware_path, O_RDWR);

  if (bios7fd < 0 || bios9fd < 0 || fwarefd < 0)
  {
    log_cb(RETRO_LOG_ERROR, "Missing bios or firmware");
    return false;
  }

  ntremu.bios7 = mmap(NULL, BIOS7SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, bios7fd, 0);
  ntremu.bios9 = mmap(NULL, BIOS9SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, bios9fd, 0);
  ntremu.firmware = mmap(NULL, FIRMWARESIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fwarefd, 0);

  close(bios7fd);
  close(bios9fd);
  close(fwarefd);

  ntremu.nds = malloc(sizeof *ntremu.nds);
  ntremu.card = create_card(ntremu.romfile);

  if (!ntremu.card)
  {
    log_cb(RETRO_LOG_ERROR, "Invalid rom file");
    return false;
  }

  ntremu.dldi_sd_fd = -1;

  arm_generate_lookup();
  thumb_generate_lookup();
  generate_adpcm_table();

  load_save_file(ntremu.card, save_path);
  emulator_reset();

  ntremu.running = true;
  ntremu.debugger = false;

  init_gpu_thread(&ntremu.nds->gpu);

  return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t info_size)
{
  return false;
}

void retro_unload_game(void)
{
  destroy_gpu_thread();

  close(ntremu.dldi_sd_fd);
  destroy_card(ntremu.card);

  free(ntremu.nds);

  munmap(ntremu.bios7, BIOS7SIZE);
  munmap(ntremu.bios9, BIOS9SIZE);
  munmap(ntremu.firmware, FIRMWARESIZE);
}

void retro_reset(void)
{
  emulator_reset();
}

void retro_run(void)
{
  check_config_variables();
  input_poll_cb();

  ntremu.nds->io7.keyinput.a = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_A);
  ntremu.nds->io7.keyinput.b = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_B);
  ntremu.nds->io7.keyinput.start = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_START);
  ntremu.nds->io7.keyinput.select = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_SELECT);
  ntremu.nds->io7.keyinput.left = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_LEFT);
  ntremu.nds->io7.keyinput.right = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_RIGHT);
  ntremu.nds->io7.keyinput.up = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_UP);
  ntremu.nds->io7.keyinput.down = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_DOWN);
  ntremu.nds->io7.keyinput.l = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_L);
  ntremu.nds->io7.keyinput.r = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_R);
  ntremu.nds->io9.keyinput = ntremu.nds->io7.keyinput;

  ntremu.nds->io7.extkeyin.x = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_X);
  ntremu.nds->io7.extkeyin.y = ~(int)get_button_state(RETRO_DEVICE_ID_JOYPAD_Y);

  bool touch = false;

  int posX = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
  int posY = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);

  int newX = (int)((posX + 0x7fff) / (float)(0x7fff * 2) * (NDS_SCREEN_W * 1));
  int newY = (int)((posY + 0x7fff) / (float)(0x7fff * 2) * (NDS_SCREEN_H * 2));

  bool inScreenX = newX >= 0 && newX <= NDS_SCREEN_W;
  bool inScreenY = newY >= NDS_SCREEN_H && newY <= (NDS_SCREEN_H * 2);

  if (inScreenX && inScreenY)
  {
    touch |= input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
    touch |= input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);

    touch_x = clamp(newX, 0, NDS_SCREEN_W);
    touch_y = clamp(newY - NDS_SCREEN_H, 0, NDS_SCREEN_H);
  }

  ntremu.nds->io7.extkeyin.pen = !touch;
  if (touch)
  {
    ntremu.nds->tsc.x = touch_x;
    ntremu.nds->tsc.y = touch_y;
  }
  else
  {
    ntremu.nds->tsc.x = -1;
    ntremu.nds->tsc.y = -1;
  }

  static uint32_t pixels[NDS_SCREEN_W * NDS_SCREEN_H * 4];
  static int16_t samples[SAMPLE_BUF_LEN];

  while (!ntremu.nds->frame_complete)
  {
    nds_run(ntremu.nds);

    if (ntremu.nds->cpuerr) break;
    if (ntremu.nds->samples_full)
    {
      for (size_t i = 0; i < SAMPLE_BUF_LEN; i++)
        samples[i] = (int16_t)(ntremu.nds->spu.sample_buf[i] * 32767.0f);

      ntremu.nds->samples_full = false;
    }
  }

  for (int i = 0; i < NDS_SCREEN_H; i++)
  {
    for (int j = 0; j < NDS_SCREEN_W; j++)
    {
      pixels[i * NDS_SCREEN_W + j] = convert_color(ntremu.nds->screen_top[i][j]);
      pixels[(NDS_SCREEN_H + i) * NDS_SCREEN_W + j] = convert_color(ntremu.nds->screen_bottom[i][j]);
    }
  }

  if (show_touch_cursor)
    draw_cursor(pixels, touch_x, touch_y, 2);

  ntremu.nds->frame_complete = false;

  video_cb(pixels, NDS_SCREEN_W, NDS_SCREEN_H * 2, NDS_SCREEN_W * 4);
  audio_batch_cb(samples, sizeof(samples) / (2 * sizeof(int16_t)));
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

size_t retro_serialize_size(void)
{
  return 0;
}

bool retro_serialize(void* data, size_t size)
{
  return false;
}

bool retro_unserialize(const void* data, size_t size)
{
  return false;
}

unsigned retro_get_region(void)
{
  return RETRO_REGION_NTSC;
}

unsigned retro_api_version()
{
  return RETRO_API_VERSION;
}

size_t retro_get_memory_size(unsigned id)
{
  if (id == RETRO_MEMORY_SYSTEM_RAM)
  {
    return RAMSIZE;
  }
  return 0;
}

void* retro_get_memory_data(unsigned id)
{
  if (id == RETRO_MEMORY_SYSTEM_RAM)
  {
    return ntremu.nds->ram;
  }
  return NULL;
}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
}

void retro_cheat_reset(void)
{
}
