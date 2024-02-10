/*
 * linux4i9300 rootfs (almost boot-)menu
 * acroreiser, 2024
 *
 * This program acts as first-stage init and asks user,
 * where to find rootfs. Then prepares rootfs and runs real init (most likely systemd)
 *
 * Dependencies(Debian/Ubuntu): libncurses-dev libevdev-dev
 *
 * Compilation(statically linked binary, on arm-linux system):
 *     gcc -o menu menu.c  -static -lncurses -ltinfo -levdev -I/usr/include/libevdev-1.0
 */
#include <ncurses.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

void draw_menu(WINDOW *menu_win, char **choices, int highlight, int n_choices) {
    int x, y, i;

    x = 2;
    y = 2;

    werase(menu_win);

    for (i = 0; i < n_choices; ++i) {
        if (highlight == i + 1) {
            wattron(menu_win, A_REVERSE);
            mvwprintw(menu_win, y, x, "%s", choices[i]);
            wattroff(menu_win, A_REVERSE);
        } else {
            mvwprintw(menu_win, y, x, "%s", choices[i]);
        }
        ++y;
    }
    wrefresh(menu_win);
}

// Switch root and release old root
// then run real init
void mount_root(char *blkdev)
{
    mount(blkdev, "/mnt", "ext4", 0, NULL);

    umount("/dev");
    chdir("/mnt");
    syscall(SYS_pivot_root, ".", "tmp");
    chroot(".");
    chdir("/");
    umount2("/tmp", MNT_DETACH);

    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    execl("/sbin/init", "/sbin/init", NULL);
}

int main() {
    struct libevdev *dev = NULL;
    int fd;

    /* Setup libevdev.
     /dev/input/eventX must be gpio_keys events assigned by kernel.
     You can get it using evtest on the running system.
     We will use keycodes 114, 115 and 116
     (Volume-, Volume+ and Power) */
    fd = open("/dev/input/event2", O_RDONLY);
    libevdev_new_from_fd(fd, &dev);

    // Initialize and setup libncurses
    initscr();
    cbreak();
    noecho();

    int highlight = 1;
    int choice = 0;
    int c;

    // Rootfs partitions, must contain an ext4 filesystem
    char emmc[] = "/dev/mmcblk0p12";
    char sdcard[] = "/dev/mmcblk1p1";

    // Menu entries
    char *choices[] = {
        "Boot from eMMC",
        "Boot from SD card",
        "Power off",
    };
    int n_choices = sizeof(choices) / sizeof(char *);

    int startx = (COLS - 30) / 2;
    int starty = (LINES - 10) / 2;
    WINDOW *menu_win;
    menu_win = newwin(10, 30, starty, startx);
    keypad(menu_win, TRUE);
    mvprintw(LINES - 2, 0, "Use volume keys to navigate and power key to confirm");
    refresh();
    // Display our menu
    draw_menu(menu_win, choices, highlight, n_choices);

    // Handle keys input
    while (1) {
        struct input_event ev;
        libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);

        // Skip key release events
    	if(ev.value)
    		continue;

        switch (ev.code) {
            case 115: // vol+
                if (highlight == 1)
                    highlight = n_choices;
                else
                    --highlight;
                break;
            case 114: // vol-
                if (highlight == n_choices)
                    highlight = 1;
                else
                    ++highlight;
                break;
            case 116: // powerkey
                choice = highlight;
                break;
            default:
                break;
        }

        draw_menu(menu_win, choices, highlight, n_choices);

        if (choice != 0) {
            switch(choice)
            {
                case 1:
                    mount_root(emmc);
                    break;
                case 2:
                    mount_root(sdcard);
                    break;
                case 3:
                    reboot(RB_POWER_OFF); // shutdown phone
                    break;
            }
            choice = 0;
        }
    }

    endwin();
    return 0;
}
