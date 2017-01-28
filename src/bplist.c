/*
 * bplist.c
 * Binary plist implementation
 *
 * Copyright (c) 2011-2016 Nikias Bassen, All Rights Reserved.
 * Copyright (c) 2008-2010 Jonathan Beck, All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <ctype.h>

#include <plist/plist.h>
#include "plist.h"
#include "hashtable.h"
#include "bytearray.h"
#include "ptrarray.h"

#include <node.h>
#include <node_iterator.h>

/* Magic marker and size. */
#define BPLIST_MAGIC            ((uint8_t*)"bplist")
#define BPLIST_MAGIC_SIZE       6

#define BPLIST_VERSION          ((uint8_t*)"00")
#define BPLIST_VERSION_SIZE     2

typedef struct __attribute__((packed)) {
    uint8_t unused[6];
    uint8_t offset_size;
    uint8_t ref_size;
    uint64_t num_objects;
    uint64_t root_object_index;
    uint64_t offset_table_offset;
} bplist_trailer_t;

enum
{
    BPLIST_NULL = 0x00,
    BPLIST_FALSE = 0x08,
    BPLIST_TRUE = 0x09,
    BPLIST_FILL = 0x0F,			/* will be used for length grabbing */
    BPLIST_UINT = 0x10,
    BPLIST_REAL = 0x20,
    BPLIST_DATE = 0x30,
    BPLIST_DATA = 0x40,
    BPLIST_STRING = 0x50,
    BPLIST_UNICODE = 0x60,
    BPLIST_UNK_0x70 = 0x70,
    BPLIST_UID = 0x80,
    BPLIST_ARRAY = 0xA0,
    BPLIST_SET = 0xC0,
    BPLIST_DICT = 0xD0,
    BPLIST_MASK = 0xF0
};

union plist_uint_ptr
{
    const void *src;
    uint8_t *u8ptr;
    uint16_t *u16ptr;
    uint32_t *u32ptr;
    uint64_t *u64ptr;
};

#define get_unaligned(ptr)			  \
  ({                                              \
    struct __attribute__((packed)) {		  \
      typeof(*(ptr)) __v;			  \
    } *__p = (void *) (ptr);			  \
    __p->__v;					  \
  })


static void byte_convert(uint8_t * address, size_t size)
{
#ifdef __LITTLE_ENDIAN__
    uint8_t i = 0, j = 0;
    uint8_t tmp = 0;

    for (i = 0; i < (size / 2); i++)
    {
        tmp = address[i];
        j = ((size - 1) + 0) - i;
        address[i] = address[j];
        address[j] = tmp;
    }
#endif
}

#ifndef bswap16
#define bswap16(x)   ((((x) & 0xFF00) >> 8) | (((x) & 0x00FF) << 8))
#endif

#ifndef bswap32
#define bswap32(x)   ((((x) & 0xFF000000) >> 24) \
                    | (((x) & 0x00FF0000) >>  8) \
                    | (((x) & 0x0000FF00) <<  8) \
                    | (((x) & 0x000000FF) << 24))
#endif

#ifndef bswap64
#define bswap64(x)   ((((x) & 0xFF00000000000000ull) >> 56) \
                    | (((x) & 0x00FF000000000000ull) >> 40) \
                    | (((x) & 0x0000FF0000000000ull) >> 24) \
                    | (((x) & 0x000000FF00000000ull) >>  8) \
                    | (((x) & 0x00000000FF000000ull) <<  8) \
                    | (((x) & 0x0000000000FF0000ull) << 24) \
                    | (((x) & 0x000000000000FF00ull) << 40) \
                    | (((x) & 0x00000000000000FFull) << 56))
#endif

#ifndef be16toh
#ifdef __BIG_ENDIAN__
#define be16toh(x) (x)
#else
#define be16toh(x) bswap16(x)
#endif
#endif

#ifndef be32toh
#ifdef __BIG_ENDIAN__
#define be32toh(x) (x)
#else
#define be32toh(x) bswap32(x)
#endif
#endif

#ifndef be64toh
#ifdef __BIG_ENDIAN__
#define be64toh(x) (x)
#else
#define be64toh(x) bswap64(x)
#endif
#endif

#ifdef __BIG_ENDIAN__
#define beNtoh(x,n) (x >> ((8-n) << 3))
#else
#define beNtoh(x,n) be64toh(x << ((8-n) << 3))
#endif

#define UINT_TO_HOST(x, n) \
	({ \
		union plist_uint_ptr __up; \
		__up.src = (n > 8) ? x + (n - 8) : x; \
		(n >= 8 ? be64toh( get_unaligned(__up.u64ptr) ) : \
		(n == 4 ? be32toh( get_unaligned(__up.u32ptr) ) : \
		(n == 2 ? be16toh( get_unaligned(__up.u16ptr) ) : \
                (n == 1 ? *__up.u8ptr : \
		beNtoh( get_unaligned(__up.u64ptr), n) \
		)))); \
	})

#define get_needed_bytes(x) \
		( ((uint64_t)x) < (1ULL << 8) ? 1 : \
		( ((uint64_t)x) < (1ULL << 16) ? 2 : \
		( ((uint64_t)x) < (1ULL << 24) ? 3 : \
		( ((uint64_t)x) < (1ULL << 32) ? 4 : 8))))

#define get_real_bytes(x) (x == (float) x ? sizeof(float) : sizeof(double))

#if (defined(__LITTLE_ENDIAN__) \
     && !defined(__FLOAT_WORD_ORDER__)) \
 || (defined(__FLOAT_WORD_ORDER__) \
     && __FLOAT_WORD_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define float_bswap64(x) bswap64(x)
#define float_bswap32(x) bswap32(x)
#else
#define float_bswap64(x) (x)
#define float_bswap32(x) (x)
#endif


#define NODE_IS_ROOT(x) (((node_t*)x)->isRoot)

struct bplist_data {
    const char* data;
    uint64_t size;
    uint64_t num_objects;
    uint8_t ref_size;
    uint8_t offset_size;
    const char* offset_table;
    uint32_t level;
    uint32_t *used_indexes;
};

static plist_t parse_bin_node_at_index(struct bplist_data *bplist, uint32_t node_index);

static plist_t parse_uint_node(const char **bnode, uint8_t size)
{
    plist_data_t data = plist_new_plist_data();

    size = 1 << size;			// make length less misleading
    switch (size)
    {
    case sizeof(uint8_t):
    case sizeof(uint16_t):
    case sizeof(uint32_t):
    case sizeof(uint64_t):
        data->length = sizeof(uint64_t);
        break;
    case 16:
        data->length = size;
        break;
    default:
        free(data);
        return NULL;
    };

    data->intval = UINT_TO_HOST(*bnode, size);

    (*bnode) += size;
    data->type = PLIST_UINT;

    return node_create(NULL, data);
}

static plist_t parse_real_node(const char **bnode, uint8_t size)
{
    plist_data_t data = plist_new_plist_data();
    uint8_t buf[8];

    size = 1 << size;			// make length less misleading
    switch (size)
    {
    case sizeof(uint32_t):
        *(uint32_t*)buf = float_bswap32(*(uint32_t*)*bnode);
        data->realval = *(float *) buf;
        break;
    case sizeof(uint64_t):
        *(uint64_t*)buf = float_bswap64(*(uint64_t*)*bnode);
        data->realval = *(double *) buf;
        break;
    default:
        free(data);
        return NULL;
    }
    data->type = PLIST_REAL;
    data->length = sizeof(double);

    return node_create(NULL, data);
}

static plist_t parse_date_node(const char **bnode, uint8_t size)
{
    plist_t node = parse_real_node(bnode, size);
    plist_data_t data = plist_get_data(node);

    data->type = PLIST_DATE;

    return node;
}

static plist_t parse_string_node(const char **bnode, uint64_t size)
{
    plist_data_t data = plist_new_plist_data();

    data->type = PLIST_STRING;
    data->strval = (char *) malloc(sizeof(char) * (size + 1));
    memcpy(data->strval, *bnode, size);
    data->strval[size] = '\0';
    data->length = strlen(data->strval);

    return node_create(NULL, data);
}

static char *plist_utf16_to_utf8(uint16_t *unistr, long len, long *items_read, long *items_written)
{
	if (!unistr || (len <= 0)) return NULL;
	char *outbuf = (char*)malloc(4*(len+1));
	int p = 0;
	int i = 0;

	uint16_t wc;
	uint32_t w;
	int read_lead_surrogate = 0; 

	while (i < len) {
		wc = unistr[i++];
		if (wc >= 0xD800 && wc <= 0xDBFF) {
			if (!read_lead_surrogate) {
				read_lead_surrogate = 1;
				w = 0x010000 + ((wc & 0x3FF) << 10);
			} else {
				// This is invalid, the next 16 bit char should be a trail surrogate. 
				// Handling error by skipping.
				read_lead_surrogate = 0;
			}
		} else if (wc >= 0xDC00 && wc <= 0xDFFF) {
			if (read_lead_surrogate) {
				read_lead_surrogate = 0;
				w = w | (wc & 0x3FF);
				outbuf[p++] = (char)(0xF0 + ((w >> 18) & 0x7));
				outbuf[p++] = (char)(0x80 + ((w >> 12) & 0x3F));
				outbuf[p++] = (char)(0x80 + ((w >> 6) & 0x3F));
				outbuf[p++] = (char)(0x80 + (w & 0x3F));
			} else {
				// This is invalid.  A trail surrogate should always follow a lead surrogate.
				// Handling error by skipping
			}
		} else if (wc >= 0x800) {
			outbuf[p++] = (char)(0xE0 + ((wc >> 12) & 0xF));
			outbuf[p++] = (char)(0x80 + ((wc >> 6) & 0x3F));
			outbuf[p++] = (char)(0x80 + (wc & 0x3F));
		} else if (wc >= 0x80) {
			outbuf[p++] = (char)(0xC0 + ((wc >> 6) & 0x1F));
			outbuf[p++] = (char)(0x80 + (wc & 0x3F));
		} else {
			outbuf[p++] = (char)(wc & 0x7F);
		}
	}
	if (items_read) {
		*items_read = i;
	}
	if (items_written) {
		*items_written = p;
	}
	outbuf[p] = 0;

	return outbuf;
}

static plist_t parse_unicode_node(const char **bnode, uint64_t size)
{
    plist_data_t data = plist_new_plist_data();
    uint64_t i = 0;
    uint16_t *unicodestr = NULL;
    char *tmpstr = NULL;
    long items_read = 0;
    long items_written = 0;

    data->type = PLIST_STRING;
    unicodestr = (uint16_t*) malloc(sizeof(uint16_t) * size);
    memcpy(unicodestr, *bnode, sizeof(uint16_t) * size);
    for (i = 0; i < size; i++)
        byte_convert((uint8_t *) (unicodestr + i), sizeof(uint16_t));

    tmpstr = plist_utf16_to_utf8(unicodestr, size, &items_read, &items_written);
    free(unicodestr);

    data->type = PLIST_STRING;
    data->strval = (char *) malloc(sizeof(char) * (items_written + 1));
    memcpy(data->strval, tmpstr, items_written);
    data->strval[items_written] = '\0';
    data->length = strlen(data->strval);
    free(tmpstr);
    return node_create(NULL, data);
}

static plist_t parse_data_node(const char **bnode, uint64_t size)
{
    plist_data_t data = plist_new_plist_data();

    data->type = PLIST_DATA;
    data->length = size;
    data->buff = (uint8_t *) malloc(sizeof(uint8_t) * size);
    memcpy(data->buff, *bnode, sizeof(uint8_t) * size);

    return node_create(NULL, data);
}

static plist_t parse_dict_node(struct bplist_data *bplist, const char** bnode, uint64_t size)
{
    uint64_t j;
    uint64_t str_i = 0, str_j = 0;
    uint64_t index1, index2;
    plist_data_t data = plist_new_plist_data();
    const char *const end_data = bplist->data + bplist->size;
    const char *index1_ptr = NULL;
    const char *index2_ptr = NULL;

    data->type = PLIST_DICT;
    data->length = size;

    plist_t node = node_create(NULL, data);

    for (j = 0; j < data->length; j++) {
        str_i = j * bplist->ref_size;
        str_j = (j + size) * bplist->ref_size;
        index1_ptr = (*bnode) + str_i;
        index2_ptr = (*bnode) + str_j;

        if ((index1_ptr < bplist->data || index1_ptr + bplist->ref_size >= end_data) ||
            (index2_ptr < bplist->data || index2_ptr + bplist->ref_size >= end_data)) {
            plist_free(node);
            return NULL;
        }

        index1 = UINT_TO_HOST(index1_ptr, bplist->ref_size);
        index2 = UINT_TO_HOST(index2_ptr, bplist->ref_size);

        if (index1 >= bplist->num_objects) {
            plist_free(node);
            return NULL;
        }
        if (index2 >= bplist->num_objects) {
            plist_free(node);
            return NULL;
        }

        /* process key node */
        plist_t key = parse_bin_node_at_index(bplist, index1);
        if (!key) {
            plist_free(node);
            return NULL;
        }

        if (plist_get_data(key)->type != PLIST_STRING) {
            fprintf(stderr, "ERROR: Malformed binary plist dict, invalid node type for key!\n");
            plist_free(node);
            return NULL;
        }

        /* enforce key type */
        plist_get_data(key)->type = PLIST_KEY;
        if (!plist_get_data(key)->strval) {
            fprintf(stderr, "ERROR: Malformed binary plist dict, invalid key node encountered!\n");
            plist_free(key);
            plist_free(node);
            return NULL;
        }

        /* process value node */
        plist_t val = parse_bin_node_at_index(bplist, index2);
        if (!val) {
            plist_free(key);
            plist_free(node);
            return NULL;
        }

        node_attach(node, key);
        node_attach(node, val);
    }

    return node;
}

static plist_t parse_array_node(struct bplist_data *bplist, const char** bnode, uint64_t size)
{
    uint64_t j;
    uint32_t str_j = 0;
    uint32_t index1;

    plist_data_t data = plist_new_plist_data();

    data->type = PLIST_ARRAY;
    data->length = size;

    plist_t node = node_create(NULL, data);

    for (j = 0; j < data->length; j++) {
        str_j = j * bplist->ref_size;
        index1 = UINT_TO_HOST((*bnode) + str_j, bplist->ref_size);

        if (index1 >= bplist->num_objects) {
            plist_free(node);
            return NULL;
        }

        /* process value node */
        plist_t val = parse_bin_node_at_index(bplist, index1);
        if (!val) {
            plist_free(node);
            return NULL;
        }

        node_attach(node, val);
    }

    return node;
}

static plist_t parse_uid_node(const char **bnode, uint8_t size)
{
    plist_data_t data = plist_new_plist_data();
    size = size + 1;
    data->intval = UINT_TO_HOST(*bnode, size);
    if (data->intval > UINT32_MAX) {
        free(data);
        return NULL;
    }

    (*bnode) += size;
    data->type = PLIST_UID;
    data->length = sizeof(uint64_t);

    return node_create(NULL, data);
}

static plist_t parse_bin_node(struct bplist_data *bplist, const char** object)
{
    uint16_t type = 0;
    uint64_t size = 0;

    if (!object)
        return NULL;

    type = (**object) & BPLIST_MASK;
    size = (**object) & BPLIST_FILL;
    (*object)++;

    switch (type)
    {

    case BPLIST_NULL:
        switch (size)
        {

        case BPLIST_TRUE:
        {
            plist_data_t data = plist_new_plist_data();
            data->type = PLIST_BOOLEAN;
            data->boolval = TRUE;
            data->length = 1;
            return node_create(NULL, data);
        }

        case BPLIST_FALSE:
        {
            plist_data_t data = plist_new_plist_data();
            data->type = PLIST_BOOLEAN;
            data->boolval = FALSE;
            data->length = 1;
            return node_create(NULL, data);
        }

        case BPLIST_NULL:
        default:
            return NULL;
        }

    case BPLIST_UINT:
        if (*object - bplist->data + (uint64_t)(1 << size) >= bplist->size)
            return NULL;
        return parse_uint_node(object, size);

    case BPLIST_REAL:
        if (*object - bplist->data + (uint64_t)(1 << size) >= bplist->size)
            return NULL;
        return parse_real_node(object, size);

    case BPLIST_DATE:
        if (3 != size)
            return NULL;
        if (*object - bplist->data + (uint64_t)(1 << size) >= bplist->size)
            return NULL;
        return parse_date_node(object, size);

    case BPLIST_DATA:
        if (BPLIST_FILL == size) {
            plist_t size_node = parse_bin_node(bplist, object);
            if (plist_get_node_type(size_node) != PLIST_UINT)
                return NULL;
            plist_get_uint_val(size_node, &size);
            plist_free(size_node);
        }

        if (*object - bplist->data + size >= bplist->size)
            return NULL;
        return parse_data_node(object, size);

    case BPLIST_STRING:
        if (BPLIST_FILL == size) {
            plist_t size_node = parse_bin_node(bplist, object);
            if (plist_get_node_type(size_node) != PLIST_UINT)
                return NULL;
            plist_get_uint_val(size_node, &size);
            plist_free(size_node);
        }

        if (*object - bplist->data + size >= bplist->size)
            return NULL;
        return parse_string_node(object, size);

    case BPLIST_UNICODE:
        if (BPLIST_FILL == size) {
            plist_t size_node = parse_bin_node(bplist, object);
            if (plist_get_node_type(size_node) != PLIST_UINT)
                return NULL;
            plist_get_uint_val(size_node, &size);
            plist_free(size_node);
        }

        if (*object - bplist->data + size * 2 >= bplist->size)
            return NULL;
        return parse_unicode_node(object, size);

    case BPLIST_SET:
    case BPLIST_ARRAY:
        if (BPLIST_FILL == size) {
            plist_t size_node = parse_bin_node(bplist, object);
            if (plist_get_node_type(size_node) != PLIST_UINT)
                return NULL;
            plist_get_uint_val(size_node, &size);
            plist_free(size_node);
        }

        if (*object - bplist->data + size >= bplist->size)
            return NULL;
        return parse_array_node(bplist, object, size);

    case BPLIST_UID:
        return parse_uid_node(object, size);

    case BPLIST_DICT:
        if (BPLIST_FILL == size) {
            plist_t size_node = parse_bin_node(bplist, object);
            if (plist_get_node_type(size_node) != PLIST_UINT)
                return NULL;
            plist_get_uint_val(size_node, &size);
            plist_free(size_node);
        }

        if (*object - bplist->data + size >= bplist->size)
            return NULL;
        return parse_dict_node(bplist, object, size);
    default:
        return NULL;
    }
    return NULL;
}

static plist_t parse_bin_node_at_index(struct bplist_data *bplist, uint32_t node_index)
{
    int i = 0;
    const char* ptr = NULL;
    plist_t plist = NULL;
    const char* idx_ptr = NULL;

    if (node_index > bplist->num_objects)
        return NULL;

    idx_ptr = bplist->offset_table + node_index * bplist->offset_size;
    if (idx_ptr < bplist->offset_table ||
        idx_ptr >= bplist->offset_table + bplist->num_objects * bplist->offset_size)
        return NULL;

    ptr = bplist->data + UINT_TO_HOST(idx_ptr, bplist->offset_size);
    /* make sure the node offset is in a sane range */
    if ((ptr < bplist->data) || (ptr >= bplist->offset_table)) {
        return NULL;
    }

    /* store node_index for current recursion level */
    bplist->used_indexes[bplist->level] = node_index;
    /* recursion check */
    if (bplist->level > 0) {
        for (i = bplist->level-1; i >= 0; i--) {
            if (bplist->used_indexes[i] == bplist->used_indexes[bplist->level]) {
                fprintf(stderr, "Recursion detected in binary plist. Aborting.\n");
                return NULL;
            }
        }
    }

    /* finally parse node */
    bplist->level++;
    plist = parse_bin_node(bplist, &ptr);
    bplist->level--;
    return plist;
}

PLIST_API void plist_from_bin(const char *plist_bin, uint32_t length, plist_t * plist)
{
    bplist_trailer_t *trailer = NULL;
    uint8_t offset_size = 0;
    uint8_t ref_size = 0;
    uint64_t num_objects = 0;
    uint64_t root_object = 0;
    char *offset_table = NULL;

    //first check we have enough data
    if (!(length >= BPLIST_MAGIC_SIZE + BPLIST_VERSION_SIZE + sizeof(bplist_trailer_t)))
        return;
    //check that plist_bin in actually a plist
    if (memcmp(plist_bin, BPLIST_MAGIC, BPLIST_MAGIC_SIZE) != 0)
        return;
    //check for known version
    if (memcmp(plist_bin + BPLIST_MAGIC_SIZE, BPLIST_VERSION, BPLIST_VERSION_SIZE) != 0)
        return;

    //now parse trailer
    trailer = (bplist_trailer_t*)(plist_bin + (length - sizeof(bplist_trailer_t)));

    offset_size = trailer->offset_size;
    ref_size = trailer->ref_size;
    num_objects = be64toh(trailer->num_objects);
    root_object = be64toh(trailer->root_object_index);
    offset_table = (char *)(plist_bin + be64toh(trailer->offset_table_offset));

    if (num_objects == 0)
        return;

    if (offset_size == 0)
        return;

    if (ref_size == 0)
        return;

    if (root_object >= num_objects)
        return;

    if (offset_table < plist_bin || offset_table >= plist_bin + length)
        return;

    if (offset_table + num_objects * offset_size >= plist_bin + length)
        return;

    if (sizeof(uint32_t) * num_objects < num_objects)
        return;

    struct bplist_data bplist;
    bplist.data = plist_bin;
    bplist.size = length;
    bplist.num_objects = num_objects;
    bplist.ref_size = ref_size;
    bplist.offset_size = offset_size;
    bplist.offset_table = offset_table;
    bplist.level = 0;
    bplist.used_indexes = (uint32_t*)malloc(sizeof(uint32_t) * num_objects);

    if (!bplist.used_indexes)
        return;

    *plist = parse_bin_node_at_index(&bplist, root_object);

    free(bplist.used_indexes);
}

static unsigned int plist_data_hash(const void* key)
{
    plist_data_t data = plist_get_data((plist_t) key);

    unsigned int hash = data->type;
    unsigned int i = 0;

    char *buff = NULL;
    unsigned int size = 0;

    switch (data->type)
    {
    case PLIST_BOOLEAN:
    case PLIST_UINT:
    case PLIST_REAL:
    case PLIST_DATE:
    case PLIST_UID:
        buff = (char *) &data->intval;	//works also for real as we use an union
        size = 8;
        break;
    case PLIST_KEY:
    case PLIST_STRING:
        buff = data->strval;
        size = data->length;
        break;
    case PLIST_DATA:
    case PLIST_ARRAY:
    case PLIST_DICT:
        //for these types only hash pointer
        buff = (char *) &key;
        size = sizeof(const void*);
        break;
    default:
        break;
    }

    // now perform hash using djb2 hashing algorithm
    // see: http://www.cse.yorku.ca/~oz/hash.html
    hash += 5381;
    for (i = 0; i < size; buff++, i++) {
        hash = ((hash << 5) + hash) + *buff;
    }

    return hash;
}

struct serialize_s
{
    ptrarray_t* objects;
    hashtable_t* ref_table;
};

static void serialize_plist(node_t* node, void* data)
{
    uint64_t *index_val = NULL;
    struct serialize_s *ser = (struct serialize_s *) data;
    uint64_t current_index = ser->objects->len;

    //first check that node is not yet in objects
    void* val = hash_table_lookup(ser->ref_table, node);
    if (val)
    {
        //data is already in table
        return;
    }
    //insert new ref
    index_val = (uint64_t *) malloc(sizeof(uint64_t));
    *index_val = current_index;
    hash_table_insert(ser->ref_table, node, index_val);

    //now append current node to object array
    ptr_array_add(ser->objects, node);

    //now recurse on children
    node_iterator_t *ni = node_iterator_create(node->children);
    node_t *ch;
    while ((ch = node_iterator_next(ni))) {
        serialize_plist(ch, data);
    }
    node_iterator_destroy(ni);

    return;
}

#define Log2(x) (x == 8 ? 3 : (x == 4 ? 2 : (x == 2 ? 1 : 0)))

static void write_int(bytearray_t * bplist, uint64_t val)
{
    int size = get_needed_bytes(val);
    uint8_t sz;
    //do not write 3bytes int node
    if (size == 3)
        size++;
    sz = BPLIST_UINT | Log2(size);

    val = be64toh(val);
    byte_array_append(bplist, &sz, 1);
    byte_array_append(bplist, (uint8_t*)&val + (8-size), size);
}

static void write_uint(bytearray_t * bplist, uint64_t val)
{
    uint8_t sz = BPLIST_UINT | 4;
    uint64_t zero = 0;

    val = be64toh(val);
    byte_array_append(bplist, &sz, 1);
    byte_array_append(bplist, &zero, sizeof(uint64_t));
    byte_array_append(bplist, &val, sizeof(uint64_t));
}

static void write_real(bytearray_t * bplist, double val)
{
    int size = get_real_bytes(val);	//cheat to know used space
    uint8_t buff[9];
    buff[0] = BPLIST_REAL | Log2(size);
    if (size == sizeof(float)) {
        float floatval = (float)val;
        *(uint32_t*)(buff+1) = float_bswap32(*(uint32_t*)&floatval);
    } else {
        *(uint64_t*)(buff+1) = float_bswap64(*(uint64_t*)&val);
    }
    byte_array_append(bplist, buff, size+1);
}

static void write_date(bytearray_t * bplist, double val)
{
    uint8_t buff[9];
    buff[0] = BPLIST_DATE | 3;
    *(uint64_t*)(buff+1) = float_bswap64(*(uint64_t*)&val);
    byte_array_append(bplist, buff, sizeof(buff));
}

static void write_raw_data(bytearray_t * bplist, uint8_t mark, uint8_t * val, uint64_t size)
{
    uint8_t *buff = NULL;
    uint8_t marker = mark | (size < 15 ? size : 0xf);
    byte_array_append(bplist, &marker, sizeof(uint8_t));
    if (size >= 15)
    {
        bytearray_t *int_buff = byte_array_new();
        write_int(int_buff, size);
        byte_array_append(bplist, int_buff->data, int_buff->len);
        byte_array_free(int_buff);
    }
    //stupid unicode buffer length
    if (BPLIST_UNICODE==mark) size *= 2;
    buff = (uint8_t *) malloc(size);
    memcpy(buff, val, size);
    byte_array_append(bplist, buff, size);
    free(buff);
}

static void write_data(bytearray_t * bplist, uint8_t * val, uint64_t size)
{
    write_raw_data(bplist, BPLIST_DATA, val, size);
}

static void write_string(bytearray_t * bplist, char *val)
{
    uint64_t size = strlen(val);
    write_raw_data(bplist, BPLIST_STRING, (uint8_t *) val, size);
}

static void write_unicode(bytearray_t * bplist, uint16_t * val, uint64_t size)
{
    uint64_t i = 0;
    uint64_t size2 = size * sizeof(uint16_t);
    uint8_t *buff = (uint8_t *) malloc(size2);
    memcpy(buff, val, size2);
    for (i = 0; i < size; i++)
        byte_convert(buff + i * sizeof(uint16_t), sizeof(uint16_t));
    write_raw_data(bplist, BPLIST_UNICODE, buff, size);
    free(buff);
}

static void write_array(bytearray_t * bplist, node_t* node, hashtable_t* ref_table, uint8_t ref_size)
{
    uint64_t idx = 0;
    uint8_t *buff = NULL;

    node_t* cur = NULL;
    uint64_t i = 0;

    uint64_t size = node_n_children(node);
    uint8_t marker = BPLIST_ARRAY | (size < 15 ? size : 0xf);
    byte_array_append(bplist, &marker, sizeof(uint8_t));
    if (size >= 15)
    {
        bytearray_t *int_buff = byte_array_new();
        write_int(int_buff, size);
        byte_array_append(bplist, int_buff->data, int_buff->len);
        byte_array_free(int_buff);
    }

    buff = (uint8_t *) malloc(size * ref_size);

    for (i = 0, cur = node_first_child(node); cur && i < size; cur = node_next_sibling(cur), i++)
    {
        idx = *(uint64_t *) (hash_table_lookup(ref_table, cur));
#ifdef __BIG_ENDIAN__
	idx = idx << ((sizeof(uint64_t) - ref_size) * 8);
#endif
        memcpy(buff + i * ref_size, &idx, ref_size);
        byte_convert(buff + i * ref_size, ref_size);
    }

    //now append to bplist
    byte_array_append(bplist, buff, size * ref_size);
    free(buff);

}

static void write_dict(bytearray_t * bplist, node_t* node, hashtable_t* ref_table, uint8_t ref_size)
{
    uint64_t idx1 = 0;
    uint64_t idx2 = 0;
    uint8_t *buff = NULL;

    node_t* cur = NULL;
    uint64_t i = 0;

    uint64_t size = node_n_children(node) / 2;
    uint8_t marker = BPLIST_DICT | (size < 15 ? size : 0xf);
    byte_array_append(bplist, &marker, sizeof(uint8_t));
    if (size >= 15)
    {
        bytearray_t *int_buff = byte_array_new();
        write_int(int_buff, size);
        byte_array_append(bplist, int_buff->data, int_buff->len);
        byte_array_free(int_buff);
    }

    buff = (uint8_t *) malloc(size * 2 * ref_size);
    for (i = 0, cur = node_first_child(node); cur && i < size; cur = node_next_sibling(node_next_sibling(cur)), i++)
    {
        idx1 = *(uint64_t *) (hash_table_lookup(ref_table, cur));
#ifdef __BIG_ENDIAN__
	idx1 = idx1 << ((sizeof(uint64_t) - ref_size) * 8);
#endif
        memcpy(buff + i * ref_size, &idx1, ref_size);
        byte_convert(buff + i * ref_size, ref_size);

        idx2 = *(uint64_t *) (hash_table_lookup(ref_table, cur->next));
#ifdef __BIG_ENDIAN__
	idx2 = idx2 << ((sizeof(uint64_t) - ref_size) * 8);
#endif
        memcpy(buff + (i + size) * ref_size, &idx2, ref_size);
        byte_convert(buff + (i + size) * ref_size, ref_size);
    }

    //now append to bplist
    byte_array_append(bplist, buff, size * 2 * ref_size);
    free(buff);

}

static void write_uid(bytearray_t * bplist, uint64_t val)
{
    val = (uint32_t)val;
    int size = get_needed_bytes(val);
    uint8_t sz;
    //do not write 3bytes int node
    if (size == 3)
        size++;
    sz = BPLIST_UID | (size-1); // yes, this is what Apple does...

    val = be64toh(val);
    byte_array_append(bplist, &sz, 1);
    byte_array_append(bplist, (uint8_t*)&val + (8-size), size);
}

static int is_ascii_string(char* s, int len)
{
  int ret = 1, i = 0;
  for(i = 0; i < len; i++)
  {
      if ( !isascii( s[i] ) )
      {
          ret = 0;
          break;
      }
  }
  return ret;
}

static uint16_t *plist_utf8_to_utf16(char *unistr, long size, long *items_read, long *items_written)
{
	uint16_t *outbuf = (uint16_t*)malloc(((size*2)+1)*sizeof(uint16_t));
	int p = 0;
	int i = 0;

	unsigned char c0;
	unsigned char c1;
	unsigned char c2;
	unsigned char c3;

	uint32_t w;

	while (i < size) {
		c0 = unistr[i];
		c1 = (i < size-1) ? unistr[i+1] : 0;
		c2 = (i < size-2) ? unistr[i+2] : 0;
		c3 = (i < size-3) ? unistr[i+3] : 0;
		if ((c0 >= 0xF0) && (i < size-3) && (c1 >= 0x80) && (c2 >= 0x80) && (c3 >= 0x80)) {
			// 4 byte sequence.  Need to generate UTF-16 surrogate pair
			w = ((((c0 & 7) << 18) + ((c1 & 0x3F) << 12) + ((c2 & 0x3F) << 6) + (c3 & 0x3F)) & 0x1FFFFF) - 0x010000;
			outbuf[p++] = 0xD800 + (w >> 10);
			outbuf[p++] = 0xDC00 + (w & 0x3FF);
			i+=4;
		} else if ((c0 >= 0xE0) && (i < size-2) && (c1 >= 0x80) && (c2 >= 0x80)) {
			// 3 byte sequence
			outbuf[p++] = ((c2 & 0x3F) + ((c1 & 3) << 6)) + (((c1 >> 2) & 15) << 8) + ((c0 & 15) << 12);
			i+=3;
		} else if ((c0 >= 0xC0) && (i < size-1) && (c1 >= 0x80)) {
			// 2 byte sequence
			outbuf[p++] = ((c1 & 0x3F) + ((c0 & 3) << 6)) + (((c0 >> 2) & 7) << 8);
			i+=2;
		} else if (c0 < 0x80) {
			// 1 byte sequence
			outbuf[p++] = c0;
			i+=1;
		} else {
			// invalid character
			fprintf(stderr, "invalid utf8 sequence in string at index %d\n", i);
			break;
		}
	}
	if (items_read) {
		*items_read = i;
	}
	if (items_written) {
		*items_written = p;
	}
	outbuf[p] = 0;

	return outbuf;

}

PLIST_API void plist_to_bin(plist_t plist, char **plist_bin, uint32_t * length)
{
    ptrarray_t* objects = NULL;
    hashtable_t* ref_table = NULL;
    struct serialize_s ser_s;
    uint8_t offset_size = 0;
    uint8_t ref_size = 0;
    uint64_t num_objects = 0;
    uint64_t root_object = 0;
    uint64_t offset_table_index = 0;
    bytearray_t *bplist_buff = NULL;
    uint64_t i = 0;
    uint8_t *buff = NULL;
    uint64_t *offsets = NULL;
    bplist_trailer_t trailer;
    //for string
    long len = 0;
    long items_read = 0;
    long items_written = 0;
    uint16_t *unicodestr = NULL;
    uint64_t objects_len = 0;
    uint64_t buff_len = 0;

    //check for valid input
    if (!plist || !plist_bin || *plist_bin || !length)
        return;

    //list of objects
    objects = ptr_array_new(256);
    //hashtable to write only once same nodes
    ref_table = hash_table_new(plist_data_hash, plist_data_compare, free);

    //serialize plist
    ser_s.objects = objects;
    ser_s.ref_table = ref_table;
    serialize_plist(plist, &ser_s);

    //now stream to output buffer
    offset_size = 0;			//unknown yet
    objects_len = objects->len;
    ref_size = get_needed_bytes(objects_len);
    num_objects = objects->len;
    root_object = 0;			//root is first in list
    offset_table_index = 0;		//unknown yet

    //setup a dynamic bytes array to store bplist in
    bplist_buff = byte_array_new();

    //set magic number and version
    byte_array_append(bplist_buff, BPLIST_MAGIC, BPLIST_MAGIC_SIZE);
    byte_array_append(bplist_buff, BPLIST_VERSION, BPLIST_VERSION_SIZE);

    //write objects and table
    offsets = (uint64_t *) malloc(num_objects * sizeof(uint64_t));
    for (i = 0; i < num_objects; i++)
    {

        plist_data_t data = plist_get_data(ptr_array_index(objects, i));
        offsets[i] = bplist_buff->len;

        switch (data->type)
        {
        case PLIST_BOOLEAN:
            buff = (uint8_t *) malloc(sizeof(uint8_t));
            buff[0] = data->boolval ? BPLIST_TRUE : BPLIST_FALSE;
            byte_array_append(bplist_buff, buff, sizeof(uint8_t));
            free(buff);
            break;

        case PLIST_UINT:
            if (data->length == 16) {
                write_uint(bplist_buff, data->intval);
            } else {
                write_int(bplist_buff, data->intval);
            }
            break;

        case PLIST_REAL:
            write_real(bplist_buff, data->realval);
            break;

        case PLIST_KEY:
        case PLIST_STRING:
            len = strlen(data->strval);
            if ( is_ascii_string(data->strval, len) )
            {
                write_string(bplist_buff, data->strval);
            }
            else
            {
                unicodestr = plist_utf8_to_utf16(data->strval, len, &items_read, &items_written);
                write_unicode(bplist_buff, unicodestr, items_written);
                free(unicodestr);
            }
            break;
        case PLIST_DATA:
            write_data(bplist_buff, data->buff, data->length);
        case PLIST_ARRAY:
            write_array(bplist_buff, ptr_array_index(objects, i), ref_table, ref_size);
            break;
        case PLIST_DICT:
            write_dict(bplist_buff, ptr_array_index(objects, i), ref_table, ref_size);
            break;
        case PLIST_DATE:
            write_date(bplist_buff, data->realval);
            break;
        case PLIST_UID:
            write_uid(bplist_buff, data->intval);
            break;
        default:
            break;
        }
    }

    //free intermediate objects
    ptr_array_free(objects);
    hash_table_destroy(ref_table);

    //write offsets
    buff_len = bplist_buff->len;
    offset_size = get_needed_bytes(buff_len);
    offset_table_index = bplist_buff->len;
    for (i = 0; i < num_objects; i++)
    {
        uint8_t *offsetbuff = (uint8_t *) malloc(offset_size);

#ifdef __BIG_ENDIAN__
	offsets[i] = offsets[i] << ((sizeof(uint64_t) - offset_size) * 8);
#endif

        memcpy(offsetbuff, &offsets[i], offset_size);
        byte_convert(offsetbuff, offset_size);
        byte_array_append(bplist_buff, offsetbuff, offset_size);
        free(offsetbuff);
    }

    //setup trailer
    memset(trailer.unused, '\0', sizeof(trailer.unused));
    trailer.offset_size = offset_size;
    trailer.ref_size = ref_size;
    trailer.num_objects = be64toh(num_objects);
    trailer.root_object_index = be64toh(root_object);
    trailer.offset_table_offset = be64toh(offset_table_index);

    byte_array_append(bplist_buff, &trailer, sizeof(bplist_trailer_t));

    //duplicate buffer
    *plist_bin = (char *) malloc(bplist_buff->len);
    memcpy(*plist_bin, bplist_buff->data, bplist_buff->len);
    *length = bplist_buff->len;

    byte_array_free(bplist_buff);
    free(offsets);
}
