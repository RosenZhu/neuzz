
/* Map size for the traced binary (2^MAP_SIZE_POW2). Must be greater than
   2; you probably want to keep it under 18 or so for performance reasons
   (adjusting AFL_INST_RATIO when compiling is probably a better way to solve
   problems with complex programs). You need to recompile the target binary
   after changing this - otherwise, SEGVs may ensue. */

#define MAP_SIZE_POW2       18
#define MAP_SIZE            (1 << MAP_SIZE_POW2)

/* Extra size for BB values */
#define BBVAL_MAP_SIZE     (MAP_SIZE * 8)

/* max file length in neuzz */
#define NEUZZ_MAX_FILE_LENGTH       10000

