/*
 * Copyright (c) 2020 Fastly, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "test.h"
#include "quicly/issued_cid.h"

#define NUM_CIDS 4

static void encrypt_cid(struct st_quicly_cid_encryptor_t *self, quicly_cid_t *encrypted, void *stateless_reset_token,
                        const quicly_cid_plaintext_t *plaintext)
{
    encrypted->cid[0] = plaintext->path_id;
    encrypted->len = 1;
}

static size_t decrypt_cid(struct st_quicly_cid_encryptor_t *self, quicly_cid_plaintext_t *plaintext, const void *encrypted,
                          size_t len)
{
    plaintext->path_id = ((const uint8_t *)encrypted)[0];
    return 1;
}

static quicly_cid_encryptor_t test_encryptor = {
    .encrypt_cid = encrypt_cid,
    .decrypt_cid = decrypt_cid,
};

/**
 * checks if the values within given CID are correct
 *
 * @return zero if okay
 */
static int verify_cid(const quicly_issued_cid_t *cid, quicly_cid_encryptor_t *encryptor)
{
    quicly_cid_plaintext_t plaintext;
    if (cid->state == QUICLY_ISSUED_CID_STATE_IDLE)
        return 0;
    if (encryptor == NULL)
        return 0;

    encryptor->decrypt_cid(encryptor, &plaintext, cid->cid.cid, cid->cid.len);
    return !(cid->sequence == plaintext.path_id);
}

/**
 * checks two properties
 * 1. PENDING CIDs are in front of the array
 * 2. each CID's values are not corrupted
 *
 * @return zero if okay
 */
static int verify_array(const quicly_issued_cid_set_t *set)
{
    int allow_pending = 1;
    for (size_t i = 0; i < set->_size; i++) {
        if (allow_pending) {
            if (set->cids[i].state != QUICLY_ISSUED_CID_STATE_PENDING)
                allow_pending = 0;
        } else if (set->cids[i].state == QUICLY_ISSUED_CID_STATE_PENDING) {
            return 1;
        }
        if (verify_cid(&set->cids[i], set->_encryptor) != 0)
            return 1;
    }

    return 0;
}

static size_t num_pending(const quicly_issued_cid_set_t *set)
{
    size_t num = 0;
    for (size_t i = 0; i < PTLS_ELEMENTSOF(set->cids); i++) {
        if (set->cids[i].state == QUICLY_ISSUED_CID_STATE_PENDING)
            num++;
    }
    return num;
}

/**
 * verifies that specified sequence with the specified state appears only once in the array
 */
static int exists_once(const quicly_issued_cid_set_t *set, uint64_t sequence, enum en_quicly_issued_cid_state_t state)
{
    size_t occurrence = 0;
    for (size_t i = 0; i < set->_size; i++) {
        if (set->cids[i].sequence == sequence) {
            if (set->cids[i].state != state)
                return 0;
            if (occurrence > 0)
                return 0;
            occurrence++;
        }
    }

    return occurrence == 1;
}

void test_issued_cid(void)
{
    PTLS_BUILD_ASSERT(QUICLY_LOCAL_ACTIVE_CONNECTION_ID_LIMIT >= NUM_CIDS);
    quicly_issued_cid_set_t set;
    quicly_cid_plaintext_t cid_plaintext = {0};

    /* initialize */
    quicly_issued_cid_init(&set, &test_encryptor, &cid_plaintext);
    ok(verify_array(&set) == 0);
    ok(num_pending(&set) == 0);
    ok(exists_once(&set, 0, QUICLY_ISSUED_CID_STATE_DELIVERED));
    cid_plaintext.path_id = 1;

    ok(quicly_issued_cid_set_size(&set, NUM_CIDS) != 0);
    ok(verify_array(&set) == 0);
    ok(num_pending(&set) == NUM_CIDS - 1);
    ok(exists_once(&set, 0, QUICLY_ISSUED_CID_STATE_DELIVERED));
    ok(exists_once(&set, 1, QUICLY_ISSUED_CID_STATE_PENDING));
    ok(exists_once(&set, 2, QUICLY_ISSUED_CID_STATE_PENDING));
    ok(exists_once(&set, 3, QUICLY_ISSUED_CID_STATE_PENDING));

    /* send three PENDING CIDs */
    quicly_issued_cid_on_sent(&set, NUM_CIDS - 1);
    ok(verify_array(&set) == 0);
    ok(exists_once(&set, 1, QUICLY_ISSUED_CID_STATE_INFLIGHT));
    ok(exists_once(&set, 2, QUICLY_ISSUED_CID_STATE_INFLIGHT));
    ok(exists_once(&set, 3, QUICLY_ISSUED_CID_STATE_INFLIGHT));

    quicly_issued_cid_on_acked(&set, 1);
    quicly_issued_cid_on_acked(&set, 3);
    ok(quicly_issued_cid_on_lost(&set, 2) != 0); /* simulate a packet loss */
    ok(verify_array(&set) == 0);
    ok(num_pending(&set) == 1);
    ok(exists_once(&set, 1, QUICLY_ISSUED_CID_STATE_DELIVERED));
    ok(exists_once(&set, 2, QUICLY_ISSUED_CID_STATE_PENDING));
    ok(exists_once(&set, 3, QUICLY_ISSUED_CID_STATE_DELIVERED));

    /* retransmit sequence=2 */
    quicly_issued_cid_on_sent(&set, 1);
    ok(num_pending(&set) == 0);

    /* retire everything */
    ok(quicly_issued_cid_retire(&set, 0) != 0);
    ok(quicly_issued_cid_retire(&set, 1) != 0);
    ok(quicly_issued_cid_retire(&set, 2) != 0);
    ok(quicly_issued_cid_retire(&set, 3) != 0);
    ok(num_pending(&set) == 4);
    /* partial send */
    quicly_issued_cid_on_sent(&set, 1);
    ok(verify_array(&set) == 0);
    ok(num_pending(&set) == 3);
    ok(exists_once(&set, 4, QUICLY_ISSUED_CID_STATE_INFLIGHT));
    ok(exists_once(&set, 5, QUICLY_ISSUED_CID_STATE_PENDING));
    ok(exists_once(&set, 6, QUICLY_ISSUED_CID_STATE_PENDING));
    ok(exists_once(&set, 7, QUICLY_ISSUED_CID_STATE_PENDING));

    /* retire one in the middle of PENDING CIDs */
    ok(quicly_issued_cid_retire(&set, 6) != 0);
    ok(verify_array(&set) == 0);

    quicly_issued_cid_on_sent(&set, 2);
    ok(quicly_issued_cid_on_lost(&set, 4) != 0);
    quicly_issued_cid_on_acked(&set, 4); /* simulate late ack */
    quicly_issued_cid_on_acked(&set, 5);
    quicly_issued_cid_on_acked(&set, 5); /* simulate duplicate ack */
    ok(exists_once(&set, 4, QUICLY_ISSUED_CID_STATE_DELIVERED));
    ok(exists_once(&set, 5, QUICLY_ISSUED_CID_STATE_DELIVERED));

    /* create a set with a NULL CID encryptor */
    quicly_issued_cid_set_t empty_set;
    quicly_issued_cid_init(&empty_set, NULL, NULL);
    ok(quicly_issued_cid_set_size(&empty_set, NUM_CIDS) == 0);
    ok(quicly_issued_cid_is_empty(&empty_set));
}
