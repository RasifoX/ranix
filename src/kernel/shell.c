#include "shell.h"
#include "stdio.h"
#include "keyboard.h"
#include "string.h"
#include "hal.h"
#include "io.h"
#include "fs.h"
#include "heap.h"

#define CMD_BUFFER_SIZE 128

static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_len = 0;

static void print_prompt()
{
    kprintf("Ranix> ");
}

static void cmd_ls()
{
    if (!fs_root)
    {
        kprintf("Error: No filesystem mounted.\n");
        return;
    }

    struct dirent *node = 0;
    int i = 0;

    kprintf("Files in /:\n");

    while ((node = readdir_fs(fs_root, i)) != 0)
    {
        kprintf(" - %s (Inode: %d)\n", node->name, node->ino);
        i++;
    }
}

static void cmd_cat(char *filename)
{
    if (!fs_root)
    {
        kprintf("Error: No filesystem mounted.\n");
        return;
    }

    fs_node_t *fsnode = finddir_fs(fs_root, filename);

    if (!fsnode)
    {
        kprintf("Error: File '%s' not found.\n", filename);
        return;
    }

    if ((fsnode->flags & 0x7) == FS_DIRECTORY)
    {
        kprintf("Error: '%s' is a directory.\n", filename);
        return;
    }

    uint32_t size = fsnode->length;
    if (size > 1024)
        size = 1024;

    char *buf = (char *)kmalloc(size + 1);
    if (!buf)
    {
        kprintf("Error: Out of memory for read buffer.\n");
        return;
    }

    uint32_t read_bytes = read_fs(fsnode, 0, size, (uint8_t *)buf);
    buf[read_bytes] = 0;

    kprintf("%s\n", buf);

    kfree(buf);
}

static void execute_command(char *input)
{
    if (cmd_len == 0)
        return;

    char cmd[32];
    char arg[64];

    int i = 0;
    while (input[i] != ' ' && input[i] != 0 && i < 31)
    {
        cmd[i] = input[i];
        i++;
    }
    cmd[i] = 0;

    if (input[i] == ' ')
    {
        i++;
        int j = 0;
        while (input[i] != 0 && j < 63)
        {
            arg[j++] = input[i++];
        }
        arg[j] = 0;
    }
    else
    {
        arg[0] = 0;
    }

    if (strcmp(input, "help") == 0)
    {
        kprintf("\nAvailable Commands:\n");
        kprintf(" - help     : Show this list\n");
        kprintf(" - clear    : Clear the screen\n");
        kprintf(" - reboot   : Reboot the system\n");
        kprintf(" - shutdown : Halt the CPU\n");
        kprintf(" - memory   : Show memory usage (TODO)\n");
        kprintf(" - ls       : List files\n");
        kprintf(" - cat <f>  : Read file content\n");
    }
    else if (strcmp(input, "clear") == 0)
    {
        hal_clear_screen();
    }
    else if (strcmp(input, "reboot") == 0)
    {
        kprintf("\nRebooting...\n");
        uint8_t good = 0x02;
        while (good & 0x02)
            good = inb(0x64);
        outb(0x64, 0xFE);
        asm volatile("hlt");
    }
    else if (strcmp(input, "shutdown") == 0)
    {
        kprintf("\nSystem Halted. It is now safe to turn off your computer.\n");
        while (1)
        {
            hal_cpu_disable_interrupts();
            hal_cpu_halt();
        }
    }
    else if (strcmp(cmd, "ls") == 0)
    {
        kprintf("\n");
        cmd_ls();
    }
    else if (strcmp(cmd, "cat") == 0)
    {
        if (arg[0] == 0)
        {
            kprintf("Usage: cat <filename>\n");
        }
        else
        {
            kprintf("\n--- --- ---\n");
            cmd_cat(arg);
            kprintf("--- --- ---\n");
        }
    }

    else
    {
        kprintf("\nUnknown command: %s\n", input);
    }
}

void shell_init()
{
    kprintf("\nWelcome to Ranix Shell!\n");
    kprintf("Type 'help' for commands.\n\n");

    cmd_len = 0;
    memset(cmd_buffer, 0, CMD_BUFFER_SIZE);
    print_prompt();
}

void shell_update()
{
    while (keyboard_has_chars())
    {
        char c = keyboard_get_char();

        if (c == '\n')
        {
            cmd_buffer[cmd_len] = '\0';

            execute_command(cmd_buffer);

            cmd_len = 0;
            memset(cmd_buffer, 0, CMD_BUFFER_SIZE);
            if (strcmp(cmd_buffer, "clear") != 0)
                kprintf("\n");

            print_prompt();
        }
        else if (c == '\b')
        {
            if (cmd_len > 0)
            {
                cmd_len--;
                cmd_buffer[cmd_len] = '\0';

                hal_kputc('\b');
            }
        }
        else
        {
            if (cmd_len < CMD_BUFFER_SIZE - 1)
            {
                cmd_buffer[cmd_len++] = c;
                cmd_buffer[cmd_len] = '\0';

                hal_kputc(c);
            }
        }
    }
}