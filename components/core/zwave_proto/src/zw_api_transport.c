/*
 * zw_api_transport.c - what a generic transport needs to run any Serial API command without a
 * per-message switch (see zw_api.h): §6's table as data, and where a built request's callback ID
 * sits so it can be overwritten with a freshly allocated one.
 */
#include "zwave_proto/zw_api.h"

typedef struct {
    uint8_t func;
    zw_api_func_info_t info;
} func_info_row_t;

/* serial-api.md §6's table, phases 1-3. "required" folds a function's own "-" (an inbound-only
 * frame that is not something the panel is required to have the module support) into false. A
 * function whose callback column names a different, uncorrelated frame (SoftReset's
 * SerialAPIStarted, SendDataAbort's SendData callback, RequestNodeInfo's ApplicationUpdate) is
 * ZW_CB_NONE: nothing here waits on a callback ID for it. */
static const func_info_row_t FUNC_INFO[] = {
    {0x02u, {true,  ZW_CB_NONE,           true,  1u}}, /* GetSerialApiInitData */
    {0x03u, {false, ZW_CB_NONE,           true,  1u}}, /* SetApplicationNodeInformation */
    {0x04u, {false, ZW_CB_NONE,           false, 1u}}, /* ApplicationCommandHandler (in) */
    {0x05u, {true,  ZW_CB_NONE,           true,  1u}}, /* GetControllerCapabilities */
    {0x06u, {true,  ZW_CB_NONE,           false, 1u}}, /* SetSerialApiTimeouts */
    {0x07u, {true,  ZW_CB_NONE,           true,  1u}}, /* GetSerialApiCapabilities */
    {0x08u, {false, ZW_CB_NONE,           true,  1u}}, /* SoftReset (SerialAPIStarted, uncorrelated) */
    {0x09u, {true,  ZW_CB_NONE,           true,  1u}}, /* GetProtocolVersion */
    {0x0Au, {false, ZW_CB_NONE,           false, 1u}}, /* SerialAPIStarted (in) */
    {0x0Bu, {true,  ZW_CB_NONE,           true,  1u}}, /* SerialAPISetup */
    {0x10u, {true,  ZW_CB_NONE,           false, 3u}}, /* SetRFReceiveMode */
    {0x13u, {true,  ZW_CB_ONE_FINAL,      true,  1u}}, /* SendData */
    {0x15u, {true,  ZW_CB_NONE,           true,  1u}}, /* GetControllerVersion */
    {0x16u, {false, ZW_CB_NONE,           true,  1u}}, /* SendDataAbort (SendData's cb, uncorrelated) */
    {0x20u, {true,  ZW_CB_NONE,           true,  1u}}, /* GetControllerId */
    {0x27u, {false, ZW_CB_NONE,           false, 3u}}, /* EnterBootloader */
    {0x2Eu, {true,  ZW_CB_NONE,           false, 3u}}, /* NVMOperations */
    {0x3Bu, {true,  ZW_CB_NONE,           false, 2u}}, /* GetBackgroundRSSI */
    {0x3Du, {true,  ZW_CB_NONE,           false, 3u}}, /* ExtendedNVMOperations */
    {0x41u, {true,  ZW_CB_NONE,           true,  1u}}, /* GetNodeProtocolInfo */
    {0x42u, {false, ZW_CB_ONE_FINAL,      true,  1u}}, /* HardReset */
    {0x46u, {true,  ZW_CB_ONE_FINAL,      false, 2u}}, /* AssignReturnRoute */
    {0x47u, {true,  ZW_CB_ONE_FINAL,      false, 2u}}, /* DeleteReturnRoute */
    {0x48u, {false, ZW_CB_ONE_FINAL,      false, 2u}}, /* RequestNodeNeighborUpdate */
    {0x49u, {false, ZW_CB_NONE,           false, 1u}}, /* ApplicationUpdate (in): the module's own frame, never in the mask check */
    {0x4Au, {false, ZW_CB_STATUS_REPORTS, true,  1u}}, /* AddNodeToNetwork */
    {0x4Bu, {false, ZW_CB_STATUS_REPORTS, true,  1u}}, /* RemoveNodeFromNetwork */
    {0x51u, {true,  ZW_CB_ONE_FINAL,      true,  1u}}, /* AssignSUCReturnRoute */
    {0x54u, {true,  ZW_CB_ONE_FINAL,      true,  1u}}, /* SetSUCNodeId */
    {0x55u, {true,  ZW_CB_ONE_FINAL,      false, 2u}}, /* DeleteSUCReturnRoute */
    {0x56u, {true,  ZW_CB_NONE,           true,  1u}}, /* GetSUCNodeId */
    {0x60u, {true,  ZW_CB_NONE,           true,  1u}}, /* RequestNodeInfo (ApplicationUpdate, uncorrelated) */
    {0x61u, {true,  ZW_CB_ONE_FINAL,      false, 2u}}, /* RemoveFailedNode */
    {0x62u, {true,  ZW_CB_NONE,           false, 2u}}, /* IsFailedNode */
    {0x63u, {true,  ZW_CB_ONE_FINAL,      false, 2u}}, /* ReplaceFailedNode */
    {0x80u, {true,  ZW_CB_NONE,           false, 2u}}, /* GetRoutingInfo */
    {0xA8u, {false, ZW_CB_NONE,           false, 1u}}, /* BridgeApplicationCommandHandler (in) */
    {0xD9u, {true,  ZW_CB_NONE,           false, 3u}}, /* Shutdown */
    {0xDAu, {true,  ZW_CB_NONE,           false, 3u}}, /* GetLongRangeNodes */
    {0xDBu, {true,  ZW_CB_NONE,           false, 3u}}, /* GetLongRangeChannel */
    {0xDCu, {true,  ZW_CB_NONE,           false, 3u}}, /* SetLongRangeChannel */
};
#define N_FUNC_INFO (sizeof(FUNC_INFO) / sizeof(FUNC_INFO[0]))

bool zw_api_func_info(uint8_t func, zw_api_func_info_t *out)
{
    if (out == NULL) {
        return false;
    }
    for (size_t i = 0; i < N_FUNC_INFO; i++) {
        if (FUNC_INFO[i].func == func) {
            *out = FUNC_INFO[i].info;
            return true;
        }
    }
    return false;
}

bool zw_api_callback_id_offset(uint8_t func, const uint8_t *payload, size_t len, size_t *offset)
{
    if (payload == NULL || offset == NULL) {
        return false;
    }
    switch (func) {
    /* mode | callback ID [| NWI home ID (4) | auth home ID (4)]: byte 1, whichever variant */
    case (uint8_t)ZW_FUNC_ADD_NODE_TO_NETWORK:
    case (uint8_t)ZW_FUNC_REMOVE_NODE_FROM_NETWORK:
        if (len < 2u) {
            return false;
        }
        *offset = 1u;
        return true;
    /* every other callback-bearing request ends with its callback ID */
    case (uint8_t)ZW_FUNC_SEND_DATA:
    case (uint8_t)ZW_FUNC_HARD_RESET:
    case (uint8_t)ZW_FUNC_ASSIGN_SUC_RETURN_ROUTE:
    case (uint8_t)ZW_FUNC_SET_SUC_NODE_ID:
    case (uint8_t)ZW_FUNC_ASSIGN_RETURN_ROUTE:
    case (uint8_t)ZW_FUNC_DELETE_RETURN_ROUTE:
    case (uint8_t)ZW_FUNC_REQUEST_NODE_NEIGHBOR_UPDATE:
    case (uint8_t)ZW_FUNC_REMOVE_FAILED_NODE:
    case (uint8_t)ZW_FUNC_REPLACE_FAILED_NODE:
    case (uint8_t)ZW_FUNC_DELETE_SUC_RETURN_ROUTE:
        if (len < 1u) {
            return false;
        }
        *offset = len - 1u;
        return true;
    default:
        return false; /* no callback ID in this function's request (or no request at all) */
    }
}
