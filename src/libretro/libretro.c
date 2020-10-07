#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "libretro.h"
#include "libretro_core_options.h"

#include "../Globals.h"
#include "../RTC.h"
#include "../Sound.h"

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;

static bool can_dupe;
char filename_bios[0x100] = {0};

extern bool soundEcho;
extern bool soundLowPass;
extern bool soundReverse;

extern void CPUUpdateRenderBuffers(bool);
extern int gbHardware;

struct EmulatedSystem emulator = {
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  false,
  0
};

int systemRedShift = 0;
int systemBlueShift = 0;
int systemGreenShift = 0;
int systemColorDepth = 0;
int systemDebug = 0;
int systemVerbose = 0;
int systemFrameSkip = 0;
int systemSaveUpdateCounter = SYSTEM_SAVE_NOT_UPDATED;

int sensorX = 2047;
int sensorY = 2047;

int emulating = 0;
u32 systemColorMap32[0x10000];
u16 systemColorMap16[0x10000];
char biosFileName[2048];
bool systemSoundOn = false;

struct AUDIO_INFO
{
   int rate;
   int len;
   int16_t *data;
} audio;

u8 libretro_save_buf[(128 + 8) * 1024];
int libretro_save_size = (128 + 8) * 1024;

void utilUpdateSystemColorMaps();

void *retro_get_memory_data(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return libretro_save_buf;
   return 0;
}

size_t retro_get_memory_size(unsigned id)
{
   if (id == RETRO_MEMORY_SAVE_RAM)
      return libretro_save_size;
   return 0;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{ }

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_cb = cb;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{ }

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
   libretro_set_core_options(environ_cb);
}

void retro_get_system_info(struct retro_system_info *info)
{
#ifdef GEKKO
   info->need_fullpath = true;
#else
   info->need_fullpath = false;
#endif
   info->valid_extensions = "gba";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version = "v1.8.0" GIT_VERSION;
   info->library_name = "VisualBoyAdvance";
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width = 240;
   info->geometry.base_height = 160;
   info->geometry.max_width = 240;
   info->geometry.max_height = 160;
   info->geometry.aspect_ratio = 3.0 / 2.0;
   info->timing.fps = 16777216.0 / 280896.0;
   info->timing.sample_rate = (double)audio.rate;
}

static void check_system_specs(void)
{
   unsigned level = 10;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
   struct retro_log_callback log;
   environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   const char* dir = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) {
      strncpy(filename_bios, dir, sizeof(filename_bios));
      strncat(filename_bios, "/gba_bios.bin", sizeof(filename_bios));
   }

#ifdef FRONTEND_SUPPORTS_RGB565
   enum retro_pixel_format rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
      log_cb(RETRO_LOG_DEBUG, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
#endif

   check_system_specs();

   memset(libretro_save_buf, 0xFF, libretro_save_size);
}

static unsigned serialize_size = 0;

typedef struct  {
	char romtitle[256];
	char romid[5];
	int flashSize;
	int saveType;
	int rtcEnabled;
	int mirroringEnabled;
	int useBios;
} ini_t;

static const ini_t gbaover[256] = {
			//romtitle,							    	romid	flash	save	rtc	mirror	bios
			{"2 Games in 1 - Dragon Ball Z - The Legacy of Goku I & II (USA)",	"BLFE",	0,	1,	0,	0,	0},
			{"2 Games in 1 - Dragon Ball Z - Buu's Fury + Dragon Ball GT - Transformation (USA)", "BUFE", 0, 1, 0, 0, 0},
			{"Boktai - The Sun Is in Your Hand (Europe)(En,Fr,De,Es,It)",		"U3IP",	0,	0,	1,	0,	0},
			{"Boktai - The Sun Is in Your Hand (USA)",				"U3IE",	0,	0,	1,	0,	0},
			{"Boktai 2 - Solar Boy Django (USA)",					"U32E",	0,	0,	1,	0,	0},
			{"Boktai 2 - Solar Boy Django (Europe)(En,Fr,De,Es,It)",		"U32P",	0,	0,	1,	0,	0},
			{"Bokura no Taiyou - Taiyou Action RPG (Japan)",			"U3IJ",	0,	0,	1,	0,	0},
			{"Card e-Reader+ (Japan)",						"PSAJ",	131072,	0,	0,	0,	0},
			{"Classic NES Series - Bomberman (USA, Europe)",			"FBME",	0,	1,	0,	1,	0},
			{"Classic NES Series - Castlevania (USA, Europe)",			"FADE",	0,	1,	0,	1,	0},
			{"Classic NES Series - Donkey Kong (USA, Europe)",			"FDKE",	0,	1,	0,	1,	0},
			{"Classic NES Series - Dr. Mario (USA, Europe)",			"FDME",	0,	1,	0,	1,	0},
			{"Classic NES Series - Excitebike (USA, Europe)",			"FEBE",	0,	1,	0,	1,	0},
			{"Classic NES Series - Legend of Zelda (USA, Europe)",			"FZLE",	0,	1,	0,	1,	0},
			{"Classic NES Series - Ice Climber (USA, Europe)",			"FICE",	0,	1,	0,	1,	0},
			{"Classic NES Series - Metroid (USA, Europe)",				"FMRE",	0,	1,	0,	1,	0},
			{"Classic NES Series - Pac-Man (USA, Europe)",				"FP7E",	0,	1,	0,	1,	0},
			{"Classic NES Series - Super Mario Bros. (USA, Europe)",		"FSME",	0,	1,	0,	1,	0},
			{"Classic NES Series - Xevious (USA, Europe)",				"FXVE",	0,	1,	0,	1,	0},
			{"Classic NES Series - Zelda II - The Adventure of Link (USA, Europe)",	"FLBE",	0,	1,	0,	1,	0},
			{"Digi Communication 2 - Datou! Black Gemagema Dan (Japan)",		"BDKJ",	0,	1,	0,	0,	0},
			{"e-Reader (USA)",							"PSAE",	131072,	0,	0,	0,	0},
			{"Dragon Ball GT - Transformation (USA)",				"BT4E",	0,	1,	0,	0,	0},
			{"Dragon Ball Z - Buu's Fury (USA)",					"BG3E",	0,	1,	0,	0,	0},
			{"Dragon Ball Z - Taiketsu (Europe)(En,Fr,De,Es,It)",			"BDBP",	0,	1,	0,	0,	0},
			{"Dragon Ball Z - Taiketsu (USA)",					"BDBE",	0,	1,	0,	0,	0},
			{"Dragon Ball Z - The Legacy of Goku II International (Japan)",		"ALFJ",	0,	1,	0,	0,	0},
			{"Dragon Ball Z - The Legacy of Goku II (Europe)(En,Fr,De,Es,It)",	"ALFP", 0,	1,	0,	0,	0},
			{"Dragon Ball Z - The Legacy of Goku II (USA)",				"ALFE",	0,	1,	0,	0,	0},
			{"Dragon Ball Z - The Legacy Of Goku (Europe)(En,Fr,De,Es,It)",		"ALGP",	0,	1,	0,	0,	0},
			{"Dragon Ball Z - The Legacy of Goku (USA)",				"ALGE",	131072,	1,	0,	0,	0},
			{"F-Zero - Climax (Japan)",						"BFTJ",	131072,	0,	0,	0,	0},
			{"Famicom Mini Vol. 01 - Super Mario Bros. (Japan)",			"FMBJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 12 - Clu Clu Land (Japan)",				"FCLJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 13 - Balloon Fight (Japan)",			"FBFJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 14 - Wrecking Crew (Japan)",			"FWCJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 15 - Dr. Mario (Japan)",				"FDMJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 16 - Dig Dug (Japan)",				"FTBJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 17 - Takahashi Meijin no Boukenjima (Japan)",	"FTBJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 18 - Makaimura (Japan)",				"FMKJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 19 - Twin Bee (Japan)",				"FTWJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 20 - Ganbare Goemon! Karakuri Douchuu (Japan)",	"FGGJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 21 - Super Mario Bros. 2 (Japan)",			"FM2J",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 22 - Nazo no Murasame Jou (Japan)",			"FNMJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 23 - Metroid (Japan)",				"FMRJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 24 - Hikari Shinwa - Palthena no Kagami (Japan)",	"FPTJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 25 - The Legend of Zelda 2 - Link no Bouken (Japan)","FLBJ",0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 26 - Famicom Mukashi Banashi - Shin Onigashima - Zen Kou Hen (Japan)","FFMJ",0,1,0,	1,	0},
			{"Famicom Mini Vol. 27 - Famicom Tantei Club - Kieta Koukeisha - Zen Kou Hen (Japan)","FTKJ",0,1,0,	1,	0},
			{"Famicom Mini Vol. 28 - Famicom Tantei Club Part II - Ushiro ni Tatsu Shoujo - Zen Kou Hen (Japan)","FTUJ",0,1,0,1,0},
			{"Famicom Mini Vol. 29 - Akumajou Dracula (Japan)",			"FADJ",	0,	1,	0,	1,	0},
			{"Famicom Mini Vol. 30 - SD Gundam World - Gachapon Senshi Scramble Wars (Japan)","FSDJ",0,1,	0,	1,	0},
			{"Game Boy Wars Advance 1+2 (Japan)",					"BGWJ",	131072,	0,	0,	0,	0},
			{"Golden Sun - The Lost Age (USA)",					"AGFE",	65536,	0,	0,	1,	0},
			{"Golden Sun (USA)",							"AGSE",	65536,	0,	0,	1,	0},
			{"Iridion II (Europe) (En,Fr,De)",							"AI2P",	0,	5,	0,	0,	0},
			{"Iridion II (USA)",							"AI2E",	0,	5,	0,	0,	0},
			{"Koro Koro Puzzle - Happy Panechu! (Japan)",				"KHPJ",	0,	4,	0,	0,	0},
			{"Mario vs. Donkey Kong (Europe)",					"BM5P",	0,	3,	0,	0,	0},
			{"Pocket Monsters - Emerald (Japan)",					"BPEJ",	131072,	0,	1,	0,	0},
			{"Pocket Monsters - Fire Red (Japan)",					"BPRJ",	131072,	0,	0,	0,	0},
			{"Pocket Monsters - Leaf Green (Japan)",				"BPGJ",	131072,	0,	0,	0,	0},
			{"Pocket Monsters - Ruby (Japan)",					"AXVJ",	131072,	0,	1,	0,	0},
			{"Pocket Monsters - Sapphire (Japan)",					"AXPJ",	131072,	0,	1,	0,	0},
			{"Pokemon Mystery Dungeon - Red Rescue Team (USA, Australia)",		"B24E",	131072,	0,	0,	0,	0},
			{"Pokemon Mystery Dungeon - Red Rescue Team (En,Fr,De,Es,It)",		"B24P",	131072,	0,	0,	0,	0},
			{"Pokemon - Blattgruene Edition (Germany)",				"BPGD",	131072,	0,	0,	0,	0},
			{"Pokemon - Edicion Rubi (Spain)",					"AXVS",	131072,	0,	1,	0,	0},
			{"Pokemon - Edicion Esmeralda (Spain)",					"BPES",	131072,	0,	1,	0,	0},
			{"Pokemon - Edicion Rojo Fuego (Spain)",				"BPRS",	131072,	1,	0,	0,	0},
			{"Pokemon - Edicion Verde Hoja (Spain)",				"BPGS",	131072,	1,	0,	0,	0},
			{"Pokemon - Eidicion Zafiro (Spain)",					"AXPS",	131072,	0,	1,	0,	0},
			{"Pokemon - Emerald Version (USA, Europe)",				"BPEE",	131072,	0,	1,	0,	0},
			{"Pokemon - Feuerrote Edition (Germany)",				"BPRD",	131072,	0,	0,	0,	0},
			{"Pokemon - Fire Red Version (USA, Europe)",				"BPRE",	131072,	0,	0,	0,	0},
			{"Pokemon - Leaf Green Version (USA, Europe)",				"BPGE",	131072,	0,	0,	0,	0},
			{"Pokemon - Rubin Edition (Germany)",					"AXVD",	131072,	0,	1,	0,	0},
			{"Pokemon - Ruby Version (USA, Europe)",				"AXVE",	131072,	0,	1,	0,	0},
			{"Pokemon - Sapphire Version (USA, Europe)",				"AXPE",	131072,	0,	1,	0,	0},
			{"Pokemon - Saphir Edition (Germany)",					"AXPD",	131072,	0,	1,	0,	0},
			{"Pokemon - Smaragd Edition (Germany)",					"BPED",	131072,	0,	1,	0,	0},
			{"Pokemon - Version Emeraude (France)",					"BPEF",	131072,	0,	1,	0,	0},
			{"Pokemon - Version Rouge Feu (France)",				"BPRF",	131072,	0,	0,	0,	0},
			{"Pokemon - Version Rubis (France)",					"AXVF",	131072,	0,	1,	0,	0},
			{"Pokemon - Version Saphir (France)",					"AXPF",	131072,	0,	1,	0,	0},
			{"Pokemon - Version Vert Feuille (France)",				"BPGF",	131072,	0,	0,	0,	0},
			{"Pokemon - Versione Rubino (Italy)",					"AXVI",	131072,	0,	1,	0,	0},
			{"Pokemon - Versione Rosso Fuoco (Italy)",				"BPRI",	131072,	0,	0,	0,	0},
			{"Pokemon - Versione Smeraldo (Italy)",					"BPEI",	131072,	0,	1,	0,	0},
			{"Pokemon - Versione Verde Foglia (Italy)",				"BPGI",	131072,	0,	0,	0,	0},
			{"Pokemon - Versione Zaffiro (Italy)",					"AXPI",	131072,	0,	1,	0,	0},
			{"Rockman EXE 4.5 - Real Operation (Japan)",				"BR4J",	0,	0,	1,	0,	0},
			{"Rocky (Europe)(En,Fr,De,Es,It)",					"AROP",	0,	1,	0,	0,	0},
			{"Rocky (USA)(En,Fr,De,Es,It)",						"AR8e",	0,	1,	0,	0,	0},
			{"Sennen Kazoku (Japan)",						"BKAJ",	131072,	0,	1,	0,	0},
			{"Shin Bokura no Taiyou - Gyakushuu no Sabata (Japan)",			"U33J",	0,	1,	1,	0,	0},
			{"Super Mario Advance 4 (Japan)",					"AX4J",	131072,	0,	0,	0,	0},
			{"Super Mario Advance 4 - Super Mario Bros. 3 (Europe)(En,Fr,De,Es,It)","AX4P",	131072,	0,	0,	0,	0},
			{"Super Mario Advance 4 - Super Mario Bros 3 - Super Mario Advance 4 v1.1 (USA)","AX4E",131072,0,0,0,0},
			{"Top Gun - Combat Zones (USA)(En,Fr,De,Es,It)",			"A2YE",	0,	5,	0,	0,	0},
			{"Yoshi's Universal Gravitation (Europe)(En,Fr,De,Es,It)",		"KYGP",	0,	4,	0,	0,	0},
			{"Yoshi no Banyuuinryoku (Japan)",					"KYGJ",	0,	4,	0,	0,	0},
			{"Yoshi - Topsy-Turvy (USA)",						"KYGE",	0,	1,	0,	0,	0},
			{"Yu-Gi-Oh! GX - Duel Academy (USA)",					"BYGE",	0,	2,	0,	0,	1},
			{"Yu-Gi-Oh! - Ultimate Masters - 2006 (Europe)(En,Jp,Fr,De,Es,It)",	"BY6P",	0,	2,	0,	0,	0},
			{"Zoku Bokura no Taiyou - Taiyou Shounen Django (Japan)",		"U32J",	0,	0,	1,	0,	0}
};

static int enableRtc = false;

static void load_image_preferences (void)
{
   char buffer[5];

	buffer[0] = rom[0xac];
	buffer[1] = rom[0xad];
	buffer[2] = rom[0xae];
	buffer[3] = rom[0xaf];
	buffer[4] = 0;

   if (log_cb)
      log_cb(RETRO_LOG_DEBUG, "GameID in ROM is: %s\n", buffer);

	bool found = false;
	int found_no = 0;

	for(int i = 0; i < 256; i++)
	{
		if(!strcmp(gbaover[i].romid, buffer))
		{
			found = true;
			found_no = i;
         break;
		}
	}

	if(found)
	{
      if (log_cb)
         log_cb(RETRO_LOG_DEBUG, "Found ROM in vba-over list.\n");

		enableRtc = gbaover[found_no].rtcEnabled;

		if(gbaover[found_no].flashSize != 0)
			flashSize = gbaover[found_no].flashSize;
		else
			flashSize = 65536;

		cpuSaveType = gbaover[found_no].saveType;

		mirroringEnable = gbaover[found_no].mirroringEnabled;
	}

   if (log_cb)
   {
      log_cb(RETRO_LOG_DEBUG, "RTC = %d.\n", enableRtc);
      log_cb(RETRO_LOG_DEBUG, "flashSize = %d.\n", flashSize);
      log_cb(RETRO_LOG_DEBUG, "cpuSaveType = %d.\n", cpuSaveType);
      log_cb(RETRO_LOG_DEBUG, "mirroringEnable = %d.\n", mirroringEnable);
   }
}

static void gba_init(void)
{
   cpuSaveType = 0;
   flashSize = 0x10000;
   enableRtc = false;
   mirroringEnable = false;

   load_image_preferences();

   if(flashSize == 0x10000 || flashSize == 0x20000)
      flashSetSize(flashSize);

   if(enableRtc)
      rtcEnable(enableRtc);

   doMirroring(mirroringEnable);

   soundSetQuality(1);

   bool usebios = false;

   struct retro_variable var;

   var.key = "vba_bios";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
     if (strcmp(var.value, "disabled") == 0)
        usebios = false;
     else if (strcmp(var.value, "enabled") == 0)
        usebios = true;
   }

	if(usebios && filename_bios[0])
		CPUInit(filename_bios, true);
	else
      CPUInit(NULL, false);

   CPUReset();

   soundReset();

   //uint8_t * state_buf = (uint8_t*)malloc(2000000);
   //serialize_size = CPUWriteState(state_buf, 2000000);
   //free(state_buf);
   emulating = 1;
}

void retro_deinit(void)
{
	CPUCleanUp();
}

void retro_reset(void)
{
   CPUReset();
}

#define MAX_BUTTONS 10
#define TURBO_BUTTONS 2
static bool option_turboEnable;
static u32 option_turboDelay;
static u32 turbo_delay_counter;

static const unsigned binds[MAX_BUTTONS] = {
	RETRO_DEVICE_ID_JOYPAD_A,
	RETRO_DEVICE_ID_JOYPAD_B,
	RETRO_DEVICE_ID_JOYPAD_SELECT,
	RETRO_DEVICE_ID_JOYPAD_START,
	RETRO_DEVICE_ID_JOYPAD_RIGHT,
	RETRO_DEVICE_ID_JOYPAD_LEFT,
	RETRO_DEVICE_ID_JOYPAD_UP,
	RETRO_DEVICE_ID_JOYPAD_DOWN,
	RETRO_DEVICE_ID_JOYPAD_R,
	RETRO_DEVICE_ID_JOYPAD_L
};

static const unsigned turbo_binds[TURBO_BUTTONS] = {
    RETRO_DEVICE_ID_JOYPAD_X,
    RETRO_DEVICE_ID_JOYPAD_Y
};

static unsigned has_frame;

static void update_variables(void)
{
   struct retro_variable var = { 0 };
   var.key = "vba_turboenable";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        option_turboEnable = (!strcmp(var.value, "enabled")) ? true : false;
    }

    var.key = "vba_turbodelay";

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
        option_turboDelay = atoi(var.value);
    }
}

static u32 J = 0;

static void update_input(void)
{
   // Reset input states
   J = 0;

   /* if (retropad_device[0] == RETRO_DEVICE_JOYPAD) */ {
      for (unsigned button = 0; button < MAX_BUTTONS; button++)
         J |= input_cb(0, RETRO_DEVICE_JOYPAD, 0, binds[button]) << button;

      if (option_turboEnable) {
         /* Handle Turbo A & B buttons */
         bool button_pressed = false;
         for (unsigned tbutton = 0; tbutton < TURBO_BUTTONS; tbutton++) {
            if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, turbo_binds[tbutton])) {
               button_pressed = true;
               if (!turbo_delay_counter)
                  J |= 1 << tbutton;
            }
         }
         if (button_pressed) {
            turbo_delay_counter++;
            if (turbo_delay_counter > option_turboDelay)
               /* Reset the toggle if delay value is reached */
               turbo_delay_counter = 0;
         }
         else
            /* If the button is not pressed, just reset the toggle */
            turbo_delay_counter = 0;
      }
      // Do not allow opposing directions
      if ((J & 0x30) == 0x30)
         J &= ~(0x30);
      else if ((J & 0xC0) == 0xC0)
         J &= ~(0xC0);
   }
}

void retro_run(void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables();

   poll_cb();
   update_input();

   has_frame = 0;
   while (!has_frame)
      CPULoop(300000);
}

size_t retro_serialize_size(void)
{
   return serialize_size;
}

bool retro_serialize(void *data, size_t size)
{
   return 0;
   //return CPUWriteState((uint8_t*)data, size);
}

bool retro_unserialize(const void *data, size_t size)
{
   return 0;
   //return CPUReadState((uint8_t*)data, size);
}

void retro_cheat_reset(void)
{
   cheatsDeleteAll(false);
}

#define ISHEXDEC \
   ((code[cursor] >= '0') && (code[cursor] <= '9')) || \
   ((code[cursor] >= 'a') && (code[cursor] <= 'f')) || \
   ((code[cursor] >= 'A') && (code[cursor] <= 'F')) \

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   char name[128];
   unsigned cursor;
   char *codeLine = NULL ;
   int codeLineSize = strlen(code)+5 ;
   int codePos = 0 ;
   int i ;

   codeLine = (char*)calloc(codeLineSize,sizeof(char)) ;

   sprintf(name, "cheat_%d", index);

   //Break the code into Parts
   for (cursor=0;;cursor++)
   {
      if (ISHEXDEC)
         codeLine[codePos++] = toupper(code[cursor]) ;
      else
      {
         if ( codePos >= 12 )
         {
            if ( codePos == 12 )
            {
               for ( i = 0 ; i < 4 ; i++ )
                  codeLine[codePos-i] = codeLine[(codePos-i)-1] ;
               codeLine[8] = ' ' ;
               codeLine[13] = '\0' ;
               cheatsAddCBACode(codeLine, name);
               log_cb(RETRO_LOG_DEBUG, "Cheat code added: '%s'\n", codeLine);
            } else if ( codePos == 16 )
            {
               codeLine[16] = '\0' ;
               cheatsAddGSACode(codeLine, name, true);
               log_cb(RETRO_LOG_DEBUG, "Cheat code added: '%s'\n", codeLine);
            } else
            {
               codeLine[codePos] = '\0' ;
               log_cb(RETRO_LOG_ERROR, "Invalid cheat code '%s'\n", codeLine);
            }
            codePos = 0 ;
            memset(codeLine,0,codeLineSize) ;
         }
      }
      if (!code[cursor])
         break;
   }


   free(codeLine) ;
}

static u32 rom_size = 0;

static void set_memory_maps(void)
{
   struct retro_memory_descriptor descs[] = {
      // flags, ptr, offset, start, select, disconnect, len, address space
      { 0, bios,              0, 0x00000000, 0,          0, 0x4000,     "BIOS" },
      { 0, workRAM,           0, 0x02000000, 0,          0, 0x40000,    "EWRAM" },
      { 0, internalRAM,       0, 0x03000000, 0,          0, 0x8000,     "IWRAM" },
      { 0, ioMem,             0, 0x04000000, 0,          0, 0x400,      "IOMEM" },
      { 0, paletteRAM,        0, 0x05000000, 0,          0, 0x400,      "PALRAM" },
      { 0, vram,              0, 0x06000000, 0xFFFE8000, 0, 0x18000,    "VRAM" },
      { 0, oam,               0, 0x07000000, 0,          0, 0x400,      "OAM" },
      { 0, rom,               0, 0x08000000, 0,          0, rom_size,   "ROM-WS0" },
      { 0, rom,               0, 0x0A000000, 0,          0, rom_size,   "ROM-WS1" },
      { 0, rom,               0, 0x0C000000, 0,          0, rom_size,   "ROM-WS2" },
      // normally, only 64K is accessible at-a-time, 128K flash are bankswitched
      { 0, libretro_save_buf, 0, 0x0E000000, 0,          0, 0x10000,    "SRAM" }
      // NOTE: the eeprom can be accessed anywhere from D000000h-DFFFFFFh. The need to map
      // eeprom pointer to a virtual address might be needed for direct and fixed access when time comes
      // For VBA Next as well as Beetle GBA, eeprom ptr can be accessed from libretro_save_buf[128 * 1024]
   };

   struct retro_memory_map mmaps = {
      descs,
      sizeof(descs) / sizeof(descs[0])
   };

   bool yes = true;
   environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmaps);
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &yes);
}

bool retro_load_game(const struct retro_game_info *game)
{
   update_variables();

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Turbo A" },
      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

	rom_size = CPULoadRom(game->path);

   if (!rom_size)
   {
      printf("Failed to load rom, %s", game->path);
      return false;
   }

   systemRedShift = 11;
   systemGreenShift = 6;
   systemBlueShift = 0;
   systemColorDepth = 16;
   utilUpdateSystemColorMaps();

   gba_init();
   set_memory_maps();

   return true;
}

bool retro_load_game_special(
  unsigned game_type,
  const struct retro_game_info *info, size_t num_info
)
{ return false; }

static unsigned g_audio_frames;
static unsigned g_video_frames;

void retro_unload_game(void)
{
   if (log_cb)
      log_cb(RETRO_LOG_DEBUG, "[VBA] Sync stats: Audio frames: %u, Video frames: %u, AF/VF: %.2f\n",
            g_audio_frames, g_video_frames, (float)g_audio_frames / g_video_frames);
   g_audio_frames = 0;
   g_video_frames = 0;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

void systemWriteDataToSoundBuffer()
{
   audio_batch_cb((const int16_t*)audio.data, audio.len);
   g_audio_frames += audio.len;
}

void systemSoundReset(void)
{
}

bool systemSoundInit(void)
{
   int rates[] = { 44100, 22050, 11025, 44100 };

   audio.rate = rates[(soundQuality >> 1) & 3];
   audio.len = (int)((double)audio.rate / (16777216.0 / 280896.0));
   audio.data = (int16_t*)soundFinalWave;

   soundBufferLen = (audio.len << 2);
   systemSoundOn = true;

   return true;
}

bool systemCanChangeSoundQuality(void)
{
   return true;
}

void systemSoundShutdown(void)
{
}

void systemDrawScreen(void)
{
   video_cb(pix, 240, 160, 484); //last arg is pitch
   g_video_frames++;
   has_frame = 1;
}

void systemMessage(int num, const char* fmt, ...)
{
   if (!log_cb)
      return;

   char buffer[256];
   va_list ap;
   va_start(ap, fmt);
   vsprintf(buffer, fmt, ap);
   log_cb(RETRO_LOG_INFO, "%s\n", buffer);
   va_end(ap);
}

void logg(const char* fmt, ...)
{
   char buffer[256];
   va_list ap;

   if (!log_cb)
      return;

   va_start(ap, fmt);
   vsprintf(buffer, fmt, ap);
   log_cb(RETRO_LOG_INFO, "%s\n", buffer);
   va_end(ap);
}

void systemFrame()
{
}

bool systemReadJoypads()
{
   return true;
}

u32 systemReadJoypad(int which)
{
   return J;
}

int  systemGetSensorX()
{
   return 0;
}

int  systemGetSensorY()
{
   return 0;
}

void systemUpdateMotionSensor()
{
}

void utilPutDword(u8 *a, u32 b)
{
}

void utilPutWord(u8 *a, u16 b)
{
}

bool utilIsGBAImage(const char * file)
{
   return 1;
}

static int utilGetSize(int size)
{
  int res = 1;
  while(res < size)
    res <<= 1;
  return res;
}

u8 *utilLoad(const char *file,
             bool (*accept)(const char *),
             u8 *data,
             int *size)
{
   u8 *image = data;

   FILE *f = fopen(file, "rb");

   if(!f) {
      systemMessage(0, "Error opening image %s", file);
      return NULL;
   }

   fseek(f,0,SEEK_END);
   int fileSize = ftell(f);
   fseek(f,0,SEEK_SET);
   if(*size == 0)
      *size = fileSize;

   if(image == NULL) {
      image = (u8 *)malloc(utilGetSize(*size));
      if(image == NULL) {
         systemMessage(0, "Failed to allocate memory for %s",
                    "data");
         fclose(f);
         return NULL;
      }
      *size = fileSize;
   }
   size_t read = fileSize <= *size ? fileSize : *size;
   size_t r = fread(image, 1, read, f);
   fclose(f);

   if(r != read) {
      systemMessage(0,
                  "Error reading image %s", file);
      if(data == NULL)
         free(image);
      return NULL;
   }

   *size = fileSize;

   return image;
}

u32 systemGetClock(void)
{
  return 0;
}

bool systemPauseOnFrame(void)
{
   return 0;
}

void system10Frames(int num)
{
}

void systemShowSpeed(int num)
{
}

void systemScreenCapture(int num)
{
}

bool utilWritePNGFile(const char *fileName, int w, int h, u8 *pix)
{
   return 0;
}

bool utilWriteBMPFile(const char *fileName, int w, int h, u8 *pix)
{
   return 0;
}

void agbPrintFlush()
{}

bool agbPrintWrite(u32 address, u16 value)
{
   return 0;
}

void agbPrintEnable(bool enable)
{
}

void systemSoundPause()
{
}

void systemSoundResume()
{
}

// memstream helpers

typedef struct {
  void *address;
  int size;
} variable_desc;

void utilWriteInt(gzFile gzFile, int i)
{
//  utilGzWrite(gzFile, &i, sizeof(int));
}

int utilReadInt(gzFile gzFile)
{
  int i = 0;
//  utilGzRead(gzFile, &i, sizeof(int));
  return i;
}

void utilReadData(gzFile gzFile, variable_desc* data)
{
//  while(data->address) {
//    utilGzRead(gzFile, data->address, data->size);
//    data++;
//  }
}

void utilWriteData(gzFile gzFile, variable_desc *data)
{
//  while(data->address) {
//    utilGzWrite(gzFile, data->address, data->size);
//    data++;
//  }
}

int utilGzWrite(gzFile file, const voidp buffer, unsigned int len)
{
   return 0;
  //return utilGzWriteFunc(file, buffer, len);
}

int utilGzRead(gzFile file, voidp buffer, unsigned int len)
{
   return 0;
  //return utilGzReadFunc(file, buffer, len);
}

void utilUpdateSystemColorMaps()
{
   switch(systemColorDepth) {
   case 16:
      for(int i = 0; i < 0x10000; i++) {
         systemColorMap16[i] = ((i & 0x1f) << systemRedShift) |
            (((i & 0x3e0) >> 5) << systemGreenShift) |
            (((i & 0x7c00) >> 10) << systemBlueShift);
      }
      break;
   case 24:
   case 32:
      for(int i = 0; i < 0x10000; i++) {
         systemColorMap32[i] = ((i & 0x1f) << systemRedShift) |
            (((i & 0x3e0) >> 5) << systemGreenShift) |
            (((i & 0x7c00) >> 10) << systemBlueShift);
         }
      break;
   }
}
