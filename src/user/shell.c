#include "lib.h"

#define CMD_BUFFER_SIZE 64

void shell_main()
{
    char cmd[CMD_BUFFER_SIZE];
    int len = 0;

    print("\n\n=== Ranix User Shell (Ring 3) ===\n");
    print("Type 'help' for commands.\n");

    while (1)
    {
        print("\nRanix$ ");

        len = 0;
        // memset -> u_memset
        u_memset(cmd, 0, CMD_BUFFER_SIZE);

        while (1)
        {
            char c = getchar();

            if (c == '\n')
            { // Enter
                break;
            }
            else if (c == '\b')
            { // Backspace
                if (len > 0)
                {
                    len--;
                    cmd[len] = 0;
                    print("\b \b");
                }
            }
            else if (len < CMD_BUFFER_SIZE - 1)
            {
                cmd[len++] = c;
                putchar(c); // Echo
            }
        }

        print("\n");

        if (len == 0)
            continue;

        // strcmp -> u_strcmp
        if (u_strcmp(cmd, "help") == 0)
        {
            print("Commands:\n");
            print("  help    - Show this message\n");
            print("  exit    - Exit shell\n");
            print("  hello   - Say hello\n");
        }
        else if (u_strcmp(cmd, "hello") == 0)
        {
            print("Hello from User Land!\n");
        }
        else if (u_strcmp(cmd, "exit") == 0)
        {
            print("Bye!\n");
            exit(0);
        }
        else
        {
            print("Unknown command: ");
            print(cmd);
            print("\n");
        }
    }
}