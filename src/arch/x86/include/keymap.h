#ifndef KEYMAP_H
#define KEYMAP_H

typedef struct
{
    char name[16];
    unsigned char map[128];
} keymap_t;

void keyboard_set_map(keymap_t *new_map);

#endif