/*
    AlceOSD - Graphical OSD
    Copyright (C) 2015  Luis Alves

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "alce-osd.h"


/* settings are stored in the program flash
 * a simple algorithm to avoid flash wear is implemented */

/* WARNING: MESSING WITH THIS CODE MIGHT BRICK YOUR ALCEOSD */
/* RECOVERY IS ONLY POSSIBLE USING A PICKIT3 TO RE-FLASH THE BOOTLOADER */

#define CONFIG_ADDR_START   (0x1000L)
#define CONFIG_ADDR_END     (0x3fffL)
#define CONFIG_ADDR_PAGE    (0x800)
#define CONFIG_PAGE_SIZE    (0x400)

#define CONFIG_VERSION_SIG  (0xffffff-9)

//#define DEBUG_CONFIG

static unsigned long valid_config_addr = 0;

/* default configuration */
struct alceosd_config config = {
    .uart = {
        { .mode = UART_CLIENT_MAVLINK, .baudrate = UART_BAUD_115200, .pins = UART_PINS_TELEMETRY },
        { .mode = UART_CLIENT_MAVLINK, .baudrate = UART_BAUD_115200, .pins = UART_PINS_CON2 },
        { .mode = UART_CLIENT_NONE,    .baudrate = UART_BAUD_115200, .pins = UART_PINS_OFF },
        { .mode = UART_CLIENT_NONE,    .baudrate = UART_BAUD_115200, .pins = UART_PINS_OFF },
    },
  
    .video = {
        {
            .mode = VIDEO_STANDARD_PAL_P | VIDEO_MODE_SYNC_MASK,
            .brightness = 200,

            .white_lvl = 0x3ff >> 4,
            .gray_lvl = 0x2d0 >> 4,
            .black_lvl = 0x190 >> 4,

            .x_offset = 40,
            .y_offset = 40,

            .x_size_id = VIDEO_XSIZE_480,
            .y_size = 260,
        },
        {
            .mode = VIDEO_STANDARD_PAL_I | VIDEO_MODE_SYNC_MASK,
            .brightness = 200,

            .white_lvl = 0x3ff >> 4,
            .gray_lvl = 0x2d0 >> 4,
            .black_lvl = 0x190 >> 4,

            .x_offset = 40,
            .y_offset = 40,

            .x_size_id = VIDEO_XSIZE_672,
            .y_size = 260,
        },
    },
    .tab_change.tab_change_ch_min = 1000,
    .tab_change.tab_change_ch_max = 2000,
    .tab_change.ch = 7,
    .tab_change.mode = TAB_CHANGE_TOGGLE,
    .tab_change.time_window = 20,

    .mav.streams = {3, 1, 4, 1, 1, 10, 10, 1},
    .mav.osd_sysid = 200,
    .mav.uav_sysid = 1,
    
    .default_units = UNITS_METRIC,
    .home_lock_sec = 15,

    .widgets = {
        //{ 5, 0, WIDGET_CONSOLE_ID,         0,   0, {JUST_VCENTER | JUST_HCENTER}},
        //{ 5, 0, WIDGET_GIMBAL_ID,          0,   0, {JUST_TOP | JUST_RIGHT}},

        { 1, 0, WIDGET_ALTITUDE_ID,        0,   0, {JUST_VCENTER | JUST_RIGHT}},
        { 1, 0, WIDGET_BATTERY_INFO_ID,    0,   0, {JUST_TOP     | JUST_LEFT}},
        { 1, 0, WIDGET_COMPASS_ID,         0,   0, {JUST_BOT     | JUST_HCENTER}},
        { 1, 0, WIDGET_FLIGHT_MODE_ID,     0, -32, {JUST_BOT     | JUST_LEFT}},
        { 1, 0, WIDGET_GPS_INFO_ID,        0,   0, {JUST_BOT     | JUST_LEFT}},
        { 1, 0, WIDGET_HORIZON_ID,        16,   0, {JUST_VCENTER | JUST_HCENTER}},
        { 1, 0, WIDGET_RSSI_ID,            0,   0, {JUST_TOP     | JUST_RIGHT}, {0, 255}},
        { 1, 0, WIDGET_SPEED_ID,           0,   0, {JUST_VCENTER | JUST_LEFT}},
        { 1, 0, WIDGET_THROTTLE_ID,       70,   0, {JUST_TOP     | JUST_LEFT}},
        { 1, 0, WIDGET_VARIOMETER_ID,      0,  -5, {JUST_BOT     | JUST_RIGHT}},
        { 1, 0, WIDGET_WIND_ID,            0,  30, {JUST_TOP     | JUST_RIGHT}},

        { 1, 0, WIDGET_HOME_INFO_ID,      80,   0, {JUST_TOP     | JUST_LEFT}},
        { 1, 0, WIDGET_RADAR_ID,          60, -44, {JUST_BOT     | JUST_LEFT}},
        //{ 1, 0, WIDGET_MESSAGES_ID,        0,  0, {JUST_TOP     | JUST_HCENTER}},

        { 2, 0, WIDGET_RC_CHANNELS_ID,     0,   0, {JUST_TOP     | JUST_LEFT}},
        { 2, 1, WIDGET_RADAR_ID,           0,   0, {JUST_TOP     | JUST_HCENTER}},
        { 2, 0, WIDGET_CONSOLE_ID,         0,   0, {JUST_BOT     | JUST_LEFT}},

        { 3, 0, WIDGET_FLIGHT_INFO_ID,     0,   0, {JUST_VCENTER | JUST_HCENTER}},

        { TABS_END, 0, 0, 0, 0, {0}},
    },
};

const struct param_def params_config[] = {
    PARAM("HOME_LOCKING", MAV_PARAM_TYPE_UINT8, &config.home_lock_sec, NULL),
    PARAM("OSD_UNITS", MAV_PARAM_TYPE_UINT8, &config.default_units, NULL),
    PARAM_END,
};

static struct uart_client config_uart_client;


unsigned char get_units(struct widget_config *cfg)
{
    if (cfg->props.units == UNITS_DEFAULT)
        return config.default_units;
    else
        return cfg->props.units;
}

static void load_config(void)
{
    unsigned long status;
    unsigned long addr;
    unsigned char buf[3];
    int ipl;

    SET_AND_SAVE_CPU_IPL(ipl, 7);

    /* find first valid config */
    addr = CONFIG_ADDR_START;

    while (addr < CONFIG_ADDR_END) {
        read_flash(addr, 3, buf);
        status = ((unsigned long) buf[0]) |
                 ((unsigned long) buf[1] << 8) |
                 ((unsigned long) buf[2] << 16);
        //printf("signature: %4x%4x\n", (unsigned int) (status>>16), (unsigned int) status);
        if (status == CONFIG_VERSION_SIG)
            break;

        addr += CONFIG_PAGE_SIZE;
    }

    if (addr < CONFIG_ADDR_END) {
        //printf("valid config found at %4x\n", (unsigned int) addr);
        valid_config_addr = addr;
        read_flash(addr + 4, sizeof(struct alceosd_config), (unsigned char *) &config);
    }

    /* setup video */
    video_apply_config(VIDEO_ACTIVE_CONFIG);

    /* setup serial ports */
    uart_set_config_pins();
    uart_set_config_baudrates();

    RESTORE_CPU_IPL(ipl);
}


static void write_config(void)
{
    unsigned long addr;
    unsigned char *b;
    int left;
    unsigned long data;
    int ipl;

    SET_AND_SAVE_CPU_IPL(ipl, 7);

    if (valid_config_addr != 0) {
        /* find a spot */
        if ((valid_config_addr + CONFIG_PAGE_SIZE) < CONFIG_ADDR_END) {
            /* still room for one more, invalidate current config */
            write_word(valid_config_addr, 0xffeeee);
            valid_config_addr += CONFIG_PAGE_SIZE;
        } else {
            //printf("no more space, reseting addr\n");
            valid_config_addr = 0;
        }
    }

    if (valid_config_addr == 0) {
        /* end of config memory or no more space, erase pages start over*/
        for (addr = CONFIG_ADDR_START; addr < CONFIG_ADDR_END; addr += CONFIG_ADDR_PAGE) {
            //printf("erasing page %4x\n", (unsigned int) addr);
            erase_page(addr);
        }

        valid_config_addr = CONFIG_ADDR_START;
    }

    /* write config */
    /* flag valid config */
#ifdef DEBUG_CONFIG
    printf("last_addr=%04x%04x\n", (unsigned int) (valid_config_addr>>16), (unsigned int) (valid_config_addr));
#endif
    write_word(valid_config_addr, CONFIG_VERSION_SIG);

    b = (unsigned char*) &config;
    left = sizeof(struct alceosd_config);
    addr = valid_config_addr + 4;
    while (left > 0) {
        data = (unsigned long) (*b++);
        if (left > 1) {
            data |= (unsigned long) (*b++) << 8;
            if (left > 2)
                data |= (unsigned long) (*b++) << 16;
            else
                data |= 0xffff0000;
        } else {
            data |= 0xffffff00;
        }

        write_word(addr, data);

        left -= 3;
        addr += 2;
    }

#ifdef DEBUG_CONFIG
    printf("last_addr=%04x%04x\n", (unsigned int) (addr>>16), (unsigned int) (addr));
#endif
    RESTORE_CPU_IPL(ipl);
}


extern unsigned char hw_rev;

static void dump_config_text(void)
{
    char param_name[17];
    float value;
    unsigned int i;
    
    printf("\nAlceOSD config hw%dv%d fw%d.%d.%d\n==\n", hw_rev >> 4, hw_rev & 0xf, VERSION_MAJOR, VERSION_MINOR, VERSION_DEV);

    for (i = 0; i < params_get_total(); i++) {
        value = params_get_value(i, param_name);
        printf("%s = %f\n", param_name, (double) value);
    }

    printf("--\n");

}

static unsigned int config_process(struct uart_client *cli, unsigned char *buf, unsigned int len);

#define MAX_LINE_LEN 50
static unsigned int load_config_text(struct uart_client *cli, unsigned char *buf, unsigned int len)
{
    static unsigned char line[MAX_LINE_LEN];
    static unsigned char llen = 0;

    char param[20];
    float value;
    unsigned int i = 0;

    while (*buf != '\n') {
        line[llen++] = *(buf++);
        if (++i == len)
            return len;
    }
    line[llen] = '\0';
    i++;

    /* process line */
    if (llen == 0)
        return i;

    /* the end */
    if (line[0] == '.') {
        config_uart_client.read = config_process;
        llen = 0;
        load_tab(0);
        return i;
    }

    /* reset widgets config */
    if (memcmp("==", line, 2) == 0)
        config.widgets[0].tab = TABS_END;

    sscanf((const char *) line, "%20s = %f", param, &value);
    params_set_value(param, value, 0);
    printf("got: '%s' = '%f'\n", param, (double) value);

    llen = 0;
    return i;
}

static void exit_config(void)
{
    uart_set_config_pins();
    uart_set_config_baudrates();
    uart_set_config_clients();
}


enum {
    MENU_MAIN,
    MENU_TABS,
    MENU_TAB_WIDGETS,
    MENU_ADD_WIDGET,
    MENU_EDIT_WIDGET,
};

const char menu_main[] = "\n\n"
                         "AlceOSD setup\n\n"
                         "3 - Configure tabs\n"
                         "4 - Units (global setting): %s\n"
                         "q/w - Decrease/increase home locking timer: %d\n"
                         "\n"
                         "s - Save settings to FLASH\n"
                         "d - Dump setting to console\n"
                         "l - Load settings from console\n"
                         "x - Exit config\n";

const char menu_tabs[] = "\n\nAlceOSD :: TAB config\n\n"
                         "1/2 - Change active tab: %d\n"
                         "3/4 - Tab change mode: %s\n";
const char menu_tabs_mode_ch[] = "5/6 - Tab change channel: RC%d\n";
const char menu_tabs_mode_tmr[] = "7/8 - Change time window: %d00ms\n";
const char menu_tabs_end[] = "e - Edit tab\n"
                             "x - Go back\n";

const char menu_tab_widgets[] = "\n\nAlceOSD :: TAB %d widgets config\n\n"
                                "0 - Add widget\n"
                                "x - Go Back\n\n";

const char menu_add_widgets[] = "\n\nAlceOSD :: Add widget\n\n"
                                "0 - Go back\n";

const char menu_edit_widget[] = "\n\nAlceOSD :: Edit widget\n\n"
                                "1 - Horizontal justification: %d\n"
                                "2 - Vertical justification: %d\n"
                                "3/4 - Mode: %d\n"
                                "5/6 - Units: %d\n"
                                "7/8 - Source: %d\n"
                                "w/e - X position: %d\n"
                                "q/a - Y position: %d\n"
                                "0 - Remove\n"
                                "\nx - Go back\n";


extern const struct widget_ops *all_widget_ops[];

static unsigned int shell_process(struct uart_client *cli, unsigned char *buf, unsigned int len)
{
    shell_parser(buf, len);
    return len;
}

static void shell_cmd_stats(char *args, void *data)
{
    shell_printf("Config stats:\n");
    shell_printf(" Signature        0x%06lx\n", CONFIG_VERSION_SIG);
    shell_printf(" Address range    0x%06lx-0x%06lx\n", CONFIG_ADDR_START, CONFIG_ADDR_END);
    if (valid_config_addr == 0)
        shell_printf(" Address          (none)\n");
    else
        shell_printf(" Address          0x%06lx\n", valid_config_addr);
    shell_printf(" Size             %u/%u (0x%x/0x%x)\n",
            (unsigned int) sizeof(struct alceosd_config),
            (unsigned int) CONFIG_PAGE_SIZE,
            (unsigned int) sizeof(struct alceosd_config),
            (unsigned int) CONFIG_PAGE_SIZE);
}

static void shell_cmd_savecfg(char *args, void *data)
{
    shell_printf("Saving config...\n");
    write_config();
}

static void shell_cmd_loadcfg(char *args, void *data)
{
    shell_printf("Loading config...\n");
    config_uart_client.read = load_config_text;
}

static void shell_cmd_dumpcfg(char *args, void *data)
{
    dump_config_text();
}

static const struct shell_cmdmap_s config_cmdmap[] = {
    {"dump", shell_cmd_dumpcfg, "dump", SHELL_CMD_SIMPLE},
    {"load", shell_cmd_loadcfg, "load", SHELL_CMD_SIMPLE},
    {"save", shell_cmd_savecfg, "save", SHELL_CMD_SIMPLE},
    {"stats", shell_cmd_stats, "config statistics", SHELL_CMD_SIMPLE},
    {"", NULL, ""},
};

void shell_cmd_cfg(char *args, void *data)
{
    shell_exec(args, config_cmdmap, data);
}


static unsigned int config_process(struct uart_client *cli, unsigned char *buf, unsigned int len)
{
    static unsigned char state = MENU_MAIN;
    unsigned int osdxsize, osdysize;
    static unsigned int options[30];
    static unsigned char nr_opt = 0;

    video_get_size(&osdxsize, &osdysize);

    static unsigned char current_tab = 1;
    static struct widget_config *wcfg;

    char c = *buf;

#ifdef DEBUG_CONFIG
    printf("cfg size=%d\n", (unsigned int) sizeof (struct alceosd_config));
#endif

    switch (state) {
        case MENU_MAIN:
        default:
            switch (c) {
                case '3':
                    state = MENU_TABS;
                    break;
                case '4':
                    if (config.default_units == UNITS_METRIC)
                        config.default_units = UNITS_IMPERIAL;
                    else
                        config.default_units = UNITS_METRIC;
                    load_tab(current_tab);
                    break;
                case 'q':
                    if (config.home_lock_sec > 5)
                        config.home_lock_sec -= 5;
                    load_tab(current_tab);
                    break;
                case 'w':
                    if (config.home_lock_sec < 56)
                        config.home_lock_sec += 5;
                    load_tab(current_tab);
                    break;
                case 's':
                    printf("Saving config to FLASH...\n");
                    write_config();
                    break;
                case 'd':
                    printf("Dumping config to console...\n");
                    dump_config_text();
                    break;
                case 'l':
                    printf("Loading config from console...\n");
                    config_uart_client.read = load_config_text;
                    return 1;
                case 'x':
                    exit_config();
                    return 1;
                    
                case '+':
                    break;
                case '-':
                    break;
                    
                case '!':
                    config_uart_client.read = shell_process;
                    break;

                case '#':
                    __asm__ volatile ("reset");
                    break;
                default:
                    break;
            }

            break;
        case MENU_TABS:
            switch (c) {
                case '1':
                    current_tab--;
                    load_tab(current_tab);
                    break;
                case '2':
                    current_tab++;
                    load_tab(current_tab);
                    break;
                case '3':
                    if (config.tab_change.mode > 0)
                        config.tab_change.mode--;
                    break;
                case '4':
                    if (config.tab_change.mode < (TAB_CHANGE_MODES_END-1))
                        config.tab_change.mode++;
                    break;
                case '5':
                    if (config.tab_change.ch > 0)
                        config.tab_change.ch--;
                    break;
                case '6':
                    if (config.tab_change.ch < 7)
                        config.tab_change.ch++;
                    break;
                case '7':
                    if (config.tab_change.time_window > 5)
                        config.tab_change.time_window--;
                    break;
                case '8':
                    if (config.tab_change.time_window < 50)
                        config.tab_change.time_window++;
                    break;
                case 'e':
                    state = MENU_TAB_WIDGETS;
                    break;
                case 'x':
                    state = MENU_MAIN;
                    break;
                default:
                    break;
            }
            break;
        case MENU_TAB_WIDGETS:
            switch (c) {
                case '0':
                    state = MENU_ADD_WIDGET;
                    break;
                case 'x':
                    state = MENU_TABS;
                    break;
                default:
                    if (c > '9')
                        c -= ('a' - 9);
                    else
                        c -= '1';
                    if (c > nr_opt)
                        break;
                    nr_opt = c;
                    state = MENU_EDIT_WIDGET;
            }
            break;
        case MENU_ADD_WIDGET:
            switch (c) {
                case '0':
                    state = MENU_TAB_WIDGETS;
                    break;
                default:
                    if (c > '9')
                        c -= ('a' - 9);
                    else
                        c -= '1';
                    if (c > nr_opt)
                        break;
                    nr_opt = c;

                    wcfg = &config.widgets[0];
                    while (wcfg->tab != TABS_END)
                        wcfg++;

                    wcfg->uid = widget_get_uid(options[nr_opt]);
                    wcfg->tab = current_tab;
                    wcfg->widget_id = options[nr_opt];
                    wcfg->x = 0;
                    wcfg->y = 0;
                    wcfg->props.raw = JUST_VCENTER | JUST_HCENTER;

                    wcfg++;

                    wcfg->tab = TABS_END;

                    load_tab(current_tab);

                    state = MENU_TAB_WIDGETS;
                    break;
            }
            break;
        case MENU_EDIT_WIDGET:
            switch (c) {
                case 'a':
                    wcfg->y++;
                    break;
                case 'q':
                    wcfg->y--;
                    break;
                case 'w':
                    wcfg->x-=4;
                    break;
                case 'e':
                    wcfg->x+=4;
                    break;
                case '1':
                    wcfg->props.hjust++;
                    if (wcfg->props.hjust > 2)
                        wcfg->props.hjust = 0;
                    break;
                case '2':
                    wcfg->props.vjust++;
                    if (wcfg->props.vjust > 2)
                        wcfg->props.vjust = 0;
                    break;
                case '3':
                    wcfg->props.mode--;
                    break;
                case '4':
                    wcfg->props.mode++;
                    break;
                case '5':
                    wcfg->props.units--;
                    break;
                case '6':
                    wcfg->props.units++;
                    break;
                case '7':
                    wcfg->props.source--;
                    break;
                case '8':
                    wcfg->props.source++;
                    break;
                case 'x':
                    state = MENU_TAB_WIDGETS;
                    break;
                case '0': {
                    struct widget_config *wcfg2 = config.widgets;
                    while (wcfg2 != wcfg)
                        wcfg2++;
                    wcfg++;
                    while (wcfg->tab != TABS_END) {
                        memcpy(wcfg2++, wcfg++, sizeof(struct widget_config));
                    }
                    wcfg2->tab = TABS_END;
                    state = MENU_TAB_WIDGETS;
                }
                default:
                    break;
            }
            load_tab(current_tab);
            break;
    }


    switch (state) {
        case MENU_MAIN:
        default:
            printf(menu_main,
                    (config.default_units == UNITS_METRIC) ? "METRIC" : "IMPERIAL",
                    config.home_lock_sec);
            break;
        case MENU_TABS:
            printf(menu_tabs, current_tab,
                    (config.tab_change.mode == TAB_CHANGE_CHANNEL) ? "RC CHANNEL PERCENT" :
                    (config.tab_change.mode == TAB_CHANGE_FLIGHTMODE) ? "FLIGHT MODE" :
                    (config.tab_change.mode == TAB_CHANGE_TOGGLE) ? "RC CHANNEL TOGGLE" :
                    (config.tab_change.mode == TAB_CHANGE_DEMO) ? "DEMO" : "????");

            switch (config.tab_change.mode) {
                case TAB_CHANGE_CHANNEL:
                default:
                    printf(menu_tabs_mode_ch, config.tab_change.ch + 1);
                    break;
                case TAB_CHANGE_FLIGHTMODE:
                case TAB_CHANGE_DEMO:
                    printf(menu_tabs_mode_tmr, config.tab_change.time_window);
                    break;
                case TAB_CHANGE_TOGGLE:
                    printf(menu_tabs_mode_ch, config.tab_change.ch + 1);
                    printf(menu_tabs_mode_tmr, config.tab_change.time_window);
                    break;
            }
            printf(menu_tabs_end);
            break;
        case MENU_TAB_WIDGETS: {
            wcfg = &config.widgets[0];
            const struct widget_ops *w_ops;
            char c = '1';
            nr_opt = 0;

            printf(menu_tab_widgets, current_tab);
            while (wcfg->tab != TABS_END) {
                if (wcfg->tab == current_tab) {
                    w_ops = get_widget_ops(wcfg->widget_id);
                    if (w_ops != NULL) {
                        printf("%c - %s\n", c++, w_ops->name);
                        if (c == ('9'+1))
                            c = 'a';
                        options[nr_opt++] = (int) wcfg;
                    }
                }
                wcfg++;
            }
            break;
        }

        case MENU_ADD_WIDGET: {
            const struct widget_ops **w_ops;
            char c = '1';
            nr_opt = 0;

            printf(menu_add_widgets);

            w_ops = all_widget_ops;
            while (*w_ops != NULL) {
                printf("%c - %s\n", c++, (*w_ops)->name);
                if (c == ('9'+1))
                    c = 'a';
                options[nr_opt++] = (*w_ops)->id;
                w_ops++;
            }
            break;
        }
        case MENU_EDIT_WIDGET: {
            wcfg = (struct widget_config*) options[nr_opt];
            // struct widget_ops *w_ops = get_widget_ops(wcfg->widget_id);

            printf(menu_edit_widget,
                wcfg->props.hjust,
                wcfg->props.vjust,
                wcfg->props.mode,
                wcfg->props.units,
                wcfg->props.source,
                wcfg->x,
                wcfg->y);
            break;
        }

    }
    return 1;
}

void config_init(void)
{
    params_add(params_config);

    load_config();

    memset(&config_uart_client, 0, sizeof(struct uart_client));
    config_uart_client.read = config_process;
    config_uart_client.id = UART_CLIENT_CONFIG;
    uart_add_client(&config_uart_client);
}
