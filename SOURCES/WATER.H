#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

void Water_ReadConfig(void);
void Water_WriteConfig(void);
void Water_RegisterCvars(void);

/* Exterior cube origin used to turn local contacts into island coordinates. */
void Water_SetScene(S32 island, S32 cubeX, S32 cubeZ);
S32 Water_IsGameCode(S32 code);
void Water_Impact(S32 x, S32 z, U32 milliseconds, float strength);
void Water_AnimationContact(S32 animation, S32 frame, S32 x, S32 z, U32 milliseconds);
