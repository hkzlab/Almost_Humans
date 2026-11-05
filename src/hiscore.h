/* hiscore.h — the interface of src/hiscore.c: HISCORES.HUM, its
 * obfuscation and the top-10 insert (re_formats.md §11). */

#ifndef HUMANSRE_HISCORE_H
#define HUMANSRE_HISCORE_H

#include <stdint.h>
/* --- HISCORES.HUM (re_formats.md §11, menu_loop 1000:012A) ------------ */
void hiscore_init(void); /* 1000:069F: load + decrypt  */
void hiscore_reset(void); /* the shipped defaults       */
void hiscore_reload(void); /* re-read the file           */
void hiscore_obfuscate(uint8_t *p, uint32_t n); /* 1000:06DE          */
void hiscore_write(void); /* 1000:06CC: encrypt + save  */
uint16_t hiscore_best(uint32_t level); /* DS:0x38A + 2*level         */
void hiscore_set_best(uint32_t level, uint16_t value);
uint32_t hiscore_count(void); /* 10                         */
uint32_t hiscore_score(uint32_t i); /* the top-10, descending     */
const char *hiscore_name(uint32_t i);
int32_t hiscore_qualifies(uint32_t score);
int32_t hiscore_insert(uint32_t score, const char *name); /* 0x01CD    */
/* `name` must have one writable byte before name[0]: an empty delete writes
 * the original's 0x756 byte (a duplicate would take the headroom). */
int32_t hiscore_name_apply(char *name, uint32_t column, uint32_t *len);

#endif /* HUMANSRE_HISCORE_H */
