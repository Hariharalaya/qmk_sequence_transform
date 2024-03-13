// Copyright 2021 Google LLC
// Copyright 2021 @filterpaper
// Copyright 2023 Pablo Martinez (@elpekenin) <elpekenin@elpekenin.dev>
// Copyright 2024 Guillaume Stordeur <guillaume.stordeur@gmail.com>
// Copyright 2024 Matt Skalecki <ikcelaks@gmail.com>
// Copyright 2024 QKekos <q.kekos.q@gmail.com>
// SPDX-License-Identifier: Apache-2.0
// Original source/inspiration: https://getreuer.info/posts/keyboards/autocorrection

#include "qmk_wrapper.h"
#include "keybuffer.h"
#include "key_stack.h"
#include "trie.h"
#include "utils.h"
#include "cursor.h"

#define CDATA(L) pgm_read_byte(&trie->completions[L])

//////////////////////////////////////////////////////////////////
void st_cursor_init(st_cursor_t *cursor, int history, uint8_t as_output_buffer)
{
    cursor->cursor_pos.index = history;
    cursor->cursor_pos.as_output_buffer = as_output_buffer;
    cursor->cursor_pos.sub_index = as_output_buffer ? 0 : 255;
    cursor->cursor_pos.segment_len = 1;
    cursor->cache_valid = false;
}
//////////////////////////////////////////////////////////////////
uint16_t st_cursor_get_keycode(st_cursor_t *cursor)
{
    const st_trie_t *trie = cursor->trie;
    const st_key_action_t *keyaction = st_key_buffer_get(cursor->buffer, cursor->cursor_pos.index);
    if (!keyaction) {
        return KC_NO;
    }
    if (cursor->cursor_pos.as_output_buffer &&
        keyaction->action_taken != ST_DEFAULT_KEY_ACTION) {
        const st_trie_payload_t *action = st_cursor_get_action(cursor);
        int index = action->completion_index;
        index += action->completion_len - 1 - cursor->cursor_pos.sub_index;
        return st_char_to_keycode(CDATA(index));
    } else {
        return keyaction->keypressed;
    }
}
//////////////////////////////////////////////////////////////////
st_trie_payload_t *st_cursor_get_action(st_cursor_t *cursor)
{
    st_trie_payload_t *action = &cursor->cached_action;
    if (cursor->cache_valid) {
        return action;
    }
    const st_key_action_t *keyaction = st_key_buffer_get(cursor->buffer, cursor->cursor_pos.index);
    if (!keyaction) {
        return NULL;
    }
    if (keyaction->action_taken == ST_DEFAULT_KEY_ACTION) {
        action->completion_index = ST_DEFAULT_KEY_ACTION;
        action->completion_len = 1;
        action->num_backspaces = 0;
        action->func_code = 0;
    } else {
        st_get_payload_from_match_index(cursor->trie, action, keyaction->action_taken);
    }
    cursor->cache_valid = true;
    return action;
}
//////////////////////////////////////////////////////////////////
bool st_cursor_next(st_cursor_t *cursor)
{
    if (!cursor->cursor_pos.as_output_buffer) {
        ++cursor->cursor_pos.index;
        ++cursor->cursor_pos.segment_len;
        cursor->cache_valid = false;
        return cursor->cursor_pos.index < cursor->buffer->context_len;
    }
    // Continue processing if simulating output buffer
    st_key_action_t *keyaction = st_key_buffer_get(cursor->buffer, cursor->cursor_pos.index);
    if (!keyaction) {
        return false;
    }
    if (keyaction->action_taken == ST_DEFAULT_KEY_ACTION) {
        // This is a normal keypress to consume
        if (++cursor->cursor_pos.index >= cursor->buffer->context_len) {
            return false;
        }
        cursor->cache_valid = false;
        cursor->cursor_pos.sub_index = 0;
        ++cursor->cursor_pos.segment_len;
        return true;
    }
    st_trie_payload_t *action = st_cursor_get_action(cursor);
    if (cursor->cursor_pos.sub_index < action->completion_len - 1) {
        ++cursor->cursor_pos.sub_index;
        ++cursor->cursor_pos.segment_len;
        return true;
    }
    // We have exhausted the key_action at the current buffer index
    // check if we need to fast-forward over any backspaced chars
    int backspaces = action->num_backspaces;
    while (true) {
        // move to next key in buffer
        if (++cursor->cursor_pos.index >= cursor->buffer->context_len) {
            return false;
        }
        cursor->cache_valid = false;
        keyaction = st_key_buffer_get(cursor->buffer, cursor->cursor_pos.index);
        if (!keyaction) {
            // We reached the end without finding the next output key
            return false;
        }
        if (keyaction->action_taken == ST_DEFAULT_KEY_ACTION) {
            if (backspaces == 0) {
                // This is a real keypress and no more backspaces to consume
                cursor->cursor_pos.sub_index = 0;
                ++cursor->cursor_pos.segment_len;
                return true;
            }
            // consume one backspace
            --backspaces;
            continue;
        }
        // Load payload of key that performed action
        action = st_cursor_get_action(cursor);
        if (backspaces < action->completion_len) {
            // This action contains the next output key. Find it's sub_pos and return true
            cursor->cursor_pos.sub_index = backspaces;
            ++cursor->cursor_pos.segment_len;
            return true;
        }
        backspaces -= action->completion_len - action->num_backspaces;
    }
}
//////////////////////////////////////////////////////////////////
bool st_cursor_move_to_history(st_cursor_t *cursor, int history, uint8_t as_output_buffer)
{
    // invalidate cache
    cursor->cache_valid = false;
    cursor->cursor_pos.index = history;
    cursor->cursor_pos.sub_index = 0;
    cursor->cursor_pos.as_output_buffer = as_output_buffer;
    return history < cursor->buffer->context_len;
}
//////////////////////////////////////////////////////////////////
st_cursor_pos_t st_cursor_save(const st_cursor_t *cursor)
{
    return cursor->cursor_pos;
}
//////////////////////////////////////////////////////////////////
void st_cursor_restore(st_cursor_t *cursor, st_cursor_pos_t *cursor_pos)
{
    cursor->cursor_pos = *cursor_pos;
    cursor->cache_valid = false;
}
//////////////////////////////////////////////////////////////////
bool st_cursor_longer_than(const st_cursor_t *cursor, const st_cursor_pos_t *past_pos)
{
    const int cur_pos = (cursor->cursor_pos.index << 8)
        + cursor->cursor_pos.sub_index;
    const int old_pos = (past_pos->index << 8)
        + past_pos->sub_index;
    return cur_pos > old_pos;
}
//////////////////////////////////////////////////////////////////
void st_cursor_print(st_cursor_t *cursor)
{
// #ifdef SEQUENCE_TRANSFORM_LOG_GENERAL
    st_cursor_pos_t cursor_pos = st_cursor_save(cursor);
    uprintf("cursor: |");
    while (cursor->cursor_pos.index < cursor->buffer->context_len) {
        uprintf("%c", st_keycode_to_char(st_cursor_get_keycode(cursor)));
        st_cursor_next(cursor);
    }
    uprintf("| (%d:%d)\n", cursor->buffer->context_len, cursor->cursor_pos.segment_len);
    st_cursor_restore(cursor, &cursor_pos);
// #endif
}
//////////////////////////////////////////////////////////////////
bool st_cursor_push_to_stack(st_cursor_t *cursor, int count)
{
    cursor->trie->key_stack->size = 0;
    for (; count > 0; --count, st_cursor_next(cursor)) {
        const uint16_t keycode = st_cursor_get_keycode(cursor);
        if (!keycode) {
            return false;
        }
        st_key_stack_push(cursor->trie->key_stack, keycode);
    }
    return true;
}