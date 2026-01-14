#ifndef KEYMAP_H
#define KEYMAP_H

typedef struct {
    char name[16];

    // [0]: Normal, [1]: Shift, [2]: AltGr
    unsigned char map[128][3]; 
} keymap_t;

void keyboard_set_map(keymap_t *new_map);

#endif