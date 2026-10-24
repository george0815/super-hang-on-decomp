/* Game objects. */

#include "game.h"

#define OBJ_LAYER       0x0C    /* word: draw layer of the object (even) */
#define LAYER_LIST_SIZE 0x20    /* count word + up to 15 object pointers */
#define LAYER_LIST_MAX  0x1E    /* count is kept in bytes */

/*
 * Queue an object on its layer's list ($709A). Lists sit below
 * OBJ_LAYER_LISTS, one per layer: a count (in bytes) followed by the low
 * words of the object addresses. Full lists drop the object.
 */
void obj_add_to_layer(uint32_t obj)
{
    uint32_t list = OBJ_LAYER_LISTS - (uint32_t)(rd16(obj + OBJ_LAYER) & 0xfffe) * 16;
    uint16_t count = rd16(list);
    if ((int16_t)count >= LAYER_LIST_MAX)
        return;
    wr16(list, count + 2);
    wr16(list + 2 + count, obj);
}

void obj_add_to_layer_68k(void)
{
    uint32_t obj = cpu.a[0];
    uint16_t layer = rd16(obj + OBJ_LAYER) & 0xfffe;
    uint32_t list = 0xFFFFE000u - (uint32_t)(uint16_t)(layer << 4);
    uint16_t count = rd16(list);
    obj_add_to_layer(obj);
    set_dn(0, 2, (uint16_t)(layer << 4));
    set_dn(1, 2, count);
    cpu.a[1] = list;
    f_cmp(2, LAYER_LIST_MAX, count);
}
