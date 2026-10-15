/*
 * board_id.c - which board this is: the CrowPanel Advanced 7" (rev V1.1), described in data in
 * board_desc.c. The rest of the board contract is the family library's (crowpanel_p4_family).
 */
#include "board.h"
#include "board_desc.h"

const board_info_t *board_info(void)
{
    return &board_crowpanel_p4_desc;
}

const char *board_name(void)
{
    return board_crowpanel_p4_desc.name;
}
