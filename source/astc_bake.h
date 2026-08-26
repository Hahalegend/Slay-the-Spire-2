/* astc_bake.h -- see astc_bake.c. MIT license; see LICENSE. */
#ifndef STS2_ASTC_BAKE_H
#define STS2_ASTC_BAKE_H

/* Must run BEFORE the renderer takes the default NWindow (main.c:1923),
 * because the progress console owns that same window. */
void astc_bake_run(const char *pck_path);

#endif
