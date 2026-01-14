#include "shell.h"
#include "stdio.h"
#include "keyboard.h"
#include "string.h"
#include "hal.h"
#include "io.h"

#define CMD_BUFFER_SIZE 128

static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_len = 0;

static void print_prompt()
{
    kprintf("Ranix> ");
}

static void execute_command(char *input)
{
    if (cmd_len == 0)
        return;

    if (strcmp(input, "help") == 0)
    {
        kprintf("\nAvailable Commands:\n");
        kprintf(" - help     : Show this list\n");
        kprintf(" - clear    : Clear the screen\n");
        kprintf(" - reboot   : Reboot the system\n");
        kprintf(" - shutdown : Halt the CPU\n");
        kprintf(" - memory   : Show memory usage (TODO)\n");
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