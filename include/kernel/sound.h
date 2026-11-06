#ifndef KERNEL_SOUND_H
#define KERNEL_SOUND_H
void hda_init(void);
int  sound_volume(void);       /* 0-100 */
void sound_set_volume(int v);
#endif
