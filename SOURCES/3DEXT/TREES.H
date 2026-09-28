#ifndef TREES_H
#define TREES_H

/* The island decor models that are trees, and which of their polygons are
   leaves: the GPU renderer shades those as foliage and sways the model in the
   wind (gfx_trees, SVGA/GPUTREE.H). Found by looking at every island's models,
   with the `decors` command on the islands themselves. */

/** Whether a decor model of the island loaded now (IslandName) is a tree, and
 *  which of its polygons are leaves: bit n for palette bank n (colours 16n to
 *  16n+15), GPUTREE_FOLIAGE_TEXTURED for its textured polygons. A trunk drawn
 *  as a model of its own is a tree with no leaves. */
extern S32 Trees_Find(S32 body, U32 *foliage);

/** Registers `decors`, which lists the current cube's decor models. */
extern void Trees_RegisterCommands(void);

#endif // TREES_H
