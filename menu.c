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
#include <string.h>
#include <sys/reboot.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

/*
 * Magic values required to use _reboot() system call.
 */

#define	LINUX_REBOOT_MAGIC1	0xfee1dead
#define	LINUX_REBOOT_MAGIC2	672274793

#define	LINUX_REBOOT_CMD_RESTART2	0xA1B2C3D4

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
    const char *msg_mount_failed = "menu: failed to mount root!\n";
    const char *msg_no_init = "menu: no or not executable /sbin/init!\n";
    int fd;

    // Open /dev/kmsg for writing
    fd = open("/dev/tty0", O_WRONLY);

    if (!mount(blkdev, "/mnt", "ext4", 0, NULL) ||
        !mount(blkdev, "/mnt", "f2fs", 0, NULL))
    {
        chdir("/mnt");
        syscall(SYS_pivot_root, ".", "tmp");

        chroot(".");
        chdir("/");

        if (access("/sbin/init", X_OK) != 0)
        {
            write(fd, msg_no_init, strlen(msg_no_init));
            close(fd);
            chdir("/tmp");
            syscall(SYS_pivot_root, ".", "mnt");
            umount2("/mnt", MNT_DETACH);
            return;
        } else
            close(fd);

        umount2("/tmp/dev", MNT_DETACH);
        umount2("/tmp", MNT_DETACH);

        mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
        execl("/sbin/init", "/sbin/init", NULL);
    } else
    {
        write(fd, msg_mount_failed, strlen(msg_mount_failed));
        close(fd);
    }
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
    char usb[] = "/dev/sda1";

    // Menu entries
    char *choices[] = {
        "Boot from eMMC",
        "Boot from SD card",
        "Boot from USB (sda1)",
        "Reboot to recovery",
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
                    mount_root(usb);
                    break;
                case 4:
                    syscall(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
                             LINUX_REBOOT_CMD_RESTART2, "recovery");
                    break;
                case 5:
                    reboot(RB_POWER_OFF); // shutdown phone
                    break;
            }
            choice = 0;
        }
    }

    endwin();
    return 0;
}
