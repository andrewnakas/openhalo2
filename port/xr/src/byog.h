#ifndef BYOG_H
#define BYOG_H
/* 1 when game\default.xbe is the supported retail build (importing it from
 * the player's disc first when the folder is missing and `interactive`). */
int byog_ensure_game(int interactive);
#endif
