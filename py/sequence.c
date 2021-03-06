/*
 * This file is part of the Micro Python project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2014 Paul Sokolovsky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "mpconfig.h"
#include "nlr.h"
#include "misc.h"
#include "qstr.h"
#include "obj.h"
#include "runtime0.h"
#include "runtime.h"

// Helpers for sequence types

#define SWAP(type, var1, var2) { type t = var2; var2 = var1; var1 = t; }

// Implements backend of sequence * integer operation. Assumes elements are
// memory-adjacent in sequence.
void mp_seq_multiply(const void *items, uint item_sz, uint len, uint times, void *dest) {
    for (int i = 0; i < times; i++) {
        uint copy_sz = item_sz * len;
        memcpy(dest, items, copy_sz);
        dest = (char*)dest + copy_sz;
    }
}

bool mp_seq_get_fast_slice_indexes(machine_uint_t len, mp_obj_t slice, machine_uint_t *begin, machine_uint_t *end) {
    mp_obj_t ostart, ostop, ostep;
    machine_int_t start, stop;
    mp_obj_slice_get(slice, &ostart, &ostop, &ostep);
    if (ostep != mp_const_none && ostep != MP_OBJ_NEW_SMALL_INT(1)) {
        nlr_raise(mp_obj_new_exception_msg(&mp_type_NotImplementedError,
            "Only slices with step=1 (aka None) are supported"));
        return false;
    }

    if (ostart == mp_const_none) {
        start = 0;
    } else {
        start = MP_OBJ_SMALL_INT_VALUE(ostart);
    }
    if (ostop == mp_const_none) {
        stop = len;
    } else {
        stop = MP_OBJ_SMALL_INT_VALUE(ostop);
    }

    // Unlike subscription, out-of-bounds slice indexes are never error
    if (start < 0) {
        start = len + start;
        if (start < 0) {
            start = 0;
        }
    } else if (start > len) {
        start = len;
    }
    if (stop < 0) {
        stop = len + stop;
        // CPython returns empty sequence in such case
        if (stop < 0) {
            stop = start;
        }
    } else if (stop > len) {
        stop = len;
    }

    // CPython returns empty sequence in such case, or point for assignment is at start
    if (start > stop) {
        stop = start;
    }

    *begin = start;
    *end = stop;
    return true;
}

// Special-case comparison function for sequences of bytes
// Don't pass MP_BINARY_OP_NOT_EQUAL here
bool mp_seq_cmp_bytes(int op, const byte *data1, uint len1, const byte *data2, uint len2) {
    if (op == MP_BINARY_OP_EQUAL && len1 != len2) {
        return false;
    }

    // Let's deal only with > & >=
    if (op == MP_BINARY_OP_LESS || op == MP_BINARY_OP_LESS_EQUAL) {
        SWAP(const byte*, data1, data2);
        SWAP(uint, len1, len2);
        if (op == MP_BINARY_OP_LESS) {
            op = MP_BINARY_OP_MORE;
        } else {
            op = MP_BINARY_OP_MORE_EQUAL;
        }
    }
    uint min_len = len1 < len2 ? len1 : len2;
    int res = memcmp(data1, data2, min_len);
    if (op == MP_BINARY_OP_EQUAL) {
        // If we are checking for equality, here're the answer
        return res == 0;
    }
    if (res < 0) {
        return false;
    }
    if (res > 0) {
        return true;
    }

    // If we had tie in the last element...
    // ... and we have lists of different lengths...
    if (len1 != len2) {
        if (len1 < len2) {
            // ... then longer list length wins (we deal only with >)
            return false;
        }
    } else if (op == MP_BINARY_OP_MORE) {
        // Otherwise, if we have strict relation, equality means failure
        return false;
    }
    return true;
}

// Special-case comparison function for sequences of mp_obj_t
// Don't pass MP_BINARY_OP_NOT_EQUAL here
bool mp_seq_cmp_objs(int op, const mp_obj_t *items1, uint len1, const mp_obj_t *items2, uint len2) {
    if (op == MP_BINARY_OP_EQUAL && len1 != len2) {
        return false;
    }

    // Let's deal only with > & >=
    if (op == MP_BINARY_OP_LESS || op == MP_BINARY_OP_LESS_EQUAL) {
        SWAP(const mp_obj_t *, items1, items2);
        SWAP(uint, len1, len2);
        if (op == MP_BINARY_OP_LESS) {
            op = MP_BINARY_OP_MORE;
        } else {
            op = MP_BINARY_OP_MORE_EQUAL;
        }
    }

    int len = len1 < len2 ? len1 : len2;
    for (int i = 0; i < len; i++) {
        // If current elements equal, can't decide anything - go on
        if (mp_obj_equal(items1[i], items2[i])) {
            continue;
        }

        // Othewise, if they are not equal, we can have final decision based on them
        if (op == MP_BINARY_OP_EQUAL) {
            // In particular, if we are checking for equality, here're the answer
            return false;
        }

        // Otherwise, application of relation op gives the answer
        return (mp_binary_op(op, items1[i], items2[i]) == mp_const_true);
    }

    // If we had tie in the last element...
    // ... and we have lists of different lengths...
    if (len1 != len2) {
        if (len1 < len2) {
            // ... then longer list length wins (we deal only with >)
            return false;
        }
    } else if (op == MP_BINARY_OP_MORE) {
        // Otherwise, if we have strict relation, sequence equality means failure
        return false;
    }

    return true;
}

// Special-case of index() which searches for mp_obj_t
mp_obj_t mp_seq_index_obj(const mp_obj_t *items, uint len, uint n_args, const mp_obj_t *args) {
    mp_obj_type_t *type = mp_obj_get_type(args[0]);
    mp_obj_t *value = args[1];
    uint start = 0;
    uint stop = len;

    if (n_args >= 3) {
        start = mp_get_index(type, len, args[2], true);
        if (n_args >= 4) {
            stop = mp_get_index(type, len, args[3], true);
        }
    }

    for (machine_uint_t i = start; i < stop; i++) {
        if (mp_obj_equal(items[i], value)) {
            // Common sense says this cannot overflow small int
            return MP_OBJ_NEW_SMALL_INT(i);
        }
    }

    nlr_raise(mp_obj_new_exception_msg(&mp_type_ValueError, "object not in sequence"));
}

mp_obj_t mp_seq_count_obj(const mp_obj_t *items, uint len, mp_obj_t value) {
    machine_uint_t count = 0;
    for (uint i = 0; i < len; i++) {
         if (mp_obj_equal(items[i], value)) {
              count++;
         }
    }

    // Common sense says this cannot overflow small int
    return MP_OBJ_NEW_SMALL_INT(count);
}
