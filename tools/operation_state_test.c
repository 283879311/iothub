#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "operation_state.h"

static void test_transitions(void)
{
    assert(app_op_transition_allowed(APP_OP_FREE, APP_OP_ACCEPTED));
    assert(!app_op_transition_allowed(APP_OP_FREE, APP_OP_CONNECTING));

    assert(app_op_transition_allowed(APP_OP_ACCEPTED, APP_OP_CONNECTING));
    assert(app_op_transition_allowed(APP_OP_ACCEPTED, APP_OP_CANCELLED));
    assert(app_op_transition_allowed(APP_OP_ACCEPTED, APP_OP_EXPIRED));
    assert(!app_op_transition_allowed(APP_OP_ACCEPTED, APP_OP_GOT_IP));
    assert(!app_op_transition_allowed(APP_OP_ACCEPTED, APP_OP_SUCCESS));

    assert(app_op_transition_allowed(APP_OP_CONNECTING, APP_OP_GOT_IP));
    assert(app_op_transition_allowed(APP_OP_CONNECTING, APP_OP_FAILED));
    assert(app_op_transition_allowed(APP_OP_CONNECTING, APP_OP_CANCELLED));

    assert(app_op_transition_allowed(APP_OP_GOT_IP, APP_OP_COMMITTING));
    assert(app_op_transition_allowed(APP_OP_GOT_IP, APP_OP_CANCELLED));
    assert(!app_op_transition_allowed(APP_OP_GOT_IP, APP_OP_SUCCESS));

    assert(app_op_transition_allowed(APP_OP_COMMITTING, APP_OP_SUCCESS));
    assert(app_op_transition_allowed(APP_OP_COMMITTING, APP_OP_FAILED));
    assert(!app_op_transition_allowed(APP_OP_COMMITTING, APP_OP_CANCELLED));

    assert(app_op_transition_allowed(APP_OP_SUCCESS, APP_OP_FREE));
    assert(!app_op_transition_allowed(APP_OP_SUCCESS, APP_OP_CANCELLED));
    assert(!app_op_transition_allowed(APP_OP_FAILED, APP_OP_SUCCESS));
}

static void test_predicates(void)
{
    assert(app_op_state_is_active(APP_OP_ACCEPTED));
    assert(app_op_state_is_active(APP_OP_CONNECTING));
    assert(app_op_state_is_active(APP_OP_GOT_IP));
    assert(app_op_state_is_active(APP_OP_COMMITTING));
    assert(!app_op_state_is_active(APP_OP_SUCCESS));
    assert(!app_op_state_is_active(APP_OP_FREE));

    assert(app_op_state_is_terminal(APP_OP_SUCCESS));
    assert(app_op_state_is_terminal(APP_OP_FAILED));
    assert(app_op_state_is_terminal(APP_OP_CANCELLED));
    assert(app_op_state_is_terminal(APP_OP_EXPIRED));
    assert(!app_op_state_is_terminal(APP_OP_GOT_IP));

    assert(app_op_state_can_cancel(APP_OP_ACCEPTED));
    assert(app_op_state_can_cancel(APP_OP_CONNECTING));
    assert(app_op_state_can_cancel(APP_OP_GOT_IP));
    assert(!app_op_state_can_cancel(APP_OP_COMMITTING));
    assert(!app_op_state_can_cancel(APP_OP_SUCCESS));
    assert(!app_op_state_can_cancel(APP_OP_FAILED));
}

int main(void)
{
    test_transitions();
    test_predicates();
    printf("operation_state_test: PASS\n");
    return 0;
}
